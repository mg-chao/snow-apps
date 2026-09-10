[CmdletBinding()]
param([string]$ReleaseHelperPath = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$root = Join-Path $repo "build/release-symbol-tests-$([guid]::NewGuid().ToString('N'))"
$stage = Join-Path $root 'stage'
$bin = Join-Path $stage 'bin'
$null = New-Item -ItemType Directory -Path $bin
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
$null = Set-SnowBuildEnvironment -Preset 'snow-shot-msvc-release'
& cl /nologo /std:c++20 /O2 /MT /Zi "/Fd:$root/compile.pdb" "/Fo:$root/fixture.obj" "/Fe:$bin/snow_shot.exe" `
    (Join-Path $repo 'snow_shot/tests/update_helper_fixture.cpp') /link /DEBUG "/PDB:$root/fixture.pdb" advapi32.lib
if ($LASTEXITCODE -ne 0) { throw 'Symbol fixture compilation failed.' }
# dumpbin reports the RSDS age in decimal. Raise the freshly linked pair's age
# to a multi-digit value so a hex misreading of the dumpbin field fails here.
$fixtureExe = [IO.File]::ReadAllBytes((Join-Path $bin 'snow_shot.exe'))
$rsdsOffset = -1
for ($i = 0; $i -lt $fixtureExe.Length - 16; $i++) {
    if ($fixtureExe[$i] -eq 0x52 -and $fixtureExe[$i + 1] -eq 0x53 -and
        $fixtureExe[$i + 2] -eq 0x44 -and $fixtureExe[$i + 3] -eq 0x53) { $rsdsOffset = $i; break }
}
if ($rsdsOffset -lt 0) { throw 'Fixture executable has no RSDS record.' }
[BitConverter]::GetBytes([uint32]10).CopyTo($fixtureExe, $rsdsOffset + 20)
[IO.File]::WriteAllBytes((Join-Path $bin 'snow_shot.exe'), $fixtureExe)
$fixturePdbPath = Join-Path $root 'fixture.pdb'
$fixturePdb = [IO.File]::ReadAllBytes($fixturePdbPath)
$guidBytes = [byte[]]::new(16)
[Array]::Copy($fixtureExe, $rsdsOffset + 4, $guidBytes, 0, 16)
$pdbAgeOffsets = @()
for ($i = 4; $i -le $fixturePdb.Length - 20; $i++) {
    $matchesGuid = $true
    for ($j = 0; $j -lt 16; $j++) {
        if ($fixturePdb[$i + $j] -ne $guidBytes[$j]) { $matchesGuid = $false; break }
    }
    if ($matchesGuid) { $pdbAgeOffsets += ($i - 4) }
}
if ($pdbAgeOffsets.Count -lt 1) { throw 'Fixture PDB identity stream was not found.' }
foreach ($pdbAgeOffset in $pdbAgeOffsets) {
    [BitConverter]::GetBytes([uint32]10).CopyTo($fixturePdb, $pdbAgeOffset)
}
[IO.File]::WriteAllBytes($fixturePdbPath, $fixturePdb)
Copy-Item -LiteralPath (Join-Path $bin 'snow_shot.exe') -Destination (Join-Path $bin 'snow-shot-updater.exe')
Copy-Item -LiteralPath (Join-Path $bin 'snow_shot.exe') -Destination (Join-Path $bin 'snow-ocr-process.exe')
foreach ($external in @($false, $true)) {
    $options = @{}
    if ($external) { $options.OcrAssetManifest = Join-Path $repo 'snow_shot/packaging/snow-shot-ocr-asset-manifest.json' }
    & (Join-Path $PSScriptRoot 'collect-snow-shot-symbols.ps1') -BuildDirectory $root -InstallDirectory $stage @options
    $archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $root 'snow-shot-symbols-windows-x64.zip'))
    try {
        $reader = [IO.StreamReader]::new($archive.GetEntry('manifest.json').Open())
        try { $manifest = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        $expected = if ($external) { 2 } else { 3 }
        if ($manifest.binaries.Count -ne $expected -or @($manifest.binaries | Where-Object { -not $_.pdb }).Count) {
            throw 'Symbol inventory is incomplete.'
        }
        if ($external -and ($null -ne $archive.GetEntry('snow-ocr-process/snow-ocr-process.exe') -or
            $manifest.externalOcrRuntime.version -cne '1.0.5')) { throw 'External OCR runtime was misrepresented as a local build.' }
    } finally { $archive.Dispose() }
    Write-Output "PASS: matching release PDBs; external OCR = $external"
}
# A missing or mismatched app/helper PDB must still stop a release.
Move-Item -LiteralPath (Join-Path $root 'fixture.pdb') -Destination (Join-Path $root 'saved-fixture.pdb')
$rejected = $false
try { & (Join-Path $PSScriptRoot 'collect-snow-shot-symbols.ps1') -BuildDirectory $root -InstallDirectory $stage @options }
catch { $rejected = $true }
if (-not $rejected) { throw 'Missing release PDB was accepted.' }
Write-Output 'PASS: missing app/helper PDB is rejected'

if ($ReleaseHelperPath) {
    # Exercise the real CMake-built helper as well as the synthetic fixture. A fixture
    # compiled with /DEBUG cannot detect missing symbol flags on the production target.
    $realStage = Join-Path $root 'release-helper'
    $realBin = Join-Path $realStage 'bin'
    $null = New-Item -ItemType Directory -Path $realBin
    Copy-Item -LiteralPath (Resolve-Path -LiteralPath $ReleaseHelperPath).Path `
        -Destination (Join-Path $realBin 'snow-shot-updater.exe')
    & (Join-Path $PSScriptRoot 'collect-snow-shot-symbols.ps1') -BuildDirectory $root -InstallDirectory $realStage
    Write-Output 'PASS: actual Release updater has a matching PDB'
}
