# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [ValidateSet('1', '1.5', '2')][string]$Scale = '1.5',
    [string]$Upstream = 'E:/ant-design',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$crateRoot = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $crateRoot
$scaleLabel = switch ($Scale) { '1' { '100' } '1.5' { '150' } '2' { '200' } }
$reference = Join-Path $repositoryRoot "build/ant-design-gpui/pagination-reference/visual-$scaleLabel"
$native = Join-Path $repositoryRoot "build/ant-design-gpui/pagination-native/visual-$scaleLabel"
$comparison = Join-Path $repositoryRoot "build/ant-design-gpui/pagination-native/comparison-$scaleLabel.json"
$cases = Join-Path $crateRoot 'fixtures/pagination/visual-cases.json'
Push-Location $crateRoot
try {
    & node scripts/prepare_pagination_reference.mjs --upstream $Upstream
    if ($LASTEXITCODE -ne 0) { throw 'Pagination browser reference preparation failed.' }
    if (-not $SkipBuild) {
        & cargo build --locked --offline --example pagination_visual
        if ($LASTEXITCODE -ne 0) { throw 'Pagination native fixture build failed.' }
    }
    & node scripts/capture_pagination_reference.mjs --cases $cases --output $reference --scale $Scale
    if ($LASTEXITCODE -ne 0) { throw 'Pagination browser capture failed.' }
    & node scripts/capture_pagination_text_reference.mjs --scale $Scale
    if ($LASTEXITCODE -ne 0) { throw 'Pagination browser text evidence failed.' }
    & (Join-Path $repositoryRoot 'build/ant-design-gpui/debug/examples/pagination_visual.exe') --cases $cases --output $native --scale $Scale
    if ($LASTEXITCODE -ne 0) { throw 'Pagination native capture failed.' }
    & python scripts/compare_pagination_visual.py --native $native --reference $reference --output $comparison
    if ($LASTEXITCODE -ne 0) { throw "Pagination visual comparison failed: $comparison" }
} finally { Pop-Location }
