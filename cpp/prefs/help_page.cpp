#include "help_page.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>

#include <functional>
#include <vector>

#include "config.hpp"

namespace zpin::help {
namespace {

// 当前配置里某动作的键位文本（未绑定时给出占位）。
QString accelOf(const QString& action) {
    const QString text = config::hotkeyAccel(action);
    return text.isEmpty() ? QStringLiteral("（未绑定）") : text;
}

// 键帽：圆角小方块，代表一次按键。字号 11px，与设置页的说明文字同档。
QLabel* keyChip(const QString& text) {
    auto* chip = new QLabel(text);
    chip->setStyleSheet(QStringLiteral(
        "QLabel { background: #F5F5F7; border: 1px solid rgba(0,0,0,30);"
        " border-radius: 5px; padding: 2px 8px; font-size: 11px; color: #1D1D1F; }"));
    return chip;
}

// 随配置刷新的键位标签：tpl 里的 %1 是键位占位，showEvent 时按当前配置重填。
struct DynText {
    QLabel* label = nullptr;
    QString tpl;
    QString action;
};

// 主题页：原生控件排版（不再走 QTextBrowser/HTML）——小节 = 灰色小标题 +
// 白色圆角卡片，卡内一行 = 短句标题（13px）+ 灰色说明（11.5px），或一排键帽 +
// 说明。字号全部走 prefsStyle 的角色体系，与设置页完全一致；内容直接平铺、
// 不自带滚动区（HelpPage 会把栈高钉到当前主题的内容高度，滚动交给外层的
// makePageShell——内层再包一个滚动区会出现两条滚动条）。
class TopicPage : public QWidget {
public:
    explicit TopicPage(int index, QWidget* parent = nullptr) : QWidget(parent) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(14);
        switch (index) {
        case 0:
            buildQuick(lay);
            break;
        case 1:
            buildCapture(lay);
            break;
        case 2:
            buildAnnotate(lay);
            break;
        default:
            buildPin(lay);
            break;
        }
        // 主题比可视区矮时把多余空间留在页尾：卡片贴顶，下方留白
        lay->addStretch(1);
    }

    // 主题页每次显示（切主题、从其它导航页回来）后由 HelpPage 重新收高；
    // 必须排在键位文本刷新之后——改键可能让说明文字换行数变化
    std::function<void()> onShown;

protected:
    void showEvent(QShowEvent* ev) override {
        QWidget::showEvent(ev);
        // 键位是渲染时现读 config 的：改键后切到帮助要能看到新键位
        for (const DynText& d : m_dynamic)
            d.label->setText(d.tpl.arg(accelOf(d.action)));
        if (onShown)
            onShown();
    }

private:
    // 普通行：短句标题（13px）+ 灰色说明（11.5px），两行式
    QWidget* textRow(const QString& title, const QString& detail, DynText* dyn = nullptr) {
        auto* box = new QWidget;
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(2);
        auto* t = new QLabel(title);
        t->setProperty("role", "rowLabel");
        t->setWordWrap(true);
        auto* d = new QLabel(dyn ? detail.arg(accelOf(dyn->action)) : detail);
        d->setProperty("role", "rowCaption");
        d->setWordWrap(true);
        if (dyn) {
            dyn->label = d;
            dyn->tpl = detail;
        }
        bl->addWidget(t);
        bl->addWidget(d);
        return box;
    }

    // 键位随配置刷新的普通行：detailTpl 里的 %1 会被替换成该动作的当前键位
    QWidget* textRowDyn(const QString& title, const QString& detailTpl,
                        const QString& action) {
        DynText dyn;
        dyn.action = action;
        QWidget* box = textRow(title, detailTpl, &dyn);
        m_dynamic.append(dyn);
        return box;
    }

    // 键帽行：一排键帽 + 说明（单行）。dynamicAction 非空时末尾追加一个
    // 随配置刷新的键帽。
    QWidget* keysRow(const QStringList& keys, const QString& detail,
                     const QString& dynamicAction = QString()) {
        auto* box = new QWidget;
        auto* bl = new QHBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(10);
        auto* chips = new QHBoxLayout;
        chips->setSpacing(4);
        for (const QString& k : keys)
            chips->addWidget(keyChip(k));
        if (!dynamicAction.isEmpty()) {
            auto* chip = keyChip(accelOf(dynamicAction));
            m_dynamic.append({chip, QStringLiteral("%1"), dynamicAction});
            chips->addWidget(chip);
        }
        chips->addStretch(1);
        bl->addLayout(chips, 0);
        auto* d = new QLabel(detail);
        d->setProperty("role", "rowLabel");
        d->setWordWrap(true);
        bl->addWidget(d, 1);
        return box;
    }

    // 小节：灰色小标题 + 白色圆角卡片（与设置页的分组卡片同一观感）
    void addSection(QVBoxLayout* lay, const QString& name,
                    const std::vector<QWidget*>& rows) {
        auto* label = new QLabel(name);
        label->setProperty("role", "section");
        lay->addWidget(label);
        auto* card = new QWidget;
        card->setObjectName("prefsCard");
        card->setAttribute(Qt::WidgetAttribute::WA_StyledBackground, true);
        auto* cl = new QVBoxLayout(card);
        cl->setContentsMargins(14, 12, 14, 12);
        cl->setSpacing(10);
        for (QWidget* r : rows)
            cl->addWidget(r);
        lay->addWidget(card);
    }

    void buildQuick(QVBoxLayout* lay) {
        addSection(lay, QStringLiteral("三步出图"),
                   {textRowDyn(QStringLiteral("框选截图"),
                               QStringLiteral("按 %1 拖出选框：悬停自动高亮界面元素 / "
                                              "内容块 / 窗口，单击即选中；按住拖动是"
                                              "自由框选。"),
                               QStringLiteral("capture")),
                    textRowDyn(QStringLiteral("全屏截图"),
                               QStringLiteral("按 %1 一键抓取全部屏幕。"),
                               QStringLiteral("capture_full")),
                    textRow(QStringLiteral("标注与输出"),
                            QStringLiteral("选区下方工具条：矩形、箭头、画笔、马赛克、智能擦除、"
                                           "文本、序号……Enter 复制，「贴图」钉到桌面，"
                                           "「保存」一键存默认目录；「⋮」里还有识别文字、"
                                           "识别表格、一键脱敏、长截图。")),
                    textRow(QStringLiteral("截图历史"),
                            QStringLiteral("每张截图都进历史墙（托盘 → 截图历史）：一格一张"
                                           "缩略图，可贴图化（标注还能接着改，收工自动存"
                                           "回这条历史）、保存、复制，或删除单条 / 清空"
                                           "全部；历史存在本机，重启不丢。")),
                    textRow(QStringLiteral("文件与格式"),
                            QStringLiteral("默认存 PNG；设置 → 输出 可换 JPEG / WEBP / "
                                           "BMP / TIFF、改文件名模板与保存目录、开"
                                           "「每次截图自动保存一份」。质量旋钮只作用于"
                                           "有损的 JPEG / WEBP，其余格式一律无损。"))});
        addSection(lay, QStringLiteral("键位"),
                   {textRow(QStringLiteral("改键"),
                            QStringLiteral("全局键位在 设置 → 快捷键 修改，托盘菜单与"
                                           "本文档同步显示。"))});
    }

    void buildCapture(QVBoxLayout* lay) {
        addSection(lay, QStringLiteral("吸附与框选"),
                   {textRow(QStringLiteral("自动吸附"),
                            QStringLiteral("悬停优先识别界面元素（按钮、文本块），认不出"
                                           "时按画面吸附内容块，过大回退整窗；单击选中，"
                                           "Enter 或双击完成截取。按住左键拖动始终是自由"
                                           "框选，不受吸附影响。")),
                    keysRow({QStringLiteral("Space")},
                            QStringLiteral("关掉悬停吸附、只留自由框选（未框选时有效）")),
                    keysRow({QStringLiteral("Tab")},
                            QStringLiteral("轮换检测层级：自动 → 仅整窗 → 仅界面元素"
                                           "（未框选时有效）"))});
        addSection(lay, QStringLiteral("调整选区"),
                   {textRow(QStringLiteral("手柄与扩选"),
                            QStringLiteral("拖 8 个手柄调整大小；点选框外自动扩选，"
                                           "框内拖动整体移动（已画标注跟着走）。")),
                    keysRow({QStringLiteral("方向键")},
                            QStringLiteral("逐像素微调（Shift 加速，Ctrl+方向 扩选一边）")),
                    textRow(QStringLiteral("对齐参考线"),
                            QStringLiteral("拖拽时自动对齐屏幕与其它窗口的边、中线；"
                                           "可在 设置 → 截图 关闭。"))});
        addSection(lay, QStringLiteral("放大镜与取色"),
                   {keysRow({QStringLiteral("Alt")},
                            QStringLiteral("召唤 / 收起 10 倍放大镜（像素网格 + 坐标色值"
                                           " + 选区边界参照）")),
                    keysRow({QStringLiteral("C")},
                            QStringLiteral("复制光标处颜色，画面顶部弹提示"))});
        addSection(lay, QStringLiteral("长截图"),
                   {textRow(QStringLiteral("滚动与拼接"),
                            QStringLiteral("「⋮」→ 长截图：用滚轮滚动页面，每滚一屏"
                                           "自动对齐拼接。只认滚轮——静止期的动画、"
                                           "悬停预览不会入画。")),
                    keysRow({QStringLiteral("空格")},
                            QStringLiteral("用已捕获内容出图（或点面板「完成」）")),
                    textRow(QStringLiteral("「没接上」提示"),
                            QStringLiteral("一下滚太快超出对齐范围，或页面在滚动间隙"
                                           "自己重排——那几帧会被跳过，慢滚重试即可"
                                           "；页面重排的接缝会自动用最新渲染回填。"))});
    }

    void buildAnnotate(QVBoxLayout* lay) {
        addSection(lay, QStringLiteral("工具与子工具组"),
                   {textRow(QStringLiteral("子工具组"),
                            QStringLiteral("右下角带小三角的按钮：右键展开切换——矩形 / "
                                           "椭圆、直线 / 虚线 / 曲线、单箭头 / 双向"
                                           "箭头、画笔 / 荧光笔、马赛克 / 高斯模糊 / 聚光灯、"
                                           "橡皮擦 / 智能擦除、文本 / 气泡 / 序号。")),
                    textRow(QStringLiteral("曲线"),
                            QStringLiteral("拖出后保持选中：拖线身把它弯出弧度，"
                                           "拖两端手柄调端点位置。")),
                    textRow(QStringLiteral("聚光灯"),
                            QStringLiteral("框住保持原样的区域，框外整幅压暗；可以框多块，"
                                           "亮区互不干扰。橡皮擦点亮区即删除。")),
                    textRow(QStringLiteral("低频出口"),
                            QStringLiteral("「⋮」菜单：识别文字、识别表格、一键脱敏、长截图。"))});
        addSection(lay, QStringLiteral("二次编辑"),
                   {textRow(QStringLiteral("选中图形"),
                            QStringLiteral("用「选择 / 编辑」（双向箭头）或不选工具时点中"
                                           "已画图形：整体拖动、拖端点改形、换色"
                                           "换粗细（即时生效，不占撤销步）。曲线画完保持"
                                           "选中，拖线身就能弯。")),
                    keysRow({QStringLiteral("Del")}, QStringLiteral("删除选中的图形")),
                    textRow(QStringLiteral("橡皮擦"),
                            QStringLiteral("对象级擦除：悬停红框预览整个将被擦除的图形，"
                                           "一次涂抹算一步撤销。")),
                    textRow(QStringLiteral("智能擦除"),
                            QStringLiteral("与橡皮擦同组（右键切换）。框选一块区域，"
                                           "后台用周围的背景把内容重建掉（去水印 / "
                                           "杂物）；计算中框是红色的，完成自动浮现，"
                                           "Ctrl+Z 整块撤销。背景越规律效果越好。"))});
        addSection(lay, QStringLiteral("文本与序号"),
                   {textRow(QStringLiteral("文本 / 气泡"),
                            QStringLiteral("字号跟随粗细档位；Enter 换行，Ctrl+Enter 或"
                                           "点击空白完成，Esc 取消。")),
                    textRow(QStringLiteral("序号标记"),
                            QStringLiteral("落下自动递增；撤销 / 擦掉中间某号后，新序号"
                                           "自动补位。"))});
    }

    void buildPin(QVBoxLayout* lay) {
        addSection(lay, QStringLiteral("日常操作"),
                   {keysRow({QStringLiteral("滚轮")},
                            QStringLiteral("以光标为中心缩放")),
                    keysRow({QStringLiteral("双击")},
                            QStringLiteral("隐藏贴图（托盘 → 显示全部贴图 找回）")),
                    textRow(QStringLiteral("右键菜单"),
                            QStringLiteral("标注、识别文字、识别表格、一键脱敏、不透明度、旋转 / "
                                           "翻转 / 灰度、鼠标穿透、另存为。"))});
        addSection(lay, QStringLiteral("快捷键"),
                   {keysRow({QStringLiteral("Ctrl+C"), QStringLiteral("Ctrl+S")},
                            QStringLiteral("复制 / 另存为")),
                    keysRow({QStringLiteral("Ctrl+0"), QStringLiteral("Ctrl+R")},
                            QStringLiteral("实际大小 / 重置全部变换")),
                    keysRow({QStringLiteral("Ctrl++"), QStringLiteral("Ctrl+-")},
                            QStringLiteral("放大 / 缩小")),
                    keysRow({QStringLiteral("Ctrl+T")}, QStringLiteral("旋转 90°")),
                    keysRow({QStringLiteral("Esc"), QStringLiteral("Del")},
                            QStringLiteral("隐藏 / 销毁贴图")),
                    keysRow({}, QStringLiteral("隐藏 / 显示全部贴图"),
                            QStringLiteral("toggle_pins"))});
        addSection(lay, QStringLiteral("鼠标穿透"),
                   {textRow(QStringLiteral("恢复可点击"),
                            QStringLiteral("托盘 → 恢复贴图可点击：穿透后的兜底，一键"
                                           "恢复所有贴图的鼠标交互。"))});
    }

    QVector<DynText> m_dynamic;   // 键位随配置刷新的标签
};

// 帮助页：4 个主题收在同一页里，顶部一排分段按钮切换。
// 早先每个主题各占侧栏一行，于是导航被文档吃掉 4 行、还和设置区的「贴图」重名，
// 加上分组头一共 16 行 —— 超出可视高度就冒出滚动条（截图里那个）。
class HelpPage : public QWidget {
public:
    explicit HelpPage(QWidget* parent = nullptr) : QWidget(parent) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(10);

        // 分段控件：灰轨道 + 白色选中块（样式在 prefs_dialog 的 prefsStyle）
        auto* tabs = new QWidget;
        tabs->setObjectName("helpTabs");
        tabs->setAttribute(Qt::WidgetAttribute::WA_StyledBackground, true);
        auto* tabLay = new QHBoxLayout(tabs);
        tabLay->setContentsMargins(2, 2, 2, 2);
        tabLay->setSpacing(2);
        m_stack = new QStackedWidget;
        static const char* kTopics[] = {"快速上手", "框选截图", "标注工具", "贴图"};
        for (int i = 0; i < 4; ++i) {
            auto* btn = new QToolButton;
            btn->setText(QString::fromUtf8(kTopics[i]));
            btn->setCheckable(true);
            btn->setAutoExclusive(true);
            btn->setFocusPolicy(Qt::FocusPolicy::NoFocus);
            btn->setCursor(Qt::CursorShape::PointingHandCursor);
            btn->setProperty("role", "helpTab");
            const int index = i;
            connect(btn, &QToolButton::clicked, this, [this, index] { showTopic(index); });
            tabLay->addWidget(btn);
            auto* page = new TopicPage(index, m_stack);
            page->onShown = [this] { refit(); };
            m_stack->addWidget(page);
            m_tabs.append(btn);
        }
        tabLay->addStretch(1);
        // 轨道随内容收：AlignLeft 让它按 sizeHint 取宽，而不是被拉满整行
        lay->addWidget(tabs, 0, Qt::AlignmentFlag::AlignLeft);
        lay->addWidget(m_stack, 1);
        showTopic(0);
    }

private:
    // 收高不在 showTopic 里做：切主题、从其它导航页回来、初次显示三条路
    // 最终都落在 TopicPage 的 showEvent（键位刷新之后）上，onShown 统一触发
    // refit，高度永远按刚刷新过的内容算。
    void showTopic(int index) {
        m_stack->setCurrentIndex(index);
        for (int i = 0; i < m_tabs.size(); ++i)
            m_tabs[i]->setChecked(i == index);
    }

    // QStackedWidget 的 sizeHint 取「最高的主题」：不收高的话矮主题下方空出
    // 一大截、滚动条也拖满整页。把栈高钉到当前主题的内容高度：矮主题贴顶
    // 留白，高主题由外层的 makePageShell 出滚动条——全程只有那一条。
    void refit() {
        m_stack->setFixedHeight(m_stack->currentWidget()->sizeHint().height());
    }

    QVector<QToolButton*> m_tabs;
    QStackedWidget* m_stack = nullptr;
};

}  // namespace

QWidget* createHelpPage(QWidget* parent) {
    return new HelpPage(parent);
}

}  // namespace zpin::help
