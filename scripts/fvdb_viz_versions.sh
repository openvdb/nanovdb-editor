#!/bin/bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

# Central definition for the fvdb viewer dependency versions.
# Scripts should source this file and use the *_DEFAULT values as fallbacks
# so that bumping versions only needs to happen in one place.
# Also used on GitHub Actions workflows to resolve the fVDB dependencies.

FVDB_VIZ_TORCH_VERSION_DEFAULT="2.10.0"
FVDB_VIZ_TORCH_INDEX_URL_DEFAULT="https://download.pytorch.org/whl/cu128"
FVDB_VIZ_CUDA_ARCH_LIST_DEFAULT="7.5;8.0;9.0;10.0;12.0+PTX"
FVDB_VIZ_CORE_VERSION_DEFAULT="0.4.2+pt210.cu128"
FVDB_VIZ_CORE_INDEX_URL_DEFAULT="https://d36m13axqqhiit.cloudfront.net/simple"
FVDB_VIZ_CORE_NIGHTLY_INDEX_URL_DEFAULT="https://d36m13axqqhiit.cloudfront.net/simple-nightly"
FVDB_VIZ_TEST_IMAGE_BASE_DEFAULT="nanovdb-editor_fvdb"
FVDB_VIZ_TEST_IMAGE_REVISION_DEFAULT="1"
FVDB_VIZ_SCRIPT_TIMEOUT_DEFAULT="480"

