// 选区覆盖层的绘制实现 —— 从 overlay.cpp 拆出：遮罩/吸附高亮/辅助线/手柄/
// 尺寸标签/提示条与高亮过渡动画。状态全部在 SelectionController，这里只画。
#include "overlay.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QTimer>
#include <chrono>
#include <cmath>

#include "config.hpp"

namespace zpin {

namespace {

using clock = std::chrono::steady_clock;

constexpr double kAnimSec = 0.11;      // 秒：高亮框位置过渡时长（ease-out）
constexpr double kHandleDraw = 3.5;
constexpr double kAnchorLen = 7.0;

}  // namespace

void SelectionController::paintScreen(OverlayWindow* window, QPainter& p) {
    const QRect geo = window->geometry();
    p.setRenderHint(QPainter::RenderHint::SmoothPixmapTransform, false);
    const QRect src = m_map.rectOf(QRectF(geo)).intersected(m_base.rect());
    if (!src.isEmpty())
        p.drawImage(window->rect(), m_base, src);

    const QColor accent(config::getStr("Interface/theme_color"));
    const int borderW = qMax(1, config::getInt("Capture/border_width"));
    auto toLocal = [geo](const QPointF& pt) {
        return QPointF(pt.x() - geo.left(), pt.y() - geo.top());
    };

    const bool sel = m_rect.isValid() && m_rect.width() >= 1 && m_rect.height() >= 1;
    if (!sel) {
        paintHoverLayer(window, p, geo, toLocal, accent, borderW);
        paintToast(window, p);
        return;
    }

    const QRectF r(toLocal(m_rect.topLeft()), m_rect.size());
    paintMask(window, p, r);

    // 标注层：形状坐标锚定在截图底图上，经选区视口映射回屏幕
    m_annot->draw(p, r);

    paintGuides(window, p, geo, r);

    // 全屏十字线
    drawCrosshairIfEnabled(p, geo, toLocal);

    // 边框
    p.setPen(QPen(accent, borderW));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);

    paintHandles(p, r, accent);
    paintSizeLabel(p, r);
    paintToast(window, p);
}

void SelectionController::paintToast(OverlayWindow* window, QPainter& p) {
    // 选区/悬停顶部中央的短提示（取色结果），1.6 秒自动消失
    if (m_toast.isEmpty() || clock::now() >= m_toastUntil)
        return;
    const QFontMetrics fm = p.fontMetrics();
    const int tw = fm.horizontalAdvance(m_toast) + 24;
    const int th = fm.height() + 10;
    const double wx = (window->width() - tw) / 2.0;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 20, 24, 220));
    p.drawRoundedRect(QRectF(wx, 22, tw, th), 6, 6);
    p.setPen(QColor("#7CE38B"));
    p.drawText(QRectF(wx, 22, tw, th), Qt::AlignCenter, m_toast);
}

void SelectionController::setSuggest(const QRectF& rect) {
    if (rect == m_suggest)
        return;
    m_animFrom = m_suggest;
    m_suggest = rect;
    // 只在两个有效矩形之间做过渡；进出「无吸附」直接切，不滑
    m_animActive = m_animFrom.isValid() && rect.isValid();
    if (!m_animActive)
        return;
    m_animAt = clock::now();
    if (!m_animTimer) {
        m_animTimer = new QTimer(this);
        m_animTimer->setInterval(16);
        connect(m_animTimer, &QTimer::timeout, this, [this] {
            if (!m_animActive) {
                m_animTimer->stop();
                return;
            }
            if (std::chrono::duration<double>(clock::now() - m_animAt).count() >= kAnimSec)
                m_animActive = false;
            updateAll();
            if (!m_animActive)
                m_animTimer->stop();
        });
    }
    m_animTimer->start();
}

QRectF SelectionController::paintSuggest() {
    if (!m_animActive || !m_animFrom.isValid() || !m_suggest.isValid())
        return m_suggest;
    const double t = std::chrono::duration<double>(clock::now() - m_animAt).count() / kAnimSec;
    if (t >= 1.0) {
        m_animActive = false;
        return m_suggest;
    }
    const double e = 1.0 - std::pow(1.0 - t, 3);  // ease-out cubic
    return QRectF(m_animFrom.left() + (m_suggest.left() - m_animFrom.left()) * e,
                  m_animFrom.top() + (m_suggest.top() - m_animFrom.top()) * e,
                  m_animFrom.width() + (m_suggest.width() - m_animFrom.width()) * e,
                  m_animFrom.height() + (m_suggest.height() - m_animFrom.height()) * e);
}

void SelectionController::paintHoverLayer(OverlayWindow* window, QPainter& p, const QRect& geo,
                                          const std::function<QPointF(const QPointF&)>& toLocal,
                                          const QColor& accent, int borderW) {
    // 未框选态：整窗遮罩 + 吸附建议框/角锚点 + 十字线。
    // 吸附建议框内部不盖遮罩：建议框挖空，透出底图真实画面。
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(QColor(config::getStr("Capture/mask_color"))));
    const QRectF suggestRect = paintSuggest();
    if (suggestRect.isValid()) {
        const QRectF sr(toLocal(suggestRect.topLeft()), suggestRect.size());
        QPainterPath windowPath;
        windowPath.addRect(QRectF(window->rect()));
        QPainterPath sugPath;
        sugPath.addRect(sr);
        p.drawPath(windowPath.subtracted(sugPath));
        // 吸附建议框 + 角锚点
        p.setPen(QPen(accent, borderW));
        p.setBrush(Qt::NoBrush);
        p.drawRect(sr);
        if (config::getBool("Capture/show_anchors"))
            drawCornerAnchors(p, sr, QColor(config::getStr("Capture/anchor_stroke_color")));
    } else {
        p.drawRect(window->rect());
    }
    drawCrosshairIfEnabled(p, geo, toLocal);
}

void SelectionController::paintMask(OverlayWindow* window, QPainter& p, const QRectF& r) {
    // 遮罩：窗口区域减去选区，选区外盖半透明遮罩色
    QPainterPath windowPath;
    windowPath.addRect(QRectF(window->rect()));
    QPainterPath selPath;
    selPath.addRect(r);
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(QColor(config::getStr("Capture/mask_color"))));
    p.drawPath(windowPath.subtracted(selPath));
}

void SelectionController::paintGuides(OverlayWindow* window, QPainter& p, const QRect& geo,
                                      const QRectF& r) {
    // 选区边延伸辅助线 + 拖拽吸附命中的对齐参考线（红色，区别于选区边框）
    if (!config::getBool("Capture/disable_guides")) {
        p.setPen(QPen(QColor(255, 255, 255, 70), 1, Qt::PenStyle::DashLine));
        const QRectF wr(window->rect());
        p.drawLine(QPointF(wr.left(), r.top()), QPointF(wr.right(), r.top()));
        p.drawLine(QPointF(wr.left(), r.bottom()), QPointF(wr.right(), r.bottom()));
        p.drawLine(QPointF(r.left(), wr.top()), QPointF(r.left(), wr.bottom()));
        p.drawLine(QPointF(r.right(), wr.top()), QPointF(r.right(), wr.bottom()));
    }
    if (!m_guides.isEmpty()) {
        p.setPen(QPen(QColor("#FF2D55"), 1));
        p.setBrush(Qt::NoBrush);
        const QRectF wr(window->rect());
        for (const auto& [kind, v] : m_guides) {
            if (kind == 'v') {
                const double x = v - geo.left();
                p.drawLine(QPointF(x, wr.top()), QPointF(x, wr.bottom()));
            } else {
                const double y = v - geo.top();
                p.drawLine(QPointF(wr.left(), y), QPointF(wr.right(), y));
            }
        }
    }
}

void SelectionController::paintHandles(QPainter& p, const QRectF& r, const QColor& accent) {
    // 选中/调整态的 8 个手柄；聚焦中的手柄用主题色加亮
    if (m_state != "selected" && m_state != "moving" && m_state != "resizing" &&
        m_state != "extending")
        return;
    for (int i = 0; i < kHandleCount; ++i) {
        const QPointF hp = handlePoint(r, i);
        p.setBrush(i == m_focusIdx ? QBrush(accent) : QBrush(QColor("#FFFFFF")));
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.drawRect(QRectF(hp.x() - kHandleDraw, hp.y() - kHandleDraw, kHandleDraw * 2,
                          kHandleDraw * 2));
    }
}

void SelectionController::paintSizeLabel(QPainter& p, const QRectF& r) {
    // 选区上方的物理尺寸标签与（可选）快捷键提示
    const QRect pr = m_map.rectOf(m_rect);
    drawLabel(p, r, QString("%1 × %2 px").arg(pr.width()).arg(pr.height()));
    if (config::getBool("Capture/show_hints"))
        drawLabel(p, r, "Enter 复制 · Esc 取消", true, true);
}

void SelectionController::drawCornerAnchors(QPainter& p, const QRectF& r, const QColor& color) {
    // 吸附建议框的四个 L 形角锚点
    p.setPen(QPen(color, 2));
    p.setBrush(Qt::NoBrush);
    const QPointF pts[4] = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
    for (int i = 0; i < 4; ++i) {
        const QPointF& pt = pts[i];
        const double sx = (i == 0 || i == 3) ? 1 : -1;  // 朝向框内
        const double sy = (i == 0 || i == 1) ? 1 : -1;
        p.drawLine(pt, QPointF(pt.x() + sx * kAnchorLen, pt.y()));
        p.drawLine(pt, QPointF(pt.x(), pt.y() + sy * kAnchorLen));
    }
}

void SelectionController::drawCrosshairIfEnabled(
    QPainter& p, const QRect& geo, const std::function<QPointF(const QPointF&)>& toLocal) {
    if (!config::getBool("Capture/show_crosshair"))
        return;
    const QPointF c = toLocal(m_cursor);
    if (!QRectF(0, 0, geo.width(), geo.height()).contains(c))
        return;
    p.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt::PenStyle::DashLine));
    p.drawLine(QPointF(0, c.y()), QPointF(geo.width(), c.y()));
    p.drawLine(QPointF(c.x(), 0), QPointF(c.x(), geo.height()));
}

void SelectionController::drawLabel(QPainter& p, const QRectF& r, const QString& text,
                                    bool below, bool dim) {
    const QFontMetrics fm = p.fontMetrics();
    const double w = fm.horizontalAdvance(text) + 16;
    const double h = fm.height() + 6;
    const double x = r.left();
    const double y = below ? r.bottom() + 6 : r.top() - h - 4;
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 20, 24, 200));
    p.drawRoundedRect(QRectF(x, y, w, h), 4, 4);
    p.setPen(QColor(255, 255, 255, dim ? 150 : 235));
    p.drawText(QRectF(x, y, w, h), Qt::AlignCenter, text);
    p.restore();
}


}  // namespace zpin
