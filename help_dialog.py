"""帮助窗口 —— 左侧分类导航 + 右侧富文本内容。

键位、路径等信息均在切页时实时读取（改键/改配置后无需重启即同步）。
"""
from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import QUrl, Qt
from PySide6.QtGui import QCloseEvent, QDesktopServices
from PySide6.QtWidgets import QDialog, QHBoxLayout, QListWidget, QTextBrowser, QWidget

import config

_LIST_W = 136


def _accel(action: str) -> str:
    """当前配置里某动作的键位文本（未绑定时给出占位）。"""
    return config.hotkey_accel(action) or "（未绑定）"


def _table(rows: list[tuple[str, str]]) -> str:
    body = "".join(
        f"<tr><td width='46%'>{k}</td><td><b>{v}</b></td></tr>" for k, v in rows
    )
    return (f"<table width='100%' cellspacing='0' cellpadding='4'>{body}</table>")


def _page_quick() -> str:
    return f"""
    <h3>三步开始</h3>
    <ol>
      <li><b>截图</b>：按 <b>{_accel('capture_full')}</b> 一键抓取全屏；
          或按 <b>{_accel('capture')}</b> 进入框选——按住拖动自由框选，
          单击直接吸附窗口 / 界面元素。</li>
      <li><b>标注</b>：框好后用工具栏画图——矩形、文字、序号、马赛克等，
          画完还能点中它继续编辑。</li>
      <li><b>输出</b>：<b>Enter</b> 复制到剪贴板；工具栏「贴图」把截图钉在桌面；
          「保存」一键存到默认目录，「另存为」自行选位置。</li>
    </ol>
    <p>每次截图都会进入 <b>托盘 → 截图历史</b>，剪贴板被覆盖后也能随时重贴 /
    复制 / 另存。</p>
    """


def _page_hotkeys() -> str:
    return f"""
    <h3>全局快捷键</h3>
    {_table([
        ("全屏截图（一键直截）", _accel("capture_full")),
        ("框选截图", _accel("capture")),
        ("隐藏 / 显示全部贴图", _accel("toggle_pins")),
        ("切换到下一贴图组", _accel("switch_group")),
    ])}
    <p>在 <b>设置 → 快捷键</b> 里改键立即生效，托盘菜单与本页同步显示。</p>
    <h3>框选状态内</h3>
    {_table([
        ("Enter / 双击", "完成截图（默认复制）"),
        ("Ctrl+C / Ctrl+S", "复制 / 快速保存到默认目录"),
        ("Esc", "取消"),
        ("方向键", "微调选区（Shift 一次 10px）"),
        ("Ctrl + 方向键", "沿该侧扩选"),
        ("Tab", "未框选：切换检测层级；已框选：循环手柄"),
        ("Space", "自由框选 / 吸附模式切换"),
        ("Alt", "召唤 / 收起放大镜"),
        ("C", "复制光标处颜色（#RRGGBB）"),
        ("Ctrl+Z / Ctrl+Y", "撤销 / 重做标注"),
        ("Del", "删除选中的标注图形"),
    ])}
    """


def _page_capture() -> str:
    return """
    <h3>吸附与框选</h3>
    <ul>
      <li>悬停自动吸附：<b>优先界面元素</b>（按钮、文本块……），
          元素过大时回退整窗；<b>单击</b>先选中吸附目标，
          <b>Enter / 双击</b>才完成截取。</li>
      <li><b>Space</b> 随时切换自由框选 / 吸附；
          <b>Tab</b> 轮换检测层级（自动 → 仅窗口 → 仅元素）。</li>
      <li>拖拽时边角自动对齐屏幕与其它窗口的边、中线（红色参考线，
          可在 设置 → 截图 关闭）。</li>
    </ul>
    <h3>调整选区</h3>
    <ul>
      <li>拖 <b>8 个手柄</b>缩放（自动出现放大镜）；<b>Tab</b> 选中手柄后
          用方向键微调。</li>
      <li>鼠标移到<b>选框外</b>点 / 拖 = 自动扩选；框内拖动 = 整体移动
          （已画标注跟着走）。</li>
      <li>方向键微调（Shift 加速、Ctrl+方向 扩选）。</li>
      <li>选了标注工具后框内变画图；点工具栏「移动」恢复拖框。</li>
    </ul>
    <h3>放大镜与取色</h3>
    <ul>
      <li>调整边角时自动出现 10 倍放大镜（像素网格 + 坐标色值）；
          <b>Alt</b> 随时召唤 / 收起。</li>
      <li>按 <b>C</b> 随时复制光标处颜色，画面顶部弹提示。</li>
    </ul>
    """


def _page_annotate() -> str:
    return """
    <h3>工具一览</h3>
    <ul>
      <li>常用：矩形、椭圆、直线、箭头、画笔、文本、马赛克、移动；</li>
      <li>「⋮」里：荧光笔、橡皮擦、气泡标注、序号标记。</li>
    </ul>
    <h3>二次编辑</h3>
    <ul>
      <li>不选工具时<b>点中已画图形</b>即可选中：整体拖动、拖端点改形状、
          换颜色 / 粗细（Snipaste 同款，不占撤销步）、Del 删除。</li>
      <li>橡皮擦为<b>对象级</b>擦除：悬停时红框预览将被擦除的整个图形，
          一次涂抹算一步撤销。</li>
      <li>序号标记每次落下<b>自动递增</b>；撤销 / 擦掉中间某号后，
          新序号自动补位。</li>
      <li>文本 / 气泡字号跟随「粗细」档位；输入框里 Enter 换行、
          Ctrl+Enter 或点击外部完成、Esc 取消。</li>
      <li>移动选区时已画标注跟着框走。</li>
    </ul>
    """


def _page_pin() -> str:
    return f"""
    <h3>贴图操作</h3>
    <ul>
      <li>左键拖动 = 移动；<b>滚轮</b> = 以光标为中心缩放；
          <b>双击</b> = 隐藏（托盘可找回）。</li>
      <li><b>右键菜单</b>：不透明度、旋转 / 翻转 / 灰度、边框 / 阴影、
          贴图组、另存为。</li>
      <li>托盘 → 贴图管理 → <b>贴剪贴板图片</b>：复制任意图片直接上屏。</li>
    </ul>
    <h3>贴图窗快捷键</h3>
    {_table([
        ("Ctrl+C / Ctrl+S", "复制 / 另存为"),
        ("Ctrl+0 / Ctrl+R", "实际大小 / 重置全部变换"),
        ("Ctrl++ / Ctrl+-", "放大 / 缩小"),
        ("Ctrl+T", "旋转 90°"),
        ("Esc / Del", "隐藏 / 销毁"),
        (_accel("toggle_pins"), "隐藏 / 显示全部贴图"),
        (_accel("switch_group"), "切换贴图组"),
    ])}
    """


def _page_data() -> str:
    d = config.app_dir().replace("\\", "/")
    href = QUrl.fromLocalFile(config.app_dir()).toString()   # 正确编码空格/中文路径
    return f"""
    <h3>数据与配置</h3>
    <ul>
      <li>配置文件：<b>{d}/config.ini</b>
          （启动时自动滚动备份为 config.ini.bak），
          <a href="{href}">打开文件夹</a>。</li>
      <li>日志：同目录 <b>zpin.log</b>（启动计时、热键注册、未捕获异常）。</li>
      <li>截图历史条数 / 内存上限：设置 → 常规；空闲整理内存可在
          常规里关闭。</li>
      <li>开机自启：设置 → 常规（写 HKCU Run 键，不要求管理员）。</li>
      <li>贴图分组保存在配置的 Groups/names，随配置一起备份。</li>
    </ul>
    <p>框选工具栏「保存」一键存到默认目录（气泡回执路径）；「另存为」自行选位置；
    「每次截图自动保存一份」默认关闭。</p>
    """


_PAGES: list[tuple[str, Callable[[], str]]] = [
    ("快速上手", _page_quick),
    ("快捷键", _page_hotkeys),
    ("框选截图", _page_capture),
    ("标注工具", _page_annotate),
    ("贴图", _page_pin),
    ("数据与配置", _page_data),
]

_BODY_STYLE = (
    "h3 { margin: 4px 0 6px 0; }"
    "ul, ol { margin: 4px 0 10px 0; }"
    "li { margin: 3px 0; }"
    "td { padding: 3px 8px 3px 0; vertical-align: top; }"
    "p { margin: 6px 0; }"
    "a { color: #1E78D7; }"
)


class HelpDialog(QDialog):
    """帮助窗口：左列分类、右侧内容；每页在切换时实时构建（键位跟随配置）。"""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("ZPin 帮助")
        self.resize(760, 540)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(10, 10, 10, 10)
        lay.setSpacing(10)

        self._list = QListWidget(self)
        self._list.setFixedWidth(_LIST_W)
        self._list.setFocusPolicy(Qt.NoFocus)
        for name, _build in _PAGES:
            self._list.addItem(name)
        lay.addWidget(self._list)

        self._view = QTextBrowser(self)
        self._view.setOpenLinks(False)
        self._view.anchorClicked.connect(self._open_link)
        lay.addWidget(self._view, 1)

        self._list.currentRowChanged.connect(self._show)
        self._list.setCurrentRow(0)

    def _show(self, row: int) -> None:
        if 0 <= row < len(_PAGES):
            name, build = _PAGES[row]
            self._view.document().setDefaultStyleSheet(_BODY_STYLE)
            self._view.setHtml(f"<h2 style='margin:2px 0 10px 0;'>{name}</h2>"
                               + build())

    def _open_link(self, url: QUrl) -> None:
        """内容里的链接交给系统打开（如配置目录）。"""
        QDesktopServices.openUrl(url)

    def closeEvent(self, ev: QCloseEvent) -> None:
        super().closeEvent(ev)
