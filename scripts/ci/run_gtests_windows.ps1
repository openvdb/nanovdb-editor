# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

$startedAt = Get-Date
ctest --test-dir build/gtests -C Release --output-on-failure --verbose
$testExitCode = $LASTEXITCODE

if ($testExitCode -ne 0) {
    try {
        $events = Get-WinEvent -FilterHashtable @{
            LogName = 'Application'
            ProviderName = 'Application Error'
            Id = 1000
            StartTime = $startedAt
        } -ErrorAction SilentlyContinue
        foreach ($event in $events) {
            $fields = @{}
            foreach ($data in ([xml]$event.ToXml()).Event.EventData.Data) {
                $fields[$data.Name] = $data.InnerText
            }
            if ($fields.AppName -ne 'EditorStartStopTest.exe') {
                continue
            }
            # Print only the module basename and numeric crash identifiers.
            $module = $fields.ModuleName
            if ($module -notmatch '\A[A-Za-z0-9_.-]{1,128}\.(dll|exe)\z') {
                $module = 'unknown'
            }
            $code = $fields.ExceptionCode
            if ($code -notmatch '\A[0-9a-fA-F]{1,16}\z') {
                $code = 'unknown'
            }
            $offset = $fields.FaultingOffset
            if ($offset -notmatch '\A[0-9a-fA-F]{1,16}\z') {
                $offset = 'unknown'
            }
            Write-Host "EditorStartStopTest.exe: module=$module exception=$code offset=$offset"
        }
    } catch {
        Write-Host 'Crash identifiers are unavailable.'
    }
}

exit $testExitCode
