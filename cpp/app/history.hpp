// 截图历史 —— 最近截图列表，供历史墙重贴/再编辑，**并落盘到 %LOCALAPPDATA%\ZPin\history\**。
// 条数与内存上限均可配；刚截完先按 QImage 存，随后（以及空闲补压时）在工作线程里
// 压成 PNG 写成文件，之后内存里只留路径，取用时才解码。
// 画过标注的那条存的是「干净底图 <id>.png + 标注文档 <id>.json」，取用时现合成，
// 所以历史条目能连标注一起重新载入继续改；另存一张 <id>.thumb.png 给历史墙当缩略图。
// 启动时读 index.json 恢复列表：只把索引读进内存，PNG 按需再读盘，不预付解码代价。
#pragma once

#include <QByteArray>
#include <QImage>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <vector>

namespace zpin {

class HistoryStore : public QObject {
    Q_OBJECT

public:
    explicit HistoryStore(QObject* parent = nullptr);
    ~HistoryStore() override;

    // 历史档存放目录（%LOCALAPPDATA%\ZPin\history，随程序固定不可改）：
    // 设置页展示与「打开」用
    static QString storageDir();

    // 按最新配置刷新条数与内存上限；limit=0 会淘汰掉全部条目，并且不再收录新截图。
    void setLimits();

    // 把一张新截图插入历史最前，并触发淘汰与变更通知。
    // doc = 与 img **同原点**的标注文档（img 此时必须是没画标注的干净底图）；
    // 空数组就是普通截图，与早先一模一样，一个附加文件都不写。
    void add(const QImage& img, double dpr = 1.0, const QJsonArray& doc = QJsonArray());

    // 这条历史截图当时的缩放比（贴图按它还原视觉大小）。
    double dprOf(int id) const;

    // 按 id 取回历史截图（画过标注的就是合成结果，与用户当时看到的一致）；
    // 条目不存在时返回空 QImage。
    QImage get(int id) const;

    // 这条历史存的**干净底图**（不含标注）——「贴图化」要的是这张，
    // 配上 docOf() 一起交给 AnnotateEngine::loadDocument 就能原样接着改。
    QImage baseOf(int id) const;

    // 这条历史的标注文档；没有标注或条目不存在时返回空数组。坐标与 <id>.png 同原点，
    // 正好是 AnnotateEngine::loadDocument 要的输入。
    QJsonArray docOf(int id) const;

    // 回写一条历史的标注文档（「贴图化」上屏的贴图收工时调用）：重写 <id>.json
    // 并作废重生成缩略图，之后 get()/thumbOf() 就是编辑后的画面。doc 为空 =
    // 标注被全擦了，条目退回普通截图。条目已被淘汰时静默忽略。
    void updateDoc(int id, const QJsonArray& doc);

    // 取缩略图（长边不超过 kThumbSide 的物理像素）。条目还没生成过缩略图时返回空图，
    // 并把生成排进工作线程，写完发 thumbReady —— 历史墙先摆占位框，图到了再填。
    QImage thumbOf(int id);

    // 把最老的未落盘条目交给工作线程压 PNG 并写文件（不阻塞主线程）。
    void compressPending();

    // 清空全部历史（连文件一起删）并通知界面。
    void clear();

    // 删除一条历史（连文件一起删）并通知界面；条目不存在时静默忽略。
    // 只动历史档，不影响任何已贴在桌面上的贴图。
    void remove(int id);

    struct EntryInfo {
        int id;
        QString text;
    };

    // 当前条目（最新在前），与 changed 信号带出来的是同一份。
    QList<EntryInfo> entries() const;

signals:
    // 历史列表变化（最新在前）。
    void changed(const QList<EntryInfo>& entries);
    // 后台补生成的缩略图已落盘（只有升级前留下的老条目会走到这）：再取一次就有了。
    void thumbReady(int id);

private slots:
    void onPacked(int id, const QString& file, const QString& thumbFile, const QByteArray& png);
    void onThumb(int id, const QString& thumbFile);

private:
    struct Item {
        int id = 0;
        QImage img;         // 原图（落盘后释放）
        QByteArray png;     // PNG 字节（未压缩时为空）
        QString file;       // 已落盘的 PNG 完整路径（空=还没写出去）
        QString docFile;    // 标注文档 <id>.json（空=这条没画标注）
        QString thumbFile;  // 缩略图 <id>.thumb.png（空=还没生成）
        QString text;
        double dpr = 1.0;
    };

    qsizetype bytesOf(const Item& it) const;
    qint64 totalBytes() const;
    const Item* find(int id) const;
    void trim();
    void emitChanged();

    // 落盘目录与索引文件
    static QString dirPath();
    static QString indexPath();
    // 一条历史的三个文件（都由 id 定名）
    static QString pngFor(int id);
    static QString jsonFor(int id);
    static QString thumbFor(int id);
    // 淘汰一条 = 三个文件一起删（不存在的自然跳过）
    void removeFiles(int id);
    void load();
    void saveIndex() const;
    // 底图 + 文档 = 用户当时看到的那张图（文档认不出来时退回干净底图）
    static QImage compose(const QImage& base, const QJsonArray& doc);
    // 把一张已合成的图缩到缩略图尺寸并写盘（在工作线程跑）
    static QImage makeThumb(const QImage& composed);
    void requestThumb(int id);

    int m_limit = 20;
    qint64 m_maxBytes = qint64(96) * 1024 * 1024;
    // 条目 id 从 1 起：文件名就是 id，0 留给「认不出来的文件」当哨兵跳过
    int m_nextId = 1;
    std::vector<Item> m_items;      // 最新在前
    QSet<int> m_pending;
    QSet<int> m_thumbBusy;          // 正在补生成缩略图的条目（防重复起任务）
    // 在途工作线程任务数（压 PNG / 补缩略图；投递完结果才减）：析构等它清零，
    // 保证任务里 invokeMethod(this) 解引用的时刻对象还活着。
    std::atomic<int> m_packing{0};
};

// 广播：把新的条数/内存上限通知所有已创建的 store（首选项改配置后调用）。
void historySetLimits();

// 空闲整理回调：把所有 store 驻留的原图交给工作线程压成 PNG。
void historyCompressPending();

}  // namespace zpin
