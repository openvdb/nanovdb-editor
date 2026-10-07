#!/bin/bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

# Keep each Torch version and CUDA index compatible with its fVDB wheel.

FVDB_VIZ_TORCH_VERSION_DEFAULT="2.11.0"
FVDB_VIZ_TORCH_INDEX_URL_DEFAULT="https://download.pytorch.org/whl/cu128"
FVDB_VIZ_CORE_VERSION_DEFAULT="0.5.1+pt211.cu128"
FVDB_VIZ_CORE_INDEX_URL_DEFAULT="https://d36m13axqqhiit.cloudfront.net/simple"
FVDB_VIZ_CORE_NIGHTLY_INDEX_URL_DEFAULT="https://d36m13axqqhiit.cloudfront.net/simple-nightly"

FVDB_VIZ_NIGHTLY_TORCH_VERSION_DEFAULT="2.13.0"
FVDB_VIZ_NIGHTLY_TORCH_INDEX_URL_DEFAULT="https://download.pytorch.org/whl/cu130"
FVDB_VIZ_CORE_NIGHTLY_CUDA_SUFFIX_DEFAULT="pt213.cu130"

resolve_fvdb_viz_versions() {
  local torch_version_default="${FVDB_VIZ_TORCH_VERSION_DEFAULT}"
  local torch_index_url_default="${FVDB_VIZ_TORCH_INDEX_URL_DEFAULT}"
  if [[ -n "${1:-}" && "${1}" != "0" ]]; then
    torch_version_default="${FVDB_VIZ_NIGHTLY_TORCH_VERSION_DEFAULT}"
    torch_index_url_default="${FVDB_VIZ_NIGHTLY_TORCH_INDEX_URL_DEFAULT}"
  fi
  : "${FVDB_VIZ_TORCH_VERSION:=${torch_version_default}}"
  : "${FVDB_VIZ_TORCH_INDEX_URL:=${torch_index_url_default}}"
  : "${FVDB_VIZ_CORE_VERSION:=${FVDB_VIZ_CORE_VERSION_DEFAULT}}"
  : "${FVDB_VIZ_CORE_INDEX_URL:=${FVDB_VIZ_CORE_INDEX_URL_DEFAULT}}"
  : "${FVDB_VIZ_CORE_NIGHTLY_INDEX_URL:=${FVDB_VIZ_CORE_NIGHTLY_INDEX_URL_DEFAULT}}"
  : "${FVDB_VIZ_CORE_NIGHTLY_CUDA_SUFFIX:=${FVDB_VIZ_CORE_NIGHTLY_CUDA_SUFFIX_DEFAULT}}"
}
