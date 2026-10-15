@echo off
setlocal
set "CONFIG=Release"
set "RUN_TESTS=0"
if not defined JOBS set "JOBS=4"

:parse
if "%~1"=="" goto build
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
if /I "%~1"=="--test" (
    set "RUN_TESTS=1"
    shift
    goto parse
)
if /I "%~1"=="-h" goto help
if /I "%~1"=="--help" goto help
echo Unknown option: %~1 1>&2
echo Usage: build.cmd [Debug^|Release] [--test] 1>&2
exit /b 2

:help
echo Usage: build.cmd [Debug^|Release] [--test]
exit /b 0

:build
pushd "%~dp0"
if errorlevel 1 exit /b 1
cmake -S . -B build\win -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto fail
cmake --build build\win --config %CONFIG% --parallel %JOBS%
if errorlevel 1 goto fail
if "%RUN_TESTS%"=="1" (
    ctest --test-dir build\win -C %CONFIG% --output-on-failure
    if errorlevel 1 goto fail
)
echo Built: %CD%\build\win\%CONFIG%\xscarlet.exe
popd
exit /b 0

:fail
set "EXITCODE=%errorlevel%"
popd
exit /b %EXITCODE%
