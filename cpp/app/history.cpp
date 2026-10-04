#include "history.hpp"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPainter>
#include <QSaveFile>
#include <QThread>

#include <algorithm>

#include "config.hpp"
#include "engine.hpp"
#include "logging.hpp"

namespace zpin {

namespace {
constexpr int kPendingMax = 4;   // 同时在压的张数（工作线程里跑，不占主线程）
constexpr int kThumbSide = 256;  // 缩略图长边（物理像素）：历史墙一格用不完，留一倍余量给高 DPI
constexpr const char* kIndexName = "index.json";

QVector<HistoryStore*>& stores() {
    static QVector<HistoryStore*> g_stores;
    return g_stores;
}

QByteArray toPng(const QImage& img) {
    // 质量 0 = 最大压缩；截图这种大色块图压得动，代价留给工作线程付
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    if (!img.save(&buf, "PNG", 0))
        return {};
    return buf.data();
}

// 读一张落盘的 PNG（底图/缩略图同一个读法）
QImage readPng(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QImage img;
    img.loadFromData(f.readAll(), "PNG");
    return img;
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile out(path);
    return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size();
}

QJsonArray readJsonArray(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).array();
}

// 索引里的条目文本（落盘时没带时间戳，用文件的创建时间还原）
QString textOf(const QFileInfo& fi, const QImage& img) {
    return QStringLiteral("%1  %2×%3")
        .arg(fi.birthTime().toString(QStringLiteral("MM-dd HH:mm:ss")))
        .arg(img.width())
        .arg(img.height());
}
}  // namespace

QString HistoryStore::dirPath() { return config::appDir() + QStringLiteral("/history"); }

QString HistoryStore::storageDir() { return dirPath(); }

QString HistoryStore::indexPath() { return dirPath() + QLatin1Char('/') + QLatin1String(kIndexName); }

QString HistoryStore::pngFor(int id) {
    return dirPath() + QLatin1Char('/') + QString::number(id) + QStringLiteral(".png");
}

QString HistoryStore::jsonFor(int id) {
    return dirPath() + QLatin1Char('/') + QString::number(id) + QStringLiteral(".json");
}

QString HistoryStore::thumbFor(int id) {
    return dirPath() + QLatin1Char('/') + QString::number(id) + QStringLiteral(".thumb.png");
}

HistoryStore::HistoryStore(QObject* parent) : QObject(parent) {
    stores().append(this);
    load();
}

HistoryStore::~HistoryStore() {
    // 在途压缩任务的 invokeMethod 会解引用 this：等它们完成投递再析构
    // （toPng 一张几十 ms，退出时最多阻塞这一会儿；投递完成后事件若还没
    // 被 event loop 处理，会随对象销毁一起被清掉，安全）。
    while (m_packing.load() > 0)
        QThread::msleep(1);
    stores().removeAll(this);
}

void HistoryStore::setLimits() {
    m_limit = qMax(0, config::getInt("General/history_limit"));
    m_maxBytes = qMax<qint64>(1, config::getInt("General/history_max_mb")) * 1024 * 1024;
    // limit=0 不必特判清空：trim() 第一轮就是「条数超了就丢最旧」，一轮下来自然全空。
    trim();
    emitChanged();
}

void HistoryStore::add(const QImage& img, double dpr, const QJsonArray& doc) {
    if (img.isNull() || m_limit <= 0)
        return;
    const QDateTime now = QDateTime::currentDateTime();
    Item it;
    it.id = m_nextId++;
    it.img = img;
    it.dpr = dpr > 0 ? dpr : 1.0;
    it.text = QString("%1  %2×%3")
                  .arg(now.toString("MM-dd HH:mm:ss"))
                  .arg(img.width())
                  .arg(img.height());
    // 标注文档只有几 KB，直接在主线程写完（原子改名），省掉「底图落盘了、文档还在
    // 内存里」这种半截状态；没画标注时一个文件都不碰。
    if (!doc.isEmpty()) {
        QDir().mkpath(dirPath());
        QSaveFile f(jsonFor(it.id));
        if (f.open(QIODevice::WriteOnly)
            && f.write(QJsonDocument(doc).toJson(QJsonDocument::Compact)) > 0 && f.commit())
            it.docFile = f.fileName();
        else
            log::warn("history", QStringLiteral("写 %1 失败，这条历史只留底图").arg(jsonFor(it.id)));
    }
    m_items.insert(m_items.begin(), std::move(it));
    trim();
    emitChanged();
    compressPending();  // 立刻开始压+写盘，不等空闲整理，免得退出时这条还没落盘
}

double HistoryStore::dprOf(int id) const {
    for (const Item& it : m_items) {
        if (it.id == id)
            return it.dpr;
    }
    return 1.0;
}

const HistoryStore::Item* HistoryStore::find(int id) const {
    for (const Item& it : m_items) {
        if (it.id == id)
            return &it;
    }
    return nullptr;
}

QImage HistoryStore::baseOf(int id) const {
    const Item* it = find(id);
    if (!it)
        return {};
    if (!it->img.isNull())
        return it->img;  // 还没压盘的条目：原图就在内存里
    if (!it->file.isEmpty())
        return readPng(it->file);
    QImage img;  // 落盘失败留下的字节
    if (!it->png.isEmpty())
        img.loadFromData(it->png, "PNG");
    return img;
}

QImage HistoryStore::get(int id) const {
    const QImage base = baseOf(id);
    const Item* it = find(id);
    if (base.isNull() || !it || it->docFile.isEmpty())
        return base;
    return compose(base, readJsonArray(it->docFile));
}

QJsonArray HistoryStore::docOf(int id) const {
    const Item* it = find(id);
    if (it && !it->docFile.isEmpty())
        return readJsonArray(it->docFile);
    return {};
}

void HistoryStore::updateDoc(int id, const QJsonArray& doc) {
    Item* it = nullptr;
    for (Item& item : m_items) {
        if (item.id == id) {
            it = &item;
            break;
        }
    }
    if (!it)
        return;
    // 与 add() 同一套写法：文档只有几 KB，主线程 QSaveFile 原子写完
    if (doc.isEmpty()) {
        if (!it->docFile.isEmpty()) {
            QFile::remove(it->docFile);
            it->docFile.clear();
        }
    } else {
        QDir().mkpath(dirPath());
        QSaveFile f(jsonFor(id));
        if (f.open(QIODevice::WriteOnly)
            && f.write(QJsonDocument(doc).toJson(QJsonDocument::Compact)) > 0 && f.commit())
            it->docFile = f.fileName();
        else {
            log::warn("history", QStringLiteral("回写 %1 失败，这条历史保持原文档").arg(jsonFor(id)));
            return;
        }
    }
    // 旧缩略图是编辑前的画面：作废重生成。底图还没落盘的条目 requestThumb
    // 起不了任务，等 onPacked 顺手做——那边用的 docFile 就是刚回写的这份。
    if (!it->thumbFile.isEmpty()) {
        QFile::remove(it->thumbFile);
        it->thumbFile.clear();
    }
    m_thumbBusy.remove(id);
    requestThumb(id);
}

QImage HistoryStore::compose(const QImage& base, const QJsonArray& doc) {
    if (doc.isEmpty())
        return base;
    AnnotateEngine eng(base);
    if (!eng.loadDocument(doc))
        return base;  // 认不出的文档整份作废（loadDocument 已落日志），给干净底图
    QImage out = base.copy();
    QPainter p(&out);
    p.setRenderHint(QPainter::RenderHint::Antialiasing, true);
    eng.draw(p);
    p.end();
    return out;
}

QImage HistoryStore::makeThumb(const QImage& composed) {
    // 缩略图只是给历史墙认个脸，长边 256 足够；KeepAspectRatio 保比例，
    // 小图一律不放大（放大不产生信息，只产生糊）。
    if (composed.isNull())
        return {};
    if (qMax(composed.width(), composed.height()) <= kThumbSide)
        return composed;
    return composed.scaled(kThumbSide, kThumbSide, Qt::KeepAspectRatio,
                           Qt::SmoothTransformation);
}

QImage HistoryStore::thumbOf(int id) {
    const Item* it = find(id);
    if (!it)
        return {};
    if (!it->thumbFile.isEmpty()) {
        if (QImage t = readPng(it->thumbFile); !t.isNull())
            return t;
    }
    // 走到这里 = 这条还没有缩略图文件（升级前留下的老历史）：交工作线程补一张
    requestThumb(id);
    return {};
}

void HistoryStore::requestThumb(int id) {
    if (m_thumbBusy.contains(id))
        return;
    const Item* it = find(id);
    if (!it || it->file.isEmpty())
        return;  // 底图还没落盘：等 onPacked 那条任务顺带把缩略图一起做了
    const QString baseFile = it->file;
    const QString docFile = it->docFile;
    m_thumbBusy.insert(id);
    HistoryStore* self = this;
    m_packing.fetch_add(1);  // 先计数再起任务：任务可能抢在前头跑完
    QThreadPool::globalInstance()->start([self, id, baseFile, docFile]() {
        QImage composed = readPng(baseFile);
        if (!composed.isNull() && !docFile.isEmpty())
            composed = compose(composed, readJsonArray(docFile));
        const QImage thumb = makeThumb(composed);
        QString thumbFile;
        if (!thumb.isNull() && writeBytes(thumbFor(id), toPng(thumb)))
            thumbFile = thumbFor(id);
        QMetaObject::invokeMethod(self, "onThumb", Qt::QueuedConnection, Q_ARG(int, id),
                                  Q_ARG(QString, thumbFile));
        self->m_packing.fetch_sub(1);
    });
}

void HistoryStore::onThumb(int id, const QString& thumbFile) {
    m_thumbBusy.remove(id);
    for (Item& it : m_items) {
        if (it.id == id)
            it.thumbFile = thumbFile;
    }
    if (!thumbFile.isEmpty())
        emit thumbReady(id);
}

void HistoryStore::compressPending() {
    int room = kPendingMax - m_pending.size();
    if (room <= 0)
        return;
    // 从最旧的一端找还没落盘的条目
    for (auto it = m_items.rbegin(); it != m_items.rend() && room > 0; ++it) {
        if (!it->img.isNull() && !m_pending.contains(it->id)) {
            m_pending.insert(it->id);
            --room;
            QImage img = it->img;
            const QString docFile = it->docFile;
            const int id = it->id;
            HistoryStore* self = this;
            m_packing.fetch_add(1);  // 先计数再起任务：任务可能抢在前头跑完
            QThreadPool::globalInstance()->start([self, img, id, docFile]() {
                QDir().mkpath(dirPath());
                // 底图（画过标注时它是干净底图）压成 PNG 落盘，之后内存里不留字节
                const QByteArray png = toPng(img);
                const QString file = writeBytes(pngFor(id), png) ? pngFor(id) : QString();
                if (file.isEmpty() && !png.isEmpty())
                    log::warn("history", QStringLiteral("写入 %1 失败，这条历史只在内存里").arg(pngFor(id)));
                // 缩略图顺手一起做：历史墙只要「认个脸」，合成与缩放都在这张图已经
                // 压在手上、这个线程已经占着的时刻付，比等用户开墙时再现场解码便宜
                QString thumbFile;
                const QImage composed =
                    docFile.isEmpty() ? img : compose(img, readJsonArray(docFile));
                const QImage thumb = makeThumb(composed);
                if (!thumb.isNull() && writeBytes(thumbFor(id), toPng(thumb)))
                    thumbFile = thumbFor(id);
                // 结果回主线程应用（QImage 的压缩替换涉及共享数据，必须串行化）
                QMetaObject::invokeMethod(self, "onPacked", Qt::QueuedConnection, Q_ARG(int, id),
                                          Q_ARG(QString, file), Q_ARG(QString, thumbFile),
                                          Q_ARG(QByteArray, png));
                self->m_packing.fetch_sub(1);
            });
        }
    }
}

void HistoryStore::onPacked(int id, const QString& file, const QString& thumbFile,
                            const QByteArray& png) {
    m_pending.remove(id);
    for (Item& it : m_items) {
        if (it.id != id || it.img.isNull() || png.isEmpty())
            continue;
        if (file.isEmpty()) {
            it.thumbFile = thumbFile;
            it.png = png;  // 落盘失败：留着字节，下次整理时再试一次
            break;
        }
        // 落盘成功后内存里什么都不留：文件就是这份历史的存储，取用时再解
        it.png = QByteArray();
        it.file = file;
        it.thumbFile = thumbFile;
        it.img = QImage();
        break;
    }
    // 历史墙开着时，新条目的格子此刻还空着（add 时缩略图还没生成）：
    // 补发 thumbReady 让墙把这一格填上——缩略图过期的回写路径也走这里
    if (!thumbFile.isEmpty())
        emit thumbReady(id);
    trim();
    saveIndex();
    // 这一张占的槽位空出来了，接着排下一张：连拍十几张时不必等下一次空闲整理
    compressPending();
}

void HistoryStore::clear() {
    for (const Item& it : m_items)
        removeFiles(it.id);
    m_items.clear();
    saveIndex();
    emitChanged();
}

void HistoryStore::remove(int id) {
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (it->id == id) {
            removeFiles(id);
            m_items.erase(it);
            saveIndex();
            emitChanged();
            return;
        }
    }
}

void HistoryStore::removeFiles(int id) {
    // 三个文件名都由 id 定名，缺哪个就是本来没有（没画标注就没有 .json）
    QFile::remove(pngFor(id));
    QFile::remove(jsonFor(id));
    QFile::remove(thumbFor(id));
}

qsizetype HistoryStore::bytesOf(const Item& it) const {
    if (!it.img.isNull())
        return it.img.sizeInBytes();
    return it.png.size();  // 已落盘的条目这里是 0
}

qint64 HistoryStore::totalBytes() const {
    qint64 total = 0;
    for (const Item& it : m_items)
        total += bytesOf(it);
    return total;
}

void HistoryStore::trim() {
    // m_items 最新在前，所以「最旧」就是尾部元素。
    auto evictOldest = [&] {
        if (m_items.empty())
            return false;
        removeFiles(m_items.back().id);
        m_items.pop_back();
        return true;
    };
    while (int(m_items.size()) > m_limit && evictOldest()) {
    }
    qint64 total = totalBytes();
    while (total > m_maxBytes && evictOldest())
        total = totalBytes();
    if (!m_items.empty()) {
        log::debug("history", QString("截图历史：%1 条，内存约 %2 MB")
                                  .arg(m_items.size())
                                  .arg(total / 1024.0 / 1024.0, 0, 'f', 1));
    }
}

QList<HistoryStore::EntryInfo> HistoryStore::entries() const {
    QList<EntryInfo> list;
    list.reserve(int(m_items.size()));
    for (const Item& it : m_items)
        list.append({it.id, it.text});
    return list;
}

void HistoryStore::emitChanged() {
    emit changed(entries());
}

void HistoryStore::load() {
    QHash<QString, QJsonObject> indexed;
    QFile f(indexPath());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonArray items =
            QJsonDocument::fromJson(f.readAll()).object().value(QLatin1String("items")).toArray();
        for (const QJsonValue& v : items) {
            const QJsonObject o = v.toObject();
            indexed.insert(QFileInfo(o.value(QLatin1String("file")).toString()).fileName(), o);
        }
    }

    // 目录里的 PNG 才是这份历史的实体；索引只是补上文本与缩放比。
    // 只认纯数字文件名（= 条目 id），别的文件一律不碰。
    QDir dir(dirPath());
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Name);
    int maxId = 0;
    for (const QFileInfo& fi : files) {
        bool numeric = false;
        const int id = fi.completeBaseName().toInt(&numeric);
        if (!numeric || id <= 0)
            continue;
        Item it;
        it.id = id;
        it.file = fi.absoluteFilePath();
        // 标注文档与缩略图按 id 定名，在不在盘上直接看一眼，索引里不用记
        if (QFile::exists(jsonFor(id)))
            it.docFile = jsonFor(id);
        if (QFile::exists(thumbFor(id)))
            it.thumbFile = thumbFor(id);
        const QJsonObject o = indexed.value(fi.fileName());
        it.dpr = o.value(QLatin1String("dpr")).toDouble(1.0);
        if (it.dpr <= 0)
            it.dpr = 1.0;
        it.text = o.value(QLatin1String("text")).toString();
        if (it.text.isEmpty()) {
            // 上次退出时索引还没写上（刚截完就关程序）：读一次尺寸还原文本，
            // 缩放比只能退回 1.0 —— 这条本来就是意外恢复出来的
            it.text = textOf(fi, readPng(it.file));
            it.dpr = 1.0;
        }
        maxId = qMax(maxId, id);
        m_items.push_back(std::move(it));
    }
    // id 单调递增 ⇒ 按 id 降序就是「最新在前」
    std::sort(m_items.begin(), m_items.end(),
              [](const Item& a, const Item& b) { return a.id > b.id; });
    m_nextId = qMax(m_nextId, maxId + 1);
    trim();  // 上限可能已被改小，顺手把多出来的文件删掉
    // 底图压盘之前崩溃会留下没有主人的伴生文件（<id>.json / <id>.thumb.png）：
    // id 对不上现存条目的一律清掉，免得历史目录里堆垃圾。
    // baseName 取第一个点之前的部分，所以 index.json 是非数字、自然跳过。
    QSet<int> live;
    for (const Item& it : m_items)
        live.insert(it.id);
    for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.json"),
                                                  QStringLiteral("*.thumb.png")},
                                                 QDir::Files)) {
        bool numeric = false;
        const int id = fi.baseName().toInt(&numeric);
        if (numeric && !live.contains(id))
            QFile::remove(fi.absoluteFilePath());
    }
    saveIndex();
    if (!m_items.empty())
        log::info("history", QString("从磁盘恢复 %1 条截图历史").arg(m_items.size()));
}

void HistoryStore::saveIndex() const {
    QJsonArray items;
    for (const Item& it : m_items) {
        if (it.file.isEmpty())
            continue;
        QJsonObject o;
        o.insert(QLatin1String("id"), it.id);
        o.insert(QLatin1String("file"), it.file);
        o.insert(QLatin1String("text"), it.text);
        o.insert(QLatin1String("dpr"), it.dpr);
        items.append(o);
    }
    QJsonObject root;
    root.insert(QLatin1String("items"), items);

    QDir().mkpath(dirPath());
    QSaveFile f(indexPath());  // 先写临时文件再原子改名，崩在半路也不会留下半个索引
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0
        || !f.commit())
        log::warn("history", QStringLiteral("写 %1 失败").arg(indexPath()));
}

void historySetLimits() {
    for (HistoryStore* s : std::as_const(stores()))
        s->setLimits();
}

void historyCompressPending() {
    for (HistoryStore* s : std::as_const(stores()))
        s->compressPending();
}

}  // namespace zpin
