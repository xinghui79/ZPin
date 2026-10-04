#include "prefs_dialog.hpp"

#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QScreen>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "config.hpp"
#include "defaults.hpp"
#include "help_page.hpp"
#include "history.hpp"
#include "hotkey.hpp"
#include "key_edit.hpp"
#include "logging.hpp"
#include "prefs_icons.hpp"
#include "prefs_pages.hpp"
#include "startup.hpp"

namespace zpin {

namespace {

// 设置对话框的窗口装饰高度（标题栏 + 边框）。首次显示时实测（那时只有它
// 量得准），缓存下来之后每次打开在 show **之前**就按它配高——显示后再挪，
// 用户会看到对话框从旧位置拖到居中位。

// 设置页样式表 —— 按 macOS 系统设置的观感重画：浅灰底、分组白卡、
// 行式排布（标题靠左、控件贴右）、主题色只用在「选中/开启/主操作」三处。
// 选择器一律靠 role 动态属性或 objectName 限定：样式表会级联到 widget 树里的
// 所有后代，而色板弹层、文件对话框这些弹出物的 parent 就在卡片里，
// 裸标签选择器（QLineEdit、QPushButton）会把它们的控件一起改掉。
// 主题色读 Interface/theme_color——标注工具条用同一个色，设置页不该自成一套。
QString prefsStyle() {
    const QColor accent(config::getStr(QStringLiteral("Interface/theme_color")));
    return QString::fromLatin1(R"CSS(
#prefsBody { background: #F5F5F7; }

/* 侧栏：彩色瓦片 + 圆角选中块。右侧一道 hairline 把侧栏和内容区分开——
   macOS System Settings 就是靠这道线划界；没有它时侧栏是 transparent、与内容区
   同为 #F5F5F7，边界整个消失，侧栏像浮在灰底上。 */
#prefsNav { background: transparent; border: none; border-right: 1px solid rgba(0,0,0,12);
    outline: none; font-size: 13px; }
/* 6px 圆角：macOS 侧栏行用的是小圆角，原来的 8px 偏「圆」不像系统设置 */
#prefsNav::item { padding: 6px 10px; margin: 3px 6px; border-radius: 6px; color: #1D1D1F; }
#prefsNav::item:hover:!selected { background: rgba(0,0,0,14); }
#prefsNav::item:selected { background: %1; color: #FFFFFF; }

#prefsScroll { border: none; background: transparent; }
#prefsScroll QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
#prefsScroll QScrollBar::handle:vertical { background: rgba(0,0,0,66); border-radius: 4px;
    min-height: 28px; }
#prefsScroll QScrollBar::handle:vertical:hover { background: rgba(0,0,0,110); }
#prefsScroll QScrollBar::add-line:vertical, #prefsScroll QScrollBar::sub-line:vertical { height: 0; }
#prefsScroll QScrollBar::add-page:vertical, #prefsScroll QScrollBar::sub-page:vertical {
    background: transparent; }

/* 文字层级：应用名 > 行标题 > 副标题/脚注 */
QLabel[role="appName"] { font-size: 19px; font-weight: 600; color: #1D1D1F; }
QLabel[role="rowLabel"] { font-size: 13px; color: #1D1D1F; }
QLabel[role="rowCaption"] { font-size: 11.5px; color: #86868B; }
QLabel[role="footer"] { font-size: 11.5px; color: #86868B; }
QLabel[role="body"] { font-size: 12.5px; color: #4A5057; }
QLabel[role="error"] { font-size: 11.5px; color: #FF3B30; }
/* 页内小节标题：卡片上方那行灰字 */
QLabel[role="section"] { font-size: 11.5px; font-weight: 600; color: #86868B; }

/* 分组卡片：一行一件事，行间细分割线左缩进、不顶到圆角 */
#prefsCard { background: #FFFFFF; border: 1px solid rgba(0,0,0,15); border-radius: 10px; }
#prefsDiv { background: rgba(0,0,0,14); }

/* 字段：白底 + 细描边 + 8px 圆角，焦点描主题色 */
QLineEdit[role="field"], QSpinBox[role="field"], QComboBox[role="popup"] {
    background: #FFFFFF; border: 1px solid rgba(0,0,0,36); border-radius: 6px;
    padding: 4px 8px; color: #1D1D1F;
    selection-background-color: %1; selection-color: #FFFFFF; }
QLineEdit[role="path"] { background: #FFFFFF; border: 1px solid rgba(0,0,0,36);
    border-radius: 6px; padding: 4px 8px; color: #4A5057; }
QLineEdit[role="field"]:focus, QSpinBox[role="field"]:focus, QComboBox[role="popup"]:focus,
QLineEdit[role="path"]:focus { border: 1px solid %1; }
/* 数值框的原生上下按钮在样式表里归零。
   不能用 setButtonSymbols(NoButtons)：一旦 QSS 作用在 QSpinBox 上，
   NoButtons 会把内部编辑框压成 1px 宽，数值直接看不见（实测）。 */
QSpinBox[role="field"]::up-button, QSpinBox[role="field"]::down-button { width: 0px;
    height: 0px; border: none; background: transparent; }

/* 下拉：原生灰三角由 paintEvent 现画的尖角顶替（QSS 引用不到运行期位图） */
QComboBox[role="popup"] { padding-right: 24px; }
QComboBox[role="popup"]::drop-down { border: none; width: 22px; }
QComboBox[role="popup"]::down-arrow { image: none; }
QComboBox[role="popup"] QAbstractItemView { background: #FFFFFF;
    border: 1px solid rgba(0,0,0,30); selection-background-color: %1;
    selection-color: #FFFFFF; outline: none; }

/* 数值步进器：上下两个尖角叠成一颗胶囊 */
#prefsStep { background: #FFFFFF; border: 1px solid rgba(0,0,0,36); border-radius: 6px; }
#prefsStepDiv { background: rgba(0,0,0,26); }
QToolButton[role="step"] { background: transparent; border: none; padding: 0; }
QToolButton[role="step"]:pressed { background: rgba(0,0,0,22); border-radius: 5px; }

/* 按钮：macOS 胶囊。主操作填主题色，其余白底细描边 */
QPushButton[role="capsule"] { background: #FFFFFF; color: #1D1D1F;
    border: 1px solid rgba(0,0,0,36); border-radius: 6px; padding: 5px 12px; }
QPushButton[role="capsule"]:hover { background: #FAFAFC; border-color: rgba(0,0,0,70); }
QPushButton[role="capsule"]:pressed { background: #F0F0F3; }
QPushButton[role="capsule"]:disabled { color: #B0B6BE; border-color: rgba(0,0,0,14); }
QPushButton[role="primary"] { background: %1; color: #FFFFFF; border: none;
    border-radius: 6px; padding: 5px 16px; font-weight: 600; }
QPushButton[role="primary"]:hover { background: %2; }
QPushButton[role="primary"]:disabled { background: #BCC9D8; color: #F4F7FA; }

/* 颜色弹层：白底 + 圆角色片，当前值描一圈主题色。
   刻意不做 QSS 圆角 + 半透明背景——那会和 Win11 的 DWM 系统圆角叠加，
   QSS 圆角外的未绘制区域露出来就是黑角（用户实测）。与工具条菜单同款处理：
   不透明窗交给系统圆角/描边/阴影。 */
#prefsSwatchMenu { background: #FFFFFF; border: 1px solid rgba(0,0,0,22); }
#prefsSwatchMenu QToolButton { background: transparent; border: 2px solid transparent;
    border-radius: 7px; }
#prefsSwatchMenu QToolButton:hover { background: rgba(0,0,0,14); }
#prefsSwatchMenu QToolButton[role="swatchOn"] { border: 2px solid %1; }

/* 帮助页顶部的分段控件：灰轨道 + 白色选中块。选中不加粗，
   否则字宽变化会让整排按钮左右跳一下。 */
#helpTabs { background: rgba(0,0,0,12); border-radius: 8px; }
QToolButton[role="helpTab"] { background: transparent; border: none; border-radius: 6px;
    padding: 4px 12px; font-size: 12px; color: #3A424C; }
QToolButton[role="helpTab"]:hover { background: rgba(0,0,0,12); }
QToolButton[role="helpTab"]:checked { background: #FFFFFF; color: #1D1D1F; }
)CSS")
        .arg(accent.name(), accent.lighter(112).name());
}

// 每页的统一外壳：大标题 + 一组一组卡片 + 滚动区。
// 设置窗口定死尺寸：800 × 450（逻辑像素），以后不再按内容高度自适应。
//
// 为什么定死而不是「按最高页配平」：内容高度会随字体、DPI、行数、乃至某页
// 增删一行而变，一旦跟着变，切页时窗口就会跳一下。现在锁死，切页与改配置都
// 不动高度；某一页内容超出就在该页内部出滚动条（实测 常规/截图/输出/帮助 四页
// 本来就高于 450，靠内部滚动即可，窗口不动比每页不滚动更重要）。
//
// 450 的来历：侧栏 7 项实测需要约 378px，加内容区上下 14×2 的留白还剩约 30px
// 余量——这是唯一「必须满足」的条件（侧栏被裁的话，设置里有哪些页得滚动才看
// 得全）。实测数据见 build\scratch\measure_prefs.cpp。
constexpr int kPrefsFixedW = 800;
constexpr int kPrefsFixedH = 450;

// 卡片按内容收缩（不再拉满一整页），底部空白由外层 stretch 兜住；
// 对话框定死尺寸，所以每页的可视宽度完全一致。
QScrollArea* makePageShell(QWidget* inner) {
    auto* scroll = new QScrollArea;
    scroll->setObjectName("prefsScroll");
    scroll->setWidgetResizable(true);
    // 滚动区默认继承 palette(Window) 那一套 #F0F0F0，遮住 #prefsBody 的 #F5F5F7
    // 还会把两个页面之间的层叠缝填成灰；这两行一起清掉才有「一张纸」的观感。
    scroll->viewport()->setAutoFillBackground(false);
    auto* page = new QWidget;
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(18, 16, 18, 18);
    lay->setSpacing(14);
    // 这里原来放过一个 19px 的页首大标题（role=pageTitle）。它是纯重复：页名
    // 已经在左侧导航里，而那一项始终可见、选中时还有 accent 药丸底。标题样式
    // 与 Windows 11 / macOS 设置一致——导航即身份，右栏直接从内容开始。
    lay->addWidget(inner, 1);
    scroll->setWidget(page);
    return scroll;
}

}  // namespace

PreferencesDialog::PreferencesDialog(ApplyHotkeys applyHotkeys, std::function<void()> applyFont,
                                     std::function<void()> applyLogLevel,
                                     std::function<void(bool)> setHotkeysDisabled,
                                     std::function<void(bool)> setHotkeysSuspended,
                                     update::UpdateFlow* updateFlow, QWidget* parent)
    : QDialog(parent),
      m_applyHotkeys(std::move(applyHotkeys)),
      m_applyFont(std::move(applyFont)),
      m_applyLogLevel(std::move(applyLogLevel)),
      m_setHotkeysDisabled(std::move(setHotkeysDisabled)),
      m_setHotkeysSuspended(std::move(setHotkeysSuspended)),
      m_updateFlow(updateFlow) {
    // 「完成」/关闭即析构，App 侧持有的是 QPointer，下次打开重新构造
    setAttribute(Qt::WidgetAttribute::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("ZPin 设置"));
    setModal(false);
    // 定死尺寸：侧栏与内容区每页完全一致，切页不能跳。
    // 宽 800 × 高 450 是常量（见 kPrefsFixedW/kPrefsFixedH 的取值理由），不再按
    // 内容高度自适应。定尺寸与居中都必须在 show **之前**完成——显示后再挪，用户
    // 会看到对话框从旧位置被拖到中间。
    m_lay = new QVBoxLayout(this);
    m_lay->setContentsMargins(0, 0, 0, 0);
    build();
    setFixedSize(kPrefsFixedW, kPrefsFixedH);
    centerOnCursorScreen();
    m_setHotkeysDisabled(config::getBool(QStringLiteral("Hotkeys/disabled")));
    refreshHotkeyStatus();
}

void PreferencesDialog::build() {
    m_edits.clear();
    m_status.clear();
    // 记住当前页：恢复默认会整页重建，不能把用户踢回第一页
    const int keepRow = m_nav ? m_nav->currentRow() : 0;
    // 整块（侧栏 + 页面栈）换掉，避免重建时叠加出第二套控件
    if (m_body) {
        m_lay->removeWidget(m_body);
        m_body->deleteLater();
    }
    m_body = new QWidget(this);
    m_body->setObjectName("prefsBody");
    m_body->setAttribute(Qt::WidgetAttribute::WA_StyledBackground, true);
    m_body->setStyleSheet(prefsStyle());
    auto* bodyLay = new QHBoxLayout(m_body);
    bodyLay->setContentsMargins(14, 14, 14, 14);
    bodyLay->setSpacing(12);

    // 侧栏导航：一行一页，rowToPage[row] = 页面栈下标（不再有分组头占位）
    m_nav = new QListWidget(m_body);
    m_nav->setObjectName("prefsNav");
    m_nav->setFixedWidth(200);
    m_nav->setIconSize(QSize(20, 20));
    m_nav->setHorizontalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOff);
    bodyLay->addWidget(m_nav);

    m_stack = new QStackedWidget(m_body);
    bodyLay->addWidget(m_stack, 1);

    struct Nav {
        const char* title;
        prefsicon::Glyph glyph;
    };
    QVector<int> rowToPage;
    auto addPage = [this, &rowToPage](const Nav& item, QWidget* page) {
        m_nav->addItem(new QListWidgetItem(prefsicon::icon(item.glyph),
                                           QString::fromUtf8(item.title)));
        rowToPage.append(m_stack->addWidget(page));
    };

    // 侧栏 7 项、不带分组头：「界面」并进「常规」，4 个帮助主题收进一页。
    // 早先是 13 项 + 3 个分组头 = 16 行，超出可视高度，侧栏自己长出滚动条；
    // 而且设置区的「贴图」和帮助区的「贴图」在导航里同名，看不出哪个是配置。
    addPage({"常规", prefsicon::Glyph::General}, makePageShell(buildGeneral(this)));
    addPage({"截图", prefsicon::Glyph::Capture}, makePageShell(buildCapture(this)));
    addPage({"贴图", prefsicon::Glyph::Pin}, makePageShell(buildPin(this)));
    addPage({"输出", prefsicon::Glyph::Output}, makePageShell(buildOutput(this)));
    auto* controlPage = makePageShell(buildControl(this));
    addPage({"快捷键", prefsicon::Glyph::Hotkey}, controlPage);
    m_helpRow = m_nav->count();
    addPage({"帮助", prefsicon::Glyph::Help}, makePageShell(help::createHelpPage(m_stack)));
    m_aboutRow = m_nav->count();
    addPage({"关于", prefsicon::Glyph::About}, makePageShell(buildAbout(m_updateFlow)));

    connect(m_nav, &QListWidget::currentRowChanged, this,
            [this, rowToPage](int row) {
                // -1 是 QListWidget 在清空/重建选择时发的事件，不是页面
                if (row < 0 || row >= rowToPage.size())
                    return;
                m_stack->setCurrentIndex(rowToPage[row]);
            });
    // 首次打开停在第一页；重建（恢复默认）则回到用户所在的页
    m_nav->setCurrentRow(keepRow < 0 || keepRow >= m_nav->count() ? 0 : keepRow);
    m_lay->addWidget(m_body);
}

void PreferencesDialog::showEvent(QShowEvent* ev) {
    QDialog::showEvent(ev);
    // 尺寸是常量（kPrefsFixedW × kPrefsFixedH），正常路径下构造时就已钉死；
    // 这里只是确保没有任何布局在显示期间改动过它——真被改动过就立刻钉回去，
    // 且重新居中（showEvent 在窗口上屏之前派发，用户看不到位置跳动）。
    if (width() != kPrefsFixedW || height() != kPrefsFixedH) {
        setFixedSize(kPrefsFixedW, kPrefsFixedH);
        centerOnCursorScreen();
    }
}

void PreferencesDialog::centerOnCursorScreen() {
    // 没有父窗定位规则（本窗 parent 为空），Qt 默认位置不居中；
    // 以光标所在屏的可用区域居中——从托盘打开时就是用户当前那块屏
    QScreen* scr = QGuiApplication::screenAt(QCursor::pos());
    if (!scr)
        scr = QGuiApplication::primaryScreen();
    if (!scr)
        return;
    const QRect av = scr->availableGeometry();
    const QRect fr = frameGeometry();
    move(av.left() + (av.width() - fr.width()) / 2,
         av.top() + (av.height() - fr.height()) / 2);
}

void PreferencesDialog::showHelpPage() {
    if (m_nav)
        m_nav->setCurrentRow(m_helpRow);
}

void PreferencesDialog::showAboutPage() {
    if (m_nav)
        m_nav->setCurrentRow(m_aboutRow);
}

void PreferencesDialog::registerHotRow(const QString& action, HotkeyEdit* edit, QLabel* status) {
    m_edits.insert(action, edit);
    m_status.insert(action, status);
    connect(edit, &HotkeyEdit::accelChanged, this, [this, action](const QString&) {
        onAccelChanged(action);
    });
    // 捕获期间挂起全局热键：否则用户想绑的组合键若已被自己占用（例如把框选
    // 改成与全屏相同），WM_HOTKEY 会把按键直接吞掉，输入框毫无反应，冲突
    // 提示也永远没机会出现（用户实测）
    connect(edit, &HotkeyEdit::captureStarted, this, [this] {
        if (m_setHotkeysSuspended)
            m_setHotkeysSuspended(true);
    });
    connect(edit, &HotkeyEdit::captureEnded, this, [this] {
        if (m_setHotkeysSuspended)
            m_setHotkeysSuspended(false);
    });
}

void PreferencesDialog::setHotkeysDisabled(bool disabled) {
    m_setHotkeysDisabled(disabled);
    refreshHotkeyStatus();
}

void PreferencesDialog::applyLogLevel() {
    m_applyLogLevel();
}

void PreferencesDialog::applyFont() {
    m_applyFont();
}

void PreferencesDialog::refreshAccent() {
    if (m_body)
        m_body->setStyleSheet(prefsStyle());
}

QMap<QString, bool> PreferencesDialog::applyHotkeys() {
    return m_applyHotkeys ? m_applyHotkeys() : QMap<QString, bool>();
}

void PreferencesDialog::onAccelChanged(const QString& action) {
    // 单行热键变更：写配置并重绑；冲突或注册失败时回滚并提示。
    if (validateHotkeys().contains(action)) {
        // 冲突不落盘，显示也必须回滚到配置值：否则输入框与 config 永久分叉——
        // 另一行解除冲突后红标撤了，显示值与实际注册值仍是两张皮，重启才发现
        // 改键没生效。
        if (HotkeyEdit* edit = m_edits.value(action))
            edit->setText(config::getStr(QStringLiteral("Hotkeys/%1").arg(action)));
        return;
    }
    const QString key = QStringLiteral("Hotkeys/%1").arg(action);
    const QString text = m_edits.value(action)->text().trimmed();
    const QString prev = config::getStr(key);
    config::set(key, text);
    config::sync();
    if (config::getBool(QStringLiteral("Hotkeys/disabled"))) {
        // 全局禁用中：apply_bindings 自会只记账不注册，这里照常调用，
        // 让管理器记住新键，总开关恢复后即按新值生效
        applyHotkeys();
        refreshHotkeyStatus();
        return;
    }
    const QMap<QString, bool> results = applyHotkeys();
    if (!text.isEmpty() && !results.value(action, true)) {
        // 新键被其它程序占用：回滚配置与输入框，并恢复旧键的注册
        config::set(key, prev);
        config::sync();
        m_edits.value(action)->setText(prev);
        QLabel* st = m_status.value(action);
        st->setText(QStringLiteral("注册失败，可能被其他程序占用（已恢复原键）"));
        st->show();
        applyHotkeys();
        return;
    }
    refreshHotkeyStatus(results);   // 结果现成的，别再全量重绑一轮
}

void PreferencesDialog::refreshHotkeyStatus(const QMap<QString, bool>& passed) {
    const bool disabled = config::getBool(QStringLiteral("Hotkeys/disabled"));
    const QMap<QString, bool> results =
        disabled ? QMap<QString, bool>()
                 : (passed.isEmpty() ? applyHotkeys() : passed);
    const QSet<QString> conflicts = validateHotkeys();
    for (auto it = m_status.cbegin(); it != m_status.cend(); ++it) {
        const QString text = m_edits.value(it.key())->text().trimmed();
        if (conflicts.contains(it.key()))
            continue;
        if (disabled) {
            // 总开关勾着本身就是答案，不必每行再刷一遍「已禁用全局快捷键」——
            // 六行同义反复，还都占着错误红，反而把真正的报错（注册失败/冲突）淹掉。
            it.value()->hide();
        } else if (!text.isEmpty() && !results.value(it.key(), true)) {
            it.value()->setText(QStringLiteral("注册失败，可能被其他程序占用"));
            it.value()->show();
        } else {
            it.value()->hide();
        }
    }
}

QSet<QString> PreferencesDialog::validateHotkeys() {
    // 检查各行加速键文本是否重复，冲突的行标红提示。
    QMap<QString, QStringList> seen;
    for (auto it = m_edits.cbegin(); it != m_edits.cend(); ++it) {
        const QString t = it.value()->text().trimmed();
        if (!t.isEmpty())
            seen[t].append(it.key());
    }
    QSet<QString> conflicted;
    for (const QStringList& list : seen.values()) {
        if (list.size() > 1)
            for (const QString& a : list)
                conflicted.insert(a);
    }
    for (auto it = m_status.cbegin(); it != m_status.cend(); ++it) {
        if (conflicted.contains(it.key())) {
            it.value()->setText(QStringLiteral("与其他动作冲突"));
            it.value()->show();
        } else if (!it.value()->text().startsWith(QStringLiteral("注册失败"))) {
            it.value()->hide();
        }
    }
    return conflicted;
}

void PreferencesDialog::syncSideEffects(const QStringList& prefixes) {
    // 把 config 当前值应用到各子系统。只跑 affected 前缀真正覆盖到的那几项：
    //   General/   -> 日志级别、自启注册表键、历史条数/内存上限
    //   Interface/ -> 界面字体
    //   Hotkeys/   -> 热键总开关与重绑
    // 其余页（Capture/ Pin/ Output/）没有运行时副作用，不该在这里被顺带触发——
    // 早先无条件全跑，导致在「输出」页点恢复默认会顺手改写 HKCU 自启键。
    // 历史条数是 General/history_* 两项，都要覆盖到。
    auto hit = [&prefixes](const char* group) {
        return prefixes.contains(QLatin1String(group));
    };
    if (hit("General/")) {
        applyLogLevel();
        startup::setEnabled(config::getBool(QStringLiteral("General/autostart")));
        historySetLimits();
    }
    if (hit("Interface/"))
        applyFont();
    if (hit("Hotkeys/")) {
        m_setHotkeysDisabled(config::getBool(QStringLiteral("Hotkeys/disabled")));
        applyHotkeys();
        refreshHotkeyStatus();
    }
}

void PreferencesDialog::restorePage(const QString& title, const QStringList& prefixes) {
    QStringList affected;
    for (const defaults::Entry& entry : defaults::table()) {
        const QString key = QString::fromLatin1(entry.key);
        bool hit = false;
        for (const QString& prefix : prefixes)
            hit |= key.startsWith(prefix);
        if (hit && !defaults::nonResettable(key))
            affected << key;
    }
    if (affected.isEmpty())
        return;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("恢复默认"),
        QStringLiteral("将「%1」页的全部设置恢复为默认值？\n（仅本页，其它页不受影响）").arg(title),
        QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::No,
        QMessageBox::StandardButton::No);
    if (answer != QMessageBox::StandardButton::Yes)
        return;
    for (const QString& key : affected)
        config::set(key, defaults::defaultValue(key));
    config::sync();
    syncSideEffects(prefixes);   // 只重放本页覆盖到的副作用
    build();                     // 整页重建，读到新值
    // build() 换掉了整套行控件，必须对**新**行再刷一次热键状态（syncSideEffects
    // 里那次刷的是旧控件，重建后新状态标签全是初始隐藏态）。
    refreshHotkeyStatus();
    log::info("zpin.prefs", QStringLiteral("已恢复「%1」页默认设置（%2 项）")
                                .arg(title)
                                .arg(affected.size()));
}

}  // namespace zpin
