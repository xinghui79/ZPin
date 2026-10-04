@echo off
rem ZPin dev environment: MSVC toolchain + Qt6 on PATH (cmake/ninja come from VS or pip).
rem Usage: cmd /c tools\vsdev.bat <any command>   (no argument = just set up the environment)
rem Comments here must stay pure ASCII: see the encoding note in build.bat.
call "D:\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "CMAKE_PREFIX_PATH=D:\Qt\6.11.3\msvc2022_64"
if not "%~1"=="" %*
