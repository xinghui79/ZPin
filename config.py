"""配置层 —— QSettings(IniFormat) 读写 %LOCALAPPDATA%\\ZPin\\config.ini。

get/set 均以 defaults.py 的键与类型为准；启动时滚动备份一份 config.ini.bak。
"""
from __future__ import annotations

import logging
import os
import shutil

from PySide6.QtCore import QSettings

import defaults

log = logging.getLogger("zpin.config")

APP_NAME = "ZPin"

_settings: QSettings | None = None


def app_dir() -> str:
    """返回应用数据目录。

    Returns:
        %LOCALAPPDATA%\\ZPin 目录的绝对路径（无 LOCALAPPDATA 时回退主目录）。
    """
    base = os.environ.get("LOCALAPPDATA", os.path.expanduser("~"))
    return os.path.join(base, APP_NAME)


def config_path() -> str:
    """返回配置文件完整路径。

    Returns:
        config.ini 的绝对路径。
    """
    return os.path.join(app_dir(), "config.ini")


def init() -> None:
    """初始化全局 QSettings 并完成启动期的配置整理。

    建目录、滚动备份一份 config.ini.bak、把默认值落盘、执行版本迁移。
    必须在任何 get/set 之前调用一次。
    """
    global _settings
    os.makedirs(app_dir(), exist_ok=True)
    ini = config_path()
    _settings = QSettings(ini, QSettings.Format.IniFormat)
    # 备份必须在任何 setValue/sync 之前做，才能留下上一轮的原始配置；
    # auto_backup 走 get() 读用户已存的值，硬编码默认会让人关不掉备份
    if bool(get("General/auto_backup")) and os.path.isfile(ini):
        bak = ini + ".bak"
        try:
            if os.path.isfile(bak):
                os.remove(bak)
            shutil.copy2(ini, bak)
        except OSError:
            log.exception("配置备份失败")
    # 迁移必须在补默认值之前：先读旧版本号，否则新装的配置会被误判为"已迁移"
    stored_version = int(_settings.value("General/config_version", 0, type=int) or 0)
    # 把全部默认值落盘，config.ini 首次运行即存在且用户可见可改
    for key, value in defaults.DEFAULTS.items():
        if not _settings.contains(key):
            _settings.setValue(key, value)
    if stored_version < defaults.CONFIG_VERSION:
        _migrate(_settings)
    _settings.setValue("General/config_version", defaults.CONFIG_VERSION)
    _settings.sync()


def _migrate(s: QSettings) -> None:
    """把有问题的旧默认值启动时换成新默认（只动用户从没改过的项）。

    判据都是「值仍等于旧默认」，用户自定义过的一律不碰。

    Args:
        s: 已初始化的全局 QSettings。
    """
    for action, legacy in defaults.LEGACY_HOTKEYS.items():
        key = f"Hotkeys/{action}"
        if str(s.value(key, "", type=str)) == legacy:
            new = str(defaults.DEFAULTS[key])
            s.setValue(key, new)
            log.info("热键 %s：旧默认 %s 常被占用，改为 %s", action, legacy, new)
    for action in defaults.LEGACY_EMPTY_HOTKEYS:
        key = f"Hotkeys/{action}"
        if not str(s.value(key, "", type=str)).strip():
            new = str(defaults.DEFAULTS[key])
            s.setValue(key, new)
            log.info("热键 %s：补充默认键 %s", action, new)
    # v3：capture（框选）的 Ctrl+Shift+A 让位给新增的全屏截取，
    # 框选入口改名「自定义截图」并换到 Ctrl+Shift+X（只动仍是旧默认的值）
    cap_key = "Hotkeys/capture"
    if str(s.value(cap_key, "", type=str)) == defaults.V3_CAPTURE_OLD_DEFAULT:
        new = str(defaults.DEFAULTS[cap_key])
        s.setValue(cap_key, new)
        log.info("热键 capture：v3 起改为「自定义截图」，键位 %s", new)
    # v4：贴图描边默认从白色改为品牌蓝（仅迁移仍是旧默认 #FFFFFF 的值）
    if str(s.value("Pin/border_color", "", type=str)) == "#FFFFFF":
        s.setValue("Pin/border_color", str(defaults.DEFAULTS["Pin/border_color"]))
        log.info("贴图描边：v4 起默认改为蓝色高亮")
    # v5：JPEG 质量旧默认 -1 其实是「让 Qt 自己定」= 实测 75（整屏 PSNR 35dB），改成 95
    if int(s.value("Output/quality", 95, type=int) or 0) < 0:
        s.setValue("Output/quality", int(defaults.DEFAULTS["Output/quality"]))
        log.info("JPEG 质量：v5 起默认 95（旧的 -1 实际等于 75，截图文字会糊）")


def settings() -> QSettings:
    """返回全局 QSettings 实例。

    Returns:
        init() 创建的 QSettings。

    Raises:
        AssertionError: 尚未调用 config.init()。
    """
    assert _settings is not None, "先调用 config.init()"
    return _settings


def get(key: str) -> object:
    """按配置键读取当前值。

    Args:
        key: defaults.DEFAULTS 中定义的完整配置键。

    Returns:
        当前值；类型与该键的默认值一致，键不存在时返回默认值。
    """
    default = defaults.DEFAULTS[key]
    return settings().value(key, default, type=type(default))


def set(key: str, value: object) -> None:
    """写入单个配置键（不立即落盘，需另行 sync）。

    Args:
        key: defaults.DEFAULTS 中定义的完整配置键。
        value: 要写入的值，类型应与该键默认值一致。
    """
    settings().setValue(key, value)


def sync() -> None:
    """把未落盘的配置立即写回 config.ini。"""
    settings().sync()


def hotkey_accel(action: str) -> str:
    """读取指定动作的全局热键文本。

    Args:
        action: 动作名（不含 Hotkeys/ 前缀）。

    Returns:
        形如 "Ctrl+Shift+X" 的文本，空串表示未绑定。
    """
    return get(f"Hotkeys/{action}")
