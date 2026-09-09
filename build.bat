@echo off
rem =====================================================================
rem ZPin 一键打包脚本  (双击即可；或命令行 build.bat [run] 打包后自动启动)
rem 依赖: 已创建虚拟环境 .venv 并安装 PySide6-Essentials + pyinstaller
rem =====================================================================
setlocal
rem 本文件为 UTF-8 编码：切到 65001 代码页，避免默认 GBK 控制台下中文乱码
chcp 65001 >nul
cd /d "%~dp0"

rem ---- 1. 检查构建环境 ----
if not exist ".venv\Scripts\pyinstaller.exe" (
    echo [错误] 找不到 .venv\Scripts\pyinstaller.exe
    echo        请先创建虚拟环境并安装依赖:
    echo          python -m venv .venv
    echo          .venv\Scripts\python -m pip install PySide6-Essentials pyinstaller
    goto :err
)

rem ---- 2. 同步图标资源 (app_icon.py 的 SVG -> assets\app_icon.ico) ----
if not exist "assets" mkdir assets
echo [1/4] 同步图标资源 ...
".venv\Scripts\python.exe" -c "import app_icon; app_icon.write_ico('assets/app_icon.ico')"
if errorlevel 1 (
    echo [警告] 图标生成失败，将使用已有 assets\app_icon.ico（如有）
)

rem ---- 3. 语法检查 ----
echo [2/4] 语法检查 ...
".venv\Scripts\python.exe" -m py_compile main.py config.py defaults.py capture.py overlay.py uia.py output.py winapi.py hotkey.py tray.py memtrim.py startup.py app_icon.py history.py pin_window.py pins.py engine.py shapes.py toolbar.py text_edit.py controller.py prefs\__init__.py prefs\key_edit.py prefs\pages.py prefs\dialog.py
if errorlevel 1 (
    echo [错误] 语法检查未通过，请修复后再打包
    goto :err
)

rem ---- 4. 结束旧实例并直接删除旧 exe (参照 TopSearch: 运行中则先退出再删) ----
echo [3/4] 结束旧实例，删除旧 exe ...
taskkill /F /IM ZPin.exe >nul 2>&1
rem 等系统释放文件锁 (taskkill 后文件可能短暂被占用)
timeout /t 1 /nobreak >nul 2>&1 || ping -n 2 127.0.0.1 >nul 2>&1
rem 直接删除旧产物: 运行中已退出，可安全删除 (PyInstaller --noconfirm 也会再清一次)
if exist "dist\ZPin.exe" del /F /Q "dist\ZPin.exe" >nul 2>&1
rem 清理历史遗留的 deprecated 旧物（沙箱 workaround 产物：一律 _deprecated_* 前缀）
for %%F in (_deprecated_*) do (
    if exist "%%F\" (
        rmdir /S /Q "%%F" >nul 2>&1
    ) else (
        if exist "%%F" del /F /Q "%%F" >nul 2>&1
    )
)
rem 顺手清掉 PyInstaller 临时目录（下次再生成）
if exist "build" rmdir /S /Q "build" >nul 2>&1
rem 清掉 dist 下我前几轮挪走的旧 exe 备份（仅匹配我产生的命名）
if exist "dist\_old_ZPin.exe" del /F /Q "dist\_old_ZPin.exe" >nul 2>&1

rem ---- 5. PyInstaller 打包 ----
echo [4/4] PyInstaller 打包中（约 1-2 分钟，请稍候）...
".venv\Scripts\pyinstaller.exe" --noconfirm ZPin.spec
if errorlevel 1 (
    echo [错误] PyInstaller 构建失败
    goto :err
)

rem ---- 完成 ----
".venv\Scripts\python.exe" -c "import os;print('===== 完成: dist/ZPin.exe (%%.1f MB) =====' %% (os.path.getsize('dist/ZPin.exe')/1e6))"
if /i "%~1"=="run" (
    echo 启动新 exe ...
    start "" "dist\ZPin.exe"
) else (
    echo 提示: 运行 build.bat run 可在打包后自动启动
)
goto :end

:err
echo.
echo ===== 打包失败 =====

:end
if not defined ZPIN_NOPAUSE (
    echo 按任意键退出 ...
    pause >nul
)
endlocal
