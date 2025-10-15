@echo off
:: Copyright Contributors to the OpenVDB Project
:: SPDX-License-Identifier: Apache-2.0

:: Find all relevant source files (respects .gitignore) and format them with clang-format
echo Formatting C/C++ files with clang-format...

for /f "delims=" %%f in ('git ls-files "*.c" "*.cpp" "*.h" "*.hpp"') do (
    clang-format -i --verbose "%%f"
)

echo.
echo Running black on Python files...
python -m black ./pymodule ./pytests --verbose --target-version=py311 --line-length=120

echo.
echo Formatting complete!
