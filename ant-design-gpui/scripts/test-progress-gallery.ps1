# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$crateRoot=Split-Path -Parent $PSScriptRoot
Push-Location $crateRoot
try {
    # A distinct filename leaves any running user gallery untouched. No linker
    # stack override: this verifies the ordinary Windows GUI stack budget.
    if (-not $SkipBuild) {
        & cargo rustc --locked --offline --example gallery -- -C extra-filename=-progress
        if ($LASTEXITCODE -ne 0) { throw 'Progress gallery build failed' }
    }
    $exe='../build/ant-design-gpui/debug/examples/gallery-progress.exe'
    $output='../build/ant-design-gpui/progress-gallery'
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $cases=@(
        @{name='default-top'; arguments=@()},
        @{name='default-dynamic'; arguments=@('--capture-scroll-y','900')},
        @{name='default-steps'; arguments=@('--capture-scroll-y','2200')},
        @{name='default-styles'; arguments=@('--capture-scroll-y','4000')},
        @{name='dark-compact-rtl'; arguments=@('--dark','--compact','--rtl')},
        @{name='narrow'; arguments=@('--window-size','640x1000')}
    )
    $captures=@()
    foreach ($case in $cases) {
        $image=Join-Path $output ($case.name+'.png')
        $arguments=@('--component','progress','--headless','--reduced-motion','--window-size','1280x1000','--capture',$image)+$case.arguments
        & $exe @arguments
        if ($LASTEXITCODE -ne 0) { throw "Native Progress gallery failed: $($case.name)" }
        if ((Get-Item -LiteralPath $image).Length -le 100) { throw "Empty native capture: $image" }
        $captures+=@{name=$case.name;sha256=(Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
    $metadata=@{status='passed';profile='debug';stack='default';binary_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant();captures=$captures;utc=[DateTime]::UtcNow.ToString('O')}
    $json=($metadata|ConvertTo-Json -Depth 8).Replace("`r`n","`n")+"`n"
    [IO.File]::WriteAllText((Join-Path $output 'result.json'),$json,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $crateRoot 'fixtures/progress/gallery-validation-windows.json'),$json,[Text.UTF8Encoding]::new($false))
    Write-Output 'Verified six native Progress gallery captures on the default Windows stack.'
} finally {Pop-Location}
