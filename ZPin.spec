# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=[],
    datas=[],
    hiddenimports=['prefs', 'prefs.dialog', 'prefs.pages', 'prefs.key_edit',
                   'controller', 'engine', 'shapes', 'toolbar', 'text_edit', 'uia'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        # 本应用只用 QtCore/QtGui/QtWidgets/QtSvg，排除 Essentials 里用不到的
        # Qt 模块以缩小体积（排除项即使不存在也无害）。
        'PySide6.QtNetwork', 'PySide6.QtOpenGL', 'PySide6.QtOpenGLWidgets',
        'PySide6.QtPrintSupport', 'PySide6.QtQml', 'PySide6.QtQuick',
        'PySide6.QtQuickWidgets', 'PySide6.QtTest', 'PySide6.QtXml',
        'PySide6.QtPdf', 'PySide6.QtPdfWidgets', 'PySide6.QtDesigner',
        'PySide6.QtUiTools', 'PySide6.QtSql', 'PySide6.QtConcurrent',
        'PySide6.QtHelp', 'PySide6.QtMultimedia', 'PySide6.QtBluetooth',
        'PySide6.QtSerialPort', 'PySide6.QtSensors', 'PySide6.QtPositioning',
        'PySide6.QtWebSockets', 'PySide6.QtWebChannel', 'PySide6.QtScxml',
        'PySide6.QtStateMachine', 'PySide6.QtTextToSpeech',
    ],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='ZPin',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=['vcruntime140.dll'],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=['assets/app_icon.ico'],
)
