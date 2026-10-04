// 截图标注工具条 —— 常用布局：撤销/重做 → 常用工具 → 颜色/粗细 → 完成动作 + ⋮ 更多。
// 图标全部用内嵌 SVG 渲染；浮动的 Qt::Tool 窗口，WA_ShowWithoutActivating 保持
// 覆盖层持有键盘焦点，自身也兜底转发 Esc/Enter/Ctrl+Z/Y/S。
#pragma once

#include <QColor>
#include <QHash>
#include <QPointer>
#include <QList>
#include <QString>
#include <QWidget>

class QEvent;
class QAction;
class QHBoxLayout;
class QLabel;
class QMenu;
class QMouseEvent;
class QToolButton;

namespace zpin {

// 渲染 SVG 字符串 -> QIcon（20×20 透明底）。
QIcon toolGlyph(const QString& name, const QString& color = "#E8E8E8",
                double strokeWidth = 2.0, const QVariant& param = {});

// 预热标准图标缓存（启动后空闲时调一次）：把首次框选时才付的 SVG 解析开销提前。
void warmToolbarIcons();

class CaptureToolbar : public QWidget {
    Q_OBJECT

public:
    struct ToolDef {
        QString name;
        QString label;
    };

    // actions 是外层一排的完成动作（保持精简，低频的进 ⋮ 菜单）；
    // menuActions 是收进 ⋮ 菜单的动作项。
    // groups 是子工具组表，留空 = 用全局 toolGroups()（含气泡/序号组；
    // 截图与贴图两条标注路径都传全局这份）。
    CaptureToolbar(const QColor& initialColor, double initialWidth,
                   QList<ToolDef> tools, QHash<QString, QList<ToolDef>> groups = {},
                   QStringList actions = {"cancel", "copy", "pin", "save"},
                   QStringList menuActions = {"ocr", "sanitize", "scroll", "save_as"},
                   QWidget* parent = nullptr);

    void setEnabledActions(bool undo, bool redo);

signals:
    void action(const QString& name);          // undo/redo/copy/pin/save/save_as/cancel/finish
    void toolSelected(const QString& name);    // 工具名（一级按钮或其子工具组里的名字）
    void colorSelected(const QColor& color);
    void widthSelected(double width);

protected:
    void paintEvent(QPaintEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;

private:
    void buildRow(QHBoxLayout* row);
    QToolButton* addButton(QHBoxLayout* row, const QString& name, const QString& tip,
                           std::function<void()> cb, bool checkable = false);
    void addSep(QHBoxLayout* row);
    // 子工具组：组内共用一个按钮（单击 = 用上次那个子工具，右键 = 展开组内菜单，
    // 图标右下角画小三角表示「还有兄弟」）。primary 是组主名（kTools 里的名字）。
    void createGroupButton(QHBoxLayout* row, const QString& primary);
    void applyGroupFace(const QString& primary);
    void showTip(const QString& text, QToolButton* btn);
    void hideTip();
    QPoint belowPos(QToolButton* btn, int menuW) const;
    void moreMenu();
    void selectTool(const QString& name);
    void syncIndicatorIcons();
    void colorMenu();
    void pickColor(const QColor& color, QMenu* menu);
    void widthMenu();
    void pickWidth(double w);

    QList<ToolDef> m_tools;
    QHash<QString, QList<ToolDef>> m_groups;   // 本工具条实际可用的子工具组表
    QStringList m_actions;
    QStringList m_menuActions;
    QColor m_color;
    double m_width = 4.0;
    QToolButton* m_btnUndo = nullptr;
    QToolButton* m_btnRedo = nullptr;
    QToolButton* m_btnColor = nullptr;
    QToolButton* m_btnWidth = nullptr;
    QToolButton* m_btnMore = nullptr;
    QHash<QString, QToolButton*> m_toolButtons;
    // 子工具组状态（见 createGroupButton）
    QHash<QString, QToolButton*> m_groupButtons;  // 组主名 -> 组按钮
    QHash<QString, QString> m_groupOf;            // 子工具名 -> 组主名
    QHash<QString, QString> m_groupCurrent;       // 组主名 -> 当前子工具
    QHash<QString, QAction*> m_groupActions;      // 子工具名 -> 组菜单项
    QHash<QToolButton*, QMenu*> m_groupMenus;     // 组按钮 -> 组菜单（右键展开，不走 setMenu）
    QPointer<QMenu> m_more;
    QPointer<QLabel> m_tip;
};

// 选区标注默认的全量工具集（贴图标注用精简子集，见 pin_annotator）。
extern const QList<CaptureToolbar::ToolDef> kTools;

// 默认的子工具组表（组主名 -> 组内成员）。贴图标注要在它基础上去掉自己
// 不支持的那几组，所以导出给外面看。
const QHash<QString, QList<CaptureToolbar::ToolDef>>& toolGroups();

}  // namespace zpin
