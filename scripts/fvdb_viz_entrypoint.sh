#!/bin/sh
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0
#
# Container entrypoint for fvdb viz integration tests.
# Sets up the lavapipe software Vulkan driver, then runs pytest.
#
# Required env: FVDB_VIZ_PYTEST_EXPR (pytest -k selector)

set -e

LVP_ICD=""
for candidate in /usr/share/vulkan/icd.d/lvp_icd*.json; do
  if [ -f "$candidate" ]; then LVP_ICD="$candidate"; break; fi
done

if [ -z "$LVP_ICD" ]; then
  echo "ERROR: No lavapipe ICD found under /usr/share/vulkan/icd.d" >&2
  ls -la /usr/share/vulkan/icd.d || true
  exit 1
fi

export VK_ICD_FILENAMES="$LVP_ICD"
export VK_DRIVER_FILES="$LVP_ICD"
export LP_NUM_THREADS=1
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe

echo "Using lavapipe ICD: $LVP_ICD"
ls -la /usr/share/vulkan/icd.d
if command -v vulkaninfo >/dev/null 2>&1; then
  vulkaninfo --summary || true
else
  echo "WARNING: vulkaninfo is not installed in the fvdb test image"
fi

if ! command -v pytest >/dev/null 2>&1; then
  echo "ERROR: pytest is not installed in the image" >&2
  exit 1
fi

python3 -c "
import importlib
mod = importlib.import_module('nanovdb_editor')
version = getattr(mod, '__version__', 'unknown')
print(f'nanovdb_editor version: {version}')
"

EXPR="${FVDB_VIZ_PYTEST_EXPR:?FVDB_VIZ_PYTEST_EXPR is required}"
set +e
pytest pytests/test_fvdb_viz_integration.py -k "$EXPR" -vv -s --maxfail=1 --full-trace -rA
PYTEST_EXIT=$?
set -e
echo "Pytest exit code: $PYTEST_EXIT"
exit $PYTEST_EXIT
