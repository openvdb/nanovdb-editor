#!/usr/bin/env bash
# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "Usage: $0 <command> [recompile-regex]" >&2
    exit 1
fi

COMMAND="$1"
PATTERN="${2:-Building CXX object}"

echo "Running no-op incremental build to verify nothing is recompiled..."
OUTPUT="$(eval "$COMMAND" 2>&1)"
echo "$OUTPUT"

if echo "$OUTPUT" | grep -Eq "$PATTERN"; then
    echo "::error::Incremental build recompiled source files unexpectedly!"
    exit 1
fi

echo "Incremental build test passed - no recompilation detected"
