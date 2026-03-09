#!/usr/bin/env bash
# Local cibuildwheel test for Ubuntu x86_64 manylinux_2_34 wheel.
# Mirrors the CI build in .github/workflows/build-wheels.yml.
#
# Prerequisites:
#   - Docker running
#   - Python 3.x with pip
#   - pip install "cibuildwheel==2.21.3"
#
# Usage:
#   ./dev_utils/test_wheel_local.sh
#
# Run from the repo root.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"

# ── Check cibuildwheel is available ───────────────────────────────────────────
if ! command -v cibuildwheel &>/dev/null; then
  echo "cibuildwheel not found. Install it with: pip install 'cibuildwheel==2.21.3'"
  exit 1
fi
echo "Using $(cibuildwheel --help 2>&1 | head -1 || true)cibuildwheel $(python -c 'import cibuildwheel; print(cibuildwheel.__version__)')"

# ── Repair wheel command (mirrors CI) ─────────────────────────────────────────
# Creates an isolated venv with pinned auditwheel+pyelftools to avoid the
# 'Section' object has no attribute 'iter_versions' error from pyelftools>=0.32.
# Applies monkey-patches to handle unparsable ELF sections gracefully.
REPAIR_CMD='
set -e
# Prefer an isolated auditwheel toolchain to avoid mismatches with
# preinstalled pipx/system packages. Fall back to system python.
AW_PYTHON=""
AW_MODE=""
AW_VENV="/tmp/nanovdb-auditwheel-venv"
rm -rf "${AW_VENV}"
if python -m venv "${AW_VENV}" >/dev/null 2>&1; then
  if "${AW_VENV}/bin/python" -m pip install --disable-pip-version-check --no-cache-dir --upgrade pip >/dev/null 2>&1 && \
     "${AW_VENV}/bin/python" -m pip install --disable-pip-version-check --no-cache-dir "auditwheel==6.6.0" "pyelftools==0.31" >/dev/null 2>&1; then
    AW_PYTHON="${AW_VENV}/bin/python"
    AW_MODE="python-module"
  else
    echo "Warning: failed to prepare isolated auditwheel environment, falling back to system python"
  fi
else
  echo "Warning: python -m venv unavailable, falling back to system python"
fi
if [ -z "${AW_MODE}" ] && python -m auditwheel --version >/dev/null 2>&1; then
  AW_PYTHON="python"
  AW_MODE="python-module"
fi
if [ -z "${AW_MODE}" ] && command -v auditwheel >/dev/null 2>&1; then
  AW_MODE="binary"
fi
if [ -z "${AW_MODE}" ]; then
  echo "ERROR: no usable auditwheel runner found (venv/python module/binary)" >&2
  python -m pip --version || true
  python --version || true
  exit 1
fi
run_auditwheel() {
  if [ "${AW_MODE}" = "python-module" ]; then
    "${AW_PYTHON}" - "$@" <<'"'"'PY'"'"'
import sys

from auditwheel import main as auditwheel_main
import auditwheel.elfutils as auditwheel_elfutils
import auditwheel.wheel_abi as auditwheel_wheel_abi

_orig_elf_find_versioned_symbols = auditwheel_elfutils.elf_find_versioned_symbols
_orig_elf_is_python_extension = auditwheel_elfutils.elf_is_python_extension
_orig_elf_find_ucs2_symbols = auditwheel_elfutils.elf_find_ucs2_symbols
_orig_elf_references_pyfpe_jbuf = auditwheel_elfutils.elf_references_pyfpe_jbuf

def _patched_elf_find_versioned_symbols(elf):
    section = elf.get_section_by_name(".gnu.version_r")
    if section is not None and not hasattr(section, "iter_versions"):
        sh_type = None
        try:
            sh_type = section["sh_type"]
        except Exception:
            sh_type = "<unknown>"
        print(
            f"Warning: skipping unparsable .gnu.version_r section (type={sh_type})",
            file=sys.stderr,
        )
        return iter(())
    return _orig_elf_find_versioned_symbols(elf)

def _patched_elf_is_python_extension(fn, elf):
    section = elf.get_section_by_name(".dynsym")
    if section is not None and not hasattr(section, "iter_symbols"):
        sh_type = None
        try:
            sh_type = section["sh_type"]
        except Exception:
            sh_type = "<unknown>"
        print(
            f"Warning: skipping unparsable .dynsym section in {fn} (type={sh_type})",
            file=sys.stderr,
        )
        return False, None
    return _orig_elf_is_python_extension(fn, elf)

def _patched_elf_find_ucs2_symbols(elf):
    section = elf.get_section_by_name(".dynsym")
    if section is not None and not hasattr(section, "iter_symbols"):
        return iter(())
    return _orig_elf_find_ucs2_symbols(elf)

def _patched_elf_references_pyfpe_jbuf(elf):
    section = elf.get_section_by_name(".dynsym")
    if section is not None and not hasattr(section, "iter_symbols"):
        return False
    return _orig_elf_references_pyfpe_jbuf(elf)

auditwheel_elfutils.elf_find_versioned_symbols = _patched_elf_find_versioned_symbols
auditwheel_elfutils.elf_is_python_extension = _patched_elf_is_python_extension
auditwheel_elfutils.elf_find_ucs2_symbols = _patched_elf_find_ucs2_symbols
auditwheel_elfutils.elf_references_pyfpe_jbuf = _patched_elf_references_pyfpe_jbuf
auditwheel_wheel_abi.elf_find_versioned_symbols = _patched_elf_find_versioned_symbols
auditwheel_wheel_abi.elf_is_python_extension = _patched_elf_is_python_extension
auditwheel_wheel_abi.elf_find_ucs2_symbols = _patched_elf_find_ucs2_symbols
auditwheel_wheel_abi.elf_references_pyfpe_jbuf = _patched_elf_references_pyfpe_jbuf

sys.argv = ["auditwheel", *sys.argv[1:]]
raise SystemExit(auditwheel_main.main())
PY
  else
    auditwheel "$@"
  fi
}
echo "Using auditwheel mode: ${AW_MODE}${AW_PYTHON:+ (${AW_PYTHON})}"
run_auditwheel --version
SLANG_LIB_DIR="${NANOVDB_SLANG_IMAGE_LIB_DIR:-}"
if [ -n "${SLANG_LIB_DIR}" ] && [ ! -d "${SLANG_LIB_DIR}" ]; then
  SLANG_LIB_DIR=""
fi
if [ -z "${SLANG_LIB_DIR}" ]; then
  SLANG_LIB_DIR="$(ls -d /tmp/cpm-cache/slang/*/lib 2>/dev/null | head -n1 || true)"
fi
SPIRV_LIB_PATH="$(python - <<'"'"'PY'"'"'
import os
targets = ("libSPIRV.so.15", "libSPIRV.so")
roots = ("/project", "/tmp/cpm-cache", "/tmp")
for root in roots:
    if not os.path.isdir(root):
        continue
    for dirpath, _, filenames in os.walk(root):
        for t in targets:
            if t in filenames:
                print(os.path.join(dirpath, t))
                raise SystemExit(0)
print("")
PY
)"
if [ -n "${SLANG_LIB_DIR}" ]; then
  export LD_LIBRARY_PATH="${SLANG_LIB_DIR}:${LD_LIBRARY_PATH:-}"
  echo "Using Slang runtime libs from ${SLANG_LIB_DIR}"
fi
if [ -n "${SPIRV_LIB_PATH}" ]; then
  SPIRV_LIB_DIR="$(dirname "${SPIRV_LIB_PATH}")"
  export LD_LIBRARY_PATH="${SPIRV_LIB_DIR}:${LD_LIBRARY_PATH:-}"
  echo "Using SPIRV runtime libs from ${SPIRV_LIB_DIR}"
  if [ ! -e "${SPIRV_LIB_DIR}/libSPIRV.so.15" ]; then
    SPIRV_COMPAT_DIR="/tmp/nanovdb-spirv-compat"
    mkdir -p "${SPIRV_COMPAT_DIR}"
    SPIRV_SRC_LIB=""
    if [ -e "${SPIRV_LIB_DIR}/libSPIRV.so" ]; then
      SPIRV_SRC_LIB="${SPIRV_LIB_DIR}/libSPIRV.so"
    else
      SPIRV_SRC_LIB="$(printf '"'"'%s\n'"'"' "${SPIRV_LIB_DIR}"/libSPIRV.so.15.* 2>/dev/null | head -n1 || true)"
    fi
    if [ -n "${SPIRV_SRC_LIB}" ] && [ -e "${SPIRV_SRC_LIB}" ]; then
      cp -f "${SPIRV_SRC_LIB}" "${SPIRV_COMPAT_DIR}/libSPIRV.so.15"
      export LD_LIBRARY_PATH="${SPIRV_COMPAT_DIR}:${LD_LIBRARY_PATH:-}"
      echo "Created compat copy: ${SPIRV_COMPAT_DIR}/libSPIRV.so.15 from ${SPIRV_SRC_LIB}"
    else
      echo "Warning: could not create SPIRV compat libSPIRV.so.15"
    fi
  fi
else
  echo "Warning: could not locate libSPIRV.so* before auditwheel"
fi
echo "=== auditwheel show ==="
run_auditwheel show {wheel} || true
run_auditwheel repair -w {dest_dir} {wheel} --lib-sdir .libs --plat manylinux_2_34_x86_64
'

# ── Run cibuildwheel ───────────────────────────────────────────────────────────
# ── Clean stale build cache ────────────────────────────────────────────────────
# cibuildwheel copies the full source tree (including gitignored build/) into
# the container. A stale CMakeCache.txt referencing the host path causes CMake
# to refuse to reconfigure. Remove it before the run.
if [ -d "${REPO_ROOT}/pymodule/build" ]; then
  echo "Removing stale pymodule/build/ to avoid CMakeCache path mismatch in container..."
  rm -rf "${REPO_ROOT}/pymodule/build"
fi

echo "Building manylinux_2_34 x86_64 wheel via Docker ..."

CIBW_BUILD="cp311-manylinux_x86_64" \
CIBW_MANYLINUX_X86_64_IMAGE="quay.io/pypa/manylinux_2_34_x86_64:latest" \
CIBW_BUILD_VERBOSITY=1 \
CIBW_CONTAINER_ENGINE="docker;create_args: --volume ${HOME}/.cache/cpm:/tmp/cpm-cache" \
CIBW_BEFORE_ALL_LINUX='
  if command -v dnf &> /dev/null; then
    dnf install -y cmake git
    dnf install -y libXrandr-devel libxcb-devel libxkbcommon-x11-devel libXinerama-devel wayland-protocols-devel
    dnf install -y miniz miniz-devel
  else
    yum install -y cmake git
    yum install -y libXrandr-devel libxcb-devel libxkbcommon-x11-devel libXinerama-devel
    yum install -y miniz miniz-devel
  fi
  cmake --version
' \
CIBW_ENVIRONMENT='CMAKE_ARGS="-DNANOVDB_EDITOR_PYPI_BUILD=ON -DNANOVDB_EDITOR_BUILD_TESTS=OFF -DNANOVDB_EDITOR_BUILD_SLANG_FROM_SOURCE=ON -DFETCHCONTENT_QUIET=OFF" CPM_SOURCE_CACHE=/tmp/cpm-cache' \
CIBW_REPAIR_WHEEL_COMMAND_LINUX="${REPAIR_CMD}" \
CIBW_TEST_REQUIRES="numpy" \
CIBW_TEST_COMMAND='python -c "import nanovdb_editor; print(\"Import successful\")"' \
python -m cibuildwheel --platform linux pymodule

echo ""
echo "Wheel built successfully. Output in ./wheelhouse/"
echo ""

# ── Verify libslang-llvm is present (mirrors CI check) ────────────────────────
echo "Verifying libslang-llvm is present in wheel ..."
python - <<'PY'
import pathlib, zipfile, sys

wheels = sorted(pathlib.Path("wheelhouse").glob("nanovdb_editor*.whl"))
if not wheels:
    print("No nanovdb_editor wheel found in wheelhouse/", file=sys.stderr)
    raise SystemExit(1)

wheel = wheels[0]
print(f"Inspecting wheel: {wheel}")
with zipfile.ZipFile(wheel) as zf:
    names = zf.namelist()

llvm_hits = [n for n in names if n.startswith("nanovdb_editor/lib/") and "libslang-llvm" in n]
if not llvm_hits:
    slang_libs = [n for n in names if n.startswith("nanovdb_editor/lib/") and "libslang" in n]
    print("libslang-llvm was NOT found in wheel!", file=sys.stderr)
    print("Available Slang libs:", file=sys.stderr)
    for name in slang_libs:
        print(f"  {name}", file=sys.stderr)
    raise SystemExit(1)

print("Found libslang-llvm entries:")
for name in llvm_hits:
    print(f"  {name}")
print("Verification passed.")
PY
