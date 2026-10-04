// 应用装配层 —— 创建并接线各子系统：托盘、全局热键、选区、贴图、截图历史、
// 设置对话框与自动更新。本类是全局装配层：唯一负责创建并接线各子系统。
// 子系统全部由 App 持有；跨线程气泡（后台写盘线程）经 notifyRequested 回主线程。
#pragma once

#include <QMap>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <optional>

class QImage;

namespace zpin {

class TrayController;
class SelectionController;
class PinManager;
class HistoryStore;
class HistoryWall;

namespace hotkey {
class Manager;
}
namespace scroll {
class Capture;
}
namespace update {
class UpdateFlow;
}

class App : public QObject {
    Q_OBJECT

public:
    explicit App(QObject* parent = nullptr);
    ~App() override;

    // 全部信号接线与启动态恢复（顺序见 README 的「目录结构」一节）。
    void wire();
    // 空闲整理接线后进入事件循环。
    int run();

    // ---- 首选项对话框的四个回调 ----
    // 按当前配置全量重绑热键（含托盘键位提示与帮助弹窗同步）。
    QMap<QString, bool> applyHotkeys();
    void applyFont();
    void applyLogLevel();
    // 托盘/首选项共用的热键总开关：反注册 + 持久化 + 托盘菜单文字同步。
    void setShortcutsDisabled(bool disabled);

signals:
    // 后台线程经此把结果气泡送回主线程的托盘。
    void notifyRequested(const QString& title, const QString& text);
    // 另存为真的写盘成功（在写盘线程发出）：带着留档要的底图与标注回主线程。
    void saveLanded(const QImage& base, const QJsonArray& doc, double dpr);

private:
    // ---- 截图入口 ----
    void startCapture(const QString& mode);
    void startFullCapture();
    void startWindowCapture();
    // 长截图（选区工具条「长截图」触发）：纯手动滚动 + 逐帧拼接，结果走标准出口
    void startScrollCapture(const QImage& first, const QRect& regionAbs,
                            const QRectF& regionLogical, double dpr);
    void onScrollFinished(const QImage& img, int frames, bool cancelled, double dpr,
                          const QString& reason);
    // 截图产物分发：入历史 ->（可选自动保存）-> 按动作保存/贴图/复制。
    // img 是烘焙好标注的成品（给剪贴板/保存/贴图用），base + doc 是留档要的那对
    // （干净底图 + 以选区为原点的标注文档），没画标注时 base 就是 img、doc 为空。
    void onCaptured(const QImage& img, const QImage& base, const QJsonArray& doc,
                    const QString& action, const QPointF& topLeft, double dpr);
    // 热键动作 -> 具体入口（未识别的动作忽略）。
    void dispatch(const QString& action);

    // ---- 贴图 / 历史（历史墙与托盘回调）----
    void togglePins();
    void restoreClickable();
    void openHistoryWall();
    void clearHistory();
    void repinFromHistory(int id);
    void saveFromHistory(int id);
    void copyFromHistory(int id);
    void deleteFromHistory(int id);
    void notifySaved(const QString& path);
    // 留档：进截图历史 + 立刻交后台压 PNG。所有「真产出成品」的动作共用这一道闸门
    // （复制 / 贴图 / 保存 / 另存为成功 / 长截图）。存的是干净底图 + 标注文档。
    void archive(const QImage& base, const QJsonArray& doc, double dpr);
    // 按「每次截图自动保存一份」补写一份到**默认保存目录**。**只给本来不落盘的动作
    // 调用** —— 保存与另存为自己就在写文件，再补一份等于同一张图存两遍（撞名后去重
    // 逻辑会安静地多留下一份 `_1` 重复文件）。
    void autoSaveCopy(const QImage& img);
    void onSaveLanded(const QImage& base, const QJsonArray& doc, double dpr);

    void openPrefs();
    void setupHotkeys();
    void restart();

    TrayController* m_tray = nullptr;
    SelectionController* m_selector = nullptr;
    PinManager* m_pins = nullptr;
    HistoryStore* m_history = nullptr;
    hotkey::Manager* m_hotkeys = nullptr;
    update::UpdateFlow* m_updateFlow = nullptr;
    QMap<QString, QString> m_accels;
    QPointer<class PreferencesDialog> m_prefs;
    QPointer<scroll::Capture> m_scroll;   // 进行中的长截图（工作线程跑）
    QPointer<HistoryWall> m_wall;         // 历史墙：开着就复用，新截图落地时实时刷新
};

}  // namespace zpin
