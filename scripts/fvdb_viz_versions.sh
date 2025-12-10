#!/bin/bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

# Central definition for the fvdb viewer dependency versions.
# Scripts should source this file and use the *_DEFAULT values as fallbacks
# so that bumping versions only needs to happen in one place.
# Also used on GitHub Actions workflows to resolve the fVDB dependencies.

FVDB_VIZ_TORCH_VERSION_DEFAULT="2.8.0"
FVDB_VIZ_TORCH_INDEX_URL_DEFAULT="https://download.pytorch.org/whl/cu128"
FVDB_VIZ_CORE_VERSION_DEFAULT="0.3.0+pt28.cu128"
FVDB_VIZ_CORE_INDEX_URL_DEFAULT="https://d36m13axqqhiit.cloudfront.net/simple"
FVDB_VIZ_SCRIPT_TIMEOUT_DEFAULT="480"

