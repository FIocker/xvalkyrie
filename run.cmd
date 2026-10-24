@echo off
setlocal
set "CONFIG=Release"
set "BUILD_ARGS="

:parse
if /I "%~1"=="Debug" (
    set "CONFIG=Debug"
    shift
    goto parse
)
if /I "%~1"=="Release" (
    set "CONFIG=Release"
    shift
    goto parse
)
if /I "%~1"=="--vulkan" (
    set "BUILD_ARGS=%BUILD_ARGS% --vulkan"
    shift
    goto parse
)
if /I "%~1"=="--reconfigure" (
    set "BUILD_ARGS=%BUILD_ARGS% --reconfigure"
    shift
    goto parse
)
if "%~1"=="--" shift

set "ARGS="
:collect
if "%~1"=="" goto run
set ARGS=%ARGS% %1
shift
goto collect

:run
call "%~dp0build.cmd" %CONFIG% --target xscarlet%BUILD_ARGS%
if errorlevel 1 exit /b %errorlevel%
"%~dp0build\win\%CONFIG%\xscarlet.exe"%ARGS%
exit /b %errorlevel%
