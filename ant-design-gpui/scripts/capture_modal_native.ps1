# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([string]$Output = (Join-Path $PSScriptRoot '../../build/ant-design-gpui/modal-native'), [double[]]$Scales = @(1,1.5,2))
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $root '../build/ant-design-gpui/debug/examples/modal.exe'
$cases = Get-Content -LiteralPath (Join-Path $root 'fixtures/modal/visual-cases.json') -Raw | ConvertFrom-Json
foreach ($scale in $Scales) {
    $directory = Join-Path $Output ([string][int]($scale*100))
    [IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($directory)) | Out-Null
    foreach ($case in $cases) {
        $arguments = @('--capture', (Join-Path $directory ($case.name + '.png')), '--scale', [string]$scale)
        if ($case.theme -like '*dark*') { $arguments += '--dark' }
        if ($case.theme -like '*compact*') { $arguments += '--compact' }
        if ($case.direction -eq 'rtl') { $arguments += '--rtl' }
        if ($case.wireframe) { $arguments += '--wireframe' }
        if ($case.centered) { $arguments += '--centered' }
        if ($case.loading) { $arguments += '--loading' }
        if ($case.kind) { $arguments += @('--kind', $case.kind) }
        if ($case.variant) { $arguments += @('--variant', $case.variant) }
        if ($case.viewportWidth) { $arguments += @('--window-size', ([string]$case.viewportWidth + 'x600')) }
        & $executable @arguments
        if ($LASTEXITCODE -ne 0) { throw "Capture failed: $($case.name) at $scale" }
    }
}
