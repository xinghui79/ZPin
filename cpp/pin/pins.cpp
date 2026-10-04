#include "pins.hpp"

#include <QCursor>
#include <QImage>
#include <QScreen>

#include <algorithm>

#include "capture.hpp"
#include "logging.hpp"
#include "memtrim.hpp"
#include "pin_window.hpp"

namespace zpin {
namespace {

constexpr int kPivotPadding = 60;  // 贴图落点距虚拟桌面边缘的最小内边距

}  // namespace

PinManager::PinManager(QObject* parent) : QObject(parent) {
    // 空闲时丢掉缩略图缓存（贴图窗口会在需要时重建）。回调捕获了 this，
    // 析构里必须注销，否则空闲定时器会打到已析构的 PinManager 上
    m_thumbCleaner = memtrim::registerCleaner([this] {
        for (PinWindow* w : allWindows())
            w->dropThumbCache();
    });
}

PinManager::~PinManager() {
    memtrim::unregisterCleaner(m_thumbCleaner);
    // 贴图窗是无 parent 的顶层窗，只有本类持有它们。这里不收掉，退出时它们会
    // 比 PinManager 活得久，~QApplication 阶段派发的 closeEvent 再去摸
    // m_manager 就是野指针。先清空列表（notifyVisibility 只能看到空集），
    // 再逐个关掉删除。
    QList<PinWindow*> wins = allWindows();
    m_windows.clear();
    for (PinWindow* w : wins) {
        w->close();
        delete w;
    }
}

QList<PinWindow*> PinManager::allWindows() const {
    QList<PinWindow*> wins;
    wins.reserve(static_cast<qsizetype>(m_windows.size()));
    for (PinWindow* w : m_windows)
        wins << w;
    return wins;
}

QVector<rcore::WindowRect> PinManager::pinSnapRects() const {
    QVector<rcore::WindowRect> out;
    for (PinWindow* w : allWindows()) {
        if (!w->isVisible())
            continue;   // 隐藏的贴图不参与吸附
        const QRect r = w->snapRect();
        if (r.isEmpty())
            continue;
        // winrect 的右/下是开区间（width = r - l），QRect 的 right() 少 1
        out.append({static_cast<qintptr>(w->winId()), r.left(), r.top(),
                    r.left() + r.width(), r.top() + r.height()});
    }
    return out;
}

void PinManager::add(PinWindow* win) {
    m_windows.push_back(win);
    win->show();
    win->raise();
    win->activateWindow();
    m_hidden = false;
}

PinWindow* PinManager::pinImage(const QImage& img, const QPointF& pos, double refDpr) {
    if (img.isNull())
        return nullptr;
    auto* win = new PinWindow(img, this, pos, refDpr);
    add(win);
    return win;
}

void PinManager::deletePin(PinWindow* win) {
    const auto it = std::find(m_windows.begin(), m_windows.end(), win);
    if (it != m_windows.end())
        m_windows.erase(it);
    win->close();
    win->deleteLater();
}

bool PinManager::toggleVisible() {
    const QList<PinWindow*> wins = allWindows();
    if (wins.isEmpty()) {
        emit notify(QStringLiteral("贴图"), QStringLiteral("当前没有贴图"));
        return m_hidden;
    }
    m_hidden = !m_hidden;
    for (PinWindow* w : wins) {
        if (m_hidden)
            w->hide();
        else
            w->show();
    }
    return m_hidden;
}

int PinManager::restoreClickable() {
    int n = 0;
    for (PinWindow* w : allWindows()) {
        if (!w->clickThrough())
            continue;
        w->setClickThrough(false);
        ++n;
    }
    return n;
}

void PinManager::notifyVisibility() {
    bool anyVisible = false;
    for (const PinWindow* w : allWindows())
        anyVisible |= w->isVisible();
    m_hidden = !anyVisible;
}

QPointF clipPivot() {
    const QRect b = capture::virtualBounds();
    const QPoint cur = QCursor::pos();
    return QPointF(std::min(std::max(cur.x(), b.left() + kPivotPadding),
                            b.right() - kPivotPadding),
                   std::min(std::max(cur.y(), b.top() + kPivotPadding),
                            b.bottom() - kPivotPadding));
}

}  // namespace zpin
