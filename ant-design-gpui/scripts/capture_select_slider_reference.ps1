# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
# Deterministic, isolated render of the locally built original components.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Cases,
    [Parameter(Mandatory)][string]$Output,
    [ValidateSet('1','1.5','2')][string]$Scale = '1.5',
    [string]$Reference,
    [string]$EdgePath = 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
)
$ErrorActionPreference = 'Stop'
$crate = Split-Path -Parent $PSScriptRoot
if (-not $Reference) { $Reference = Join-Path $crate '../build/ant-design-gpui/select-slider-reference/browser' }
$Reference = [IO.Path]::GetFullPath($Reference)
$Output = [IO.Path]::GetFullPath($Output)
[IO.Directory]::CreateDirectory($Output) | Out-Null
& node (Join-Path $PSScriptRoot 'capture_select_slider_reference.mjs') --cases $Cases --output $Output --scale $Scale --reference $Reference --edge $EdgePath
if ($LASTEXITCODE -ne 0) { throw 'Reference capture failed.' }
