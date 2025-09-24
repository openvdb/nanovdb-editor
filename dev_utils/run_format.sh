# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

#!/bin/bash

# Find all relevant source files, excluding build directories, and format them with clang-format
find . -type f \
    \( -name "*.c" \
    -o -name "*.cpp" \
    -o -name "*.h" \
    -o -name "*.hpp" \) \
    -not -path "*/build/*" \
    -print0 | xargs -0 clang-format -i --verbose

echo "Running black on Python files..."
python -m black ./pymodule ./pytests --verbose --target-version=py311 --line-length=120
