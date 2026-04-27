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
FVDB_CORE_NIGHTLY_INDEX_URL="${FVDB_VIZ_CORE_NIGHTLY_INDEX_URL:-${FVDB_VIZ_CORE_NIGHTLY_INDEX_URL_DEFAULT}}"
FVDB_CORE_NIGHTLY="${FVDB_VIZ_CORE_NIGHTLY:-0}"
FVDB_CORE_NIGHTLY_CUDA_SUFFIX="${FVDB_VIZ_CORE_NIGHTLY_CUDA_SUFFIX:-${FVDB_VIZ_CORE_NIGHTLY_CUDA_SUFFIX_DEFAULT}}"
IMAGE_BASE_NAME="${FVDB_VIZ_TEST_IMAGE_BASE:-${FVDB_VIZ_TEST_IMAGE_BASE_DEFAULT}}"
IMAGE_CACHE_ENABLED="${FVDB_VIZ_IMAGE_CACHE_ENABLED:-1}"
DOCKER_BUILD_NETWORK="${FVDB_VIZ_DOCKER_BUILD_NETWORK:-}"
DOCKER_BUILDER="${FVDB_VIZ_DOCKER_BUILDER:-}"
CORE_VERSION_TAG="${FVDB_CORE_VERSION%%+*}"
if [[ -z "${CORE_VERSION_TAG}" ]]; then
  CORE_VERSION_TAG="${FVDB_CORE_VERSION}"
fi
if [[ "${FVDB_CORE_NIGHTLY}" != "0" && -n "${FVDB_CORE_NIGHTLY}" ]]; then
  CORE_SOURCE_TAG="nightly"
else
  CORE_SOURCE_TAG="${CORE_VERSION_TAG}"
fi
IMAGE_NAME="${FVDB_VIZ_TEST_IMAGE:-${IMAGE_BASE_NAME}-${CORE_SOURCE_TAG}}"
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
FVDB_CORE_NIGHTLY=${FVDB_CORE_NIGHTLY}
FVDB_CORE_NIGHTLY_INDEX_URL=${FVDB_CORE_NIGHTLY_INDEX_URL}
FVDB_CORE_NIGHTLY_CUDA_SUFFIX=${FVDB_CORE_NIGHTLY_CUDA_SUFFIX}
DOCKER_BUILDER=${DOCKER_BUILDER}
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
  if [[ "${IMAGE_CACHE_ENABLED}" != "1" ]]; then
    echo "Skipping local image tarball cache for ${IMAGE_NAME}"
    return
  fi
  ensure_cache_dir
  echo "Saving ${IMAGE_NAME} image tarball to ${TAR_PATH}"
  if ! docker save "${IMAGE_NAME}" -o "${TAR_PATH}"; then
    echo "Warning: failed to save local image tarball cache for ${IMAGE_NAME}; continuing without cache." >&2
    rm -f "${TAR_PATH}" "${META_PATH}"
    return 0
  fi
  if ! write_metadata; then
    echo "Warning: failed to write local image cache metadata for ${IMAGE_NAME}; continuing without cache metadata." >&2
    rm -f "${META_PATH}"
  fi
}

build_image() {
  echo "Building ${IMAGE_NAME} for fvdb viz tests (Torch ${TORCH_VERSION})"
  local docker_build_args=(
    -f "${REPO_ROOT}/docker/Dockerfile.fvdb_viz_test"
    --build-arg "TORCH_VERSION=${TORCH_VERSION}"
    --build-arg "TORCH_INDEX_URL=${TORCH_INDEX_URL}"
    --build-arg "FVDB_CORE_VERSION=${FVDB_CORE_VERSION}"
    --build-arg "FVDB_CORE_INDEX_URL=${FVDB_CORE_INDEX_URL}"
    --build-arg "FVDB_CORE_NIGHTLY=${FVDB_CORE_NIGHTLY}"
    --build-arg "FVDB_CORE_NIGHTLY_INDEX_URL=${FVDB_CORE_NIGHTLY_INDEX_URL}"
    --build-arg "FVDB_CORE_NIGHTLY_CUDA_SUFFIX=${FVDB_CORE_NIGHTLY_CUDA_SUFFIX}"
    -t "${IMAGE_NAME}"
  )
  if [[ -n "${DOCKER_BUILD_NETWORK}" ]]; then
    docker_build_args+=(--network "${DOCKER_BUILD_NETWORK}")
  fi
  if [[ -n "${DOCKER_BUILDER}" ]]; then
    docker buildx build \
      --builder "${DOCKER_BUILDER}" \
      --load \
      "${docker_build_args[@]}" \
      "${REPO_ROOT}"
  else
    docker build \
      "${docker_build_args[@]}" \
      "${REPO_ROOT}"
  fi
  save_to_cache
}

if [[ "${FORCE_REBUILD}" == "1" ]]; then
  build_image
  exit 0
fi

if image_exists; then
  echo "Docker image ${IMAGE_NAME} already available. Use --force-rebuild to recreate it."
  exit 0
fi

if [[ "${IMAGE_CACHE_ENABLED}" == "1" ]]; then
  if cache_metadata_matches && load_from_cache; then
    echo "Loaded ${IMAGE_NAME} from cache."
    exit 0
  fi

  if [[ -f "${META_PATH}" && ! cache_metadata_matches ]]; then
    echo "Cached ${IMAGE_NAME} metadata out of date; rebuilding image."
  fi
else
  echo "Image tarball cache disabled for ${IMAGE_NAME}"
fi

build_image
