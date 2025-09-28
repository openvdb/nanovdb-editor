@echo off
setlocal

rem Copyright Contributors to the OpenVDB Project
rem SPDX-License-Identifier: Apache-2.0

rem Ensure we run relative to this script's directory
set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%"

set "BUILD_TYPE=Release"
if /I "%~1"=="debug" set "BUILD_TYPE=Debug"
if /I "%~1"=="--debug" set "BUILD_TYPE=Debug"
if /I "%~1"=="-d" set "BUILD_TYPE=Debug"

echo Building test in %BUILD_TYPE%...

set "BUILD_DIR=%SCRIPT_DIR%build"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
pushd "%BUILD_DIR%"

rem Configure
cmake -DCMAKE_BUILD_TYPE=%BUILD_TYPE% "%SCRIPT_DIR%"
if errorlevel 1 goto build_fail

rem Build (pass --config for multi-config generators like Visual Studio)
cmake --build . --config %BUILD_TYPE%
if errorlevel 1 goto build_fail

popd
popd
echo Build completed successfully
exit /b 0

:build_fail
echo Failure while building test 1>&2
popd
popd
exit /b 1


