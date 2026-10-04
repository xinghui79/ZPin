#include "history_wall.hpp"

#include <QCursor>
#include <QEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "config.hpp"

namespace zpin {

namespace {

// 一格 = 缩略图 + 下面一行说明文字。列数与窗口尺寸都是常量：条目多少只影响
// 要不要出滚动条，不动窗口本身（与设置窗同一理由——会跳的窗口没法练成肌肉记忆）。
constexpr int kCols = 4;
constexpr int kThumbW = 160;
constexpr int kThumbH = 100;
constexpr int kCellW = 172;
constexpr int kCellH = 132;
constexpr int kGap = 10;
constexpr int kMargin = 14;
constexpr int kRows = 3;  // 可见行数：超出就在墙内部滚
constexpr int kBarH = 40;  // 底栏（状态行 + 四个按钮）高度

int wallWidth() { return kMargin * 2 + kCols * kCellW + (kCols - 1) * kGap + 12; }
int wallHeight() {
    return kMargin * 2 + kRows * kCellH + (kRows - 1) * kGap + kGap + kBarH;
}

// 与设置页同一套观感：浅灰底、白色卡片格、主题色只用在「选中/主操作」。
// 选择器一律用 objectName 或 role 限定，样式表会级联到所有后代。
QString wallStyle() {
    const QColor accent(config::getStr(QStringLiteral("Interface/theme_color")));
    return QString::fromLatin1(R"CSS(
#wallBody { background: #F5F5F7; }
#wallScroll { border: none; background: transparent; }
#wallScroll QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
#wallScroll QScrollBar::handle:vertical { background: rgba(0,0,0,66); border-radius: 4px;
    min-height: 28px; }
#wallScroll QScrollBar::handle:vertical:hover { background: rgba(0,0,0,110); }
#wallScroll QScrollBar::add-line:vertical, #wallScroll QScrollBar::sub-line:vertical { height: 0; }
#wallScroll QScrollBar::add-page:vertical, #wallScroll QScrollBar::sub-page:vertical {
    background: transparent; }

/* 一格就是一张缩略图按钮：白卡 + 细描边，选中的那一格描主题色。
   按下/悬停只动描边不动位置，免得点下去格子跳一下。 */
QToolButton[role="cell"] { background: #FFFFFF; border: 1px solid rgba(0,0,0,22);
    border-radius: 8px; padding: 4px; font-size: 11.5px; color: #4A5057; }
QToolButton[role="cell"]:hover { border-color: rgba(0,0,0,60); }
/* 选中的那一格描主题色。伪状态必须是 :checked（QToolButton 的可勾选态），
   写成 :selected 只有 item view 认，按钮上静默不生效。 */
QToolButton[role="cell"]:checked { border: 2px solid %1; color: #1D1D1F; }

QLabel[role="hint"] { font-size: 11.5px; color: #86868B; }
QPushButton[role="capsule"] { background: #FFFFFF; color: #1D1D1F;
    border: 1px solid rgba(0,0,0,36); border-radius: 6px; padding: 5px 12px; }
QPushButton[role="capsule"]:hover { background: #FAFAFC; border-color: rgba(0,0,0,70); }
QPushButton[role="capsule"]:pressed { background: #F0F0F3; }
QPushButton[role="capsule"]:disabled { color: #B0B6BE; border-color: rgba(0,0,0,14); }
QPushButton[role="primary"] { background: %1; color: #FFFFFF; border: none;
    border-radius: 6px; padding: 5px 16px; font-weight: 600; }
QPushButton[role="primary"]:hover { background: %2; }
QPushButton[role="primary"]:disabled { background: #BCC9D8; color: #F4F7FA; }
)CSS")
        .arg(accent.name(), accent.lighter(112).name());
}

QPixmap fitThumb(const QImage& img) {
    // 缩略图按格子的图框等比缩放（不放大：放大只是把糊摊开）
    if (img.isNull())
        return {};
    const QImage s = img.width() <= kThumbW && img.height() <= kThumbH
                         ? img
                         : img.scaled(kThumbW, kThumbH, Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation);
    return QPixmap::fromImage(s);
}

}  // namespace

HistoryWall::HistoryWall(HistoryStore* store, QWidget* parent)
    : QWidget(parent), m_store(store) {
    setWindowTitle(QStringLiteral("截图历史"));
    setStyleSheet(wallStyle());

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* body = new QWidget(this);
    body->setObjectName("wallBody");
    body->setAttribute(Qt::WidgetAttribute::WA_StyledBackground, true);
    auto* bodyLay = new QVBoxLayout(body);
    bodyLay->setContentsMargins(kMargin, kMargin, kMargin, kMargin);
    bodyLay->setSpacing(kGap);

    m_scroll = new QScrollArea(body);
    m_scroll->setObjectName("wallScroll");
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOff);
    m_scroll->viewport()->setAutoFillBackground(false);
    bodyLay->addWidget(m_scroll, 1);

    auto* bar = new QHBoxLayout;
    bar->setSpacing(8);
    m_status = new QLabel(QStringLiteral("双击缩略图 = 贴图化"), body);
    m_status->setProperty("role", "hint");
    bar->addWidget(m_status);
    // 破坏性的在左、常用的贴着状态行读序走；最右填主题色 = 与双击同一个动作
    m_clear = new QPushButton(QStringLiteral("清空截图历史"), body);
    m_clear->setProperty("role", "capsule");
    bar->addWidget(m_clear);
    // 删除选中的这一条（与左侧「清空」同为破坏性动作；只删历史档，已贴在
    // 桌面上的贴图不受影响）
    m_del = new QPushButton(QStringLiteral("删除"), body);
    m_del->setProperty("role", "capsule");
    bar->addWidget(m_del);
    m_save = new QPushButton(QStringLiteral("保存"), body);
    m_save->setProperty("role", "capsule");
    bar->addWidget(m_save);
    m_copy = new QPushButton(QStringLiteral("复制"), body);
    m_copy->setProperty("role", "capsule");
    bar->addWidget(m_copy);
    // 「贴图化」把这条历史上屏：标注是活的，进标注模式能接着改，收工回写历史。
    // 早先旁边还有个「继续编辑」，与它只在「要不要立刻拉出工具条」上有差别，
    // 按钮语义重复——合并进这里（贴图化不强制进标注模式）。
    m_repin = new QPushButton(QStringLiteral("贴图化"), body);
    m_repin->setProperty("role", "primary");
    bar->addWidget(m_repin);
    // 五颗按钮定宽等排：文字长短不一（清空截图历史 vs 保存），跟着文字走会
    // 长长短短一排不齐。宽度取最宽那颗的 sizeHint（跟随应用字体/缩放，不写死），
    // 其余对齐到它。
    int btnW = 0;
    for (QPushButton* b : {m_clear, m_del, m_save, m_copy, m_repin})
        btnW = qMax(btnW, b->sizeHint().width());
    for (QPushButton* b : {m_clear, m_del, m_save, m_copy, m_repin})
        b->setFixedWidth(btnW);
    for (QPushButton* b : {m_repin, m_save, m_copy, m_del, m_clear})
        b->setEnabled(false);
    bodyLay->addLayout(bar);
    lay->addWidget(body);

    connect(m_repin, &QPushButton::clicked, this, [this] {
        if (int id = selectedId())
            emit repinRequested(id);
    });
    connect(m_save, &QPushButton::clicked, this, [this] {
        if (int id = selectedId())
            emit saveRequested(id);
    });
    connect(m_copy, &QPushButton::clicked, this, [this] {
        if (int id = selectedId())
            emit copyRequested(id);
    });
    connect(m_del, &QPushButton::clicked, this, [this] {
        if (int id = selectedId())
            emit deleteRequested(id);
    });
    connect(m_clear, &QPushButton::clicked, this, &HistoryWall::clearRequested);
    // 缩略图是后台补的（升级前留下的老条目还没有这张小图）：到了只补那一格
    connect(m_store, &HistoryStore::thumbReady, this, [this](int id) {
        const auto it = m_cells.find(id);
        if (it != m_cells.end() && *it)
            (*it)->setIcon(QIcon(fitThumb(m_store->thumbOf(id))));
    });

    // 定死尺寸并居中：必须在 show 之前做完，否则用户会看见窗口从旧位置挪到中间
    setFixedSize(wallWidth(), wallHeight());
    refresh();
}

void HistoryWall::refresh() {
    const QList<HistoryStore::EntryInfo> list = m_store->entries();
    m_cells.clear();
    m_selected = nullptr;
    if (m_board) {
        m_scroll->takeWidget();
        m_board->deleteLater();
    }
    m_board = new QWidget;
    m_board->setObjectName("wallBoard");
    auto* grid = new QGridLayout(m_board);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(kGap);
    grid->setVerticalSpacing(kGap);
    for (int i = 0; i < list.size(); ++i)
        m_cells.insert(list[i].id, makeCell(list[i], grid, i));
    if (list.isEmpty()) {
        auto* empty = new QLabel(QStringLiteral("（暂无截图）"), m_board);
        empty->setProperty("role", "hint");
        grid->addWidget(empty, 0, 0);
    }
    m_scroll->setWidget(m_board);
    for (QPushButton* b : {m_repin, m_save, m_copy, m_del})
        b->setEnabled(false);
    // 清空不需要选中哪一条，只要有历史就能按
    m_clear->setEnabled(!list.isEmpty());
    m_status->setText(QStringLiteral("双击缩略图 = 贴图化"));
}

QToolButton* HistoryWall::makeCell(const HistoryStore::EntryInfo& entry, QGridLayout* grid,
                                   int index) {
    auto* cell = new QToolButton(m_board);
    cell->setProperty("role", "cell");
    cell->setCheckable(true);
    cell->setToolButtonStyle(Qt::ToolButtonStyle::ToolButtonTextUnderIcon);
    cell->setIconSize(QSize(kThumbW, kThumbH));
    cell->setFixedSize(kCellW, kCellH);
    cell->setText(entry.text);
    cell->setToolTip(entry.text);
    cell->setProperty("entryId", entry.id);
    if (QImage thumb = m_store->thumbOf(entry.id); !thumb.isNull())
        cell->setIcon(QIcon(fitThumb(thumb)));
    grid->addWidget(cell, index / kCols, index % kCols);
    connect(cell, &QToolButton::clicked, this, [this, cell] { select(cell); });
    // 双击重贴：QAbstractButton 没有 doubleClicked 信号（那是 item view 的），
    // 所以只盯双击那一次按下。返回 true 把第二次 clicked 吃掉，选中态由第一击给。
    cell->installEventFilter(this);
    return cell;
}

bool HistoryWall::eventFilter(QObject* watched, QEvent* ev) {
    if (ev->type() == QEvent::Type::MouseButtonDblClick) {
        if (auto* cell = qobject_cast<QToolButton*>(watched)) {
            const int id = cell->property("entryId").toInt();
            if (id)
                emit repinRequested(id);
            return true;
        }
    }
    return QWidget::eventFilter(watched, ev);
}

void HistoryWall::select(QToolButton* cell) {
    if (m_selected && m_selected != cell)
        m_selected->setChecked(false);
    m_selected = cell;
    cell->setChecked(true);
    const int id = selectedId();
    const QString text = cell->toolTip();
    m_status->setText(m_store->docOf(id).isEmpty()
                          ? text
                          : QStringLiteral("%1（含标注，贴图化后可接着改）").arg(text));
    for (QPushButton* b : {m_repin, m_save, m_copy, m_del, m_clear})
        b->setEnabled(true);
}

int HistoryWall::selectedId() const {
    return m_selected ? m_selected->property("entryId").toInt() : 0;
}

void HistoryWall::open() {
    // 关掉只是藏起来（不销毁），所以每次打开都按 store 的当前条目重铺一次：
    // 期间可能截了新图、也可能在别处清了历史。
    refresh();
    // 没有父窗定位规则，Qt 默认位置不居中；以光标所在屏的可用区域居中
    // ——从托盘打开时就是用户当前那块屏（与设置窗同一套做法）。
    QScreen* scr = QGuiApplication::screenAt(QCursor::pos());
    if (!scr)
        scr = QGuiApplication::primaryScreen();
    if (scr) {
        const QRect av = scr->availableGeometry();
        const QRect fr = frameGeometry();
        move(av.left() + (av.width() - fr.width()) / 2,
             av.top() + (av.height() - fr.height()) / 2);
    }
    show();
    raise();
    activateWindow();
}

void HistoryWall::keyPressEvent(QKeyEvent* ev) {
    if (ev->key() == Qt::Key::Key_Escape) {
        close();
        return;
    }
    QWidget::keyPressEvent(ev);
}

}  // namespace zpin
