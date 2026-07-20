#!/usr/bin/env bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"
echo "Running formatters from ${REPO_ROOT}"

echo ""
echo "==> clang-format (C/C++)"
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

echo ""
echo "==> black (Python)"
python -m black ./pymodule ./pytests --verbose --target-version=py311 --line-length=120 --extend-exclude='_skbuild|dist|\.egg-info'

echo ""
echo "==> fix_whitespace (trailing spaces and tabs)"
python dev_utils/fix_whitespace.py .

echo ""
echo "All formatting complete."
