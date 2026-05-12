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
  -l, --local-wheel [PATH]   Path to nanovdb_editor*.whl inside the repo.
                             If PATH is omitted, defaults to pymodule/dist/nanovdb_editor*.whl.
                             Mirrors the GitHub Actions behavior when a local
                             wheel artifact is provided.
  --fvdb-nightly             Install the latest fvdb-core[viewer] nightly wheel
                             (prebuilt, tracks fvdb-core main).
  --upstream-test-ref REF    Upstream fvdb viz test source to use:
                             main (default) or release
  -i, --image-name NAME      Override the Docker image tag (default derived
                             from fvdb-core version, e.g. nanovdb-editor_fvdb-0.4.2)
  -f, --force-rebuild        Force a rebuild of the fvdb-viz test image cache
  --skip-image-build         Skip building/checking the test image (assume it
                             already exists, useful in CI)
  -h, --help                 Show this help message and exit
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd -P)"
source "${SCRIPT_DIR}/fvdb_viz_versions.sh"

PACKAGE_STREAM="release"
LOCAL_WHEEL=""
FVDB_NIGHTLY="0"
UPSTREAM_TEST_REF="main"
IMAGE_NAME=""
FORCE_REBUILD="0"
SKIP_IMAGE_BUILD="0"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -s|--stream)
      PACKAGE_STREAM="${2:-}"
      shift 2
      ;;
    -l|--local-wheel)
      # If next arg is missing or starts with '-', use default
      if [[ -z "${2:-}" || "${2:-}" == -* ]]; then
        LOCAL_WHEEL="__DEFAULT__"
        shift 1
      else
        LOCAL_WHEEL="${2}"
        shift 2
      fi
      ;;
    --upstream-test-ref)
      UPSTREAM_TEST_REF="${2:-}"
      shift 2
      ;;
    --fvdb-nightly)
      FVDB_NIGHTLY="1"
      shift 1
      ;;
    -i|--image-name)
      IMAGE_NAME="${2:-}"
      shift 2
      ;;
    -f|--force-rebuild)
      FORCE_REBUILD="1"
      shift 1
      ;;
    --skip-image-build)
      SKIP_IMAGE_BUILD="1"
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

case "$UPSTREAM_TEST_REF" in
  main|release) ;;
  *)
    echo "Unsupported upstream test ref: $UPSTREAM_TEST_REF" >&2
    exit 1
    ;;
esac

USE_LOCAL="false"
LOCAL_WHEEL_ABS=""
if [[ -n "$LOCAL_WHEEL" ]]; then
  if [[ "$LOCAL_WHEEL" == "__DEFAULT__" ]]; then
    # Expand glob to find default wheel
    DEFAULT_WHEEL_GLOB="${REPO_ROOT}/pymodule/dist/nanovdb_editor*.whl"
    WHEELS=( $DEFAULT_WHEEL_GLOB )
    if [[ ${#WHEELS[@]} -eq 0 || ! -f "${WHEELS[0]}" ]]; then
      echo "No wheel found matching: $DEFAULT_WHEEL_GLOB" >&2
      exit 1
    fi
    if [[ ${#WHEELS[@]} -gt 1 ]]; then
      echo "Multiple wheels found matching $DEFAULT_WHEEL_GLOB:" >&2
      printf '  %s\n' "${WHEELS[@]}" >&2
      echo "Please specify which wheel to use with --local-wheel PATH" >&2
      exit 1
    fi
    LOCAL_WHEEL="${WHEELS[0]}"
    echo "Using default wheel: $LOCAL_WHEEL"
  fi
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
: "${FVDB_VIZ_CORE_NIGHTLY_INDEX_URL:=${FVDB_VIZ_CORE_NIGHTLY_INDEX_URL_DEFAULT}}"
: "${FVDB_VIZ_TEST_IMAGE_BASE:=${FVDB_VIZ_TEST_IMAGE_BASE_DEFAULT}}"
: "${FVDB_VIZ_SKIP_BASE_INSTALL:=1}"

CORE_VERSION_TAG="${FVDB_VIZ_CORE_VERSION%%+*}"
if [[ -z "${CORE_VERSION_TAG}" ]]; then
  CORE_VERSION_TAG="${FVDB_VIZ_CORE_VERSION}"
fi
if [[ "${FVDB_NIGHTLY}" == "1" ]]; then
  CORE_SOURCE_TAG="nightly"
else
  CORE_SOURCE_TAG="${CORE_VERSION_TAG}"
fi

if [[ -z "${FVDB_VIZ_TEST_IMAGE:-}" ]]; then
  FVDB_VIZ_TEST_IMAGE="${FVDB_VIZ_TEST_IMAGE_BASE}-${CORE_SOURCE_TAG}"
fi

if [[ -n "$IMAGE_NAME" ]]; then
  FVDB_VIZ_TEST_IMAGE="$IMAGE_NAME"
fi

if [[ "$SKIP_IMAGE_BUILD" != "1" ]]; then
  echo "Ensuring base fvdb viz test image (${FVDB_VIZ_TEST_IMAGE}) exists (torch index: ${FVDB_VIZ_TORCH_INDEX_URL})"
  (
    cd "${REPO_ROOT}"
    FVDB_VIZ_TEST_IMAGE="${FVDB_VIZ_TEST_IMAGE}" \
    FVDB_VIZ_TORCH_VERSION="${FVDB_VIZ_TORCH_VERSION}" \
    FVDB_VIZ_TORCH_INDEX_URL="${FVDB_VIZ_TORCH_INDEX_URL}" \
    FVDB_VIZ_CORE_VERSION="${FVDB_VIZ_CORE_VERSION}" \
    FVDB_VIZ_CORE_INDEX_URL="${FVDB_VIZ_CORE_INDEX_URL}" \
    FVDB_VIZ_CORE_NIGHTLY_INDEX_URL="${FVDB_VIZ_CORE_NIGHTLY_INDEX_URL}" \
    FVDB_VIZ_CORE_NIGHTLY="${FVDB_NIGHTLY}" \
    FVDB_VIZ_FORCE_IMAGE_REBUILD="${FORCE_REBUILD}" \
      ./scripts/build_fvdb_viz_test_image.sh
  )
else
  echo "Skipping image build, using existing image: ${FVDB_VIZ_TEST_IMAGE}"
fi

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
  -e "FVDB_VIZ_SKIP_BASE_INSTALL=${FVDB_VIZ_SKIP_BASE_INSTALL}"
  -e "PYTHONPATH=/workspace"
  -e "PYTHONUNBUFFERED=1"
  -v "${REPO_ROOT}:/workspace"
  -w /workspace
)

echo "Running pytest selector: ${PYTEST_EXPR}"
set +e
"${DOCKER_RUN_COMMON[@]}" \
  -e "FVDB_VIZ_UPSTREAM_TEST_REF=${UPSTREAM_TEST_REF}" \
  -e "FVDB_VIZ_PYTEST_EXPR=${PYTEST_EXPR}" \
  "${LOCAL_DIST_ARGS[@]}" \
  "${FVDB_VIZ_TEST_IMAGE}" \
  /workspace/scripts/fvdb_viz_entrypoint.sh
DOCKER_EXIT_CODE=$?
set -e

echo "Docker exit code: ${DOCKER_EXIT_CODE}"
exit "${DOCKER_EXIT_CODE}"

