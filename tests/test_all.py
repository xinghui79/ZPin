"""ZPin 全量回归测试 —— 不依赖 pytest，直接运行本文件即可。

用法（仓库根目录）：
    .venv\\Scripts\\python tests\\test_all.py

离屏（offscreen）平台运行，不弹任何窗口；文本渲染相关断言刻意回避
（离屏平台缺中文字体，会出现"豆腐块"，与真实运行无关）。
"""
import os
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtCore import QEvent, QPoint, QPointF, QRect, QRectF, Qt  # noqa: E402
from PySide6.QtGui import QColor, QImage, QKeyEvent  # noqa: E402
from PySide6.QtWidgets import QApplication, QLabel, QPushButton  # noqa: E402

app = QApplication(sys.argv)  # noqa: E402 — 必须先建 QApplication 再 import 业务模块

import capture  # noqa: E402
import config  # noqa: E402
import toolbar  # noqa: E402
import winapi  # noqa: E402
from engine import AnnotateEngine  # noqa: E402
from help_dialog import HelpDialog  # noqa: E402
from overlay import SelectionController, _Magnifier  # noqa: E402
from shapes import (Arrow, Callout, Ellipse, Line, Mosaic, Rect, Stroke,  # noqa: E402
                    TextShape)
from tray import TrayController  # noqa: E402

config.init()
_RESULTS: list[str] = []


def section(name: str) -> None:
    """打印小节标题。"""
    print(f"—— {name}")


def done(name: str) -> None:
    _RESULTS.append(name)
    print(f"  ✓ {name}")


# ---------------------------------------------------------------- 吸附管线
def test_snap_pipeline() -> None:
    section("吸附管线（窗口缓存 + 任务栏白名单 + 全屏伪元素拒收）")
    sc = SelectionController()
    sc._map = capture.build_map()
    sc._bounds = capture.virtual_bounds()
    sc._hwnds = set()
    sc._win_cache = winapi.visible_window_rects(sc._hwnds)
    sc._win_cache_at = time.monotonic()
    taskbar = [e for e in sc._win_cache if e[1:5] == (0, 0, 1920, 40)]
    assert taskbar, f"任务栏应出现在窗口缓存里: {sc._win_cache}"
    sc._cursor = QPointF(760, 16)          # 任务栏（顶部 40px）上
    sc._el_at_cursor = QPointF(-1e6, -1e6)
    sc._el_at_time = 0.0
    sc._el_rect = None
    assert sc._snap_target().isValid(), "悬停任务栏应有吸附建议"
    # 全屏伪元素（UIA 在桌面空白处偶发返回）必须被拒收
    full = QRectF(0, 0, sc._bounds.width(), sc._bounds.height())
    assert not sc._element_useful(full, QRectF()), "全屏伪元素应被拒收"
    icon = QRectF(95, 351, 94, 67)
    assert sc._element_useful(icon, QRectF()), "正常大小元素应放行"
    done("吸附管线")


# ---------------------------------------------------------------- 引擎撤销
def test_engine_undo() -> None:
    section("引擎撤销/重做（z 序保持）")
    base = QImage(200, 200, QImage.Format_ARGB32)
    base.fill(0xFFFFFFFF)
    e = AnnotateEngine(base)
    a = Line(QColor("red"), 2); a.p1, a.p2 = QPointF(0, 0), QPointF(10, 10)
    b = Rect(QColor("blue"), 2); b.p1, b.p2 = QPointF(20, 20), QPointF(30, 30)
    c = Line(QColor("green"), 2); c.p1, c.p2 = QPointF(40, 40), QPointF(50, 50)
    for s in (a, b, c):
        e.add(s)
    e.remove(b)
    e.undo();  assert e.shapes == [a, b, c], "撤销删除应恢复原 z 序"
    e.redo();  assert e.shapes == [a, c], "重做删除"
    e.begin_erase_stroke()
    e.erase_at(QPointF(45, 45), tol=5)
    e.end_erase_stroke()
    e.undo();  assert e.shapes == [a, c], "涂抹擦除撤销应恢复原 z 序"
    done("引擎撤销/重做")


# ---------------------------------------------------------------- 12 工具端到端
def test_tools_e2e() -> None:
    section("标注工具端到端（按下→移动→抬起）")
    sc = SelectionController()
    sc._map = capture.DesktopMap([], QRect(), QPointF().toPoint())
    sc._bounds = QRect(0, 0, 1920, 1080)
    sc._base = QImage(1000, 800, QImage.Format_ARGB32)
    sc._base.fill(0xFF202020)
    sc._rect = QRectF(100, 100, 600, 400)
    sc._state = "selected"
    sc._annot.ensure()
    a = sc._annot
    assert a.engine is not None

    def drag(tool, p1, p2, moves=4):
        a._on_tool_selected(tool)
        sc.on_press(p1)
        for i in range(1, moves + 1):
            t = i / moves
            sc.on_move(QPointF(p1.x() + (p2.x() - p1.x()) * t,
                               p1.y() + (p2.y() - p1.y()) * t))
        sc.on_release(p2)

    def key(k, ctrl=False):
        mod = (Qt.KeyboardModifier.ControlModifier if ctrl
               else Qt.KeyboardModifier.NoModifier)
        sc.on_key(QKeyEvent(QEvent.KeyPress, k, mod))

    drag("rect", QPointF(120, 120), QPointF(300, 220))
    assert isinstance(a.engine.shapes[-1], Rect), "rect"
    drag("ellipse", QPointF(320, 120), QPointF(500, 220))
    assert isinstance(a.engine.shapes[-1], Ellipse), "ellipse"
    drag("line", QPointF(120, 300), QPointF(300, 380))
    assert isinstance(a.engine.shapes[-1], Line), "line"
    drag("arrow", QPointF(320, 300), QPointF(500, 380))
    assert isinstance(a.engine.shapes[-1], Arrow), "arrow"
    drag("pen", QPointF(520, 120), QPointF(650, 200))
    assert isinstance(a.engine.shapes[-1], Stroke), "pen"
    drag("marker", QPointF(520, 250), QPointF(650, 320))
    assert a.engine.shapes[-1].highlight, "marker 应为荧光笔"
    drag("mosaic", QPointF(120, 400), QPointF(280, 470))
    assert isinstance(a.engine.shapes[-1], Mosaic), "mosaic"

    a._on_tool_selected("text")
    sc.on_press(QPointF(400, 420))
    a._text_editor.setPlainText("测试")
    a._text_editor._commit()
    assert isinstance(a.engine.shapes[-1], TextShape), "text"

    a._on_tool_selected("callout")
    sc.on_press(QPointF(150, 150))
    sc.on_move(QPointF(260, 180))
    sc.on_release(QPointF(260, 180))
    a._text_editor.setPlainText("说明")
    a._text_editor._commit()
    assert isinstance(a.engine.shapes[-1], Callout), "callout"

    drag("step", QPointF(600, 420), QPointF(600, 420))
    assert a.engine.shapes[-1].number == 1, "step1"
    drag("step", QPointF(640, 420), QPointF(640, 420))
    assert a.engine.shapes[-1].number == 2, "step2"
    t0 = len(a.engine.shapes)
    key(Qt.Key_Z, ctrl=True)
    assert len(a.engine.shapes) == t0 - 1, "撤销"
    drag("step", QPointF(680, 420), QPointF(680, 420))
    assert a.engine.shapes[-1].number == 2, "序号补位"

    n0 = len(a.engine.shapes)
    a._on_tool_selected("eraser")
    pos_calls: list[int] = []
    orig_pos = a._position_toolbar
    a._position_toolbar = lambda: (pos_calls.append(1), orig_pos())[0]  # 监听置顶调用
    sc.on_press(QPointF(300, 380))
    assert sc._state == "erasing", "橡皮擦应进入 erasing 态"
    sc.on_release(QPointF(300, 380))
    a._position_toolbar = orig_pos
    assert len(a.engine.shapes) == n0 - 1, "对象级擦除"
    assert pos_calls, "擦除抬手后应重新置顶工具栏（否则工具栏被遮罩盖住）"

    a._on_tool_selected(None)
    r = next(x for x in a.engine.shapes if isinstance(x, Rect))
    old = QPointF(r.p1)
    sc.on_press(QPointF(200, 121))          # 矩形上边框（内部不是命中区）
    assert a._edit_shape is r, "二次编辑抓取"
    sc.on_move(QPointF(260, 181))
    sc.on_release(QPointF(260, 181))
    assert r.p1 - old == QPointF(60, 60), "端点拖动"

    a._on_tool_selected("move")
    sc.on_press(QPointF(400, 300))
    sc.on_move(QPointF(450, 330))
    sc.on_release(QPointF(450, 330))
    assert r.p1.x() > old.x() + 100, "整体平移应带动标注"

    a.select_shape(r)
    assert a.handle_key(QKeyEvent(QEvent.KeyPress, Qt.Key_Delete,
                                  Qt.KeyboardModifier.NoModifier)), "Del 删除"
    assert r not in a.engine.shapes
    done("标注工具端到端")


# ---------------------------------------------------------------- 放大镜
def test_magnifier() -> None:
    section("放大镜（画布坐标 + 贴边对位）")
    class Stub:
        class _map:
            @staticmethod
            def phys_of(pt):
                return pt
        _base = QImage(20, 20, QImage.Format_ARGB32)
    Stub._base.fill(QColor(255, 0, 0))
    mag = _Magnifier(Stub())
    mag.update_at(QPointF(19.0, 5.0))       # 贴右/上缘，取样区被裁
    assert mag._src == QRect(12, 0, 8, 13) and mag._src_off == QPoint(0, 2)
    assert mag._info.startswith("x:19 y:5")
    mag.update_at(QPointF(10.0, 10.0))      # 不贴边
    assert mag._src == QRect(3, 3, 15, 15) and mag._src_off == QPoint(0, 0)
    mag.grab()
    done("放大镜")


# ---------------------------------------------------------------- 快速保存
def test_quick_save() -> None:
    section("快速保存（默认目录、自动去重、并发安全）")
    import output
    img = QImage(10, 10, QImage.Format_ARGB32)
    img.fill(0xFFFF0000)
    tmp = tempfile.mkdtemp()
    paths: list[str] = []
    output.save_image_async(img, directory=tmp, on_done=paths.append)
    output.save_image_async(img, directory=tmp, on_done=paths.append)
    end = time.time() + 10
    while len(paths) < 2 and time.time() < end:
        time.sleep(0.05)
    assert len(os.listdir(tmp)) == 2, f"并发保存不应互相覆盖: {paths}"
    assert all(paths), f"保存回调失败: {paths}"
    done("快速保存")


# ---------------------------------------------------------------- 帮助窗口
def test_help() -> None:
    section("帮助窗口（分类导航 + 键位实时读配置）")
    hd = HelpDialog()
    for i in range(hd._list.count()):
        hd._list.setCurrentRow(i)
        html = hd._view.toHtml()
        assert len(html) > 500, f"第 {i} 页内容过短"
        assert "{_accel" not in html, f"第 {i} 页存在未替换占位符"
    accel = str(config.get("Hotkeys/capture"))
    hd._list.setCurrentRow(1)
    assert accel in hd._view.toHtml(), "键位未实时读配置"
    hd.close()
    done("帮助窗口")


# ---------------------------------------------------------------- 设置/关于
def test_prefs() -> None:
    section("设置对话框（恢复默认按钮 + 关于页）")
    import prefs
    dlg = prefs.PreferencesDialog(lambda: {}, lambda: None, lambda: None,
                                  lambda v: None)
    texts = [b.text() for b in dlg.findChildren(QPushButton)]
    assert texts.count("恢复默认") == 6, texts
    about = [l.text() for l in dlg._tabs.widget(6).findChildren(QLabel)]
    assert any(defaults_version() in t for t in about), "关于页应显示版本"
    assert any("配置与日志" in t for t in about), "关于页应显示数据路径"
    dlg.close()
    done("设置/关于页")


def defaults_version() -> str:
    import defaults
    return f"版本 {defaults.VERSION}"


# ---------------------------------------------------------------- 图标
def test_icons() -> None:
    section("工具栏图标（SVG 全部可渲染）")
    from PySide6.QtSvg import QSvgRenderer
    for n in list(toolbar._SVG_BODIES) + ["__fallback__"]:
        assert QSvgRenderer(toolbar._svg(n, "#fff").encode()).isValid(), n
    done("工具栏图标")


# ---------------------------------------------------------------- 托盘
def test_tray() -> None:
    section("托盘菜单（贴图组重建 + 分隔符）")
    tray = TrayController({})
    tray.set_groups([("A", 1, True), ("B", 0, False)])
    acts = [x.text() for x in tray.pin_menu.actions()]
    # 前 2 项固定（下一组 / 贴剪贴板图片），第 3 项是分隔符
    assert acts[0] == "下一组" and acts[1] == "贴剪贴板图片", acts[:2]
    assert acts[2] == "", f"组列表前应有分隔符: {acts}"
    assert "A（1）（当前）" in acts and "B（0）" in acts
    tray.set_groups([])
    acts2 = [x.text() for x in tray.pin_menu.actions()]
    assert acts2[2] == "" and "（暂无贴图）" in acts2
    done("托盘菜单")


def main() -> int:
    t0 = time.perf_counter()
    test_snap_pipeline()
    test_engine_undo()
    test_tools_e2e()
    test_magnifier()
    test_quick_save()
    test_help()
    test_prefs()
    test_icons()
    test_tray()
    print(f"\n全部 {len(_RESULTS)} 项回归通过 "
          f"({(time.perf_counter() - t0) * 1000:.0f}ms)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
