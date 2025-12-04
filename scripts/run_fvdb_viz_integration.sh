#!/bin/bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: run_fvdb_viz_integration.sh [options]

Run the same Docker build + pytest workflow defined in
.github/workflows/fvdb-viz-integration.yml.

Options:
  -s, --stream {release|dev}  Package stream to validate (default: release)
  -l, --local-wheel PATH     Path to nanovdb_editor*.whl inside the repo.
                             Only valid with --stream release. Mirrors the
                             GitHub Actions behavior when a local wheel
                             artifact is provided.
  -i, --image-name NAME      Override the Docker image tag (default derived
                             from stream, e.g. fvdb-viz-release)
  -f, --force-rebuild        Force a rebuild of the fvdb-viz test image cache
  -h, --help                 Show this help message and exit
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd -P)"
source "${SCRIPT_DIR}/fvdb_viz_versions.sh"

PACKAGE_STREAM="release"
LOCAL_WHEEL=""
IMAGE_NAME=""
FORCE_REBUILD="0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -s|--stream)
      PACKAGE_STREAM="${2:-}"
      shift 2
      ;;
    -l|--local-wheel)
      LOCAL_WHEEL="${2:-}"
      shift 2
      ;;
    -i|--image-name)
      IMAGE_NAME="${2:-}"
      shift 2
      ;;
    -f|--force-rebuild)
      FORCE_REBUILD="1"
      shift 1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

case "$PACKAGE_STREAM" in
  release|dev) ;;
  *)
    echo "Unsupported package stream: $PACKAGE_STREAM" >&2
    exit 1
    ;;
esac

if [[ -n "$LOCAL_WHEEL" && "$PACKAGE_STREAM" != "release" ]]; then
  echo "A local wheel can only be used with the release stream." >&2
  exit 1
fi

USE_LOCAL="false"
LOCAL_WHEEL_ABS=""
if [[ -n "$LOCAL_WHEEL" ]]; then
  LOCAL_WHEEL_ABS="$(realpath "$LOCAL_WHEEL")"
  if [[ ! -f "$LOCAL_WHEEL_ABS" ]]; then
    echo "Local wheel not found: $LOCAL_WHEEL" >&2
    exit 1
  fi
  if [[ "$LOCAL_WHEEL_ABS" != "$REPO_ROOT"* ]]; then
    echo "Local wheel must reside inside the repository so it is mounted into the container." >&2
    exit 1
  fi
  USE_LOCAL="true"
fi

: "${FVDB_VIZ_TORCH_VERSION:=${FVDB_VIZ_TORCH_VERSION_DEFAULT}}"
: "${FVDB_VIZ_TORCH_INDEX_URL:=${FVDB_VIZ_TORCH_INDEX_URL_DEFAULT}}"
: "${FVDB_VIZ_CORE_VERSION:=${FVDB_VIZ_CORE_VERSION_DEFAULT}}"
: "${FVDB_VIZ_CORE_INDEX_URL:=${FVDB_VIZ_CORE_INDEX_URL_DEFAULT}}"
: "${FVDB_VIZ_SCRIPT_TIMEOUT:=${FVDB_VIZ_SCRIPT_TIMEOUT_DEFAULT}}"

CORE_VERSION_TAG="${FVDB_VIZ_CORE_VERSION%%+*}"
if [[ -z "${CORE_VERSION_TAG}" ]]; then
  CORE_VERSION_TAG="${FVDB_VIZ_CORE_VERSION}"
fi

if [[ -z "${FVDB_VIZ_TEST_IMAGE:-}" ]]; then
  FVDB_VIZ_TEST_IMAGE="fvdb-test-image-${CORE_VERSION_TAG}"
fi

if [[ -n "$IMAGE_NAME" ]]; then
  FVDB_VIZ_TEST_IMAGE="$IMAGE_NAME"
fi

echo "Ensuring base fvdb viz test image (${FVDB_VIZ_TEST_IMAGE}) exists (torch index: ${FVDB_VIZ_TORCH_INDEX_URL})"
(
  cd "${REPO_ROOT}"
  FVDB_VIZ_TEST_IMAGE="${FVDB_VIZ_TEST_IMAGE}" \
  FVDB_VIZ_TORCH_VERSION="${FVDB_VIZ_TORCH_VERSION}" \
  FVDB_VIZ_TORCH_INDEX_URL="${FVDB_VIZ_TORCH_INDEX_URL}" \
  FVDB_VIZ_CORE_VERSION="${FVDB_VIZ_CORE_VERSION}" \
  FVDB_VIZ_CORE_INDEX_URL="${FVDB_VIZ_CORE_INDEX_URL}" \
  FVDB_VIZ_FORCE_IMAGE_REBUILD="${FORCE_REBUILD}" \
    ./scripts/build_fvdb_viz_test_image.sh
)

if [[ "$USE_LOCAL" == "true" ]]; then
  RELATIVE_WHEEL="${LOCAL_WHEEL_ABS#${REPO_ROOT}/}"
  FVDB_VIZ_LOCAL_DIST="/workspace/${RELATIVE_WHEEL}"
  LOCAL_DIST_ARGS=(-e "FVDB_VIZ_LOCAL_DIST=${FVDB_VIZ_LOCAL_DIST}")
  PYTEST_EXPR="test_fvdb_viz_with_local_package"
else
  LOCAL_DIST_ARGS=()
  if [[ "$PACKAGE_STREAM" == "dev" ]]; then
    PYTEST_EXPR="test_fvdb_viz_with_dev_package"
  else
    PYTEST_EXPR="test_fvdb_viz_with_release_package"
  fi
fi

if [[ -t 1 ]]; then
  DOCKER_TTY=(-it)
else
  DOCKER_TTY=()
fi

DOCKER_RUN_COMMON=(
  docker run --rm
  "${DOCKER_TTY[@]}"
  -e "FVDB_VIZ_TESTS=1"
  -e "FVDB_VIZ_TORCH_VERSION=${FVDB_VIZ_TORCH_VERSION}"
  -e "FVDB_VIZ_TORCH_INDEX_URL=${FVDB_VIZ_TORCH_INDEX_URL}"
  -e "FVDB_VIZ_CORE_VERSION=${FVDB_VIZ_CORE_VERSION}"
  -e "FVDB_VIZ_CORE_INDEX_URL=${FVDB_VIZ_CORE_INDEX_URL}"
  -e "FVDB_VIZ_SCRIPT_TIMEOUT=${FVDB_VIZ_SCRIPT_TIMEOUT}"
  -e "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
  -e "VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
  -e "LP_NUM_THREADS=1"
  -e "LIBGL_ALWAYS_SOFTWARE=1"
  -e "GALLIUM_DRIVER=llvmpipe"
  -e "FVDB_VIZ_SKIP_BASE_INSTALL=1"
  -e "PYTHONUNBUFFERED=1"
  -v "${REPO_ROOT}:/workspace"
  -w /workspace
)

PYTEST_CMD=$'python3 -m pip install --break-system-packages pytest >/tmp/pip.log 2>&1; '\
$'python3 -c "import importlib; mod = importlib.import_module(\'nanovdb_editor\'); '\
$'version = getattr(mod, \'__version__\', \'unknown\'); '\
$'print(f\'nanovdb_editor version: {version}\')"; '\
$'pytest pytests/test_fvdb_viz_integration.py -k "'"${PYTEST_EXPR}"'" -vv -s --maxfail=1 --full-trace -rA; '\
$'PYTEST_EXIT=$?; echo "Pytest exit code: $PYTEST_EXIT"; exit $PYTEST_EXIT'

echo "Running pytest selector: ${PYTEST_EXPR}"
set +e
"${DOCKER_RUN_COMMON[@]}" \
  "${LOCAL_DIST_ARGS[@]}" \
  "${FVDB_VIZ_TEST_IMAGE}" \
  sh -c "${PYTEST_CMD}"
DOCKER_EXIT_CODE=$?
set -e

echo "Docker exit code: ${DOCKER_EXIT_CODE}"
exit "${DOCKER_EXIT_CODE}"

