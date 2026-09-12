"""ZPin 默认配置 —— 所有键与默认值的唯一事实来源。

首选项界面、恢复默认、config.py 的类型转换都以这里的键为准。
热键值为显示文本（如 "Ctrl+F1"），空串表示未绑定。
"""
from __future__ import annotations

import os


def default_desktop() -> str:
    """返回当前用户的桌面目录。

    Returns:
        桌面目录绝对路径；优先 OneDrive 重定向的 Desktop，否则用主目录下的
        Desktop（即使不存在也返回该路径）。
    """
    one_drive = os.environ.get("OneDrive")
    if one_drive:
        p = os.path.join(one_drive, "Desktop")
        if os.path.isdir(p):
            return p
    return os.path.join(os.path.expanduser("~"), "Desktop")


DEFAULTS: dict[str, object] = {
    # 常规
    "General/autostart": False,
    "General/auto_backup": True,
    "General/keep_responsive": True,
    "General/log_level": "普通",          # 普通=INFO / 详细=DEBUG
    "General/history_limit": 20,          # 截图历史保留条数
    "General/history_max_mb": 96,         # 截图历史占用上限（MB），超出丢最旧
    "General/config_version": 5,          # 配置迁移用，别手改
    # 界面
    "Interface/font": "Microsoft YaHei UI,9",
    "Interface/theme_color": "#1E78D7",
    # 截图 · 显示
    "Capture/border_width": 3,
    "Capture/mask_color": "#6E000000",    # Qt 记法 #AARRGGBB：rgba(0,0,0,110)
    "Capture/show_anchors": True,
    "Capture/anchor_stroke_color": "#FFFFFF",
    "Capture/show_crosshair": False,
    "Capture/snap_elements": True,        # hover 优先吸附界面元素（按钮/文字块…），关掉只吸整窗
    "Capture/snap_guides": True,          # 拖拽/缩放时吸到屏幕与窗口的边、中线
    "Capture/disable_guides": False,
    "Capture/show_hints": True,
    # 贴图 · 显示
    "Pin/shadow": True,
    "Pin/default_opacity": 100,
    "Pin/max_window_size": 12000,         # 贴图窗口单边上限（逻辑 px），超出自动缩
    "Pin/border_color": "#1E78D7",        # 贴图描边：默认蓝色高亮（发光感）
    "Pin/border_glow": True,              # 描边外再叠一层淡色光晕
    # 输出 · 文件
    "Output/quality": 95,                 # JPEG 保存质量（PNG/BMP 无损不受影响）；实测 75→95 PSNR 35→46dB
    "Output/name_template": "ZPin_$yyyy-MM-dd_HH-mm-ss$.png",
    "Output/remember_ext": True,
    "Output/last_ext": "png",
    "Output/default_dir": default_desktop(),
    "Output/auto_save": False,            # 每次截图自动存一份
    "Output/auto_save_dir": default_desktop(),
    # 控制 · 全局快捷键
    # 默认值避开 F1/F3/Alt+L 等常被系统/驱动/其它工具占用的组合
    "Hotkeys/disabled": False,
    # v3：capture=框选入口（改名「自定义截图」，键位让给全屏截取）
    "Hotkeys/capture": "Ctrl+Shift+X",
    "Hotkeys/capture_full": "Ctrl+Shift+A",   # 截图：一键截取全屏
    "Hotkeys/toggle_pins": "Ctrl+Shift+H",
    "Hotkeys/switch_group": "Ctrl+Shift+G",
    # 贴图组
    "Groups/names": "贴图",
}

VERSION = "1.2.0"     # 软件版本号（关于页/README 的唯一事实来源）
CONFIG_VERSION = 5

# DEFAULTS 里少数键不是「设置」而是内部状态或用户数据，按页恢复默认时必须跳过：
# config_version 是迁移标记（清掉会重跑迁移）、last_ext 记住上次格式、names 是用户建的贴图组名。
NON_RESETTABLE_KEYS: frozenset[str] = frozenset({
    "General/config_version",
    "Output/last_ext",
    "Groups/names",
})

# 旧版默认键（实测在多数机器上被系统/驱动/其它工具占用，注册必失败）：
# 若用户从未改过（值仍等于旧默认），启动时自动换成新默认。
LEGACY_HOTKEYS: dict[str, str] = {
    "toggle_pins": "Shift+F3",
    "switch_group": "Ctrl+F3",
}

# v3：capture（框选）旧默认 Ctrl+Shift+A 让给新增的全屏截取 capture_full，
# 框选入口改名为「自定义截图」并换到 Ctrl+Shift+X（仅当值仍是旧默认才迁移）。
V3_CAPTURE_OLD_DEFAULT = "Ctrl+Shift+A"

# 旧版里这三个动作根本没给默认键（空串），v2 才补上。
LEGACY_EMPTY_HOTKEYS: tuple[str, ...] = (
        "capture",
)


def font_tuple(value: str) -> tuple[str, int]:
    """把 Interface/font 的文本解析为 (字体名, 字号)。

    Args:
        value: 逗号分隔的字体配置文本（如 "Microsoft YaHei UI,9"）。

    Returns:
        (字体名, 字号) 二元组；字号不是整数时回退整组默认字体。
    """
    name, _, size = value.rpartition(",")
    try:
        return (name or "Microsoft YaHei UI").strip(), int(size)
    except ValueError:
        return "Microsoft YaHei UI", 9
