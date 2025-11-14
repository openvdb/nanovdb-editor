# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

#!/bin/bash

# Find all relevant source files, excluding build directories and gitignored paths, and format them with clang-format
find . -type f \
    \( -name "*.c" \
    -o -name "*.cpp" \
    -o -name "*.h" \
    -o -name "*.hpp" \) \
    -not -path "*/build/*" \
    -not -path "*/.cache/*" \
    -not -path "*/.cpm_cache/*" \
    -not -path "*/vcpkg_installed/*" \
    -not -path "*/_skbuild/*" \
    -not -path "*/dist/*" \
    -not -path "*/__pycache__/*" \
    -not -path "*/*.egg-info/*" \
    -print0 | xargs -0 clang-format -i --verbose

echo "Running black on Python files..."
python -m black ./pymodule ./pytests --verbose --target-version=py311 --line-length=120 --extend-exclude='_skbuild|dist|\.egg-info'
