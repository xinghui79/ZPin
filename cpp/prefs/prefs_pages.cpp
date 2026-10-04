#include "prefs_pages.hpp"

#include <QAbstractButton>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QUrl>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWidgetAction>

#include "config.hpp"
#include "defaults.hpp"
#include "history.hpp"
#include "hotkey.hpp"
#include "key_edit.hpp"
#include "logging.hpp"
#include "output.hpp"
#include "prefs_dialog.hpp"
#include "prefs_icons.hpp"
#include "startup.hpp"
#include "ui_app_icon.hpp"
#include "update.hpp"

namespace zpin {
namespace {

// ---- 版面令牌 ----
// 像素只在这里出现，页面构建函数只写语义（哪一行属于哪一组）。
constexpr int kRowPadX = 16;      // 卡片内左右留白，同时是分割线的左缩进
constexpr int kRowPadY = 5;       // 上下留白：13px 行高 + 5×2 ≈ 28px。macOS System
                                  // Settings 一行约 28~30px；原先 9 是 36px，整页
                                  // 松垮、每屏能看到的行数少一截
constexpr int kGroupGap = 20;     // 卡片与卡片之间（macOS ≈20）
constexpr int kSectionLead = 6;   // 小节标题上方额外留白：组间距 20 + 这个 6 ≈ 26。
                                  // macOS 里「上一张卡片 → 下一个小节标题」的距离
                                  // 比「小节标题 → 本组卡片」远，一眼能分清组边界
constexpr int kNumFieldW = 76;    // 数值框统一宽度：同一页里并排的必须等宽
constexpr int kStepW = 18;        // 步进器胶囊宽度
constexpr int kStatusColW = 280;  // 关于页的更新状态列：要换行就得给死宽度，
                                  // 否则 wordWrap 标签按窄 sizeHint 算行高会切字

// 一行的底层形态：左列（任意控件）+ 右列（控件），控件贴卡片右边界。
// 两列都要显式给对齐标记：QBoxLayout 对「撑不满格子」的项默认居中，
// 而这两列的宽度都由内容决定。表现就是标题比邻居缩进一截 / 宽控件不肯铺满。
QWidget* rowWidget(QWidget* left, QWidget* control, bool wide = false) {
    auto* w = new QWidget;
    w->setObjectName("prefsRow");
    auto* lay = new QHBoxLayout(w);
    lay->setContentsMargins(kRowPadX, kRowPadY, kRowPadX, kRowPadY);
    lay->setSpacing(16);
    lay->addWidget(left, wide ? 0 : 1, Qt::AlignmentFlag::AlignLeft | Qt::AlignVCenter);
    lay->addWidget(control, wide ? 1 : 0,
                   wide ? Qt::Alignment()
                        : (Qt::AlignmentFlag::AlignRight | Qt::AlignVCenter));
    return w;
}

// 一行：左侧标题（可带灰色副标题）+ 右侧控件。
// 标题与副标题都不换行：本页所有文案都是短语，而开了 wordWrap 的 QLabel
// 的 sizeHint 只给一个很窄的宽度，行高按它算就会把第二行切掉。
// wide=true 时控件吃掉剩余宽度——路径、文件名模板这类内容长度不可知。
QWidget* row(const QString& label, const QString& caption, QWidget* control, bool wide = false) {
    auto* title = new QLabel(label);
    title->setProperty("role", "rowLabel");
    if (caption.isEmpty())
        return rowWidget(title, control, wide);

    auto* sub = new QLabel(caption);
    sub->setProperty("role", "rowCaption");
    auto* col = new QWidget;
    auto* cl = new QVBoxLayout(col);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(2);
    cl->addWidget(title);
    cl->addWidget(sub);
    return rowWidget(col, control, wide);
}

// 一组 = 卡片上方的灰色小标题 + 白卡（行间细分割线）+ 卡片下方的脚注。
// 一个卡片只管一件事，卡片之间留呼吸——不再是一整页塞一张十行的大白卡。
QWidget* group(const QString& title, const QList<QWidget*>& rows, const QString& footer = {}) {
    auto* col = new QWidget;
    auto* lay = new QVBoxLayout(col);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);

    if (!title.isEmpty()) {
        // 标题上方再加 kSectionLead：卡片之间那点等距留白（lay->setSpacing(6)）
        // 分不出「组结束」和「组内标题」，macOS 靠标题上方更大的空隙来分组
        lay->addSpacing(kSectionLead);
        auto* h = new QLabel(title);
        h->setProperty("role", "section");
        lay->addWidget(h);
    }

    auto* card = new QWidget;
    card->setObjectName("prefsCard");
    card->setAttribute(Qt::WidgetAttribute::WA_StyledBackground, true);
    auto* cl = new QVBoxLayout(card);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    for (int i = 0; i < rows.size(); ++i) {
        if (i > 0) {   // 分割线左缩进，不顶到卡片圆角
            auto* div = new QFrame;
            div->setObjectName("prefsDiv");
            div->setFixedHeight(1);
            div->setAttribute(Qt::WidgetAttribute::WA_StyledBackground, true);
            auto* dl = new QHBoxLayout;
            dl->setContentsMargins(kRowPadX, 0, 0, 0);
            dl->addWidget(div);
            cl->addLayout(dl);
        }
        cl->addWidget(rows[i]);
    }
    lay->addWidget(card);

    if (!footer.isEmpty()) {
        auto* f = new QLabel(footer);
        f->setProperty("role", "footer");
        f->setWordWrap(true);
        lay->addWidget(f);
    }
    return col;
}

// QSS 把 ::down-arrow 置空了（原生那个灰三角是 Windows 的形状），尖角只能现画：
// 10px 宽，右边距 10px、垂直居中。
void paintPopupArrow(QWidget* w) {
    QPainter p(w);
    p.setRenderHint(QPainter::Antialiasing);
    const QPixmap pm = prefsicon::chevron(false, QColor("#6E737C"), QSize(10, 10));
    p.drawPixmap(w->width() - 20, (w->height() - 10) / 2, pm);
}

QToolButton* stepButton(QSpinBox* spin, bool up) {
    auto* btn = new QToolButton;
    btn->setProperty("role", "step");
    btn->setIcon(QIcon(prefsicon::chevron(up, QColor("#6E737C"), QSize(10, 10))));
    btn->setIconSize(QSize(10, 10));
    btn->setFixedSize(kStepW, 14);
    btn->setCursor(Qt::CursorShape::PointingHandCursor);
    btn->setFocusPolicy(Qt::FocusPolicy::NoFocus);
    QObject::connect(btn, &QToolButton::clicked, spin, [spin, up] {
        spin->stepBy(up ? 1 : -1);
        // Qt 步进后默认全选数值，蓝底一直挂在框上像「焦点卡在里面」；
        // 点完步进就平铺显示，要改值再点进框（那时会重新全选）
        if (auto* edit = spin->findChild<QLineEdit*>())
            edit->deselect();
    });
    return btn;
}

// 数值框的统一交互（绑定框与字号框共用）：
//  - 点击/聚焦进框即全选：键入即替换，而不是插在光标处拼成怪数；
//  - 键入中不逐键生效（keyboardTracking 关掉，失焦/回车才提交）；
//  - 悬停滚轮不改值（焦点在框上才步进）。
class NumberFieldBehavior : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* watched, QEvent* ev) override {
        // 事件由内部的 QLineEdit 接收（文本点击定位、滚轮都在它身上）
        auto* edit = qobject_cast<QLineEdit*>(watched);
        if (!edit)
            return false;
        if (ev->type() == QEvent::Type::Wheel && !edit->hasFocus())
            return true;
        if (ev->type() == QEvent::Type::MouseButtonPress ||
            ev->type() == QEvent::Type::FocusIn)
            // 排队全选：不同队列的话光标定位会跟在全选后面执行，选择被冲掉
            QMetaObject::invokeMethod(edit, "selectAll", Qt::QueuedConnection);
        return false;
    }
};

// macOS 式数值控件：右对齐数值框 + 上下步进的胶囊。原生上下按钮是方形灰按钮，
// 放进圆角卡片里像贴错了地方——它们在样式表里被归零，这里只留可编辑的数值框。
QWidget* numberField(QSpinBox* spin) {
    static NumberFieldBehavior behavior;   // 无状态过滤器，全局一份即可
    spin->setKeyboardTracking(false);
    // lineEdit() 是 protected 的，外面用 findChild 拿内部输入框装过滤器
    if (auto* edit = spin->findChild<QLineEdit*>())
        edit->installEventFilter(&behavior);
    spin->setAlignment(Qt::AlignmentFlag::AlignRight);
    spin->setFixedWidth(kNumFieldW);
    spin->setProperty("role", "field");

    auto* step = new QWidget;
    step->setObjectName("prefsStep");
    step->setAttribute(Qt::WidgetAttribute::WA_StyledBackground, true);
    auto* sl = new QVBoxLayout(step);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(0);
    sl->addWidget(stepButton(spin, true));
    auto* line = new QFrame;
    line->setObjectName("prefsStepDiv");
    line->setFixedHeight(1);
    line->setAttribute(Qt::WidgetAttribute::WA_StyledBackground, true);
    sl->addWidget(line);
    sl->addWidget(stepButton(spin, false));

    auto* box = new QWidget;
    auto* lay = new QHBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);
    lay->addWidget(spin);
    lay->addWidget(step);
    return box;
}

// 页面底部「恢复默认」按钮；作用于当前页（页名只在确认弹窗里交代）。
QWidget* restoreRow(PreferencesDialog* dialog, const QString& title,
                    const QStringList& prefixes) {
    auto* rowWidget = new QWidget;
    auto* lay = new QHBoxLayout(rowWidget);
    lay->setContentsMargins(0, 4, 0, 0);
    lay->addStretch(1);
    auto* btn = new QPushButton(QStringLiteral("恢复默认"));
    btn->setProperty("role", "capsule");
    btn->setToolTip(QStringLiteral("把本页（%1）的全部设置恢复为默认值，其它页不受影响").arg(title));
    btn->setCursor(Qt::CursorShape::PointingHandCursor);
    QObject::connect(btn, &QPushButton::clicked, dialog, [dialog, title, prefixes] {
        dialog->restorePage(title, prefixes);
    });
    lay->addWidget(btn);
    return rowWidget;
}

// 只读路径输入框 + 「打开」（在资源管理器里定位到这个目录）+「浏览...」改路径。
QWidget* dirPickerRow(const QString& key, const QString& title) {
    auto* lay = new QHBoxLayout;
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);
    // 显示与保存都统一成系统风格的反斜杠：配置里的默认值是 Qt 的 '/' 风格，
    // 从资源管理器选回来的是 '\' 风格，两个目录行并排看着像两套东西
    auto* edit = new QLineEdit(QDir::toNativeSeparators(config::getStr(key)));
    edit->setReadOnly(true);
    edit->setMinimumWidth(200);
    edit->setProperty("role", "path");
    auto* openBtn = new QPushButton(QStringLiteral("打开"));
    openBtn->setProperty("role", "capsule");
    openBtn->setToolTip(QStringLiteral("在资源管理器里打开这个目录"));
    auto* btn = new QPushButton(QStringLiteral("浏览..."));
    btn->setProperty("role", "capsule");
    QObject::connect(btn, &QPushButton::clicked, edit, [edit, key, title] {
        const QString dir = QFileDialog::getExistingDirectory(edit, title, edit->text());
        if (dir.isEmpty())
            return;
        const QString native = QDir::toNativeSeparators(dir);
        edit->setText(native);
        config::set(key, native);
        config::sync();
    });
    QObject::connect(openBtn, &QPushButton::clicked, edit, [edit] {
        const QString dir = edit->text().trimmed();
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);   // 一次都没保存过时目录可能还不存在，开了个空壳也算符合预期
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    lay->addWidget(edit, 1);
    lay->addWidget(openBtn);
    lay->addWidget(btn);
    auto* wrap = new QWidget;
    wrap->setLayout(lay);
    return wrap;
}

// 截图历史目录：随程序固定在 %LOCALAPPDATA%，不可改 —— 只读展示 + 在资源管理器打开。
QWidget* historyDirRow() {
    auto* lay = new QHBoxLayout;
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);
    auto* edit = new QLineEdit(QDir::toNativeSeparators(HistoryStore::storageDir()));
    edit->setReadOnly(true);
    edit->setMinimumWidth(200);
    edit->setProperty("role", "path");
    auto* openBtn = new QPushButton(QStringLiteral("打开"));
    openBtn->setProperty("role", "capsule");
    openBtn->setToolTip(QStringLiteral("在资源管理器里打开这个目录"));
    QObject::connect(openBtn, &QPushButton::clicked, edit, [edit] {
        const QString dir = edit->text().trimmed();
        if (dir.isEmpty())
            return;
        QDir().mkpath(dir);   // 一张都没截过时目录还不存在，开了个空壳也算符合预期
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    lay->addWidget(edit, 1);
    lay->addWidget(openBtn);
    auto* wrap = new QWidget;
    wrap->setLayout(lay);
    return wrap;
}

}  // namespace

QString qtColorText(const QColor& c) {
    if (c.alpha() < 255)
        return QStringLiteral("#%1%2%3%4")
            .arg(c.alpha(), 2, 16, QLatin1Char('0'))
            .arg(c.red(), 2, 16, QLatin1Char('0'))
            .arg(c.green(), 2, 16, QLatin1Char('0'))
            .arg(c.blue(), 2, 16, QLatin1Char('0'))
            .toUpper();
    return c.name().toUpper();
}

// ---- 绑定控件 ----

SwitchButton::SwitchButton(QWidget* parent) : QAbstractButton(parent) {
    setCheckable(true);
    setFixedSize(40, 24);
    setCursor(Qt::CursorShape::PointingHandCursor);
    m_slide = new QVariantAnimation(this);
    m_slide->setDuration(150);
    m_slide->setEasingCurve(QEasingCurve::Type::OutCubic);
    connect(m_slide, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_pos = v.toReal();
        update();
    });
    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        m_slide->stop();
        m_slide->setStartValue(m_pos);
        m_slide->setEndValue(on ? 1.0 : 0.0);
        m_slide->start();
    });
}

void SwitchButton::setCheckedNow(bool on) {
    const bool wasBlocked = signalsBlocked();
    blockSignals(true);   // 程序化落位不是用户改动：别写配置、别起动画
    setChecked(on);
    blockSignals(wasBlocked);
    m_slide->stop();
    m_pos = on ? 1.0 : 0.0;
    update();
}

void SwitchButton::paintEvent(QPaintEvent* ev) {
    Q_UNUSED(ev)
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal h = height();
    const qreal w = width();
    QPainterPath track;
    track.addRoundedRect(QRectF(0, 0, w, h), h / 2.0, h / 2.0);
    p.fillPath(track, QColor("#E5E5EA"));   // 关：浅灰轨道
    QColor on(config::getStr(QStringLiteral("Interface/theme_color")));
    if (m_pos > 0.001) {                    // 开：主题色按动画进度淡入
        on.setAlphaF(m_pos);
        p.fillPath(track, on);
    }
    p.setClipPath(track);
    p.setPen(QPen(QColor(0, 0, 0, 16), 1.0));   // 轨道内描边：白底上纯灰轨道会看不出边界
    p.setBrush(Qt::BrushStyle::NoBrush);
    p.drawPath(track);
    const qreal d = h - 4.0;
    const qreal x = 2.0 + m_pos * (w - d - 4.0);
    p.setClipping(false);
    p.setPen(QPen(QColor(0, 0, 0, 30), 1.0));
    p.setBrush(Qt::white);
    p.drawEllipse(QRectF(x, 2.0, d, d));
}

BoundCheck::BoundCheck(const QString& key, std::function<void(bool)> onChange) {
    setCheckedNow(config::getBool(key));
    connect(this, &QAbstractButton::toggled, this, [key, onChange](bool on) {
        config::set(key, on);
        config::sync();
        if (onChange)
            onChange(on);
    });
}

InvertedCheck::InvertedCheck(const QString& key) {
    setCheckedNow(!config::getBool(key));
    connect(this, &QAbstractButton::toggled, this, [key](bool on) {
        config::set(key, !on);
        config::sync();
    });
}

BoundSpin::BoundSpin(const QString& key, int lo, int hi, const QString& suffix,
                     std::function<void(int)> onChange) {
    setRange(lo, hi);
    if (!suffix.isEmpty())
        setSuffix(suffix);
    const int stored = config::getInt(key);
    setValue(stored);
    // 越界值必须回写。setValue 对越界输入的钳制是 QSpinBox 的内部行为：
    // 它不产生 valueChanged（而信号是下面才接的），所以 config.ini 里那个越界值
    // 会原封不动留着——界面显示 16 MB、实际生效的却是用户手改的 8，
    // 而且不碰这个微调框就永远不会自愈。窗口的 maxSide/历史上限等都读 config。
    if (value() != stored) {
        log::warn("prefs", QString("配置 %1 的值 %2 超出范围 %3~%4，已改为 %5")
                            .arg(key)
                            .arg(stored)
                            .arg(lo)
                            .arg(hi)
                            .arg(value()));
        config::set(key, value());
        config::sync();
        if (onChange)
            onChange(value());
    }
    connect(this, &QSpinBox::valueChanged, this, [key, onChange](int v) {
        config::set(key, v);
        config::sync();
        if (onChange)
            onChange(v);
    });
}

BoundCombo::BoundCombo(const QString& key, const QStringList& items,
                       std::function<void(const QString&)> onChange,
                       const QStringList& values) {
    // 存储值挂在 itemData 上：显示文案与配置值解耦（PNG 图片显示、png 落盘）
    for (int i = 0; i < items.size(); ++i)
        addItem(items[i], values.isEmpty() ? items[i] : values.value(i));
    setProperty("role", "popup");
    const QString cur = config::getStr(key);
    const int idx = findData(cur);
    if (idx >= 0)
        setCurrentIndex(idx);
    // 与 BoundSpin 同一套自愈：手改 ini 写进未知值时，界面停在第一项却不写回，
    // 显示与生效就是两张皮（当前唯一用户 log_level 恰好显示=生效，但规矩不该靠巧）。
    if (!cur.isEmpty() && idx < 0) {
        log::warn("prefs", QStringLiteral("配置 %1 的值「%2」不在可选项里，已改为「%3」")
                              .arg(key, cur, currentData().toString()));
        config::set(key, currentData().toString());
        config::sync();
    }
    connect(this, &QComboBox::currentIndexChanged, this, [this, key, onChange](int) {
        const QString v = currentData().toString();
        config::set(key, v);
        config::sync();
        if (onChange)
            onChange(v);
    });
}

void BoundCombo::paintEvent(QPaintEvent* ev) {
    QComboBox::paintEvent(ev);
    paintPopupArrow(this);
}

FontPopUp::FontPopUp(QWidget* parent) : QFontComboBox(parent) {
    setProperty("role", "popup");
}

void FontPopUp::paintEvent(QPaintEvent* ev) {
    QFontComboBox::paintEvent(ev);
    paintPopupArrow(this);
}

ColorButton::ColorButton(const QString& key) : m_key(key) {
    setFixedSize(24, 24);
    setCursor(Qt::CursorShape::PointingHandCursor);
    syncSwatch(QColor(config::getStr(key)));
    connect(this, &QPushButton::clicked, this, &ColorButton::pick);
}

void ColorButton::pick() {
    // 预设色板弹层：和截图工具栏同一套色（自定义取色对话框已按用户要求删掉）。
    // 遮罩色单独一套——半透明才有「压暗选区外」的效果，给一堆实色等于没得选。
    const QStringList& palette = m_key == QLatin1String("Capture/mask_color")
                                     ? defaults::maskPalette()
                                     : defaults::colorPalette();
    const QString current = qtColorText(QColor(config::getStr(m_key)));
    QMenu menu(this);
    menu.setObjectName("prefsSwatchMenu");
    // 不设 WA_TranslucentBackground / QSS 圆角：与 Win11 的 DWM 系统圆角叠加后，
    // QSS 圆角外的未绘制区域会露成黑角（用户实测）。样式表里也不再写 radius，
    // 圆角、描边、阴影全交给系统——与工具条菜单同款处理。
    auto* gridW = new QWidget;
    auto* grid = new QGridLayout(gridW);
    grid->setContentsMargins(8, 8, 8, 8);
    grid->setSpacing(6);
    for (int i = 0; i < palette.size(); ++i) {
        const QColor c(palette[i]);
        auto* b = new QToolButton;
        b->setFixedSize(30, 30);
        b->setCursor(Qt::CursorShape::PointingHandCursor);
        b->setIcon(prefsicon::swatch(c, 26));
        b->setIconSize(QSize(26, 26));
        b->setProperty("role", qtColorText(c) == current ? "swatchOn" : "swatch");
        QObject::connect(b, &QToolButton::clicked, &menu, [this, &menu, c] {
            config::set(m_key, qtColorText(c));
            config::sync();
            syncSwatch(c);
            emit colorChanged(c);
            menu.close();
        });
        grid->addWidget(b, i / 6, i % 6);
    }
    auto* wa = new QWidgetAction(&menu);
    wa->setDefaultWidget(gridW);
    menu.addAction(wa);
    menu.exec(mapToGlobal(QPoint(0, height() + 6)));
}

void ColorButton::syncSwatch(const QColor& c) {
    // 圆形色板（Apple 的颜色井）。半透明色直接压在白卡片上会显得「没上色」，
    // 所以描边给到能看清轮廓的浓度。
    setStyleSheet(QStringLiteral("QPushButton { background: rgba(%1,%2,%3,%4);"
                                 "border: 1px solid rgba(0,0,0,60); border-radius: 12px; }")
                      .arg(c.red())
                      .arg(c.green())
                      .arg(c.blue())
                      .arg(c.alpha()));
}

// ---- 页面 ----

QWidget* buildGeneral(PreferencesDialog* dialog) {
    // 「常规」+ 原「界面」页合成一页三小节：字体与主题色两项撑不起一栏导航，
    // 而侧栏条目一多就会挤出滚动条。
    auto* col = new QWidget;
    auto* lay = new QVBoxLayout(col);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(kGroupGap);

    auto* autostart = new BoundCheck(QStringLiteral("General/autostart"));
    // 开机自启：写完必须回读校验。注册表可能被组策略/管控软件拒绝写入，而
    // BoundCheck 已经把 config 存成 true 了 —— 不回读的话设置页永远显示
    // 「已开启」，开机却什么都不发生，这种不一致从用户侧完全看不出来。
    QObject::connect(autostart, &QAbstractButton::toggled, autostart, [dialog, autostart](bool on) {
        startup::setEnabled(on);
        if (startup::isEnabled() == on)
            return;
        config::set(QStringLiteral("General/autostart"), !on);   // 按系统实际状态回滚
        config::sync();
        autostart->setCheckedNow(!on);
        QMessageBox::warning(
            dialog, QStringLiteral("开机自动运行"),
            QStringLiteral("系统拒绝了注册表写入（可能被组策略或管控软件限制），"
                           "已恢复为未开启。\n\n可以在 Windows「设置 → 应用 → 启动」里手动打开。"));
    });

    std::function<void(const QString&)> onLog =
        [dialog](const QString&) { dialog->applyLogLevel(); };

    lay->addWidget(group(
        QStringLiteral("启动与运行"),
        {row(QStringLiteral("开机自动运行"), QString(), autostart),
         row(QStringLiteral("启动时自动备份配置"), QString(),
             new BoundCheck(QStringLiteral("General/auto_backup"))),
         row(QStringLiteral("空闲时整理内存"),
             QStringLiteral("闲置超 1 分钟后压缩历史并释放内存；之后第一次截图会略卡。重启后生效"),
             new BoundCheck(QStringLiteral("General/keep_responsive"))),
         row(QStringLiteral("日志级别"), QString(),
             new BoundCombo(QStringLiteral("General/log_level"),
                            {QStringLiteral("普通"), QStringLiteral("详细")}, onLog))}));

    lay->addWidget(group(
        QStringLiteral("截图历史"),
        {row(QStringLiteral("历史保留条数"), QString(),
             numberField(new BoundSpin(QStringLiteral("General/history_limit"), 0, 100,
                                       QStringLiteral(" 条"),
                                       [](int) { historySetLimits(); }))),
         row(QStringLiteral("历史占用上限"), QString(),
             numberField(new BoundSpin(QStringLiteral("General/history_max_mb"), 16, 512,
                                       QStringLiteral(" MB"),
                                       [](int) { historySetLimits(); })))},
        QStringLiteral("截图落地即存本机（AppData\\Local\\ZPin\\history），重启不丢；"
                       "到达任一上限从最旧的开始淘汰。历史墙里可贴图化 / 保存 / 复制，"
                       "删除单条与清空会真删文件。")));

    auto* combo = new FontPopUp;
    auto* sizeSpin = new QSpinBox;
    sizeSpin->setRange(defaults::kFontPtMin, defaults::kFontPtMax);
    sizeSpin->setSuffix(QStringLiteral(" pt"));
    const auto [name, size] = defaults::fontTuple(config::getStr(QStringLiteral("Interface/font")));
    // name 默认是「A,B,C」的字体栈，下拉只认单个家族：显示栈首的那个（系统 UI
    // 字体），别把整串塞进去——QFontComboBox 匹配不到任何已装家族，会显示成
    // 一串乱码似的东西。用户一动下拉就写成单一家族，语义与从前一致。
    combo->setCurrentFont(QFont(name.split(QLatin1Char(','), Qt::SkipEmptyParts).value(0)));
    sizeSpin->setValue(size);
    auto applyFont = [dialog, combo, sizeSpin] {
        config::set(QStringLiteral("Interface/font"),
                    QStringLiteral("%1,%2").arg(combo->currentFont().family())
                                           .arg(sizeSpin->value()));
        config::sync();
        dialog->applyFont();
    };
    QObject::connect(combo, &QFontComboBox::currentFontChanged, combo,
                     [applyFont](const QFont&) { applyFont(); });
    QObject::connect(sizeSpin, qOverload<int>(&QSpinBox::valueChanged), sizeSpin,
                     [applyFont](int) { applyFont(); });
    auto* fontBox = new QWidget;
    auto* fontLay = new QHBoxLayout(fontBox);
    fontLay->setContentsMargins(0, 0, 0, 0);
    fontLay->setSpacing(6);
    fontLay->addWidget(combo, 1);
    fontLay->addWidget(numberField(sizeSpin));

    auto* accent = new ColorButton(QStringLiteral("Interface/theme_color"));
    QObject::connect(accent, &ColorButton::colorChanged, dialog,
                     [dialog](const QColor&) { dialog->refreshAccent(); });
    lay->addWidget(group(QStringLiteral("界面外观"),
                         {row(QStringLiteral("字体"), QString(), fontBox, true),
                          row(QStringLiteral("主题色"), QString(), accent)}));

    lay->addWidget(restoreRow(dialog, QStringLiteral("常规"),
                              QStringList{QStringLiteral("General/"),
                                         QStringLiteral("Interface/")}));
    lay->addStretch(1);
    return col;
}

QWidget* buildCapture(PreferencesDialog* dialog) {
    auto* col = new QWidget;
    auto* lay = new QVBoxLayout(col);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(kGroupGap);

    lay->addWidget(group(
        QStringLiteral("自动吸附"),
        {row(QStringLiteral("吸附界面元素"), QStringLiteral("按钮、文字块等"),
             new BoundCheck(QStringLiteral("Capture/snap_elements"))),
         row(QStringLiteral("吸附内容块"), QStringLiteral("界面元素认不出时按画面找边界"),
             new BoundCheck(QStringLiteral("Capture/snap_content"))),
         row(QStringLiteral("拖拽时对齐参考线"), QStringLiteral("屏幕/窗口的边与中线"),
             new BoundCheck(QStringLiteral("Capture/snap_guides")))}));

    lay->addWidget(group(
        QStringLiteral("选区外观"),
        {row(QStringLiteral("边框宽度"), QString(),
             numberField(new BoundSpin(QStringLiteral("Capture/border_width"), 1, 10,
                                       QStringLiteral(" px")))),
         row(QStringLiteral("遮罩颜色"), QString(),
             new ColorButton(QStringLiteral("Capture/mask_color"))),
         row(QStringLiteral("显示可调节锚点"), QString(),
             new BoundCheck(QStringLiteral("Capture/show_anchors"))),
         row(QStringLiteral("锚点描边颜色"), QString(),
             new ColorButton(QStringLiteral("Capture/anchor_stroke_color")))}));

    lay->addWidget(group(
        QStringLiteral("辅助显示"),
        {row(QStringLiteral("显示全屏十字线"), QString(),
             new BoundCheck(QStringLiteral("Capture/show_crosshair"))),
         row(QStringLiteral("显示辅助线"), QString(),
             new InvertedCheck(QStringLiteral("Capture/disable_guides"))),
         row(QStringLiteral("显示快捷键提示"), QString(),
             new BoundCheck(QStringLiteral("Capture/show_hints")))}));

    lay->addWidget(restoreRow(dialog, QStringLiteral("截图"), {"Capture/"}));
    lay->addStretch(1);
    return col;
}

QWidget* buildPin(PreferencesDialog* dialog) {
    auto* col = new QWidget;
    auto* lay = new QVBoxLayout(col);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(kGroupGap);

    // 这四组值都只在 PinWindow 构造时读一次配置（pin_window.cpp:63/67/68/69），
    // 改完只作用于之后新建的贴图。原先只有「描边外发光」一行写了这句话，另外三行
    // 空着 —— 用户改「边框颜色」看不到任何反应，只会以为没保存。说明收进组标题
    // 写一次，比四行各重复一遍好。
    lay->addWidget(group(
        QStringLiteral("外观（对新贴图生效）"),
        {row(QStringLiteral("显示阴影"), QString(), new BoundCheck(QStringLiteral("Pin/shadow"))),
         row(QStringLiteral("默认不透明度"), QString(),
             numberField(new BoundSpin(QStringLiteral("Pin/default_opacity"),
                                       defaults::kOpacityMin, defaults::kOpacityMax,
                                       QStringLiteral(" %")))),
         row(QStringLiteral("边框颜色"), QString(),
             new ColorButton(QStringLiteral("Pin/border_color"))),
         row(QStringLiteral("描边外发光"), QString(),
             new BoundCheck(QStringLiteral("Pin/border_glow")))}));

    lay->addWidget(group(
        QStringLiteral("尺寸"),
        {row(QStringLiteral("贴图窗口尺寸上限"), QString(),
             numberField(new BoundSpin(QStringLiteral("Pin/max_window_size"),
                                       defaults::kMaxWindowMin, defaults::kMaxWindowMax,
                                       QStringLiteral(" px"))))},
        QStringLiteral("新贴图按此上限自动缩小；已打开的贴图不受影响。")));

    lay->addWidget(restoreRow(dialog, QStringLiteral("贴图"), {"Pin/"}));
    lay->addStretch(1);
    return col;
}

QWidget* buildOutput(PreferencesDialog* dialog) {
    auto* col = new QWidget;
    auto* lay = new QVBoxLayout(col);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(kGroupGap);

    auto* tplEdit = new QLineEdit(config::getStr(QStringLiteral("Output/name_template")));
    tplEdit->setMinimumWidth(220);
    tplEdit->setProperty("role", "field");
    auto* preview = new QLabel;
    preview->setProperty("role", "rowCaption");
    // 落盘改到 editingFinished（失焦/回车）：textEdited 每敲一个字符就全量重写
    // 一次 config.ini。预览仍跟手（textEdited 只刷预览不落盘）。
    auto refreshPreview = [tplEdit, preview] {
        preview->setText(QStringLiteral("预览：%1").arg(output::previewName(tplEdit->text())));
    };
    QObject::connect(tplEdit, &QLineEdit::textEdited, tplEdit, refreshPreview);
    QObject::connect(tplEdit, &QLineEdit::editingFinished, tplEdit, [tplEdit] {
        config::set(QStringLiteral("Output/name_template"), tplEdit->text());
        config::sync();
    });
    refreshPreview();
    auto* tplBox = new QWidget;
    auto* tplLay = new QVBoxLayout(tplBox);
    tplLay->setContentsMargins(0, 0, 0, 0);
    tplLay->setSpacing(4);
    tplLay->addWidget(tplEdit);
    tplLay->addWidget(preview);

    lay->addWidget(group(QStringLiteral("文件命名"),
                         {row(QStringLiteral("文件名模板"), QString(), tplBox, true)}));

    // 默认输出格式：配置里存 png/jpg/bmp/webp/tif，空 = 跟随模板后缀（默认，保持老行为）。
    // 换格式会改变无对话框保存的落盘后缀，预览跟着重算。
    auto* fmtCombo = new BoundCombo(
        QStringLiteral("Output/default_ext"),
        {QStringLiteral("跟随文件名模板"), QStringLiteral("PNG"), QStringLiteral("JPEG"),
         QStringLiteral("WEBP"), QStringLiteral("BMP"), QStringLiteral("TIFF")},
        [refreshPreview](const QString&) { refreshPreview(); },
        {QString(), QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("webp"),
         QStringLiteral("bmp"), QStringLiteral("tif")});

    lay->addWidget(group(
        QStringLiteral("画质与格式"),
        {row(QStringLiteral("默认输出格式"),
             QStringLiteral("工具条「保存」、自动保存与历史重新保存落盘用的格式"),
             fmtCombo),
         row(QStringLiteral("压缩质量"),
             QStringLiteral("仅作用于 jpg / webp，其余格式无损"),
             numberField(new BoundSpin(QStringLiteral("Output/quality"), 30, 100))),
         row(QStringLiteral("记住上次保存的格式"), QString(),
             new BoundCheck(QStringLiteral("Output/remember_ext")))}));

    lay->addWidget(group(
        QStringLiteral("保存位置"),
        {row(QStringLiteral("默认保存目录"), QString(),
             dirPickerRow(QStringLiteral("Output/default_dir"), QStringLiteral("选择保存目录")),
             true),
         row(QStringLiteral("每次截图自动保存一份"), QString(),
             new BoundCheck(QStringLiteral("Output/auto_save"))),
         row(QStringLiteral("截图历史目录"), QStringLiteral("随程序固定，不可改"),
             historyDirRow(), true)}));

    lay->addWidget(restoreRow(dialog, QStringLiteral("输出"), {"Output/"}));
    lay->addStretch(1);
    return col;
}

QWidget* buildControl(PreferencesDialog* dialog) {
    auto* col = new QWidget;
    auto* lay = new QVBoxLayout(col);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(kGroupGap);

    lay->addWidget(group(QString(),
                         {row(QStringLiteral("禁用所有快捷键"), QString(),
                              new BoundCheck(QStringLiteral("Hotkeys/disabled"),
                                             [dialog](bool on) { dialog->setHotkeysDisabled(on); }))}));

    QList<QWidget*> rows;
    for (const QString& action : hotkey::actionOrder()) {
        auto* edit = new HotkeyEdit(config::getStr(QStringLiteral("Hotkeys/%1").arg(action)));
        edit->setProperty("role", "field");
        auto* status = new QLabel;
        status->setProperty("role", "error");
        status->hide();
        auto* textCol = new QWidget;
        auto* tl = new QVBoxLayout(textCol);
        tl->setContentsMargins(0, 0, 0, 0);
        tl->setSpacing(2);
        tl->addWidget(edit);
        tl->addWidget(status);
        auto* clear = new QPushButton(QStringLiteral("清除"));
        clear->setProperty("role", "capsule");
        clear->setFocusPolicy(Qt::FocusPolicy::NoFocus);
        QObject::connect(clear, &QPushButton::clicked, edit, &HotkeyEdit::clearAccel);
        auto* cell = new QWidget;
        auto* cl = new QHBoxLayout(cell);
        cl->setContentsMargins(0, 0, 0, 0);
        cl->setSpacing(8);
        cl->addWidget(textCol);
        cl->addWidget(clear);
        rows << row(hotkey::actionLabel(action), QString(), cell);
        dialog->registerHotRow(action, edit, status);
    }
    lay->addWidget(group(QStringLiteral("键位"), rows,
                         QStringLiteral("点击输入框后按下新组合键；Esc 取消，「清除」"
                                        "解绑。冲突会标红并自动还原；被其它程序占用时"
                                        "提示注册失败。")));

    lay->addWidget(restoreRow(dialog, QStringLiteral("快捷键"), {"Hotkeys/"}));
    lay->addStretch(1);
    return col;
}

QWidget* buildAbout(update::UpdateFlow* updateFlow) {
    auto* col = new QWidget;
    auto* lay = new QVBoxLayout(col);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(kGroupGap);

    auto* head = new QHBoxLayout;
    head->setSpacing(14);
    auto* icon = new QLabel;
    icon->setPixmap(appicon::appIcon().pixmap(56, 56));
    head->addWidget(icon);
    auto* nameCol = new QVBoxLayout;
    nameCol->setSpacing(2);
    auto* title = new QLabel(QStringLiteral("ZPin"));
    title->setProperty("role", "appName");
    auto* ver = new QLabel(QStringLiteral("版本 %1").arg(QString::fromLatin1(defaults::VERSION)));
    ver->setProperty("role", "rowCaption");
    nameCol->addWidget(title);
    nameCol->addWidget(ver);
    head->addLayout(nameCol);
    head->addStretch(1);
    lay->addLayout(head);

    auto* tagline = new QLabel(
        QStringLiteral("截图 · 标注 · 贴图，一气呵成的 Windows 桌面工具"));
    tagline->setWordWrap(true);
    tagline->setProperty("role", "body");
    lay->addWidget(tagline);
    // 这里原来还有一块 4 条 bullet 的功能清单（控件级吸附、对齐参考线、放大镜与
    // 取色、双击隐藏……）。那些是「帮助」页的素材：关于页要的是一眼认出这是个
    // 什么工具，不是一份功能文档；子功能写在这里只会让人逐条读、读完记不住。
    // 现在只留 tagline，具体用法由左侧「帮助」页承担。

    // ---- 更新：检查 / 下载换装 / 发布页，状态机在 UpdateFlow（属 App）----
    auto* status = new QLabel;
    status->setWordWrap(true);
    status->setFixedWidth(kStatusColW);
    status->setProperty("role", "rowLabel");

    auto* updBox = new QWidget;
    auto* updLay = new QHBoxLayout(updBox);
    updLay->setContentsMargins(0, 0, 0, 0);
    updLay->setSpacing(8);
    auto* checkBtn = new QPushButton(QStringLiteral("检查更新"));
    checkBtn->setProperty("role", "capsule");
    auto* installBtn = new QPushButton;
    installBtn->setProperty("role", "primary");
    installBtn->hide();
    auto* pageLink = new QLabel(QStringLiteral("<a href=\"%1\">查看发布页</a>")
                                    .arg(QLatin1String(defaults::UPDATE_PAGE_URL)));
    pageLink->setOpenExternalLinks(true);
    // 源码仓库紧挨发布页：两个都是「去 GitHub 看」，分开摆反而要点两次才找到
    auto* repoLink = new QLabel(QStringLiteral("<a href=\"%1\">源代码</a>")
                                    .arg(QLatin1String(defaults::REPO_URL)));
    repoLink->setOpenExternalLinks(true);
    updLay->addWidget(checkBtn);
    updLay->addWidget(installBtn);
    updLay->addWidget(pageLink);
    updLay->addWidget(repoLink);

    lay->addWidget(group(
        QStringLiteral("软件更新"),
        {row(QStringLiteral("启动时自动检查更新"), QString(),
             new BoundCheck(QStringLiteral("General/update_auto_check"))),
         // 状态文字在左、按钮在右：早先状态在按钮下面，一眼扫过去只看到两个
         // 没边框的「像文字的按钮」，状态还得往下找。
         rowWidget(status, updBox)}));

    auto refresh = [updateFlow, checkBtn, installBtn, status] {
        status->setText(updateFlow->statusText());
        const auto state = updateFlow->state();
        const bool busy = state == update::UpdateFlow::State::Checking ||
                          state == update::UpdateFlow::State::Downloading;
        checkBtn->setEnabled(!busy);
        installBtn->setVisible(updateFlow->canDownload());
        installBtn->setText(state == update::UpdateFlow::State::Ready
                                ? QStringLiteral("安装并重启")
                                : QStringLiteral("下载更新"));
        installBtn->setEnabled(!busy);
    };
    refresh();
    // 本页随「恢复默认」整页重建：以控件为 context，销毁时自动断开，不悬空
    QObject::connect(updateFlow, &update::UpdateFlow::changed, status, refresh);
    QObject::connect(checkBtn, &QPushButton::clicked, updateFlow,
                     [updateFlow] { updateFlow->check(); });
    QObject::connect(installBtn, &QPushButton::clicked, updateFlow,
                     [updateFlow] { updateFlow->downloadAndInstall(); });

    auto* dim = new QLabel(QStringLiteral("技术栈：C++ / Qt 6 · Rust · OpenCV\n配置与日志：%1")
                               .arg(config::appDir()));
    dim->setProperty("role", "footer");
    dim->setWordWrap(true);
    lay->addWidget(dim);
    lay->addStretch(1);
    return col;
}

}  // namespace zpin
