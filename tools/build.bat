@echo off
REM Build WhackedRecomp. cmake --build alone is not enough: the MSVC compiler is
REM invoked by absolute path from CMakeCache, but windows.h / libs come from the
REM developer environment, so vcvars64 must be loaded first.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\18\Community"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [build] vcvars64.bat not found under "%VSDIR%" 1>&2
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
cmake --build build %*
exit /b %ERRORLEVEL%
