# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

param(
    [Parameter(Mandatory = $true)]
    [string]$Command,
    [string]$Pattern = 'CL\.exe.+/c\s'
)

Write-Host "Running no-op incremental build to verify nothing is recompiled..."
$output = cmd /c "$Command 2>&1"
$output | ForEach-Object { Write-Host $_ }

$compiled = $output | Select-String -Pattern $Pattern
if ($compiled) {
    Write-Host "::error::Incremental build recompiled source files unexpectedly!"
    Write-Host "Recompiled lines:"
    $compiled | ForEach-Object { Write-Host $_.Line }
    exit 1
}

Write-Host "Incremental build test passed - no recompilation detected"
