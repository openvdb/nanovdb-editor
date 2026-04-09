#!/bin/bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0
#
# Local cibuildwheel test for Linux x86_64.
# Mirrors the current CI flow closely enough to debug wheel repair/import issues.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"

if ! command -v cibuildwheel >/dev/null 2>&1; then
  echo "cibuildwheel not found. Install it with: pip install 'cibuildwheel==2.21.3'" >&2
  exit 1
fi

echo "Using cibuildwheel $(python -c 'import cibuildwheel; print(cibuildwheel.__version__)')"

if [ -z "${LOCAL_BUILD_SLANG_FROM_SOURCE+x}" ]; then
  LOCAL_BUILD_SLANG_FROM_SOURCE="ON"
fi
echo "LOCAL_BUILD_SLANG_FROM_SOURCE=${LOCAL_BUILD_SLANG_FROM_SOURCE}"
LOCAL_WHEEL_TAG="${LOCAL_WHEEL_TAG:-${LOCAL_MANYLINUX_X86_64_TAG:-manylinux_2_27_x86_64}}"
LOCAL_BUILD_IMAGE="${LOCAL_BUILD_IMAGE:-${LOCAL_MANYLINUX_X86_64_IMAGE:-quay.io/pypa/manylinux_2_28_x86_64:latest}}"
LOCAL_MANYLINUX_X86_64_TAG="${LOCAL_WHEEL_TAG}"
LOCAL_MANYLINUX_X86_64_IMAGE="${LOCAL_BUILD_IMAGE}"
LOCAL_SLANG_IMAGE_REF="${LOCAL_SLANG_IMAGE_REF:-}"
LOCAL_SLANG_IMAGE_ROOT_DIR="${REPO_ROOT}/.ci/slang-image"
LOCAL_SLANG_IMAGE_LIB_DIR="${LOCAL_SLANG_IMAGE_ROOT_DIR}/lib"
LOCAL_SLANG_IMAGE_INCLUDE_DIR="${LOCAL_SLANG_IMAGE_ROOT_DIR}/include"
LOCAL_SLANG_IMAGE_CONTAINER_ROOT_DIR="/project/.ci/slang-image"
LOCAL_SLANG_IMAGE_CONTAINER_LIB_DIR="${LOCAL_SLANG_IMAGE_CONTAINER_ROOT_DIR}/lib"
echo "LOCAL_WHEEL_TAG=${LOCAL_WHEEL_TAG}"
echo "LOCAL_BUILD_IMAGE=${LOCAL_BUILD_IMAGE}"
if [ -n "${LOCAL_SLANG_IMAGE_REF}" ]; then
  echo "LOCAL_SLANG_IMAGE_REF=${LOCAL_SLANG_IMAGE_REF}"
fi

LOCAL_USE_STAGING_COPY="${LOCAL_USE_STAGING_COPY:-ON}"
LOCAL_KEEP_STAGING_COPY="${LOCAL_KEEP_STAGING_COPY:-OFF}"

if [ "${LOCAL_USE_STAGING_COPY}" = "ON" ] && [ "${LOCAL_STAGING_ACTIVE:-0}" != "1" ]; then
  ORIGINAL_REPO_ROOT="${REPO_ROOT}"
  STAGING_ROOT="${LOCAL_STAGING_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/nanovdb-wheel-local.XXXXXX")}"

  echo "Creating filtered staging copy in ${STAGING_ROOT} ..."

  python - <<'PY' "${REPO_ROOT}" "${STAGING_ROOT}"
import os
import pathlib
import shutil
import sys

src = pathlib.Path(sys.argv[1]).resolve()
dst = pathlib.Path(sys.argv[2]).resolve()

excluded_dir_names = {
    ".cache",
    ".git",
    ".pytest_cache",
    ".venv",
    "__pycache__",
    "build",
    "data",
    "wheelhouse",
    "wheelhouse-local-ci",
    "wheelhouse-local-off",
}
excluded_relative_paths = {
    ".ci/slang-image",
    "pymodule/build",
}

if dst.exists():
    shutil.rmtree(dst)

dst.parent.mkdir(parents=True, exist_ok=True)

def ignore(path, names):
    rel_dir = pathlib.Path(path).resolve().relative_to(src)
    ignored = []
    for name in names:
      rel_path = (rel_dir / name).as_posix() if rel_dir != pathlib.Path(".") else name
      if name in excluded_dir_names:
          ignored.append(name)
          continue
      if rel_path in excluded_relative_paths:
          ignored.append(name)
    return ignored

shutil.copytree(src, dst, ignore=ignore, symlinks=True)
PY

  status=0
  (
    export LOCAL_STAGING_ACTIVE=1
    export LOCAL_STAGING_DIR="${STAGING_ROOT}"
    export LOCAL_KEEP_STAGING_COPY="${LOCAL_KEEP_STAGING_COPY}"
    cd "${STAGING_ROOT}"
    bash "./dev_utils/test_wheel_local.sh"
  ) || status=$?

  if [ -d "${STAGING_ROOT}/wheelhouse" ]; then
    rm -rf "${ORIGINAL_REPO_ROOT}/wheelhouse"
    cp -a "${STAGING_ROOT}/wheelhouse" "${ORIGINAL_REPO_ROOT}/wheelhouse"
    echo "Copied built wheels back to ${ORIGINAL_REPO_ROOT}/wheelhouse"
  fi

  if [ "${LOCAL_KEEP_STAGING_COPY}" = "ON" ] || [ "${status}" -ne 0 ]; then
    echo "Keeping staging copy at ${STAGING_ROOT}"
  else
    rm -rf "${STAGING_ROOT}"
  fi

  exit "${status}"
fi

if [ -d "${REPO_ROOT}/pymodule/build" ]; then
  echo "Removing stale pymodule/build/ to avoid container path mismatch..."
  rm -rf "${REPO_ROOT}/pymodule/build"
fi

if [ -d "${REPO_ROOT}/build" ]; then
  echo "Removing stale top-level build/ to avoid copying large local artifacts into the manylinux container..."
  rm -rf "${REPO_ROOT}/build"
fi

rm -rf "${REPO_ROOT}/wheelhouse"

if [ -n "${LOCAL_SLANG_IMAGE_REF}" ]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "docker not found. Install Docker to reuse a Slang package image locally." >&2
    exit 1
  fi

  echo "Pulling Slang package image ${LOCAL_SLANG_IMAGE_REF} ..."
  docker pull "${LOCAL_SLANG_IMAGE_REF}"

  rm -rf "${LOCAL_SLANG_IMAGE_ROOT_DIR}"
  mkdir -p "${LOCAL_SLANG_IMAGE_LIB_DIR}" "${LOCAL_SLANG_IMAGE_INCLUDE_DIR}"

  cid=""
  cleanup_slang_image_extract() {
    if [ -n "${cid}" ]; then
      docker rm "${cid}" >/dev/null 2>&1 || true
    fi
  }
  trap cleanup_slang_image_extract EXIT

  cid="$(docker create "${LOCAL_SLANG_IMAGE_REF}" /bin/true)"
  docker cp "${cid}:/opt/slang/lib/." "${LOCAL_SLANG_IMAGE_LIB_DIR}/"
  docker cp "${cid}:/opt/slang/include/." "${LOCAL_SLANG_IMAGE_INCLUDE_DIR}/"
  rm -f "${LOCAL_SLANG_IMAGE_LIB_DIR}"/*.dwarf

  if ! ls "${LOCAL_SLANG_IMAGE_LIB_DIR}"/libslang*.so* >/dev/null 2>&1; then
    echo "Extracted Slang image is missing libslang*.so*: ${LOCAL_SLANG_IMAGE_REF}" >&2
    exit 1
  fi
  if ! find "${LOCAL_SLANG_IMAGE_INCLUDE_DIR}" -type f | read -r _; then
    echo "Extracted Slang image is missing headers: ${LOCAL_SLANG_IMAGE_REF}" >&2
    exit 1
  fi

  cleanup_slang_image_extract
  trap - EXIT

  # Reusing a published Slang package image should bypass rebuilding Slang locally.
  LOCAL_BUILD_SLANG_FROM_SOURCE="OFF"
  echo "Reusing Slang package from cached image via ${LOCAL_SLANG_IMAGE_ROOT_DIR}"
fi

REPAIR_CMD='
set -e
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
      echo "Created compatibility copy: ${SPIRV_COMPAT_DIR}/libSPIRV.so.15 from ${SPIRV_SRC_LIB}"
    else
      echo "Warning: could not create SPIRV compatibility libSPIRV.so.15; source library not found in ${SPIRV_LIB_DIR}"
    fi
  fi
else
  echo "Warning: could not locate libSPIRV.so* before auditwheel; relying on wheel-installed runtime libs"
fi
echo "=== auditwheel show ==="
AUDITWHEEL_SHOW_FILE="/tmp/nanovdb-auditwheel-show.txt"
rm -f "${AUDITWHEEL_SHOW_FILE}"
run_auditwheel show {wheel} 2>&1 | tee "${AUDITWHEEL_SHOW_FILE}" || true
AUDITWHEEL_DEFAULT_PLAT="${LOCAL_WHEEL_TAG}"
AUDITWHEEL_PLAT="$(python - <<'"'"'PY'"'"' "${AUDITWHEEL_SHOW_FILE}" "${AUDITWHEEL_DEFAULT_PLAT}"
import pathlib
import re
import sys

show_path = pathlib.Path(sys.argv[1])
default_plat = sys.argv[2]
text = show_path.read_text(encoding="utf-8") if show_path.exists() else ""
match = re.search(r'"'"'This constrains the platform tag to "([^"]+)"'"'"', text)
print(match.group(1) if match else default_plat)
PY
)"
echo "Using auditwheel platform from show output: ${AUDITWHEEL_PLAT}"
run_auditwheel repair -w {dest_dir} {wheel} --lib-sdir .libs --plat "${AUDITWHEEL_PLAT}"
'

echo "Building Linux x86_64 wheel via Docker (wheel tag ${LOCAL_WHEEL_TAG}, image ${LOCAL_BUILD_IMAGE}) ..."

CMAKE_ARGS_VALUE="-DNANOVDB_EDITOR_PYPI_BUILD=ON -DNANOVDB_EDITOR_BUILD_TESTS=OFF -DNANOVDB_EDITOR_BUILD_SLANG_FROM_SOURCE=${LOCAL_BUILD_SLANG_FROM_SOURCE} -DFETCHCONTENT_QUIET=OFF"
CIBW_ENVIRONMENT_VALUE="CMAKE_ARGS=\"${CMAKE_ARGS_VALUE}\" CPM_SOURCE_CACHE=/tmp/cpm-cache LOCAL_WHEEL_TAG=${LOCAL_WHEEL_TAG}"

if [ -n "${LOCAL_SLANG_IMAGE_REF}" ]; then
  CMAKE_ARGS_VALUE="-DNANOVDB_EDITOR_PYPI_BUILD=ON -DNANOVDB_EDITOR_BUILD_TESTS=OFF -DNANOVDB_EDITOR_BUILD_SLANG_FROM_SOURCE=OFF -DNANOVDB_SLANG_IMAGE_ROOT_DIR=${LOCAL_SLANG_IMAGE_CONTAINER_ROOT_DIR} -DFETCHCONTENT_QUIET=OFF"
  CIBW_ENVIRONMENT_VALUE="CMAKE_ARGS=\"${CMAKE_ARGS_VALUE}\" CPM_SOURCE_CACHE=/tmp/cpm-cache LOCAL_WHEEL_TAG=${LOCAL_WHEEL_TAG} NANOVDB_SLANG_IMAGE_LIB_DIR=${LOCAL_SLANG_IMAGE_CONTAINER_LIB_DIR}"
fi

CIBW_BUILD="cp311-manylinux_x86_64" \
CIBW_MANYLINUX_X86_64_IMAGE="${LOCAL_BUILD_IMAGE}" \
CIBW_BUILD_VERBOSITY=1 \
CIBW_TEST_SKIP="cp311-manylinux_x86_64" \
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
CIBW_ENVIRONMENT="${CIBW_ENVIRONMENT_VALUE}" \
CIBW_REPAIR_WHEEL_COMMAND_LINUX="${REPAIR_CMD}" \
python -m cibuildwheel --platform linux pymodule

echo
echo "Wheel built successfully. Output in ./wheelhouse/"
echo

echo "Verifying libslang-llvm is present in wheel ..."
python - <<'PY'
import pathlib
import sys
import zipfile

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
    print("libslang-llvm was not found in wheel; available Slang libs:", file=sys.stderr)
    for name in slang_libs:
        print(f" - {name}", file=sys.stderr)
    raise SystemExit(1)

print("Found libslang-llvm entries:")
for name in llvm_hits:
    print(f" - {name}")
PY

echo
echo "Smoke testing repaired wheel in a fresh host venv ..."
TEST_VENV="$(mktemp -d)/venv"
python -m venv "${TEST_VENV}"
"${TEST_VENV}/bin/python" -m pip install --upgrade pip
"${TEST_VENV}/bin/python" -m pip install numpy ./wheelhouse/nanovdb_editor*.whl
"${TEST_VENV}/bin/python" -c "import nanovdb_editor; print('Import successful')"

echo
echo "Local wheel smoke test passed."
