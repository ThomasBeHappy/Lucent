@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Build helper for Lucent (Windows).
rem Usage:
rem   build.bat [preset] [extra cmake --build args...]
rem Examples:
rem   build.bat
rem   build.bat debug
rem   build.bat release --target lucent_editor
rem   build.bat debug-ninja -- -j 8

cd /d "%~dp0"

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=debug"

if /I "%PRESET%"=="help" goto :help
if /I "%PRESET%"=="-h" goto :help
if /I "%PRESET%"=="--help" goto :help

if "%VCPKG_ROOT%"=="" (
  echo [ERROR] VCPKG_ROOT is not set.
  echo         Set it to your vcpkg folder, e.g.:
  echo         setx VCPKG_ROOT "C:\path\to\vcpkg"
  exit /b 1
)

rem Collect any extra args to forward to `cmake --build`
set "BUILD_ARGS="
shift
:collect_args
if "%~1"=="" goto :run
set "BUILD_ARGS=%BUILD_ARGS% %~1"
shift
goto :collect_args

:run
echo [Lucent] Configure preset: %PRESET%
cmake --preset "%PRESET%"
if errorlevel 1 exit /b %errorlevel%

echo [Lucent] Build preset: %PRESET%
cmake --build --preset "%PRESET%" %BUILD_ARGS%
exit /b %errorlevel%

:help
echo Usage:
echo   build.bat [preset] [extra cmake --build args...]
echo.
echo Presets (from CMakePresets.json):
echo   debug, debug-ninja, release, release-ninja, relwithdebinfo
echo.
echo Examples:
echo   build.bat
echo   build.bat release
echo   build.bat debug --target lucent_editor
echo   build.bat debug-ninja -- -j 8
exit /b 0


