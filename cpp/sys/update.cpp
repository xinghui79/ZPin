#include "update.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QRunnable>
#include <QThreadPool>
#include <QTimer>
#include <QtGlobal>

#include "config.hpp"
#include "defaults.hpp"
#include "logging.hpp"
#include "rcore.hpp"
#include "rust/cxx.h"
#include "win32util.hpp"

namespace zpin {
namespace update {
namespace {

constexpr const char* kLogCat = "zpin.update";

// GitHub API 要求 User-Agent；调 API 还要 vnd.github 的 Accept。
QString apiHeaders() {
    return QStringLiteral("User-Agent: ZPin/%1\r\nAccept: application/vnd.github+json")
        .arg(QLatin1String(defaults::VERSION));
}

QString downloadHeaders() {
    return QStringLiteral("User-Agent: ZPin/%1").arg(QLatin1String(defaults::VERSION));
}

QString mbText(qint64 bytes) {
    return QString::number(bytes / 1048576.0, 'f', 1);
}

// 自动更新专用的私有线程池（单线程）。
//
// 早先这些任务全丢在 QThreadPool::globalInstance() 上，和历史压缩、OCR、长截图
// 抢同一批槽位，而默认 maxThreadCount 就是 CPU 核数：一次下载（最长几分钟）
// 就占掉一个槽，长截图（几十秒）再占一个，四核机器上 OCR 只能排在队尾——
// 用户点「识别文字」却要为一次后台下载干等。检查/下载/换装本来就互斥
// （状态机串着走），单线程私有池既够用又不会饿死别的功能。
//
// 故意 new 出来不释放：~QThreadPool 会 waitForDone()，静态析构时若正好有一次
// 下载在飞（最长几分钟），进程退出就会被卡住等它下完。泄漏掉，OS 会回收。
QThreadPool& pool() {
    static QThreadPool* g = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(1);
        p->setExpiryTimeout(30'000);
        return p;
    }();
    return *g;
}

// 工作线程任务：跑完把结果投递回 flow 主线程。flow 由 App 持有，而 App 是
// main 的栈对象——exec() 返回后 ~App 会先于拖网（最长一次网络超时几十秒）的
// 池线程析构它，所以这里必须 QPointer 判活：flow 已亡就丢弃结果，退出路径
// 不再对悬空指针 invokeMethod。
class CheckTask : public QRunnable {
public:
    CheckTask(UpdateFlow* flow, bool silent) : m_flow(flow), m_silent(silent) {}

    void run() override {
        const Latest latest = latestRelease();
        UpdateFlow* flow = m_flow.data();
        if (!flow)
            return;
        QMetaObject::invokeMethod(
            flow,
            [flow, latest, silent = m_silent] { flow->onChecked(latest, silent); },
            Qt::QueuedConnection);
    }

private:
    QPointer<UpdateFlow> m_flow;
    bool m_silent;
};

class InstallTask : public QRunnable {
public:
    InstallTask(UpdateFlow* flow, Release release, QString zip, QString unpack)
        : m_flow(flow), m_release(std::move(release)), m_zip(std::move(zip)),
          m_unpack(std::move(unpack)) {}

    void run() override {
        QString error;
        if (!downloadTo(m_release, m_zip, &error)) {
            post({}, error);
            return;
        }
        post(extractZip(m_zip, m_unpack, &error), error);
    }

private:
    void post(const QString& sourceDir, const QString& error) {
        UpdateFlow* flow = m_flow.data();
        if (!flow)
            return;
        QMetaObject::invokeMethod(
            flow,
            [flow, sourceDir, error] { flow->onDownloaded(sourceDir, error); },
            Qt::QueuedConnection);
    }

    QPointer<UpdateFlow> m_flow;
    Release m_release;
    QString m_zip;
    QString m_unpack;
};

// QProcess 拉起控制台子进程（tar / cmd）时不闪黑框。
void runHidden(QProcess& proc) {
    proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= win32::kCreateNoWindow;
    });
}

}  // namespace

int compareVersions(const QString& a, const QString& b) {
    const auto segment = [](const QStringList& list, int i) -> qlonglong {
        if (i >= list.size())
            return 0;
        const QString& s = list.at(i);
        qlonglong v = 0;
        for (int k = 0; k < s.size() && s.at(k).isDigit(); ++k)
            v = v * 10 + s.at(k).digitValue();
        return v;
    };
    const QStringList as = a.split(QLatin1Char('.'));
    const QStringList bs = b.split(QLatin1Char('.'));
    for (int i = 0; i < qMax(as.size(), bs.size()); ++i) {
        const qlonglong d = segment(as, i) - segment(bs, i);
        if (d != 0)
            return d > 0 ? 1 : -1;
    }
    // 数值段全等时按预发布号分胜负（semver 语义）：带 "-beta" 这类后缀的低于
    // 不带后缀的正式版。否则 "1.4.0-beta" == "1.4.0"，正式版一发用户收不到提示。
    const bool preA = a.contains(QLatin1Char('-'));
    const bool preB = b.contains(QLatin1Char('-'));
    if (preA != preB)
        return preA ? -1 : 1;
    return 0;
}

Latest latestRelease() {
    Latest out;
    try {
        const QByteArray body =
            rcore::httpGet(QLatin1String(defaults::UPDATE_API_URL), apiHeaders());
        QJsonParseError parse = {};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
            out.error = QStringLiteral("响应不是合法 JSON");
            return out;
        }
        const QJsonObject obj = doc.object();
        Release release;
        release.version = obj.value(QStringLiteral("tag_name")).toString();
        if (release.version.startsWith(QLatin1Char('v')) ||
            release.version.startsWith(QLatin1Char('V')))
            release.version.remove(0, 1);
        // tag 会拼进 %TEMP% 的下载/解压路径：GitHub tag 语法允许 '/'，放进来
        // 轻则建不出目录必败，重则被 ".." 带出临时目录。版本号只留
        // [A-Za-z0-9.-]，其余一律丢弃（版本比较只看数字与预发布号，不受影响）。
        QString sane;
        for (const QChar c : release.version)
            if (c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('-'))
                sane += c;
        release.version = sane;
        if (release.version.isEmpty()) {
            out.error = QStringLiteral("发布数据里没有 tag_name");
            return out;
        }
        const QJsonArray assets = obj.value(QStringLiteral("assets")).toArray();
        for (const QJsonValue& asset : assets) {
            const QJsonObject item = asset.toObject();
            if (item.value(QStringLiteral("name"))
                    .toString()
                    .endsWith(QLatin1String(".zip"), Qt::CaseSensitivity::CaseInsensitive)) {
                release.downloadUrl =
                    item.value(QStringLiteral("browser_download_url")).toString();
                release.size = qint64(item.value(QStringLiteral("size")).toDouble());
                // digest 在**资产**对象上（"sha256:<hex>"），发布对象根本没有这个字段：
                // 早先写在外面，读出来恒为空，于是 downloadTo 里的 SHA-256 校验
                // 一次都没跑过，只剩字节数比对。
                release.digest = item.value(QStringLiteral("digest")).toString();
                break;
            }
        }
        out.release = release;
    } catch (const rust::Error& e) {
        const QString what = QString::fromUtf8(e.what());
        // 仓库还没发过版时 GitHub 对 releases/latest 回 404，这不算故障
        out.error = what == QLatin1String("HTTP 404")
                        ? QStringLiteral("发布仓库还没有任何版本")
                        : what;
        log::warn(kLogCat, QStringLiteral("检查更新失败：%1").arg(out.error));
    }
    return out;
}

bool downloadTo(const Release& release, const QString& dest, QString* error) {
    try {
        rcore::downloadToFile(release.downloadUrl, dest, downloadHeaders());
    } catch (const rust::Error& e) {
        const QString what = QString::fromUtf8(e.what());
        if (error)
            *error = what;
        log::warn(kLogCat, QStringLiteral("下载失败：%1").arg(what));
        return false;
    }
    // 大小对得上才算下完整：残缺 zip 会让换装拷出个四不像
    const qint64 got = QFileInfo(dest).size();
    if (release.size > 0 && got != release.size) {
        if (error)
            *error = QStringLiteral("下载不完整（%1 / %2 MB）")
                         .arg(mbText(got), mbText(release.size));
        log::warn(kLogCat, QStringLiteral("下载不完整：%1 / %2 字节").arg(got).arg(release.size));
        return false;
    }
    // 发布资产带 sha256 摘要时做完整性校验（防 CDN 截断/替换）。
    // 打不开文件同样算校验失败：早先写成 if (f.open(...)) 把校验整段跳过，
    // 却仍落到函数末尾 return true，于是「文件被占用/被杀软锁定」会直接
    // 变成「校验通过」，残缺包接着覆盖用户的安装目录。
    if (release.digest.startsWith(QLatin1String("sha256:"))) {
        QFile f(dest);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("无法读取安装包以校验（%1）").arg(f.errorString());
            log::warn(kLogCat, QStringLiteral("安装包打不开，SHA-256 校验中止：%1").arg(dest));
            return false;
        }
        QCryptographicHash hash(QCryptographicHash::Algorithm::Sha256);
        hash.addData(&f);
        if (hash.result().toHex() != release.digest.mid(6)) {
            if (error)
                *error = QStringLiteral("安装包校验失败（SHA-256 不符）");
            log::warn(kLogCat, QStringLiteral("安装包 SHA-256 校验失败：%1").arg(dest));
            return false;
        }
        log::info(kLogCat, QStringLiteral("安装包 SHA-256 校验通过"));
    }
    log::info(kLogCat,
              QStringLiteral("安装包下载完成：%1（%2 MB）").arg(dest, mbText(got)));
    return true;
}

QString extractZip(const QString& zip, const QString& dir, QString* error) {
    QDir().mkpath(dir);
    QProcess tar;
    runHidden(tar);
    tar.setProgram(QStringLiteral("tar"));
    tar.setArguments({QStringLiteral("-xf"), zip, QStringLiteral("-C"), dir});
    tar.start();
    if (!tar.waitForStarted(5000) || !tar.waitForFinished(180000)) {
        if (error)
            *error = QStringLiteral("系统 tar 无法执行（Win10 1803+ 自带）");
        return {};
    }
    if (tar.exitStatus() != QProcess::ExitStatus::NormalExit || tar.exitCode() != 0) {
        const QString message = QString::fromLocal8Bit(tar.readAllStandardError()).trimmed();
        if (error)
            *error = QStringLiteral("解压失败：%1").arg(message);
        log::warn(kLogCat, QStringLiteral("解压失败：%1").arg(message));
        return {};
    }
    // zip 内容若包在单一顶层文件夹里，取那层当内容根；否则解压根就是内容根
    const QFileInfoList entries =
        QDir(dir).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    if (entries.size() == 1 && entries.first().isDir())
        return entries.first().absoluteFilePath();
    return dir;
}

void startInstaller(const QString& sourceDir, const QString& workDir, const QString& zip) {
    const QString installDir = QCoreApplication::applicationDirPath();
    const QString exeName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    const QString pid = QString::number(QCoreApplication::applicationPid());
    const QString bat = QDir::tempPath() + QStringLiteral("/ZPin_update.bat");
    // 路径一律转 8.3 短名后再进脚本：cmd 按**本机 OEM 码页**解析 .bat，而安装目录
    // 与临时目录经常含中文（%USERPROFILE%、D:\软件\ZPin）。非中文码页的系统上
    // toLocal8Bit 会把中文写成 '?'，于是 xcopy 拷了个不存在的路径、静默失败，
    // 用户保留半套文件还以为已经更新完。短名是纯 ASCII，绕开整个码页问题；
    // 取不到短名（卷关了 8.3 名创建）时 shortPath 原样返回，行为不退步。
    const QString src = win32::shortPath(sourceDir);
    const QString dst = win32::shortPath(installDir);
    const QString work = win32::shortPath(workDir);
    const QString zipShort = win32::shortPath(zip);
    // 失败日志的位置跟应用自己的日志目录走（config::appDir 会兜 LOCALAPPDATA
    // 缺失的情况），不再硬写 %LOCALAPPDATA%\ZPin —— 那是第二个事实来源。
    const QString logFile = win32::shortPath(config::appDir()) + QLatin1String("\\update.log");
    const QString script = QStringLiteral(
        "@echo off\r\n"
        "rem ZPin update: wait for the old process to exit, replace files, relaunch.\r\n"
        ":wait\r\n"
        "tasklist /FI \"PID eq %1\" 2>nul | find /I \"%2\" >nul 2>&1\r\n"
        "if not errorlevel 1 (\r\n"
        "  timeout /t 1 /nobreak >nul\r\n"
        "  goto wait\r\n"
        ")\r\n"
        "xcopy /Y /E /I /Q \"%3\\*\" \"%4\\\" >nul\r\n"
        "if errorlevel 1 (\r\n"
        "  echo %date% %time% update failed: cannot overwrite \"%4\" >> \"%7\"\r\n"
        "  exit /b 1\r\n"
        ")\r\n"
        "start \"\" \"%4\\%2\"\r\n"
        "rmdir /S /Q \"%5\"\r\n"
        "if exist \"%6\" del \"%6\" >nul 2>&1\r\n"
        "del \"%~f0\" >nul 2>&1\r\n")
        .arg(pid)
        .arg(exeName)
        .arg(src)
        .arg(dst)
        .arg(work)
        .arg(zipShort)
        .arg(logFile);
    QFile file(bat);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        log::warn(kLogCat, QStringLiteral("换装脚本写不进去：%1").arg(bat));
        return;
    }
    // 上面的路径都是短名（纯 ASCII），命令与注释也保持 ASCII，两种码页都读得对
    file.write(script.toLocal8Bit());
    file.close();

    QProcess starter;
    runHidden(starter);
    starter.setProgram(QStringLiteral("cmd"));
    starter.setArguments({QStringLiteral("/c"), bat});
    if (!starter.startDetached())
        log::warn(kLogCat, QStringLiteral("换装脚本启动失败"));
    // 调用方（confirmInstall）随后放开单实例互斥体并退出进程
}

UpdateFlow::UpdateFlow(QObject* parent) : QObject(parent) {
    // 下载进度不搞跨桥回调：定时器轮询 zip 文件大小就够用
    m_poll = new QTimer(this);
    m_poll->setInterval(500);
    connect(m_poll, &QTimer::timeout, this, [this] {
        m_received = QFileInfo(m_zip).size();
        emit changed();
    });
}

void UpdateFlow::autoCheck() {
    if (!config::getBool(QStringLiteral("General/update_auto_check")))
        return;
    const qint64 last = config::get(QStringLiteral("General/last_update_check")).toLongLong();
    if (QDateTime::currentMSecsSinceEpoch() - last < 24 * 3600 * 1000)
        return;
    m_state = State::Checking;
    emit changed();
    pool().start(new CheckTask(this, true));
}

void UpdateFlow::check() {
    if (m_state == State::Checking || m_state == State::Downloading)
        return;
    m_error.clear();
    m_state = State::Checking;
    emit changed();
    pool().start(new CheckTask(this, false));
}

void UpdateFlow::downloadAndInstall() {
    if (m_state == State::Ready) {
        confirmInstall();
        return;
    }
    if (m_state == State::Checking || m_state == State::Downloading || !canDownload())
        return;
    m_error.clear();
    m_received = 0;
    m_zip = QDir::tempPath() + QStringLiteral("/ZPin_update_%1.zip").arg(m_available.version);
    m_workDir = QDir::tempPath() + QStringLiteral("/ZPin_update_%1").arg(m_available.version);
    const QString unpack = m_workDir + QStringLiteral("/unpack");
    QFile::remove(m_zip);   // 上次可能留了残包，进度轮询会从旧字节数起跳
    m_state = State::Downloading;
    emit changed();
    m_poll->start();
    pool().start(new InstallTask(this, m_available, m_zip, unpack));
}

void UpdateFlow::onChecked(const Latest& latest, bool silent) {
    m_state = State::Idle;
    m_checked = true;
    config::set(QStringLiteral("General/last_update_check"),
                QDateTime::currentMSecsSinceEpoch());
    config::sync();
    if (!latest.release) {
        m_error = latest.error;
        log::warn(kLogCat, QStringLiteral("检查更新失败：%1").arg(latest.error));
        emit changed();
        return;
    }
    m_error.clear();
    if (compareVersions(QLatin1String(defaults::VERSION), latest.release->version) < 0) {
        m_available = *latest.release;
        log::info(kLogCat, QStringLiteral("发现新版本 v%1").arg(m_available.version));
        if (silent)
            emit newVersionFound(m_available.version);
    } else {
        m_available = {};
        if (silent) {
            log::info(kLogCat, QStringLiteral("已是最新版本 v%1")
                                   .arg(QLatin1String(defaults::VERSION)));
        }
    }
    emit changed();
}

void UpdateFlow::onDownloaded(const QString& sourceDir, const QString& error) {
    m_poll->stop();
    if (!error.isEmpty()) {
        m_state = State::Idle;   // 回到「发现新版本」，可重试
        m_error = error;
        emit changed();
        return;
    }
    m_sourceDir = sourceDir;
    m_state = State::Ready;
    m_error.clear();
    emit changed();
    confirmInstall();
}

void UpdateFlow::confirmInstall() {
    const auto answer = QMessageBox::question(
        nullptr, QStringLiteral("安装更新"),
        QStringLiteral("新版本 v%1 已就绪。关闭 ZPin 并完成安装？").arg(m_available.version),
        QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::No,
        QMessageBox::StandardButton::Yes);
    if (answer != QMessageBox::StandardButton::Yes)
        return;
    // 与托盘「重新启动」同一套顺序：先放开单实例互斥体再退，新进程才不会被顶掉
    startInstaller(m_sourceDir, m_workDir, m_zip);
    win32::releaseSingleInstance();
    qApp->quit();
}

QString UpdateFlow::statusText() const {
    switch (m_state) {
    case State::Checking:
        return QStringLiteral("正在检查更新……");
    case State::Downloading:
        return QStringLiteral("正在下载 %1 / %2 MB……")
            .arg(mbText(m_received), mbText(m_available.size));
    case State::Ready:
        return QStringLiteral("新版本 v%1 已下载，点「安装并重启」完成更新")
            .arg(m_available.version);
    case State::Idle:
        break;
    }
    if (hasUpdate()) {
        QString text = QStringLiteral("发现新版本 v%1（%2 MB），可下载安装")
                           .arg(m_available.version, mbText(m_available.size));
        if (m_available.downloadUrl.isEmpty())
            text += QStringLiteral("；发布没带安装包，请到发布页手动下载");
        if (!m_error.isEmpty())
            text += QStringLiteral("\n上次尝试失败：%1").arg(m_error);
        return text;
    }
    if (!m_error.isEmpty())
        return QStringLiteral("检查更新失败，请检查网络后重试：%1").arg(m_error);
    if (m_checked)
        return QStringLiteral("已是最新版本");   // 与主流软件的检查更新文案一致
    return QStringLiteral("尚未检查更新");
}

}  // namespace update
}  // namespace zpin
