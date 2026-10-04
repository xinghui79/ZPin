// 贴图管理 —— 屏幕上的一组贴图窗（**不分组**：切组/建组/重命名这一整套对多数人
// 是多余概念，留着只会让托盘菜单、右键菜单和多出一个全局快捷键）。
// 这里只保留真正用得上的：贴图、销毁、隐藏/显示全部、恢复可点击。
#pragma once

#include <QObject>
#include <QPointF>

#include <vector>

#include "memtrim.hpp"
#include "rcore.hpp"

class QImage;

namespace zpin {

class PinWindow;

class PinManager : public QObject {
    Q_OBJECT

public:
    explicit PinManager(QObject* parent = nullptr);
    ~PinManager() override;

    // 迭代用副本：hide()/deletePin() 会回调进本类改动列表，直接遍历成员会踩空
    QList<PinWindow*> allWindows() const;

    // 窗口吸附用的贴图内容矩形（绝对物理像素，右/下开区间）+ 贴图窗句柄。
    // 贴图窗本体比可见内容大一圈阴影边距，DWM 枚举拿到的是整个窗口——
    // 吸附方据此把整窗矩形排除、换成内容矩形，否则吸附框比贴图大一圈。
    QVector<rcore::WindowRect> pinSnapRects() const;

    // ---- 贴图入口与销毁 ----
    PinWindow* pinImage(const QImage& img, const QPointF& pos, double refDpr = 0.0);
    void deletePin(PinWindow* win);

    // ---- 全局动作 ----
    bool toggleVisible();
    int restoreClickable();
    // 按窗口实际可见性同步内部隐藏状态（窗口自行关闭/隐藏时调用）。
    void notifyVisibility();

signals:
    void notify(const QString& title, const QString& text);

private:
    void add(PinWindow* win);

    memtrim::CleanerId m_thumbCleaner = 0;
    std::vector<PinWindow*> m_windows;
    bool m_hidden = false;
};

// 贴图落点：光标位置夹取到虚拟桌面内边距（60px）范围内。
QPointF clipPivot();

}  // namespace zpin
