@echo off
chcp 65001 >nul
rem =====================================================================
rem One-time OpenCV build for the smart-erase feature.
rem   Source:  build\opencv\opencv.zip  (OpenCV 4.10.0, github release
rem            https://github.com/opencv/opencv/archive/refs/tags/4.10.0.zip)
rem            extracted to build\opencv\opencv-4.10.0\
rem   Output:  build\opencv\install\{include,lib}  (static core+imgproc)
rem Requires: MSVC (vcvars64), CMake 3.24 or newer, Ninja. Takes 10-20 minutes.
rem
rem ENCODING RULE FOR THIS FILE (same trap build.bat documents at its top):
rem every "rem" line must stay pure ASCII, anywhere in the file. This file is
rem UTF-8 *without* BOM, so cmd.exe loses track of its byte offsets across a
rem multi-byte line and can execute a later line as a command. Observed here
rem for real: a CJK comment made the "CMake >= 3.24" line below run as a
rem command, printing "'3.24' is not recognized" and dropping an empty file
rem named "3.24" into the repo root. chcp 65001 on line 2 does NOT make CJK
rem comments safe here. Keep CJK inside echo strings only.
rem
rem Everything lives under the git-ignored build\ dir. It is deliberately not
rem in third_party\, so the "all build output goes to build\" rule (AGENTS
rem rule 7) actually holds. Rerun this script any time to rebuild.
rem =====================================================================
setlocal
cd /d "%~dp0.."

set "VCVARS=D:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "ROOT=build\opencv"
set "SRC=%ROOT%\opencv-4.10.0"
set "BUILD=%ROOT%\build"
set "INSTALL=%ROOT%\install"

if not exist "%VCVARS%" (
    echo [error] vcvars64 not found: %VCVARS%
    goto :err
)
if not exist "%SRC%\CMakeLists.txt" (
    if not exist "%ROOT%\opencv.zip" (
        echo [error] %ROOT%\opencv.zip not found. Download
        echo        https://github.com/opencv/opencv/archive/refs/tags/4.10.0.zip
        echo        and put it next to this script's build\opencv\ folder.
        goto :err
    )
    echo [1/3] Extracting opencv.zip ...
    tar -xf "%ROOT%\opencv.zip" -C "%ROOT%"
)
if not exist "%SRC%\CMakeLists.txt" (
    echo [error] extraction failed
    goto :err
)
call "%VCVARS%" >nul
if errorlevel 1 goto :err

echo [2/3] Configuring (static, core+imgproc only) ...
rem All downloads (ADE/ffmpeg/ippicv) disabled: we are offline and only use core+imgproc.
cmake -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_INSTALL_PREFIX=%INSTALL% -DBUILD_SHARED_LIBS=OFF ^
  -DBUILD_WITH_STATIC_CRT=OFF ^
  -DBUILD_LIST=core,imgproc -DBUILD_opencv_gapi=OFF -DBUILD_opencv_videoio=OFF ^
  -DBUILD_opencv_imgcodecs=OFF -DBUILD_opencv_highgui=OFF -DBUILD_opencv_flann=OFF ^
  -DBUILD_opencv_python3=OFF -DBUILD_opencv_python2=OFF ^
  -DWITH_IPP=OFF -DWITH_OPENCL=OFF -DWITH_TBB=OFF ^
  -DWITH_FFMPEG=OFF -DWITH_ADE=OFF -DWITH_1394=OFF -DWITH_VA=OFF -DWITH_VA_INTEL=OFF ^
  -DWITH_MSMF=OFF -DWITH_DSHOW=OFF -DWITH_DIRECTX=OFF -DWITH_VTK=OFF ^
  -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOCS=OFF ^
  -DBUILD_opencv_apps=OFF -DOPENCV_ENABLE_NONFREE=OFF
if errorlevel 1 goto :err

echo [3/3] Building + installing ...
cmake --build "%BUILD%" --target install
if errorlevel 1 goto :err

echo Done: %INSTALL%
goto :end
:err
echo ===== OPENCV BUILD FAILED =====
exit /b 1
:end
if not defined ZPIN_NOPAUSE (
    echo Press any key to exit ...
    pause >nul
)
endlocal
