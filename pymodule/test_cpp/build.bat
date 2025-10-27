@echo off
REM Copyright Contributors to the OpenVDB Project
REM SPDX-License-Identifier: Apache-2.0

if not exist build mkdir build
cd build

REM Determine build type (default Release; use Debug if 'debug', '--debug' or '-d')
set BUILD_TYPE=Release
if "%1"=="debug" set BUILD_TYPE=Debug
if "%1"=="--debug" set BUILD_TYPE=Debug
if "%1"=="-d" set BUILD_TYPE=Debug

echo Building test in %BUILD_TYPE%...

cmake -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ..
if %errorlevel% neq 0 (
    echo Failure while building test >&2
    cd ..
    exit /b 1
)

cmake --build .
if %errorlevel% neq 0 (
    echo Failure while building test >&2
    cd ..
    exit /b 1
)

REM Return to original directory
cd ..

echo Build completed successfully
exit /b 0
