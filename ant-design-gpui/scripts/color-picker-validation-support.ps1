# Copyright (C) 2026 mg-chao
# SPDX-License-Identifier: Apache-2.0

function Get-ColorPickerSourceFingerprint([string]$Root) {
    Push-Location $Root
    try {
        $taskFiles = @(& rg --files src -g '*.rs') + @(& rg --files --hidden --no-ignore resources)
        if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate native source/assets.' }
    } finally { Pop-Location }
    $taskFiles += @(
        'Cargo.toml', 'Cargo.lock', 'README.md', '.cargo/config.toml',
        'examples/color_picker_visual.rs', 'benches/color_picker.rs',
        'fixtures/color_picker/visual-cases.json',
        'scripts/color-picker-validation-support.ps1',
        'scripts/capture-color-picker-native.ps1', 'scripts/run-color-picker-perf.ps1'
    )
    $taskFiles += @('build.rs', 'rust-toolchain.toml') | Where-Object { Test-Path -LiteralPath (Join-Path $Root $_) }
    $taskHashes = [ordered]@{}
    foreach ($taskFile in @($taskFiles | Sort-Object -Unique)) {
        $taskHashes[$taskFile.Replace('\', '/')] = (Get-FileHash -LiteralPath (Join-Path $Root $taskFile) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    return $taskHashes
}

function Assert-ColorPickerSourceFingerprint($Expected, [string]$Root) {
    $taskActual = Get-ColorPickerSourceFingerprint $Root
    if (($Expected | ConvertTo-Json -Compress -Depth 5) -cne ($taskActual | ConvertTo-Json -Compress -Depth 5)) {
        throw 'Source changed during validation. Rebuild and capture the stable source again.'
    }
}

function Get-ColorPickerToolchain {
    return [ordered]@{
        rustc = (& rustc -Vv) -join "`n"
        cargo = (& cargo -V) -join "`n"
        os = [Environment]::OSVersion.VersionString
        architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
        processor_count = [Environment]::ProcessorCount
        processor_model = @((Get-CimInstance Win32_Processor).Name | Sort-Object -Unique) -join '; '
    }
}
