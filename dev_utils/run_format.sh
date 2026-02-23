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

echo "Formatting YAML files (removing trailing whitespace)..."
find .github -type f -name "*.yml" -print0 | xargs -0 -I {} sed -i 's/[[:space:]]*$//' {}

if command -v yamllint &> /dev/null; then
    echo "Running yamllint on YAML files..."
    yamllint -d relaxed .github/workflows/*.yml || true
else
    echo "yamllint not found, skipping YAML lint (install with: pip install yamllint)"
fi
