@echo off
REM Copyright Contributors to the OpenVDB Project
REM SPDX-License-Identifier: Apache-2.0

setlocal

set PROJECT_NAME=nanovdb_editor
set PROJECT_DIR=%~dp0
set BUILD_DIR=%PROJECT_DIR%build
set CONFIG_FILE=config.ini
set SLANG_DEBUG_OUTPUT=OFF
set CLEAN_SHADERS=OFF
set USE_VCPKG=OFF
set GLFW_ON=ON

set clean_build=0
set release=0
set debug=0
set verbose=0
set python_only=0
set test_only=0

:parse_args
set args=%1
if "%args%"=="" goto end_parse_args
if "%args:~0,1%"=="-" (
    set "arg=%args:~1%"
    shift
    goto parse_arg
)

echo Invalid option: %args%
goto Usage

:parse_arg
if "%arg%"=="" goto parse_args
if "%arg:~0,1%"=="x" (
    set clean_build=1
    goto check_next_char
)
if "%arg:~0,1%"=="r" (
    set release=1
    goto check_next_char
)
if "%arg:~0,1%"=="d" (
    set debug=1
    goto check_next_char
)
if "%arg:~0,1%"=="v" (
    set verbose=1
    goto check_next_char
)
if "%arg:~0,1%"=="s" (
    set SLANG_DEBUG_OUTPUT=ON
    set CLEAN_SHADERS=ON
    goto check_next_char
)
if "%arg:~0,1%"=="p" (
    set python_only=1
    goto check_next_char
)
if "%arg:~0,1%"=="t" (
    set test_only=1
    goto check_next_char
)
if "%arg:~0,1%"=="f" (
    set GLFW_ON=OFF
    goto check_next_char
)
if "%arg:~0,1%"=="h" (
    goto Usage
)

echo Invalid option: %arg%
goto Usage

:check_next_char
set "arg=%arg:~1%"
goto parse_arg

:end_parse_args

::: set defaults
if %release%==0 (
    if %debug%==0 (
        set release=1
    )
)

::: set env vars from a config file
for /f "tokens=1,2 delims==" %%i in (%PROJECT_DIR%%CONFIG_FILE%) do (
  set %%i=%%j
)

if not defined MSVS_VERSION (
    echo MSVS_VERSION not set, using default CMake generator
    set MSVS_VERSION=
)

if %USE_VCPKG%==ON (
    if not defined VCPKG_ROOT (
        echo VCPKG_ROOT not set, please set path to vcpkg
        goto Error
    )
    :: Install dependencies using vcpkg.json
    if exist %VCPKG_ROOT%\vcpkg.exe (
        echo -- Installing dependencies with vcpkg...
        call %VCPKG_ROOT%\vcpkg.exe install --triplet x64-windows
        if errorlevel 1 (
            echo vcpkg install failed
            goto Error
        )
        set VCPKG_PREFIX_PATH=%VCPKG_ROOT%\installed\x64-windows
    ) else (
        echo vcpkg.exe not found in %VCPKG_ROOT%
        goto Error
    )
    set VCPKG_CMAKE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
)

goto Build

:Success
echo -- Build of %PROJECT_NAME% completed
exit /b 0

:Error
if not defined BUILD_ERROR set BUILD_ERROR=1
echo Failure while building %PROJECT_NAME%
exit /b %BUILD_ERROR%

:Usage
echo Usage: build [-x] [-r] [-d] [-v] [-s] [-p] [-t] [-f]
echo        -x  Perform a clean build
echo        -r  Build in release (default)
echo        -d  Build in debug
echo        -v  Enable CMake verbose output
echo        -s  Compile slang into ASM
echo        -p  Build only python module (requires Python 3.8+, auto-installs scikit-build ^& wheel)
echo        -t  Run only tests using ctest
echo        -f  Disable GLFW (headless build)
exit /b 1

:Build
if %python_only%==1 (
    call :BuildPython
    if errorlevel 1 goto Error
    goto Success
)

if %test_only%==1 (
    call :RunTests
    if errorlevel 1 goto Error
    goto Success
)

if %clean_build%==1 (
    echo -- Performing a clean build...
    if exist %BUILD_DIR% (
        rmdir /s /q %BUILD_DIR%
        echo -- Deleted %BUILD_DIR%
    )
    rmdir /s /q vcpkg_installed
    set CLEAN_SHADERS=ON
)

if %verbose%==1 (
    set CMAKE_VERBOSE=--verbose
 ) else (
    set CMAKE_VERBOSE=
 )

::: need to create config directories in advance
if not exist %BUILD_DIR% mkdir %BUILD_DIR%

if %release%==1 (
    call :CreateConfigDir Release
    if errorlevel 1 goto Error
)
if %debug%==1 (
    call :CreateConfigDir Debug
    if errorlevel 1 goto Error
)

echo -- Building %PROJECT_NAME%...

set SLANG_PROFILE_ARG=
if defined SLANG_PROFILE (
    set SLANG_PROFILE_ARG=-DNANOVDB_EDITOR_SLANG_PROFILE=%SLANG_PROFILE%
)

set CMAKE_ARGS=%PROJECT_DIR% -B %BUILD_DIR% ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_CMAKE% ^
    -DCMAKE_PREFIX_PATH=%VCPKG_PREFIX_PATH% ^
    -DNANOVDB_EDITOR_USE_VCPKG=%USE_VCPKG% ^
    -DNANOVDB_EDITOR_CLEAN_SHADERS=%CLEAN_SHADERS% ^
    -DNANOVDB_EDITOR_SLANG_DEBUG_OUTPUT=%SLANG_DEBUG_OUTPUT% ^
    -DNANOVDB_EDITOR_BUILD_TESTS=ON ^
    -DNANOVDB_EDITOR_USE_GLFW=%GLFW_ON% ^
    %SLANG_PROFILE_ARG%

if defined MSVS_VERSION (
    cmake -G %MSVS_VERSION% %CMAKE_ARGS%
) else (
    cmake %CMAKE_ARGS%
)

set BUILD_ERROR=%errorlevel%
if %BUILD_ERROR% neq 0 (
    echo CMake configuration failed
    goto Error
)

if %release%==1 (
    call :BuildConfig Release
    if errorlevel 1 goto Error
)
if %debug%==1 (
    call :BuildConfig Debug
    if errorlevel 1 goto Error
)

goto Success

:CreateConfigDir
set BUILD_DIR_CONFIG=%BUILD_DIR%\%1
if not exist %BUILD_DIR_CONFIG% mkdir %BUILD_DIR_CONFIG%
exit /b 0

:BuildConfig
set CONFIG=%1
echo -- Building config %CONFIG%...
cmake --build %BUILD_DIR% --config %CONFIG% %CMAKE_VERBOSE%
if errorlevel 1 (
    echo Failure while building %CONFIG%
    exit /b 1
)
echo Built config %CONFIG%
exit /b 0

:BuildPython
if %debug%==1 (
    echo -- Building python module in debug mode...
    set CMAKE_BUILD_TYPE=Debug
) else (
    echo -- Building python module in release mode...
    set CMAKE_BUILD_TYPE=Release
)

python --version >nul 2>&1
if errorlevel 1 (
    echo Error: Python not found. Please install Python 3.8 or later.
    goto BuildPythonError
)

echo    -- Checking Python dependencies...

echo    -- Installing scikit-build and wheel...
python -m pip install --upgrade pip
if errorlevel 1 (
    echo Error: Failed to upgrade pip
    goto BuildPythonError
)
python -m pip install --upgrade scikit-build wheel build
if errorlevel 1 (
    echo Error: Failed to install required packages
    goto BuildPythonError
)
python -c "import skbuild" >nul 2>&1
if errorlevel 1 (
    echo Error: Failed to install scikit-build. Please install manually:
    echo   python -m pip install scikit-build
    goto BuildPythonError
)
python -c "import wheel" >nul 2>&1
if errorlevel 1 (
    echo Error: Failed to install wheel. Please install manually:
    echo   python -m pip install wheel
    goto BuildPythonError
)
echo    -- Python dependencies verified successfully

cd pymodule

echo -- Cleaning up old python module builds...
if exist build rmdir /s /q build 2>nul
if exist dist rmdir /s /q dist 2>nul
if exist _skbuild rmdir /s /q _skbuild 2>nul
for /d %%i in (*.egg-info) do if exist "%%i" rmdir /s /q "%%i" 2>nul
for /d %%i in (__pycache__) do if exist "%%i" rmdir /s /q "%%i" 2>nul
del /q *.whl 2>nul

python -m build --wheel
if errorlevel 1 (
    echo Error: Failed to build wheel
    goto BuildPythonErrorInPymodule
)

echo -- Installing python module...
pip install --force-reinstall .
if errorlevel 1 goto BuildPythonErrorInPymodule

cd ..
exit /b 0

:BuildPythonErrorInPymodule
cd ..
:BuildPythonError
exit /b 1

:RunTests
if %debug%==1 (
    echo -- Running tests in debug mode...
    set CONFIG=Debug
) else (
    echo -- Running tests in release mode...
    set CONFIG=Release
)

if %verbose%==1 (
    set CTEST_VERBOSE=--verbose
 ) else (
    set CTEST_VERBOSE=
 )

echo -- Running tests with ctest...
ctest --test-dir build\gtests -C %CONFIG% --output-on-failure %CTEST_VERBOSE%
if errorlevel 1 (
    echo Error: Tests failed
    goto RunTestsError
)

echo -- Running Python tests with pytest...
python --version >nul 2>&1
if errorlevel 1 (
    echo Warning: Python not found, skipping pytest
) else (
    python -c "import nanovdb_editor; import parameterized" >nul 2>&1
    if errorlevel 1 (
        echo Warning: nanovdb_editor Python module or test dependencies not installed
        echo          Run 'build.bat -p' to build the Python module, then:
        echo          pip install parameterized pytest numpy
    ) else (
        pytest pytests -vvv
        if errorlevel 1 (
            echo Error: Python tests failed
            goto RunTestsError
        )
    )
)

echo -- Tests completed successfully
exit /b 0

:RunTestsError
exit /b 1
