// 截图历史墙 —— 托盘「截图历史」打开的缩略图网格窗：一格一张截图（缩略图已含标注），
// 单击选中、双击贴图化，底栏四个动作（贴图化 / 保存 / 复制 / 清空）。
// 「贴图化」把这条的**干净底图 + 标注文档**一起上屏：看着与原图一致，进标注模式后
// 画过的笔迹还能挪、能改、能删；收工回写这条历史（见 HistoryStore::updateDoc），
// 缩略图跟着变成编辑后的画面。
// 窗口定死尺寸（与设置窗同一理由）：条目多少不该让它跳一下，超出就在内部滚动。
#pragma once

#include <QHash>
#include <QList>
#include <QWidget>

#include "history.hpp"

class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QToolButton;

namespace zpin {

class HistoryWall : public QWidget {
    Q_OBJECT

public:
    explicit HistoryWall(HistoryStore* store, QWidget* parent = nullptr);

    // 按 store 的当前条目重铺格子（新截图落档、清空、缩略图补齐时都调它）。
    void refresh();
    // 在光标所在屏居中后显示并提到最前（与设置窗同一套定位规则）。
    void open();

signals:
    void repinRequested(int id);     // 贴图化（带标注文档上屏）
    void saveRequested(int id);
    void copyRequested(int id);      // 合成结果（含标注）进剪贴板
    void deleteRequested(int id);    // 删除这一条历史（不动已贴的贴图）
    void clearRequested();

protected:
    void keyPressEvent(QKeyEvent* ev) override;
    // 格子的双击（QAbstractButton 没有 doubleClicked 信号）
    bool eventFilter(QObject* watched, QEvent* ev) override;

private:
    QToolButton* makeCell(const HistoryStore::EntryInfo& entry, QGridLayout* grid, int index);
    void select(QToolButton* cell);
    int selectedId() const;

    HistoryStore* m_store;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_board = nullptr;          // 网格容器：refresh 时整块换掉
    QToolButton* m_selected = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_repin = nullptr;
    QPushButton* m_save = nullptr;
    QPushButton* m_copy = nullptr;
    QPushButton* m_del = nullptr;
    QPushButton* m_clear = nullptr;
    QHash<int, QToolButton*> m_cells;    // id -> 格子（缩略图到达时只补这一格）
};

}  // namespace zpin
