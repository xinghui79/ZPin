#include "toolbar.hpp"

#include <QCloseEvent>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMoveEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QSvgRenderer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <functional>

#include "defaults.hpp"

namespace zpin {

// 一级工具：高频常用。矩形/椭圆、单/双箭头、画笔/荧光笔、马赛克/高斯模糊、
// 橡皮擦/智能擦除、文本/气泡/序号 各为一个子工具组（见 toolGroups）。
const QList<CaptureToolbar::ToolDef> kTools = {
    {"move", "选择 / 编辑（点图形改形状；空白处拖动选框）"},
    {"rect", "矩形"},
    {"line", "直线"},
    {"arrow", "箭头"},
    {"pen", "画笔"},
    {"text", "文本（点选位置后输入）"},
    {"mosaic", "马赛克（涂抹区域）"},
    {"eraser", "橡皮擦（擦除整个图形，悬停有红框预览）"},
};

// 子工具组：组内子工具共用一个按钮 —— 点图标 = 选中「上次用的子工具」，
// 右键 = 展开组内切换；切换后按钮图标与气泡跟着换。键 = kTools 里的组主名。
const QHash<QString, QList<CaptureToolbar::ToolDef>>& toolGroups() {
    static const QHash<QString, QList<CaptureToolbar::ToolDef>> kGroups = {
        {"rect", {{"rect", "矩形"}, {"ellipse", "椭圆"}}},
        {"line", {{"line", "直线"}, {"dash_line", "虚线（分隔线 / 指示范围）"},
                  {"curve", "曲线（拖出后拖线身弯出弧度）"}}},
        {"arrow", {{"arrow", "单箭头"}, {"double_arrow", "双向箭头"}}},
        {"pen", {{"pen", "画笔"}, {"marker", "荧光笔（半透明粗笔迹）"}}},
        {"mosaic", {{"mosaic", "马赛克（涂抹区域）"},
                    {"blur", "高斯模糊（涂抹区域，比马赛克更平滑）"},
                    {"spotlight", "聚光灯（框选区域，框外整幅压暗）"}}},
        // 橡皮擦组：对象级擦除与像素级智能擦除收在一起 —— 都是「去掉东西」，
        // 一个擦整个图形、一个擦画面内容
        {"eraser", {{"eraser", "橡皮擦（擦除整个图形，悬停有红框预览）"},
                    {"smart_erase", "智能擦除（框选区域，重建背景去掉杂物）"}}},
        // 文字类三兄弟：都是「落一个文字块 + 输入框编辑」那一套，收在一起
        {"text", {{"text", "文本（点选位置后输入）"},
                  {"callout", "气泡标注（拖出气泡后输入文字）"},
                  {"step", "序号标记（自动递增编号）"}}},
    };
    return kGroups;
}

// 三档 2× 等比步进。细档取 1.5 而不是 2：截图上最常用的动作是画分隔线/指示范围，
// 2px 在 1080p 上偏重。虚线的空隙公式 (5+w)/w 是按线宽为单位的，圆帽各吃掉半个
// 线宽，所以可见空隙恒为 5px —— 换档不用改它。
const QList<std::pair<QString, double>> kWidths = {{"细", 1.5}, {"中", 4.0}, {"粗", 8.0}};

const char* kToolbarStyle = R"(
QToolButton { background: transparent; border: none; border-radius: 5px; padding: 3px; }
QToolButton:hover { background: rgba(255,255,255,28); }
QToolButton:pressed { background: rgba(255,255,255,44); }
QToolButton:checked { background: rgba(111,195,255,60); }
QMenu { background: rgba(23,23,28,244); color:#E8E8E8; border:1px solid rgba(255,255,255,32);
        border-radius: 8px; padding: 4px; font-size: 12px; }
QMenu::item { padding: 6px 22px 6px 8px; border-radius: 5px; }
QMenu::item:selected { background: rgba(255,255,255,30); }
)";

// 动作名: (图标名, 悬停气泡)
const QHash<QString, std::pair<QString, QString>> kToolbarActionBtns = {
    {"cancel", {"cancel", "取消 (Esc)"}},
    {"copy", {"copy", "复制 (Enter)"}},
    {"ocr", {"ocr", "识别文字（结果复制到剪贴板）"}},
    {"table", {"table", "识别表格（复制到剪贴板，可粘进 Excel/WPS）"}},
    {"sanitize", {"sanitize", "一键脱敏（自动给手机号/邮箱/身份证打码）"}},
    {"scroll", {"scroll", "长截图（用滚轮滚动页面，实时拼接；空格完成，Esc 取消）"}},
    {"pin", {"pin", "贴图"}},
    {"save", {"save", "快速保存 (Ctrl+S，存到默认目录)"}},
    {"save_as", {"saveas", "另存为（选择位置）"}},
    {"finish", {"check", "完成（结束标注）"}},
};

namespace {

// 色井（color well）：实心色点 + 半透明外环，Apple 取色控件同款。
QString svgColorWell(const QString& c, const QVariant& param) {
    const QString hex = param.isValid() ? param.toString() : c;
    return QString("<circle cx=\"12\" cy=\"12\" r=\"5.5\" fill=\"%1\" stroke=\"none\"/>"
                   "<circle cx=\"12\" cy=\"12\" r=\"8.4\" fill=\"none\" stroke=\"%2\" "
                   "stroke-opacity=\"0.5\"/>")
        .arg(hex, c);
}

// 一条横线，粗细实时等于当前档位（所见即所选）。
// 两端是圆帽，各向外伸出半个线宽，所以直段要按线宽往里收：
// x = 3.5 + w/2 .. 20.5 - w/2 让**墨迹**恒等于名义框 3.5..20.5，
// 粗档也不会糊到框外。早先写死 5..19 又把线宽乘 0.8 还夹到 6.4，
// 三档显示成 1.6/3.2/6.4 —— 细档在 20 像素图标里几乎看不见，跟中档分不出来。
QString svgWidthLine(const QString& c, const QVariant& param) {
    const double d = param.isValid() ? param.toDouble() : 4.0;
    const double w = qMin(9.0, qMax(1.4, d));
    const double x0 = 3.5 + w / 2.0;
    const double x1 = 20.5 - w / 2.0;
    return QString("<line x1=\"%1\" y1=\"12\" x2=\"%2\" y2=\"12\" stroke-width=\"%3\"/>")
        .arg(x0, 0, 'f', 2)
        .arg(x1, 0, 'f', 2)
        .arg(w, 0, 'f', 1);
}

using BodyFn = QString (*)(const QString&, const QVariant&);

// 每个图标一行 SVG 主体；未列出的名字兜底为空心圆。
const QHash<QString, BodyFn>& svgBodies() {
    // 名义框：墨迹落在 3.5..20.5（2.0 线宽的两端各伸出 1.0，所以几何坐标写
    // 4.5..19.5）。整套图标按这个框量过，收不进去的在各自注释里写明原因。
    static const QHash<QString, BodyFn> kBodies = {
        {"rect", [](const QString&, const QVariant&) {
             return QStringLiteral("<rect x=\"4.5\" y=\"6\" width=\"15\" height=\"12\" rx=\"2.6\"/>");
         }},
        {"ellipse", [](const QString&, const QVariant&) {
             return QStringLiteral("<ellipse cx=\"12\" cy=\"12\" rx=\"7.6\" ry=\"5.7\"/>");
         }},
        {"line", [](const QString&, const QVariant&) {
             return QStringLiteral("<line x1=\"4.5\" y1=\"19.5\" x2=\"19.5\" y2=\"4.5\"/>");
         }},
        // 同一条斜杆画成短划，与直线一眼可分。两个坑都用 20px 实际尺寸量过：
        // 1) 外层统一 stroke-linecap="round"，圆帽从每段两端各伸出半个线宽，所以虚线
        //    元素必须自己改成 butt，否则空隙归零；
        // 2) 段数要正好落满杆长，否则末尾拖一小截。斜杆长 22.9（=16.2×√2），
        //    取「3 段 3.3 + 2 缝 6.5」= 22.9 刚好收在端点，缝在 20px 下约 5.4 像素。
        {"dash_line", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<line x1=\"3.9\" y1=\"20.1\" x2=\"20.1\" y2=\"3.9\" stroke-linecap=\"butt\" "
                 "stroke-dasharray=\"3.3 6.5\"/>");
         }},
        // 曲线：过两个端点的一条三次贝塞尔弧，与直线的斜杆同向（左下到右上），
        // 弧顶朝右上一眼与直线区分
        {"curve", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<path d=\"M4.5 19.5C10.5 19.5 13.5 4.5 19.5 4.5\"/>");
         }},
        // 斜杆 + 对称箭头头：两翼自杆端出发、与杆轴各成 27°（真实箭头几何），
        // 翼长 7.8；整条杆占满名义框的对角
        {"arrow", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<line x1=\"4.8\" y1=\"19.2\" x2=\"19.2\" y2=\"4.8\"/>"
                 "<line x1=\"19.2\" y1=\"4.8\" x2=\"11.8\" y2=\"7.2\"/>"
                 "<line x1=\"19.2\" y1=\"4.8\" x2=\"16.8\" y2=\"12.2\"/>");
         }},
        // 双头箭头：跟 arrow 同轴向（45°），两端各一个对称箭头头 —— 早先它是水平的，
        // 两个成对的工具画成两个方向，读起来不像一家
        {"double_arrow", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<line x1=\"5.6\" y1=\"18.4\" x2=\"18.4\" y2=\"5.6\"/>"
                 "<line x1=\"18.4\" y1=\"5.6\" x2=\"12.2\" y2=\"7.6\"/>"
                 "<line x1=\"18.4\" y1=\"5.6\" x2=\"16.4\" y2=\"11.8\"/>"
                 "<line x1=\"5.6\" y1=\"18.4\" x2=\"11.8\" y2=\"16.4\"/>"
                 "<line x1=\"5.6\" y1=\"18.4\" x2=\"7.6\" y2=\"12.2\"/>");
         }},
        // 铅笔：斜杆 + 笔杆箍线
        {"pen", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<path d=\"M4.9 19.1l1-3.6L16.6 4.8a2 2 0 0 1 2.8 2.8L8.5 18.1l-3.6 1z\"/>"
                 "<line x1=\"14.8\" y1=\"6.6\" x2=\"17.4\" y2=\"9.2\"/>");
         }},
        // 荧光笔：斜杆更粗 + 靠近笔头的斜切箍线
        {"marker", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<path d=\"M5.3 18.7l1.2-3.7 9.3-9.3a2.05 2.05 0 0 1 2.9 2.9l-9.3 9.3-4.1.8z\" "
                 "stroke-width=\"2.4\"/>"
                 "<line x1=\"6.5\" y1=\"15\" x2=\"9.4\" y2=\"17.9\"/>");
         }},
        // 字模 "A"
        {"text", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<path d=\"M5.2 19L12 4.4 18.8 19\"/>"
                 "<line x1=\"7.7\" y1=\"13.8\" x2=\"16.3\" y2=\"13.8\"/>");
         }},
        // 四块填充圆角方块，20px 下即读作"像素化"。方块收小到 5.6 见方：
        // 原来是 6.6，墨量 30% 在整排图标里明显过重
        {"mosaic", [](const QString& c, const QVariant&) {
             return QString("<g fill=\"%1\" stroke=\"none\">"
                            "<rect x=\"4.8\" y=\"4.8\" width=\"5.6\" height=\"5.6\" rx=\"1.4\"/>"
                            "<rect x=\"13.6\" y=\"4.8\" width=\"5.6\" height=\"5.6\" rx=\"1.4\"/>"
                            "<rect x=\"4.8\" y=\"13.6\" width=\"5.6\" height=\"5.6\" rx=\"1.4\"/>"
                            "<rect x=\"13.6\" y=\"13.6\" width=\"5.6\" height=\"5.6\" rx=\"1.4\"/>"
                            "</g>")
                 .arg(c);
         }},
        // 实心水滴：Photoshop 的模糊工具就是水滴，沿用
        {"blur", [](const QString& c, const QVariant&) {
             return QString("<path fill=\"%1\" stroke=\"none\" d=\"M12 3.6c3.9 4.6 6.2 8 6.2 11"
                            "a6.2 6.2 0 1 1-12.4 0C5.8 11.6 8.1 8.2 12 3.6z\"/>")
                 .arg(c);
         }},
        // 聚光灯：中间一块实心亮区 + 外面一道半透明画布框 —— 「这块留下、其余压暗」。
        // 不画「压暗那一圈」：图标是浅色画在深色工具条上，半透明的圈反而比中间更亮，
        // 读起来正好反了；实心的那块才读得出「被留下来的区域」。
        // 外框几何 4.5..19.5（2.0 描边 → 墨迹 3.5..20.5 名义框），亮块 8.2..15.8，
        // 两者之间留 2.7 单位（20px 下约 2.2px）空隙。
        {"spotlight", [](const QString& c, const QVariant&) {
             return QString("<rect x=\"4.5\" y=\"4.5\" width=\"15\" height=\"15\" rx=\"3.2\" "
                            "stroke-opacity=\"0.45\"/>"
                            "<rect x=\"8.2\" y=\"8.2\" width=\"7.6\" height=\"7.6\" rx=\"1.8\" "
                            "fill=\"%1\" stroke=\"none\"/>")
                 .arg(c);
         }},
// 智能擦除：**虚线选区 + 实心四角星**。原先它沿用橡皮擦那个「倾斜圆角块」，
// 只在右下角加一颗小星——两个图标并排摆在右键菜单里时（`cachedGlyph` 原尺寸
// 20px）几乎分不出来，用户实测反馈「橡皮擦和智能擦除的图标相近」；而且那个
// 倾斜块加右上角的小星，近看更像一支铅笔。现在换成完全不同的形状语言：
// 橡皮擦 = **实心倾斜块**，智能擦除 = **轴对齐虚线框 + 居中实心星**，虚实与
// 倾角两个维度都拉开。星居中而非塞在角落，右上角那点空隙在 20px 下不够画星。
// 虚线段长 5 / 缝 3.4 是量过的：3.2 的段落到 20px 只有 2.4×1.5px，糊成点。
{"smart_erase", [](const QString& c, const QVariant&) {
             return QString(
                        "<rect x=\"5\" y=\"5\" width=\"14\" height=\"14\" rx=\"2\" "
                        "stroke-dasharray=\"5 3.4\"/>"
                        "<path fill=\"%1\" stroke=\"none\" "
                        "d=\"M12 7.4l1.35 3.25L16.6 12l-3.25 1.35L12 16.6l-1.35-3.25L7.4 12"
                        "l3.25-1.35z\"/>")
                 .arg(c);
         }},
        // 十字 + 四向 V 形箭头
        {"move", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<line x1=\"12\" y1=\"4.6\" x2=\"12\" y2=\"19.4\"/>"
                 "<line x1=\"4.6\" y1=\"12\" x2=\"19.4\" y2=\"12\"/>"
                 "<polyline points=\"9.6 7 12 4.6 14.4 7\"/>"
                 "<polyline points=\"9.6 17 12 19.4 14.4 17\"/>"
                 "<polyline points=\"7 9.6 4.6 12 7 14.4\"/>"
                 "<polyline points=\"17 9.6 19.4 12 17 14.4\"/>");
         }},
// 橡皮：实心倾斜圆角块 + 一道分隔线（经典橡皮造型，与智能擦除的虚线框
// 在「实心/虚线」和「倾斜/轴对齐」上都不重合）
{"eraser", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<g transform=\"rotate(-40 12 12.5)\">"
                 "<rect x=\"4.6\" y=\"9\" width=\"14.8\" height=\"7\" rx=\"2.2\"/>"
                 "<line x1=\"10.9\" y1=\"9\" x2=\"10.9\" y2=\"16\"/>"
                 "</g>");
         }},
        // 圆角气泡 + 左下尾。原形状墨迹撑到 2.2..21.8（名义框的 1.15 倍），
        // 整体缩到 0.86 并把线宽补回 2.0（0.86×2.33≈2.0）
        {"callout", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<g transform=\"translate(12 12) scale(0.86) translate(-12.1 -12)\" "
                 "stroke-width=\"2.33\">"
                 "<path d=\"M21 14.6a2.2 2.2 0 0 1-2.2 2.2H7.4l-4 3.8V5.6a2.2 2.2 0 0 1 "
                 "2.2-2.2h13.2A2.2 2.2 0 0 1 21 5.6v9z\"/></g>");
         }},
        // 撤销/重做：实心三角头 + 回勾粗尾巴（他给的参考样式）。尾巴单独加粗到 2.8，
        // 跟工具栏默认线宽走会头重脚轻；箭头要 fill，所以自己取颜色、关掉描边。
        // 两个严格互为镜像（x' = 24 - x）且墨迹中心落在 12 上。
        {"undo", [](const QString& c, const QVariant&) {
             return QString(
                 "<path stroke-width=\"2.8\" d=\"M9.4 8.8C12.6 7.6 16 8.4 17.6 11.8"
                 "C18.8 14.4 17.8 17 16 18.8\"/>"
                 "<path fill=\"%1\" stroke=\"none\" d=\"M4.8 8.8L10.6 4V13.6Z\"/>")
                 .arg(c);
         }},
        {"check", [](const QString&, const QVariant&) {
             return QStringLiteral("<polyline points=\"4.6 12.6 10 18 19.4 6.8\"/>");
         }},
        {"redo", [](const QString& c, const QVariant&) {
             return QString(
                 "<path stroke-width=\"2.8\" d=\"M14.6 8.8C11.4 7.6 8 8.4 6.4 11.8"
                 "C5.2 14.4 6.2 17 8 18.8\"/>"
                 "<path fill=\"%1\" stroke=\"none\" d=\"M19.2 8.8L13.4 4V13.6Z\"/>")
                 .arg(c);
         }},
        {"color", &svgColorWell},
        {"width", &svgWidthLine},
        // 取景四角框里一个 A —— 识别文字出口。四角收到 4.6..19.4（原来 4.2..19.8
        // 加上圆帽会顶出名义框）
        {"ocr", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<path d=\"M4.6 8.2V6a1.4 1.4 0 0 1 1.4-1.4h2.2\"/>"
                 "<path d=\"M15.8 4.6H18A1.4 1.4 0 0 1 19.4 6v2.2\"/>"
                 "<path d=\"M19.4 15.8V18a1.4 1.4 0 0 1-1.4 1.4h-2.2\"/>"
                 "<path d=\"M8.2 19.4H6a1.4 1.4 0 0 1-1.4-1.4v-2.2\"/>"
                 "<polyline points=\"8.7 15 12 8.6 15.3 15\"/>"
                 "<line x1=\"9.9\" y1=\"12.7\" x2=\"14.1\" y2=\"12.7\"/>");
         }},
        // 圆角外框 + 井字格线 —— 识别表格出口。格线单独降到 1.5：
        // 整套图标里它原来最重（墨量 40%），跟 3% 的虚线差 13 倍
        {"table", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<rect x=\"4.5\" y=\"4.5\" width=\"15\" height=\"15\" rx=\"2.2\" stroke-width=\"1.8\"/>"
                 "<g stroke-width=\"1.3\">"
                 "<line x1=\"4.5\" y1=\"9.5\" x2=\"19.5\" y2=\"9.5\"/>"
                 "<line x1=\"4.5\" y1=\"14.5\" x2=\"19.5\" y2=\"14.5\"/>"
                 "<line x1=\"9.5\" y1=\"4.5\" x2=\"9.5\" y2=\"19.5\"/>"
                 "<line x1=\"14.5\" y1=\"4.5\" x2=\"14.5\" y2=\"19.5\"/></g>");
         }},
        // 一键脱敏：两行文字 + 一条涂黑遮罩条（redaction 的通用画法）。
        // 早先是「取景框里三块方块」，跟 mosaic 的方块、ocr 的取景框都撞车
        {"sanitize", [](const QString& c, const QVariant&) {
             return QString(
                 "<line x1=\"4.6\" y1=\"6.6\" x2=\"19.4\" y2=\"6.6\"/>"
                 "<rect x=\"4.6\" y=\"9.9\" width=\"14.8\" height=\"4.2\" rx=\"1.2\" "
                 "fill=\"%1\" stroke=\"none\"/>"
                 "<line x1=\"4.6\" y1=\"17.4\" x2=\"14.2\" y2=\"17.4\"/>")
                 .arg(c);
         }},
        // 长截图：圆角长画布 + 向下滚动的箭头。画布加宽到名义框内 5.4..18.6
        {"scroll", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<rect x=\"5.4\" y=\"4.5\" width=\"13.2\" height=\"15\" rx=\"2.2\"/>"
                 "<path d=\"M12 8.4v6.2\"/>"
                 "<path d=\"M9.4 12.2 12 14.8l2.6-2.6\"/>"
                 "<path d=\"M9.4 6.4h5.2\"/>");
         }},
        // 两张叠放的圆角卡片。原来前卡 8.8..21 + 后卡到 2.2，墨量 28% 且撑出框
        {"copy", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<rect x=\"9\" y=\"9\" width=\"10.5\" height=\"10.5\" rx=\"2.2\"/>"
                 "<path d=\"M15 4.5H6.6a2.1 2.1 0 0 0-2.1 2.1V15\"/>");
         }},
        // 图钉：钉头 + 针脚
        {"pin", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<path d=\"M16.6 4H7.4l.9 6.1-3.2 3.7a1.05 1.05 0 0 0 .8 1.7h12.2a1.05 1.05 0 0 0 "
                 ".8-1.7L15.1 10.1 16 4z\"/>"
                 "<line x1=\"12\" y1=\"15.4\" x2=\"12\" y2=\"19.2\"/>");
         }},
        // 快速保存：托盘 + 下箭头（他觉得这版本来就好，软盘在 20px 下糊成一团）
        {"save", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<line x1=\"12\" y1=\"4.4\" x2=\"12\" y2=\"13\"/>"
                 "<polyline points=\"8.4 9.6 12 13.2 15.6 9.6\"/>"
                 "<path d=\"M7.4 8.4h-.6a2.2 2.2 0 0 0-2.2 2.2v6.2a2.2 2.2 0 0 0 2.2 2.2h10.4"
                 "a2.2 2.2 0 0 0 2.2-2.2v-6.2a2.2 2.2 0 0 0-2.2-2.2h-.6\"/>");
         }},
        // 另存为：同一张托盘缩到左下（0.78，线宽补回 2.0/0.78=2.56），右上角留白放
        // 一个加号 —— 存成新的。角标只有两笔，20px 下比铅笔读得出来；
        // 量过两者不重叠：加号墨迹下沿 9.4，托盘口沿在 11.0
        {"saveas", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<g transform=\"translate(10.4 13.6) scale(0.78) translate(-12 -11.7)\" "
                 "stroke-width=\"2.56\">"
                 "<line x1=\"12\" y1=\"4.4\" x2=\"12\" y2=\"13\"/>"
                 "<polyline points=\"8.4 9.6 12 13.2 15.6 9.6\"/>"
                 "<path d=\"M7.4 8.4h-.6a2.2 2.2 0 0 0-2.2 2.2v6.2a2.2 2.2 0 0 0 2.2 2.2h10.4"
                 "a2.2 2.2 0 0 0 2.2-2.2v-6.2a2.2 2.2 0 0 0-2.2-2.2h-.6\"/></g>"
                 "<line x1=\"17.2\" y1=\"4\" x2=\"17.2\" y2=\"8.4\"/>"
                 "<line x1=\"15\" y1=\"6.2\" x2=\"19.4\" y2=\"6.2\"/>");
         }},
        {"cancel", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<line x1=\"4.8\" y1=\"4.8\" x2=\"19.2\" y2=\"19.2\"/>"
                 "<line x1=\"19.2\" y1=\"4.8\" x2=\"4.8\" y2=\"19.2\"/>");
         }},
        // 序号标记：圆圈里的 1。圈半径从 8.4 收到 7.6（加上圆帽正好贴名义框）
        {"step", [](const QString&, const QVariant&) {
             return QStringLiteral(
                 "<circle cx=\"12\" cy=\"12\" r=\"7.6\"/>"
                 "<path d=\"M10.2 9.5l2.1-1.4v8.2\"/>");
         }},
        {"more", [](const QString& c, const QVariant&) {
             return QString("<g fill=\"%1\" stroke=\"none\">"
                            "<circle cx=\"5\" cy=\"12\" r=\"1.9\"/>"
                            "<circle cx=\"12\" cy=\"12\" r=\"1.9\"/>"
                            "<circle cx=\"19\" cy=\"12\" r=\"1.9\"/></g>")
                 .arg(c);
         }},
    };
    return kBodies;
}

QString svg(const QString& name, const QString& color, double sw, const QVariant& param) {
    const QHash<QString, BodyFn>& bodies = svgBodies();
    QString body;
    if (bodies.contains(name)) {
        body = bodies[name](color, param);
    } else {
        body = QString("<circle cx=\"12\" cy=\"12\" r=\"8\" stroke=\"%1\"/>").arg(color);
    }
    return QString(
               "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" "
               "stroke=\"%1\" stroke-width=\"%2\" stroke-linecap=\"round\" stroke-linejoin=\"round"
               "\">%3</svg>")
        .arg(color)
        .arg(sw)
        .arg(body);
}

}  // namespace

QIcon toolGlyph(const QString& name, const QString& color, double sw, const QVariant& param) {
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    const QByteArray data = svg(name, color, sw, param).toUtf8();
    QSvgRenderer renderer(data);
    if (renderer.isValid()) {
        QImage img(20, 20, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        renderer.render(&p, QRectF(1, 1, 18, 18));
        p.end();
        pm = QPixmap::fromImage(img);
    }
    return QIcon(pm);
}

namespace {
// 图标缓存：同一批图标会在「首次建工具栏 / 每次开 ⋮ 菜单 / 开颜色粗细菜单」被
// 反复要求渲染，SVG 解析 + 光栅化不该重复付。只收标准图标（默认色、默认线宽、
// 无参数），组合有限；颜色井与粗细指示器带参数且调用极少，不进缓存。
QIcon cachedGlyph(const QString& name) {
    // 故意 new 不释放：缓存里是 QIcon（底层 QPixmap 持平台资源），若作为函数内
    // static，它的析构会排在静态析构序列里——那时机身 QGuiApplication 可能已经
    // 没了。Qt 内部有防护不至于崩，但这是纯进程退出期开销换来的隐患，
    // 直接泄漏掉更干净（OS 会回收）。
    static QHash<QString, QIcon>* kCache = new QHash<QString, QIcon>();
    if (const auto it = kCache->constFind(name); it != kCache->constEnd())
        return it.value();
    const QIcon icon = toolGlyph(name);
    kCache->insert(name, icon);
    return icon;
}

// 子工具组的图标 = 标准图标 + 右下角一个小三角（PS 那种「这个工具还有兄弟」的
// 记号）。画进 pixmap 而不是靠样式表的 ::menu-indicator：Qt 默认把箭头画在按钮
// 右侧居中、还要占一条点击区，位置和大小都调不到 PS 那个角落去。
QIcon cachedGroupGlyph(const QString& name) {
    static QHash<QString, QIcon>* kCache = new QHash<QString, QIcon>();
    const QString key = name + QLatin1String("#group");
    if (const auto it = kCache->constFind(key); it != kCache->constEnd())
        return it.value();
    QPixmap pm = cachedGlyph(name).pixmap(20, 20);
    pm.setDevicePixelRatio(1.0);   // 下面按 20x20 的绝对坐标画角标
    QPainter p(&pm);
    p.setRenderHint(QPainter::RenderHint::Antialiasing, false);
    p.setPen(Qt::PenStyle::NoPen);
    p.setBrush(QColor(255, 255, 255, 215));   // 工具条底色是深灰，白色读得清
    QPolygonF tri;
    tri << QPointF(14, 19.5) << QPointF(19.5, 19.5) << QPointF(19.5, 14);
    p.drawPolygon(tri);
    p.end();
    const QIcon icon(pm);
    kCache->insert(key, icon);
    return icon;
}
}  // namespace

void warmToolbarIcons() {
    // 预热：进程里第一次用 QSvgRenderer 的模块初始化 + 逐个解析 SVG 的开销，
    // 挪到启动后空闲时付掉，免得第一次框选卡在工具栏构造上。
    for (const CaptureToolbar::ToolDef& def : kTools)
        cachedGlyph(def.name);
    // 子工具组的成员不在 kTools 里（只有组主名在）：椭圆/双向箭头/荧光笔/高斯模糊/
    // 智能擦除/气泡/序号 都得在这儿过一遍，否则第一次点开组菜单才现渲染。
    for (auto it = toolGroups().constBegin(); it != toolGroups().constEnd(); ++it)
        for (const CaptureToolbar::ToolDef& def : it.value())
            cachedGlyph(def.name);
    // 动作按钮的图标名直接取 kToolbarActionBtns——早先在这里手抄了 12 个名字，
    // 加个动作/换图标要记得回来改一遍，漏改的表现只是「第一次框选卡一下」，
    // 极轻所以最难发现。改成与按钮构造同源。
    for (auto it = kToolbarActionBtns.constBegin(); it != kToolbarActionBtns.constEnd(); ++it)
        cachedGlyph(it.value().first);
    // 撤销/重做/更多这三个按钮不走 kToolbarActionBtns
    for (const char* n : {"undo", "redo", "more"})
        cachedGlyph(QLatin1String(n));
}

// 悬停气泡：底板必须手绘，和工具栏面板走同一条路径。
// 早先底板写在样式表里（QLabel { background:#26262C }），但这是个
// WA_TranslucentBackground 的独立顶层窗，layered 呈现一不稳定底板就整块消失，
// 只剩白字压在截图画面上——遇到白色背景的截图就是白字白底，什么都看不见。
class TipBubble : public QLabel {
public:
    explicit TipBubble(QWidget* parent = nullptr) : QLabel(parent) {}

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(23, 23, 28, 248));
        p.drawRoundedRect(QRectF(rect()).adjusted(0, 0, -1, -1), 6, 6);
        // 先描深色再填白：底板万一还是没画出来，白字在任何截图内容上都读得出
        const QRect r = contentsRect();
        p.setPen(QPen(QColor(0, 0, 0, 210), 3.0));
        p.drawText(r, Qt::AlignCenter, text());
        p.setPen(QColor(242, 242, 242));
        p.drawText(r, Qt::AlignCenter, text());
    }
};

// ---- CaptureToolbar ----

CaptureToolbar::CaptureToolbar(const QColor& initialColor, double initialWidth,
                               QList<ToolDef> tools, QHash<QString, QList<ToolDef>> groups,
                               QStringList actions, QStringList menuActions, QWidget* parent)
    : QWidget(parent, Qt::WindowType::FramelessWindowHint |
                          Qt::WindowType::WindowStaysOnTopHint | Qt::WindowType::Tool |
                          Qt::WindowType::WindowDoesNotAcceptFocus),
      m_tools(std::move(tools)),
      m_groups(groups.isEmpty() ? toolGroups() : std::move(groups)),
      m_actions(std::move(actions)),
      m_menuActions(std::move(menuActions)),
      m_color(initialColor),
      m_width(initialWidth) {
    setAttribute(Qt::WidgetAttribute::WA_ShowWithoutActivating);
    // 圆角面板由 paintEvent 手绘：WA_TranslucentBackground 下透明四角才是真圆角
    setAttribute(Qt::WidgetAttribute::WA_TranslucentBackground);
    setStyleSheet(QLatin1String(kToolbarStyle));
    QVBoxLayout* lay = new QVBoxLayout(this);
    lay->setContentsMargins(7, 5, 7, 5);
    lay->setSpacing(2);
    QWidget* row = new QWidget();
    QHBoxLayout* rowLay = new QHBoxLayout(row);
    rowLay->setContentsMargins(0, 0, 0, 0);
    rowLay->setSpacing(1);
    buildRow(rowLay);
    lay->addWidget(row);
    syncIndicatorIcons();
}

void CaptureToolbar::addSep(QHBoxLayout* row) {
    QFrame* line = new QFrame();
    line->setFrameShape(QFrame::Shape::VLine);
    line->setStyleSheet("QFrame { color: rgba(255,255,255,40); }");
    line->setFixedSize(1, 20);
    row->addWidget(line);
    row->addSpacing(3);
}

QToolButton* CaptureToolbar::addButton(QHBoxLayout* row, const QString& name,
                                       const QString& tip, std::function<void()> cb,
                                       bool checkable) {
    QToolButton* b = new QToolButton();
    b->setIcon(cachedGlyph(name));
    b->setIconSize(QSize(20, 20));
    b->setToolTip(tip);
    b->setCheckable(checkable);
    b->setFocusPolicy(Qt::FocusPolicy::NoFocus);
    b->setCursor(Qt::CursorShape::PointingHandCursor);
    b->installEventFilter(this);  // 自绘中文气泡（Qt 原生 tooltip 在非激活窗上不弹）
    connect(b, &QToolButton::clicked, b, [cb](bool) { cb(); });
    row->addWidget(b);
    return b;
}

void CaptureToolbar::buildRow(QHBoxLayout* row) {
    // 1. 撤销 / 重做
    m_btnUndo = addButton(row, "undo", "撤销 (Ctrl+Z)",
                          [this] { emit action("undo"); });
    m_btnRedo = addButton(row, "redo", "重做 (Ctrl+Y)",
                          [this] { emit action("redo"); });
    addSep(row);

    // 2. 一级常用工具（组按钮带 ▾ 角标）+ ⋮ 更多贴在工具区末尾
    for (const ToolDef& def : m_tools) {
        if (m_groups.contains(def.name))
            createGroupButton(row, def.name);
        else
            m_toolButtons[def.name] =
                addButton(row, def.name, def.label,
                          [this, n = def.name] { selectTool(n); }, true);
    }
    m_btnMore = addButton(row, "more", "更多工具", [this] { moreMenu(); });
    if (m_menuActions.isEmpty())
        m_btnMore->hide();   // 贴图工具条没有低频出口，留一个弹不出东西的按钮更糟
    addSep(row);

    // 3. 颜色 / 粗细
    m_btnColor = new QToolButton();
    m_btnColor->setIconSize(QSize(20, 20));
    m_btnColor->setToolTip("颜色");
    m_btnColor->setFocusPolicy(Qt::FocusPolicy::NoFocus);
    m_btnColor->setCursor(Qt::CursorShape::PointingHandCursor);
    m_btnColor->installEventFilter(this);
    connect(m_btnColor, &QToolButton::clicked, this, [this](bool) { colorMenu(); });
    row->addWidget(m_btnColor);

    m_btnWidth = new QToolButton();
    m_btnWidth->setIconSize(QSize(20, 20));
    m_btnWidth->setToolTip("粗细");
    m_btnWidth->setFocusPolicy(Qt::FocusPolicy::NoFocus);
    m_btnWidth->setCursor(Qt::CursorShape::PointingHandCursor);
    m_btnWidth->installEventFilter(this);
    connect(m_btnWidth, &QToolButton::clicked, this, [this](bool) { widthMenu(); });
    row->addWidget(m_btnWidth);
    addSep(row);

    // 4. 完成动作：按构造参数装配（另存为已上主排，⋮ 里只剩低频出口）
    for (const QString& name : m_actions) {
        const auto& [icon, tip] = kToolbarActionBtns[name];
        addButton(row, icon, tip, [this, n = name] { emit action(n); });
    }
}

void CaptureToolbar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(23, 23, 28, 242));
    p.drawRoundedRect(QRectF(rect()).adjusted(0, 0, -1, -1), 8, 8);
}

void CaptureToolbar::setEnabledActions(bool undo, bool redo) {
    m_btnUndo->setEnabled(undo);
    m_btnRedo->setEnabled(redo);
}

bool CaptureToolbar::eventFilter(QObject* obj, QEvent* ev) {
    if (qobject_cast<QToolButton*>(obj)) {
        QToolButton* btn = static_cast<QToolButton*>(obj);
        if (ev->type() == QEvent::Type::Enter && !btn->toolTip().isEmpty()) {
            showTip(btn->toolTip(), btn);
        } else if (ev->type() == QEvent::Type::Leave) {
            hideTip();
        } else if (ev->type() == QEvent::Type::ContextMenu) {
            // 子工具组只认右键展开（按住不弹：DelayedPopup 的等待手感不好掌握）。
            // 弹出后按钮的残留态由 createGroupButton 里挂的 aboutToHide 负责清。
            QMenu* gm = m_groupMenus.value(btn, nullptr);
            if (gm && !gm->isVisible())
                gm->popup(btn->mapToGlobal(QPoint(0, btn->height() + 2)));
            return true;   // 别再去弹系统自己的上下文菜单
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void CaptureToolbar::showTip(const QString& text, QToolButton* btn) {
    if (m_tip.isNull()) {
        m_tip = new TipBubble(nullptr);
        m_tip->setWindowFlags(Qt::WindowType::FramelessWindowHint |
                              Qt::WindowType::WindowStaysOnTopHint |
                              Qt::WindowType::Tool);
        m_tip->setAttribute(Qt::WidgetAttribute::WA_ShowWithoutActivating);
        m_tip->setAttribute(Qt::WidgetAttribute::WA_TransparentForMouseEvents);
        m_tip->setAttribute(Qt::WidgetAttribute::WA_TranslucentBackground);
        m_tip->setAlignment(Qt::AlignmentFlag::AlignCenter);
        // 底板交给 paintEvent 手绘，样式表只留内边距与字体
        m_tip->setStyleSheet(
            "QLabel { background:transparent; border:none; color:#F2F2F2;"
            " padding:3px 9px; font-size:12px; }");
    }
    m_tip->setText(text);
    m_tip->adjustSize();
    const QScreen* scr = btn->screen();   // 工具条还没落到任何屏上时为 null
    if (!scr)
        return;
    const QRect geo = scr->availableGeometry();
    const QPoint gx = btn->mapToGlobal(QPoint(btn->width() / 2, btn->height()));
    int x = qBound(geo.left() + 4, gx.x() - m_tip->width() / 2,
                   geo.right() - m_tip->width() - 4);
    int y = gx.y() + 4;
    if (y + m_tip->height() > geo.bottom() && btn->y() > m_tip->height())
        y = gx.y() - m_tip->height() - 8;  // 空间不足则放按钮上方
    m_tip->move(x, y);
    m_tip->show();
    m_tip->raise();
}

void CaptureToolbar::hideTip() {
    if (!m_tip.isNull())
        m_tip->hide();
}

void CaptureToolbar::moveEvent(QMoveEvent* ev) {
    hideTip();  // 工具栏移动时收起气泡，避免残留
    QWidget::moveEvent(ev);
}

void CaptureToolbar::closeEvent(QCloseEvent* ev) {
    // 工具栏销毁时一并销毁气泡窗与菜单
    if (!m_tip.isNull()) {
        m_tip->hide();
        m_tip->deleteLater();
        m_tip.clear();
    }
    if (!m_more.isNull()) {
        m_more->close();
        m_more->deleteLater();
        m_more.clear();
    }
    QWidget::closeEvent(ev);
}

void CaptureToolbar::selectTool(const QString& name) {
    for (auto it = m_toolButtons.begin(); it != m_toolButtons.end(); ++it)
        it.value()->setChecked(it.key() == name);
    // 组按钮：name 是组内成员（或组主名本身）时点亮该组
    for (auto it = m_groupButtons.begin(); it != m_groupButtons.end(); ++it)
        it.value()->setChecked(m_groupOf.value(name, name) == it.key());
    for (auto it = m_groupActions.begin(); it != m_groupActions.end(); ++it)
        it.value()->setChecked(it.key() == name);
    // 选中组内某个子工具：按钮换脸（图标/气泡）并记住为「上次用的」
    const QString group = m_groupOf.value(name);
    if (!group.isEmpty()) {
        m_groupCurrent[group] = name;
        applyGroupFace(group);
    }
    emit toolSelected(name);
}

void CaptureToolbar::createGroupButton(QHBoxLayout* row, const QString& primary) {
    const QList<ToolDef> members = m_groups.value(primary);
    QToolButton* b = new QToolButton();
    b->setIconSize(QSize(20, 20));
    b->setCheckable(true);
    b->setFocusPolicy(Qt::FocusPolicy::NoFocus);
    b->setCursor(Qt::CursorShape::PointingHandCursor);
    b->installEventFilter(this);  // 自绘中文气泡（Qt 原生 tooltip 在非激活窗上不弹）
    // 单击 = 选中「上次用的子工具」
    connect(b, &QToolButton::clicked, this, [this, primary] {
        selectTool(m_groupCurrent.value(primary, primary));
    });
    // 组内菜单**不交给 setMenu()**：那样 Qt 必然带一种弹出方式 ——
    // MenuButtonPopup 会在按钮右半切出一条点击区（「点开子工具的那个太大」），
    // DelayedPopup 要按住（手感难说）。所以只把 QMenu 挂在按钮下随它析构，弹出完全
    // 由 eventFilter 的右键分支负责：单击用工具、右键展开，没有第三种手势。
    QMenu* menu = new QMenu(b);
    menu->setAttribute(Qt::WidgetAttribute::WA_TranslucentBackground);
    // 症状（用户实测）：右键弹出子工具菜单、不选就关掉后，按钮上留一小块灰底，鼠标
    // 移走也不消失。样式表里的灰底只可能来自 :pressed 或 :hover 两个态；菜单会抢走
    // 鼠标，但到底是哪个态没被清掉，离屏探针模拟不出抢焦点这一段，所以两个一起清再
    // 重绘。
    connect(menu, &QMenu::aboutToHide, b, [b] {
        b->setDown(false);
        b->setAttribute(Qt::WidgetAttribute::WA_UnderMouse, false);
        b->update();
    });
    for (const ToolDef& def : members) {
        QAction* act = menu->addAction(cachedGlyph(def.name), def.label);
        act->setCheckable(true);
        connect(act, &QAction::triggered, this,
                [this, n = def.name] { selectTool(n); });
        m_groupActions.insert(def.name, act);
        m_groupOf.insert(def.name, primary);
    }
    m_groupMenus.insert(b, menu);
    m_groupButtons.insert(primary, b);
    m_groupCurrent.insert(primary, members.first().name);
    applyGroupFace(primary);
    row->addWidget(b);
}

void CaptureToolbar::applyGroupFace(const QString& primary) {
    QToolButton* b = m_groupButtons.value(primary);
    if (!b)
        return;
    const QString current = m_groupCurrent.value(primary, primary);
    for (const ToolDef& def : m_groups.value(primary)) {
        if (def.name == current) {
            b->setIcon(cachedGroupGlyph(def.name));   // 带右下角小三角
            b->setToolTip(def.label);
        }
    }
}

QPoint CaptureToolbar::belowPos(QToolButton* btn, int menuW) const {
    QPoint tl = btn->mapToGlobal(QPoint(0, btn->height() + 2));
    if (QScreen* scr = btn->screen()) {
        const QRect g = scr->availableGeometry();
        tl.setX(qBound(g.left() + 4, tl.x(), g.right() - menuW - 3));
    }
    return tl;
}

void CaptureToolbar::moreMenu() {
    // ⋮ 菜单：popup 非阻塞弹出，再点一次 ⋮ 即收起（切换而非重开）
    if (!m_more.isNull() && m_more->isVisible()) {
        m_more->close();
        return;
    }
    if (m_more.isNull()) {
        m_more = new QMenu(this);
        m_more->setAttribute(Qt::WidgetAttribute::WA_TranslucentBackground);
        // 低频出口收进 ⋮：外层一排只留常用的，工具条不再横向膨胀
        for (const QString& name : m_menuActions) {
            const auto it = kToolbarActionBtns.constFind(name);
            if (it == kToolbarActionBtns.constEnd())
                continue;
            QAction* act = m_more->addAction(cachedGlyph(it->first), it->second);
            connect(act, &QAction::triggered, this,
                    [this, name](bool) { emit action(name); });
        }
    }
    m_more->popup(belowPos(m_btnMore, m_more->sizeHint().width()));
}

void CaptureToolbar::syncIndicatorIcons() {
    m_btnColor->setIcon(toolGlyph("color", "#E8E8E8", 2.0, m_color.name()));
    m_btnWidth->setIcon(toolGlyph("width", "#E8E8E8", 2.0, m_width));
}

void CaptureToolbar::colorMenu() {
    QMenu* menu = new QMenu(this);
    menu->setAttribute(Qt::WidgetAttribute::WA_TranslucentBackground);
    QWidget* gridW = new QWidget();
    QGridLayout* grid = new QGridLayout(gridW);
    grid->setContentsMargins(6, 6, 6, 6);
    grid->setSpacing(5);
    const QStringList& palette = defaults::colorPalette();
    for (int i = 0; i < palette.size(); ++i) {
        const QString& hexColor = palette[i];
        QToolButton* b = new QToolButton();
        b->setFixedSize(22, 22);
        b->setCursor(Qt::CursorShape::PointingHandCursor);
        b->setStyleSheet(QString(
            "QToolButton { background: %1; border: 1px solid rgba(255,255,255,90);"
            "border-radius: 4px; } QToolButton:hover { border-color: #FFFFFF; }").arg(hexColor));
        connect(b, &QToolButton::clicked, this, [this, hexColor, menu](bool) {
            pickColor(QColor(hexColor), menu);
        });
        grid->addWidget(b, i / 6, i % 6);
    }
    QWidgetAction* wa = new QWidgetAction(menu);
    wa->setDefaultWidget(gridW);
    menu->addAction(wa);
    // 不提供「自定义颜色」：色板里的预设色够用（用户要求删掉）。
    // 设置页的颜色按钮现在也用这套色板，取色对话框已整个删掉。
    menu->exec(belowPos(m_btnColor, menu->sizeHint().width()));
    menu->deleteLater();
}

void CaptureToolbar::pickColor(const QColor& color, QMenu* menu) {
    menu->close();
    if (!color.isValid())
        return;
    m_color = color;
    syncIndicatorIcons();
    emit colorSelected(color);
}

void CaptureToolbar::widthMenu() {
    QMenu* menu = new QMenu(this);
    menu->setAttribute(Qt::WidgetAttribute::WA_TranslucentBackground);
    for (const auto& [label, w] : kWidths) {
        QAction* act = menu->addAction(toolGlyph("width", "#E8E8E8", 2.0, w),
                                       QString("%1（%2px）").arg(label, QString::number(w, 'g', 3)));
        connect(act, &QAction::triggered, this, [this, w](bool) { pickWidth(w); });
    }
    menu->exec(belowPos(m_btnWidth, menu->sizeHint().width()));
    menu->deleteLater();
}

void CaptureToolbar::pickWidth(double w) {
    m_width = w;
    syncIndicatorIcons();
    emit widthSelected(w);
}

void CaptureToolbar::keyPressEvent(QKeyEvent* ev) {
    // 焦点兜底：工具条获得焦点时也响应常用键
    const Qt::KeyboardModifiers m = ev->modifiers();
    if (ev->key() == Qt::Key_Escape) {
        emit action("cancel");
    } else if (ev->key() == Qt::Key_Return || ev->key() == Qt::Key_Enter) {
        emit action("copy");
    } else if (ev->key() == Qt::Key_Z && m & Qt::ControlModifier) {
        emit action(m & Qt::ShiftModifier ? "redo" : "undo");
    } else if (ev->key() == Qt::Key_Y && m & Qt::ControlModifier) {
        emit action("redo");
    } else if (ev->key() == Qt::Key_S && m & Qt::ControlModifier) {
        emit action("save");
    } else {
        QWidget::keyPressEvent(ev);
    }
}

}  // namespace zpin
