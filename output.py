"""保存输出 —— 文件名模板渲染 + 另存为对话框 + 格式记忆（写盘均在后台线程）。

模板令牌：$yyyy-MM-dd_HH-mm-ss$ 等，渲染为当前时间。
软件内所有手动保存都走 save_image_dialog_async（弹「另存为」让用户选位置），
不设静默的"默认保存地址"；Output/default_dir 仅作对话框的初始目录。
整屏 PNG 编码约 200ms，放主线程会在保存瞬间卡顿，因此统一走后台线程。
"""
from __future__ import annotations

import logging
import os
import re
import threading
from collections.abc import Callable
from datetime import datetime

from PySide6.QtGui import QImage
from PySide6.QtWidgets import QFileDialog, QWidget

import config

log = logging.getLogger("zpin.output")

_GROUP = re.compile(r"\$([A-Za-z\-_]*)\$")
_FIELD = re.compile(r"yyyy|MM|dd|HH|mm|ss")
_KNOWN_EXT = ("png", "jpg", "jpeg", "bmp")


def render_name(template: str, now: datetime | None = None) -> str:
    """把文件名模板里的 $yyyy-MM-dd_HH-mm-ss$ 等令牌渲染为时间字段。

    Args:
        template: 含 $...$ 日期令牌的文件名模板。
        now: 用于渲染的时间；缺省取当前时间。

    Returns:
        渲染后的文件名。
    """
    now = now or datetime.now()
    fields = {
        "yyyy": f"{now.year:04d}",
        "MM": f"{now.month:02d}",
        "dd": f"{now.day:02d}",
        "HH": f"{now.hour:02d}",
        "mm": f"{now.minute:02d}",
        "ss": f"{now.second:02d}",
    }
    return _GROUP.sub(
        lambda g: _FIELD.sub(lambda f: fields[f.group(0)], g.group(1)), template
    )


def default_dir() -> str:
    """返回「另存为」对话框的默认初始目录。

    Returns:
        配置的 Output/default_dir；未配置时回退桌面，再回退用户主目录。
    """
    return _resolve_dir("Output/default_dir")


def auto_save_dir() -> str:
    """返回自动保存的目标目录。

    Returns:
        配置的 Output/auto_save_dir；未配置时回退桌面，再回退用户主目录。
    """
    return _resolve_dir("Output/auto_save_dir")


def _resolve_dir(key: str) -> str:
    d = str(config.get(key) or "").strip()
    if d:
        return d  # 目录可能尚不存在，由 save_image 负责创建
    fallback = os.path.join(os.path.expanduser("~"), "Desktop")
    return fallback if os.path.isdir(fallback) else os.path.expanduser("~")


def _effective_ext(template: str, ext: str | None) -> str:
    """决定实际使用的保存扩展名。

    优先级：显式参数 > 记住的上次格式（勾选时）> 模板自带后缀 > png。
    模板里的 ".png" 只当占位，否则 remember_ext 对自动保存永远无效。

    Args:
        template: 文件名模板。
        ext: 显式指定的扩展名，可为 None。

    Returns:
        限定在已知格式内的扩展名（不含点）。
    """
    if not ext and config.get("Output/remember_ext"):
        ext = str(config.get("Output/last_ext"))
    if not ext:
        ext = os.path.splitext(template)[1].lstrip(".")
    ext = ext.lower().lstrip(".") or "png"
    return ext if ext in _KNOWN_EXT else "png"


def _write_async(img: QImage, path: str, ext: str,
                 on_done: Callable[[str], None] | None) -> None:
    """在工作线程里编码写盘（QImage.save 只读位数据，跨线程安全）。

    Args:
        img: 待保存的图像（保存前不再被改写）。
        path: 目标完整路径。
        ext: 不带点的扩展名，决定编码器。
        on_done: 完成回调，参数为成功时的路径、失败为空串；在子线程调用。
    """
    def _work() -> None:
        try:
            if ext in ("jpg", "jpeg"):
                ok = img.save(path, quality=int(config.get("Output/quality")))
            else:
                ok = img.save(path)
        except Exception:
            log.exception("保存异常：%s", path)
            ok = False
        if not ok:
            log.error("保存失败：%s", path)
        if on_done:
            on_done(path if ok else "")

    threading.Thread(target=_work, name="ZPinSave", daemon=True).start()


def save_image_async(img: QImage, ext: str | None = None, directory: str | None = None,
                     on_done: Callable[[str], None] | None = None) -> None:
    """按模板把图像异步保存到指定目录（自动保存场景用，不弹框）。

    Args:
        img: 待保存的图像。
        ext: 指定扩展名；缺省按配置与模板推断。
        directory: 目标目录；缺省用 default_dir()。
        on_done: 完成回调（子线程调用），成功传完整路径、失败传空串；可省略。
    """
    name = render_name(str(config.get("Output/name_template")))
    stem = os.path.splitext(name)[0]
    name_ext = "." + _effective_ext(name, ext)
    directory = directory or default_dir()
    try:
        os.makedirs(directory, exist_ok=True)
        path = os.path.join(directory, stem + name_ext)
        i = 1
        while os.path.exists(path):
            path = os.path.join(directory, f"{stem}_{i}{name_ext}")
            i += 1
    except OSError:
        log.exception("保存异常：%s", directory)
        if on_done:
            on_done("")
        return
    if config.get("Output/remember_ext"):
        config.set("Output/last_ext", name_ext.lstrip("."))
        config.sync()
    _write_async(img, path, name_ext.lstrip("."), on_done)


def save_image_dialog_async(img: QImage, parent: QWidget | None = None,
                            on_done: Callable[[str | None], None] | None = None) -> None:
    """弹出「另存为」确定路径后，在工作线程编码写盘。

    软件内所有手动保存入口都用它，不静默存到默认地址。

    Args:
        img: 待保存的图像。
        parent: 对话框父窗口，可为 None。
        on_done: 结果回调（子线程调用）：成功传完整路径、失败传空串、
            用户取消传 None；可省略。
    """
    ext = (str(config.get("Output/last_ext")) if config.get("Output/remember_ext")
           else "png")
    ext = ext.lower().lstrip(".") or "png"
    name = render_name(str(config.get("Output/name_template")))
    stem, _ = os.path.splitext(name)
    path, _sel = QFileDialog.getSaveFileName(
        parent, "另存为", os.path.join(default_dir(), f"{stem}.{ext}"),
        "PNG 图片 (*.png);;JPEG 图片 (*.jpg);;BMP 图片 (*.bmp)")
    if not path:
        if on_done:
            on_done(None)
        return
    ext2 = os.path.splitext(path)[1].lstrip(".").lower() or ext
    if config.get("Output/remember_ext"):
        config.set("Output/last_ext", ext2)
        config.sync()
    _write_async(img, path, ext2, on_done)
