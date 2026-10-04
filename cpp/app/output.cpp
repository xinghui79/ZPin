#include "output.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSet>
#include <QThread>
#include <QtGlobal>

#include <atomic>
#include <chrono>
#include <memory>

#include "config.hpp"
#include "defaults.hpp"
#include "logging.hpp"
#include "win32util.hpp"

namespace zpin::output {

namespace {

constexpr const char* kKnownExt[] = {"png", "jpg", "jpeg", "bmp", "webp", "tif", "tiff"};

// 这些格式的编码器认 quality 参数（有损格式）。webp 拉满 100 = 无损 webp；
// tiff 虽在列表外（Qt 的 tiff 编码器忽略 quality），png/bmp/tiff 一律无损没这个旋钮。
bool extTakesQuality(const QString& ext) {
    return ext == QLatin1String("jpg") || ext == QLatin1String("jpeg") ||
           ext == QLatin1String("webp");
}

// 在途写盘计数：QThread::create 起的线程自删、没人 join，进程退出时若直接
// 走人，QImage::save 会被腰斩留下截断文件（drain() 靠它收口）。
std::atomic<int> g_inflight{0};

// RAII 计数：比手工 ++/-- 可靠，worker 里 return/抛异常都不会漏减
class Inflight {
public:
    Inflight() { g_inflight.fetch_add(1, std::memory_order_relaxed); }
    ~Inflight() { g_inflight.fetch_sub(1, std::memory_order_release); }
};

// 自动保存的「查重 + 写盘」串行锁：查重在写盘前完成才有意义，两次快速截图
// 各自起线程时，不在锁内先查后写会选到同一个路径互相覆盖
struct NameLock {
    std::mutex m;
};
NameLock& nameLock() {
    static NameLock g;
    return g;
}

QString resolveDir(const char* key) {
    const QString d = config::getStr(key).trimmed();
    if (!d.isEmpty())
        return d;  // 目录可能尚不存在，由保存负责创建
    // 用户把这个键清空的场合：回退到 defaults 的桌面判定（它会认 OneDrive 重定向的
    // 桌面），别自己再拼一遍 homePath()/Desktop —— 那会把桌面在云盘上的用户
    // 存进一个不存在的目录。
    const QString fallback = defaults::defaultDesktop();
    return QDir(fallback).exists() ? fallback : QDir::homePath();
}

// 决定实际使用的保存扩展名。优先级：显式参数 > 记住的上次格式（仅 useLast，且要勾了
// 「记住上次保存的格式」）> 设置页「默认输出格式」（空 = 不指定）> 模板自带后缀 >
// png。模板里的 ".png" 只当占位。
//
// 快速保存 / 自动保存 / 历史保存一律传 useLast=false：那条路径上根本没有对话框，
// 让很久以前在别处选过一次 jpg 就把此后每一次无损保存悄悄换成有损，正是"截图工具
// 存出糊图"的经典来源。「记住上次格式」只该决定下次对话框的初始选项。
QString effectiveExt(const QString& tmpl, const QString& ext, bool useLast) {
    QString e = ext;
    if (e.isEmpty() && useLast && config::getBool("Output/remember_ext"))
        e = config::getStr("Output/last_ext");
    if (e.isEmpty())
        e = config::getStr("Output/default_ext");
    if (e.isEmpty())
        e = QFileInfo(tmpl).suffix();
    e = e.toLower();
    while (e.startsWith(QLatin1Char('.')))
        e.remove(0, 1);
    if (e.isEmpty())
        e = "png";
    for (const char* k : kKnownExt) {
        if (e == k)
            return e;
    }
    return "png";
}

// quality 必须由**调用方在主线程**取好传进来，不能在这里读 config：
// 本函数跑在写盘线程上，而 QSettings 官方明确「reentrant but not thread-safe」，
// 同一个实例被主线程的 config::set/sync 改、被这里读，就是 QHash/QSet 并发
// detach 竞争——能崩，也可能读到一个撕裂的 quality 存进文件。
bool saveToPath(const QImage& img, const QString& path, const QString& ext, int quality) {
    // 先写同目录的 .part，成功了才改名换上：直接往终体写的话，编码途中进程被走
    // 或磁盘满就留下「文件在、打不开」的截断图（output.hpp 为 drain() 记的那个坑），
    // 而改名是同一目录内的原子操作，终体只会是完整的。
    // 必须显式传格式：QImage::save 靠后缀判格式，"xxx.png.part" 的后缀是 part，
    // 不传 format 它会认不出来而直接失败。
    const QString tmp = path + QLatin1String(".part");
    QFile::remove(tmp);   // 上次没走完的残留
    const QByteArray fmt = ext.toLatin1();
    const bool saved = extTakesQuality(ext) ? img.save(tmp, fmt.constData(), quality)
                                            : img.save(tmp, fmt.constData());
    if (!saved) {
        QFile::remove(tmp);
        log::warn("output", QStringLiteral("临时文件写入失败：%1").arg(tmp));
        return false;
    }
    // 覆盖已存在的目标必须走 replaceFile（MoveFileEx REPLACE_EXISTING）：
    // QFile::rename 不覆盖，而「先删旧再改名」在改名失败时（杀软短暂锁文件等）
    // 两头都空——旧文件已经没了、新的又没就位。
    if (!win32::replaceFile(tmp, path)) {
        QFile::remove(tmp);
        log::warn("output", QStringLiteral("改名换装失败：%1 -> %2").arg(tmp, path));
        return false;
    }
    return true;
}

}  // namespace

QString renderName(const QString& tmpl) {
    const QDateTime now = QDateTime::currentDateTime();
    // 字段按长度降序排列：先试 yyyy 再试 MM，避免 "MM" 吃掉 "yyyy" 的后半。
    // 表里只存「占位名 -> Qt 日期格式」，值必须每次用当次 now 现取：早先写成
    // static { {"yyyy", now.toString("yyyy")}, ... }，函数内 static 只初始化一次，
    // 会把整个进程的文件名时间戳冻死在第一次保存那一刻。
    static const std::vector<std::pair<QString, QString>> kFields = {
        {"yyyy", "yyyy"}, {"MM", "MM"}, {"dd", "dd"},
        {"HH", "HH"},     {"mm", "mm"}, {"ss", "ss"},
    };
    const auto renderGroup = [&now](const QString& group) {
        QString out;
        qsizetype i = 0;
        while (i < group.size()) {
            bool matched = false;
            for (const auto& [name, fmt] : kFields) {
                if (group.mid(int(i), name.size()) == name) {
                    out += now.toString(fmt);
                    i += name.size();
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                out += group[int(i)];
                ++i;
            }
        }
        return out;
    };
    QString out;
    qsizetype i = 0;
    while (i < tmpl.size()) {
        const qsizetype start = tmpl.indexOf(QLatin1Char('$'), i);
        if (start < 0) {
            out += tmpl.mid(i);
            break;
        }
        out += tmpl.mid(i, start - i);
        const qsizetype end = tmpl.indexOf(QLatin1Char('$'), start + 1);
        if (end < 0) {
            out += tmpl.mid(start);
            break;
        }
        out += renderGroup(tmpl.mid(start + 1, end - start - 1));
        i = end + 1;
    }
    return out;
}

// 渲染结果净化成一个「单层、合法」的文件名。
//
// renderName 只做令牌替换，用户模板是自由文本，两个坑：
//   1) 模板里带 '/'：QFileInfo("图集/2026/09/29.png").completeBaseName() == "29"，
//      目录部分被静默吞掉，全部文件堆在保存根目录用 _1/_2 去重——用户以为按
//      日期分了目录，实际没有，且界面上毫无提示。
//   2) 模板里带 Windows 非法字符（: * ? " < > | 等，常见于 $HH$$mm$$ss$ 里
//      手写冒号）：路径拼出来必然 save 失败，只有日志一行 + 气泡"保存失败"。
// 名字模板**不支持子目录**（没这个设计），所以这里剥掉路径成分并把非法字符
// 换成下划线，同时打日志说明——宁可让用户看见一句提醒，也不要静默写错地方。
QString sanitizeFileName(const QString& rendered) {
    QString name = QFileInfo(rendered).fileName();   // 丢掉目录成分
    if (name != rendered) {
        log::warn("output", QString("文件名模板「%1」渲染出路径成分（%2），已只取文件名部分；"
                                    "模板不支持子目录")
                           .arg(rendered, name));
    }
    static const QString kIllegal = QStringLiteral(R"(<>:"/\|?*)");
    QString cleaned;
    cleaned.reserve(name.size());
    bool replaced = false;
    for (const QChar& c : name) {
        // 结尾的点与空格在 Windows 上会被 API 静默截断，一并换掉
        if (kIllegal.contains(c) || c.unicode() < 0x20) {
            cleaned += QLatin1Char('_');
            replaced = true;
        } else {
            cleaned += c;
        }
    }
    while (cleaned.endsWith(QLatin1Char('.')) || cleaned.endsWith(QLatin1Char(' '))) {
        cleaned.chop(1);
        replaced = true;
    }
    if (replaced)
        log::warn("output", QString("文件名模板渲染出 Windows 不允许的字符：%1 -> %2")
                           .arg(name, cleaned));
    // Windows 保留设备名（CON/PRN/AUX/NUL/COM1..9/LPT1..9）也不能当文件名
    static const QSet<QString> kReserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9"),
    };
    const QString base = QFileInfo(cleaned).completeBaseName().toUpper();
    if (kReserved.contains(base)) {
        log::warn("output", QString("文件名「%1」是 Windows 保留名，已加前缀 _").arg(cleaned));
        cleaned = QLatin1Char('_') + cleaned;
    }
    return cleaned.isEmpty() ? QStringLiteral("ZPin") : cleaned;
}

// 「无对话框保存」会实际落盘的文件名（设置页的模板预览用）：saveImageAsync 先
// 剥掉模板自带的后缀、再按裁决链补一个真实后缀，所以「默认输出格式」不是模板
// 后缀时，直接预览 renderName 会和落盘文件名对不上。不借道 sanitizeFileName——
// 它每净化一处就打一行日志，预览跟着用户敲字刷屏。
QString previewName(const QString& tmpl) {
    const QString name = renderName(tmpl);
    const QString stem = QFileInfo(name).completeBaseName();
    return stem + QLatin1Char('.') + effectiveExt(name, QString(), false);
}

QString defaultDir() {
    return resolveDir("Output/default_dir");
}

void saveImageAsync(const QImage& img, const QString& ext, const QString& directory,
                    std::function<void(QString)> onDone) {
    const QString name = sanitizeFileName(renderName(config::getStr("Output/name_template")));
    const QString stem = QFileInfo(name).completeBaseName();
    const QString nameExt = "." + effectiveExt(name, ext, false);
    const QString dir = directory.isEmpty() ? defaultDir() : directory;
    // 质量在这里（主线程）取，worker 只收值——见 saveToPath 的注释
    const int quality = config::getInt("Output/quality");
    // 这里**不**写 Output/last_ext：本函数是快速保存 / 自动保存 / 历史保存的共用
    // 路径，它自己也不读 last_ext（见 effectiveExt 的 useLast）。「记住上次保存的
    // 格式」记的是上次在「另存为」对话框里选的那个，只有对话框那条路径该更新它。
    // Inflight 必须在 start() **之前**自增（表达「已提交、尚未收尾」，drain()
    // 靠它判断能不能安全退出），但**递减**必须发生在 worker 写盘收尾：栈局部
    // guard 的话函数一返回计数就归零，drain() 看到 0 直接放行，退出时正在写的
    // PNG 照样被腰斩——机制整个失效。所以递 shared_ptr 进捕获列表，最后一份
    // 副本随 lambda 析构时才减。
    auto guard = std::make_shared<Inflight>();
    QThread* worker = QThread::create([img, dir, stem, nameExt, quality, onDone, guard]() {
        QString path;
        bool ok = false;
        // 不套 try/catch：块内全是返回错误码的 Qt API，没有抛点（无请求不防御）。
        std::lock_guard<std::mutex> lock(nameLock().m);
        QDir().mkpath(dir);
        path = dir + "/" + stem + nameExt;
        int i = 1;
        while (QFile::exists(path))
            path = QString("%1/%2_%3%4").arg(dir, stem).arg(i++).arg(nameExt);
        ok = saveToPath(img, path, nameExt.mid(1), quality);
        if (!ok)
            log::error("output", QString("保存失败：%1").arg(path.isEmpty() ? dir : path));
        if (onDone)
            onDone(ok ? path : QString());
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void saveImageDialogAsync(const QImage& img, QWidget* parent,
                          std::function<void(std::optional<QString>)> onDone) {
    // 弹对话框（主线程）确定路径后，在工作线程编码写盘
    const QString name = sanitizeFileName(renderName(config::getStr("Output/name_template")));
    const QString stem = QFileInfo(name).completeBaseName();
    // 扩展名裁决与 saveImageAsync 共用 effectiveExt（记住的上次格式 -> 模板后缀 -> png），
    // 非法值统一回落 png，避免两处规则各写一套而漂移
    const QString ext = effectiveExt(name, QString(), true);
    QString path = QFileDialog::getSaveFileName(
        parent, "另存为",
        QString("%1/%2.%3").arg(defaultDir(), stem, ext),
        "PNG 图片 (*.png);;JPEG 图片 (*.jpg);;WEBP 图片 (*.webp);;BMP 图片 (*.bmp);;"
        "TIFF 图片 (*.tif)");
    if (path.isEmpty()) {
        if (onDone)
            onDone(std::nullopt);
        return;
    }
    QString ext2 = QFileInfo(path).suffix().toLower();
    // 手输的野后缀（".foo"、".doc"）不认。但不能只把**内容**换成默认格式、却留着
    // 用户写的后缀——saveToPath 用显式 format 写盘，会得到一个「扩展名自称 doc、
    // 里面装着 PNG」的文件，双击用 Word 打开只会报错。后缀一并换成真实格式。
    bool known = false;
    for (const char* k : kKnownExt)
        known |= (ext2 == QLatin1String(k));
    if (!known) {
        if (!ext2.isEmpty())
            path.chop(ext2.length() + 1);
        path += QLatin1Char('.') + ext;
        ext2 = ext;
    }
    if (config::getBool("Output/remember_ext")) {
        config::set("Output/last_ext", ext2);
        config::sync();
    }
    const int quality = config::getInt("Output/quality");
    // 递减必须发生在 worker 收尾（理由同 saveImageAsync 的长注释）。
    auto guard = std::make_shared<Inflight>();
    QThread* worker = QThread::create([img, path, ext2, quality, onDone, guard]() {
        const bool ok = saveToPath(img, path, ext2, quality);
        if (!ok)
            log::error("output", QString("保存失败：%1").arg(path));
        if (onDone)
            onDone(ok ? std::optional<QString>(path) : std::optional<QString>(QString()));
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void drain() {
    // 与 history.cpp 析构里的做法一致：自旋等在途任务收尾。写的是本地文件，
    // 单张几百毫秒，退出时多等这一会儿远比留个截断文件划算。
    //
    // 但必须有上限：慢盘 / 杀软占着文件时主线程会永久自旋，那等于进程杀不掉，
    // 用户只能强杀——强杀又正好留下 drain 想防的截断文件，机制反噬自己。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_inflight.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() > deadline) {
            log::warn("output", QStringLiteral("等待写盘收尾超过 5 秒（仍有 %1 个在途），"
                                               "放弃等待——那个文件可能是截断的")
                                    .arg(g_inflight.load(std::memory_order_relaxed)));
            return;
        }
        QThread::msleep(1);
    }
}

}  // namespace zpin::output
