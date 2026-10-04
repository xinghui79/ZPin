// 贴图窗口 —— 无边框置顶小窗：拖动 / 光标锚点缩放 / 透明度 / 边框阴影 / 右键全项菜单。
// 缩放规则：≥100% 最近邻（保持锐利），50%~100% 平滑，<50% 用预生成缩略图
// （长边上限 512）。窗口四周含 kMargin 阴影边距，锚点换算已扣除。
// HiDPI：图是物理像素、窗口走逻辑坐标，1:1 显示靠除以参考比例 refDpr。
#pragma once

#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

class QCloseEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QShowEvent;
class QWheelEvent;

namespace zpin {

class PinAnnotator;
class PinManager;

class PinWindow : public QWidget {
    Q_OBJECT

public:
    // refDpr<=0 时用落点屏的比例；截图产物必须传入那次截图所用屏的比例，
    // 否则混合 DPI 下「贴出来比截图大一圈」。
    PinWindow(const QImage& image, PinManager* manager, const QPointF& pos,
              double refDpr = 0.0);
    ~PinWindow() override;

    // ---- 标注层要的坐标换算（标注坐标 = 底图物理像素）----
    // 标注锚定在**当前显示图**（旋转/翻转/灰度后的 m_source）上：形状坐标、
    // 马赛克/模糊取色都按这张图算，composite 也是把它画在这上面。
    QImage displayImage() const { return m_source; }
    double refDpr() const { return m_refDpr; }
    double scaleFactor() const { return m_scale; }
    QRectF imgRect() const;
    QPointF localToImage(const QPointF& pos) const;
    QPointF imageToGlobal(const QPointF& ip) const;
    // 可见内容的全局物理矩形（图像区，不含四周阴影边距）：窗口吸附用。
    QRect snapRect() const;

    bool clickThrough() const { return m_clickThrough; }
    void setClickThrough(bool on);
    bool annotating() const;
    void setAnnotate(bool on);
    // 带着标注文档上屏（历史条目「贴图化」）：构造贴图时给的必须是
    // **没画标注的干净底图**，文档坐标与它同原点，否则标注会画两遍。
    // activate=false 只装文档不拉工具条（「贴图化」：看着与原图一致，进标注
    // 模式后旧笔迹还能接着改）。
    void setAnnotateWithDoc(const QJsonArray& doc, bool activate = true);
    // 标注会话收工回调（历史「贴图化」的回写用）：贴图隐藏/关闭/退出标注时把
    // 最新文档交给回调，文档没动过/已烘进像素则不交。不设置 = 不回写。
    void setDocSaveCallback(std::function<void(const QJsonArray&)> cb) {
        m_docSave = std::move(cb);
    }

    // 空闲整理：丢掉缩略图缓存（下次绘制自动重建）
    void dropThumbCache() { m_thumb.reset(); }

    void copyImage();
    void ocrText();
    void ocrTable();
    void sanitizeImage();
    void saveAs();
    void zoomIn();
    void zoomOut();
    void actualSize();
    // 缩放到完整落进所在屏（留 40px 边距）；长截图等超屏内容入场用。
    void fitToScreen();
    void reset();
    void rotate90();
    void flipH();
    void flipV();
    void toggleGrayscale();
    void setOpacityPct(int pct);
    void setBorderOn(bool on);
    void setShadowOn(bool on);
    void hidePin();
    void destroyPin();

protected:
    void showEvent(QShowEvent* ev) override;
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseDoubleClickEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void wheelEvent(QWheelEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;

private:
    QPointF imgSize() const;
    void updateSize();
    void rebuildSource();
    void clampScale();
    void applyScale();
    void zoomAt(double factor, const QPointF& globalPos);
    void keepOnScreen();
    void bakeAnnotations();
    QImage composite() const;
    void showMenu();
    // 开菜单前清掉待用的拖拽偏移（QMenu 抓鼠标会让 release 丢失）
    void dropPendingDrag();
    double maxSide() const;
    const QImage& thumbFor();
    void drawShadow(QPainter& p, const QRectF& r);
    void drawLabel(QPainter& p, const QRectF& r);
    // 右下角短促角标：缩放显示尺寸，脱敏显示结果。到时自动消失（重绘由 m_labelTimer 触发）
    void flashLabel(const QString& text, int ms = 900);
    // 把最新标注文档交给 m_docSave（隐藏/关闭收工时调一次）
    void flushDocSave();

    PinManager* m_manager;
    QImage m_base;              // 原始图（灰度只是它的视图变换；几何变换烘焙后进入其中）
    QImage m_source;            // 变换后（旋转/翻转/灰度）= 显示与标注的锚定图
    std::optional<QImage> m_thumb;
    std::unique_ptr<PinAnnotator> m_annot;

    double m_scale = 1.0;
    double m_rotation = 0.0;
    bool m_flipH = false;
    bool m_flipV = false;
    bool m_grayscale = false;
    bool m_border = true;
    bool m_glow = false;
    bool m_shadow = false;
    double m_opacity = 1.0;
    double m_refDpr = 1.0;
    std::optional<QPointF> m_dragOffset;
    bool m_clickThrough = false;
    bool m_annotConsumed = false;   // 标注模式下本次左键按下是否被标注层消费
    quint64 m_sanitizeSeq = 0;      // 一键脱敏的代号：回调回来时代号不符就说明贴图已换图/退标注
    bool m_sanitizePending = false; // 脱敏在途防重入：连点两次会压入两批重叠马赛克
    QColor m_borderColor;
    QTimer m_labelTimer;            // 角标闪现的存活计时（到点触发一次重绘让它消失）
    QString m_labelText;            // 当前角标文字（尺寸 / 脱敏结果）
    std::function<void(const QJsonArray&)> m_docSave;  // 标注收工回写（贴图化）
    bool m_docInvalid = false;      // 标注已烘进像素（旋转/翻转）：文档与底图错位，不可回写
    QJsonArray m_lastFlushed;       // 上次回写的文档快照：没改过/刚回写过就不重复写
};

}  // namespace zpin
