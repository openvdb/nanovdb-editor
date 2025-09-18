# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

#!/bin/bash

format_files() {
    find . -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" -o -name "*.slang" \) -exec clang-format -i {} +
}

pushd ./
format_files
popd
