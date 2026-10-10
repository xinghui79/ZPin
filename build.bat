@echo off
chcp 65001 >nul
rem =====================================================================
rem ZPin native packaging script (C++/Qt6 + Rust core). Double-click to run.
rem   build.bat        build and stage into dist\ZPin\
rem   build.bat run    build, then launch once
rem Requires: Qt 6.11.3 (msvc2022_64), Visual Studio 18 Community, Rust toolchain.
rem Output is a FOLDER (exe + Qt DLLs + plugins), not a single file.
rem
rem NOTE on encoding (this file is UTF-8 *without* BOM):
rem       chcp 65001 must stay on line 2, BEFORE any line containing CJK.
rem       RULE: CJK is allowed ONLY inside `echo` strings. Every rem/comment
rem       line must stay pure ASCII, anywhere in the file.
rem       Why: cmd.exe reads a .bat with the code page active at that moment and
rem       loses track of its byte offset across multi-byte lines, so a CJK
rem       comment can get cut mid-line and the remainder executed. Observed for
rem       real: a Chinese rem line above the translation copy printed
rem         "'zh_CN.qm' is not recognized as an internal or external command"
rem       mid-run while the script still exited 0 -- a silently half-executed
rem       packaging step. A CJK comment placed above chcp breaks the same way
rem       before the build even starts.
rem       Do NOT add a UTF-8 BOM as a workaround: the BOM leaks into the first
rem       token, "@echo off" becomes "<BOM>@echo off", and every later line gets
rem       echoed. Tried and reverted.
rem       => keep line 2 where it is; keep new CJK below it and inside echo only.
rem =====================================================================
setlocal
cd /d "%~dp0"

set "VCVARS=D:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "QTDIR=D:\Qt\6.11.3\msvc2022_64"
rem Single build root (CMake + cargo outputs); the cmake\ dir holds CMake scripts.
set "BUILD=build"
set "OUT=dist\ZPin"

if not exist "%VCVARS%" (
    echo [错误] 找不到 MSVC 环境脚本：%VCVARS%
    goto :err
)
if not exist "%QTDIR%\bin\Qt6Core.dll" (
    echo [错误] 找不到 Qt：%QTDIR%
    echo        改一下本文件里的 QTDIR 指向你的 Qt 6.11 msvc2022_64 目录
    goto :err
)
where cargo >nul 2>&1
if errorlevel 1 (
    echo [错误] 找不到 cargo：Rust 核心是必需的，请安装 Rust 工具链后重试
    goto :err
)

rem Refuse to start while ZPin is running. The running exe AND the Qt/CRT DLLs it
rem has mapped are locked, so the "rmdir /S /Q dist\ZPin" below only manages to
rem delete the unlocked half (models, plugins, translations) and the exe copy then
rem fails -- leaving a broken, half-empty package on disk. Observed for real.
rem Absolute path: when PATH puts Git Bash / MSYS ahead of System32, bare
rem "find" resolves to GNU find, which chokes on /I -- the guard then silently
rem passes and packaging proceeds onto locked files. Observed for real.
tasklist /FI "IMAGENAME eq ZPin.exe" 2>nul | %SystemRoot%\System32\find.exe /I "ZPin.exe" >nul
if not errorlevel 1 (
    echo [错误] ZPin 正在运行，请先在托盘里退出再打包
    goto :err
)

echo [1/4] 进入 MSVC 环境并配置构建 ...
call "%VCVARS%" >nul
if errorlevel 1 goto :err

cmake -S . -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QTDIR%
if errorlevel 1 goto :err

echo [2/4] 编译 Rust 核心 + C++ 界面 ...
cmake --build "%BUILD%"
if errorlevel 1 goto :err

echo [3/4] 拷贝 exe 并用 windeployqt 收依赖 ...
rem Only wipe this script's own output dir; leave anything else under dist\ alone.
rem (A running ZPin does NOT block this: Windows deletes a running exe as
rem "pending delete", so the old copy unlinks and the new one lands beside it.)
if exist "%OUT%" rmdir /S /Q "%OUT%"
mkdir "%OUT%"
copy /Y "%BUILD%\zpin.exe" "%OUT%\ZPin.exe" >nul
if errorlevel 1 goto :err
rem OCR models (PP-OCRv6 small: det + rec + dict) plus the table layout model
rem (picodet_layout_1x_table, 7.2MB) ship in models\ next to
rem the exe; the app resolves them from its own path.
xcopy /Y /I /Q "%~dp0assets\models" "%OUT%\models\" >nul
if errorlevel 1 goto :err
rem Qt's own dialogs (color picker, Save-As, message-box buttons) need Qt's zh_CN
rem translation or they show up in English ("Custom colors" / OK / Cancel).
rem windeployqt without --no-translations copies ALL 31 languages (+5.6MB) and
rem RENAMES qtbase_zh_CN.qm to qt_zh_CN.qm, so we do it by hand instead: keep
rem the name the loader expects and ship only the language we need (144KB).
"%QTDIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-compiler-runtime --no-opengl-sw "%OUT%\ZPin.exe"
if errorlevel 1 goto :err
rem The MSVC runtime: ZPin.exe imports VCRUNTIME140.dll / VCRUNTIME140_1.dll /
rem MSVCP140.dll / MSVCP140_1.dll, and windeployqt --compiler-runtime only drops
rem the 18.7MB vc_redist INSTALLER there -- the user would still have to run it,
rem which kills the "copy the folder and go" promise. Copy the loose CRT DLLs
rem (~1.8MB) instead. VCToolsRedistDir is set by vcvars64.bat above; the toolset
rem folder name changes between VS versions (VC143/VC145/...), so glob it.
for /d %%D in ("%VCToolsRedistDir%\x64\Microsoft.VC*.CRT") do xcopy /Y /Q "%%D\*.dll" "%OUT%\" >nul
if errorlevel 1 goto :err
mkdir "%OUT%\translations" >nul 2>&1
rem Ship the repo's PATCHED translation: upstream Qt marks Blue's mnemonic on 'u'
rem so the color dialog reads "Blue(U)"; ours is corrected to "Blue(B)".
rem Source: Qt's qtbase_zh_CN.qm repaired with lconvert/lrelease, stored in
rem assets\translations (keep the file name the loader expects).
copy /Y "%~dp0assets\translations\qtbase_zh_CN.qm" "%OUT%\translations\" >nul
if errorlevel 1 goto :err

echo [4/4] 完成
for %%F in ("%OUT%\ZPin.exe") do echo       ZPin.exe  %%~zF 字节
echo       依赖与插件已备齐：%OUT%\
echo       整个文件夹可以拷走直接用（无需安装）。
if /i "%~1"=="zip" goto :pack
if /i "%~1"=="run" (
    echo 启动 ZPin ...
    start "" "%OUT%\ZPin.exe"
)
goto :end

:pack
rem ---- release zip: contents sit flat at the zip root so auto-update can
rem extract straight over an install ----
rem Single source of the version number: VERSION in cpp\sys\defaults.hpp
for /f "tokens=2 delims==" %%A in ('findstr /C:"const char* VERSION" cpp\sys\defaults.hpp') do set "VER=%%A"
for /f "tokens=1 delims=;" %%A in ("%VER%") do set "VER=%%A"
set "VER=%VER: =%"
rem defaults.hpp holds a quoted literal ("1.3.0"), so strip the quotes before
rem using it as a file name
set VER=%VER:"=%
if "%VER%"=="" (
    echo [错误] 没从 cpp\sys\defaults.hpp 读到 VERSION
    goto :err
)
pushd "%OUT%"
rem Absolute path: bare "tar" resolves to GNU tar when Git Bash / MSYS is ahead
rem in PATH -- GNU tar cannot write zip at all, it silently writes a tar archive
rem named .zip. bsdtar's zip writer also defaults to STORE (no compression),
rem hence the deflate option.
%SystemRoot%\System32\tar.exe --options zip:compression=deflate -a -c -f "..\ZPin_%VER%.zip" . || goto :err
popd
echo       发布包：dist\ZPin_%VER%.zip（发 GitHub Release 时作为资产上传；
echo       自动更新按 tag 比版本、取首个 .zip 下载换装）
goto :end

:err
echo.
echo ===== PACKAGING FAILED =====
rem Propagate failure: endlocal resets errorlevel, so without an explicit
rem exit code here a failed build reports success to the caller. This bit us
rem for real: `copy /Y dist\ZPin\ZPin.exe` failed with "Access is denied"
rem (a running ZPin was holding the exe) yet the script still exited 0, and
rem the caller happily used a half-written dist\.
exit /b 1

:end
if not defined ZPIN_NOPAUSE (
    echo Press any key to exit ...
    pause >nul
)
endlocal
