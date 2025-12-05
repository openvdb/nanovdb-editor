#!/bin/bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: build_fvdb_viz_test_image.sh [--force-rebuild]

Options:
  -f, --force-rebuild   Ignore cached tarball/image and rebuild.
  -h, --help            Show this message and exit.
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd -P)"
CACHE_DIR="${REPO_ROOT}/.cache"
source "${SCRIPT_DIR}/fvdb_viz_versions.sh"

TORCH_VERSION="${FVDB_VIZ_TORCH_VERSION:-${FVDB_VIZ_TORCH_VERSION_DEFAULT}}"
TORCH_INDEX_URL="${FVDB_VIZ_TORCH_INDEX_URL:-${FVDB_VIZ_TORCH_INDEX_URL_DEFAULT}}"
FVDB_CORE_VERSION="${FVDB_VIZ_CORE_VERSION:-${FVDB_VIZ_CORE_VERSION_DEFAULT}}"
FVDB_CORE_INDEX_URL="${FVDB_VIZ_CORE_INDEX_URL:-${FVDB_VIZ_CORE_INDEX_URL_DEFAULT}}"
CORE_VERSION_TAG="${FVDB_CORE_VERSION%%+*}"
if [[ -z "${CORE_VERSION_TAG}" ]]; then
  CORE_VERSION_TAG="${FVDB_CORE_VERSION}"
fi
IMAGE_NAME="${FVDB_VIZ_TEST_IMAGE:-fvdb-test-image-${CORE_VERSION_TAG}}"
FORCE_REBUILD="${FVDB_VIZ_FORCE_IMAGE_REBUILD:-0}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -f|--force-rebuild)
      FORCE_REBUILD="1"
      shift
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

TAR_PATH="${CACHE_DIR}/${IMAGE_NAME}.tar"
META_PATH="${CACHE_DIR}/${IMAGE_NAME}.meta"

ensure_cache_dir() {
  mkdir -p "${CACHE_DIR}"
}

metadata_content() {
  cat <<EOF
IMAGE_NAME=${IMAGE_NAME}
TORCH_VERSION=${TORCH_VERSION}
TORCH_INDEX_URL=${TORCH_INDEX_URL}
FVDB_CORE_VERSION=${FVDB_CORE_VERSION}
FVDB_CORE_INDEX_URL=${FVDB_CORE_INDEX_URL}
EOF
}

cache_metadata_matches() {
  [[ -f "${META_PATH}" ]] || return 1
  local current desired
  desired="$(metadata_content)"
  current="$(cat "${META_PATH}")"
  [[ "${current}" == "${desired}" ]]
}

write_metadata() {
  ensure_cache_dir
  metadata_content > "${META_PATH}"
}

image_exists() {
  docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1
}

load_from_cache() {
  if [[ -f "${TAR_PATH}" ]]; then
    echo "Loading cached image from ${TAR_PATH}"
    docker load -i "${TAR_PATH}"
  else
    return 1
  fi
}

save_to_cache() {
  ensure_cache_dir
  echo "Saving ${IMAGE_NAME} image tarball to ${TAR_PATH}"
  docker save "${IMAGE_NAME}" -o "${TAR_PATH}"
  write_metadata
}

build_image() {
  echo "Building ${IMAGE_NAME} for fvdb viz tests (Torch ${TORCH_VERSION})"
  docker build \
    -f "${REPO_ROOT}/docker/Dockerfile.fvdb_viz_test" \
    --build-arg "TORCH_VERSION=${TORCH_VERSION}" \
    --build-arg "TORCH_INDEX_URL=${TORCH_INDEX_URL}" \
    --build-arg "FVDB_CORE_VERSION=${FVDB_CORE_VERSION}" \
    --build-arg "FVDB_CORE_INDEX_URL=${FVDB_CORE_INDEX_URL}" \
    -t "${IMAGE_NAME}" \
    "${REPO_ROOT}"
  save_to_cache
}

if [[ "${FORCE_REBUILD}" == "1" ]]; then
  build_image
  exit 0
fi

if image_exists; then
  echo "Docker image ${IMAGE_NAME} already available. Force deleting the image with force_rebuild."
  exit 0
fi

if cache_metadata_matches && load_from_cache; then
  echo "Loaded ${IMAGE_NAME} from cache."
  exit 0
fi

if [[ -f "${META_PATH}" && ! cache_metadata_matches ]]; then
  echo "Cached ${IMAGE_NAME} metadata out of date; rebuilding image."
fi

ensure_cache_dir
build_image
