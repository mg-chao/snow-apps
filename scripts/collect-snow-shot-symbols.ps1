[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$InstallDirectory,
    [string]$OcrAssetManifest
)

$ErrorActionPreference = "Stop"
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
$installRoot = (Resolve-Path -LiteralPath $InstallDirectory).Path
$dumpbin = Join-Path $env:VCToolsInstallDir "bin\Hostx64\x64\dumpbin.exe"
if (-not (Test-Path -LiteralPath $dumpbin -PathType Leaf)) {
    throw "Run symbol collection in the Snow build environment."
}
function Assert-PdbIdentity {
    param([string]$Path, [string]$Signature, [string]$Age)
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $magic = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(32))
        if (-not $magic.StartsWith("Microsoft C/C++ MSF 7.00")) {
            throw "Unsupported PDB format: $Path"
        }
        $blockSize = $reader.ReadUInt32()
        $stream.Position = 44
        $directorySize = $reader.ReadUInt32()
        $stream.Position = 52
        $directoryMap = $reader.ReadUInt32()
        if ($blockSize -lt 512 -or $blockSize -gt 65536 -or $directorySize -gt 64MB) {
            throw "Invalid PDB directory: $Path"
        }
        $stream.Position = [long]$directoryMap * $blockSize
        $directoryBlocks = @()
        for ($index = 0; $index -lt [Math]::Ceiling($directorySize / $blockSize); $index++) {
            $directoryBlocks += $reader.ReadUInt32()
        }
        $directory = [IO.MemoryStream]::new()
        try {
            foreach ($block in $directoryBlocks) {
                $stream.Position = [long]$block * $blockSize
                $bytes = $reader.ReadBytes([int]$blockSize)
                $directory.Write($bytes, 0, $bytes.Length)
            }
            $directory.Position = 0
            $entries = [IO.BinaryReader]::new($directory)
            $count = $entries.ReadUInt32()
            if ($count -lt 2 -or $count -gt ($directorySize / 4)) {
                throw "Invalid PDB streams: $Path"
            }
            $streamZeroSize = $entries.ReadUInt32()
            $infoSize = $entries.ReadUInt32()
            if ($infoSize -lt 28 -or $infoSize -eq [uint32]::MaxValue) {
                throw "Missing PDB identity stream: $Path"
            }
            $skipBlocks = if ($streamZeroSize -eq [uint32]::MaxValue) { 0 } else { [Math]::Ceiling($streamZeroSize / $blockSize) }
            $directory.Position = 4 + $count * 4 + $skipBlocks * 4
            $infoBlock = $entries.ReadUInt32()
            $stream.Position = [long]$infoBlock * $blockSize + 8
            $pdbAge = $reader.ReadUInt32()
            $pdbSignature = [guid]::new($reader.ReadBytes(16))
            if ($pdbSignature -ne [guid]$Signature -or $pdbAge -ne [Convert]::ToUInt32($Age, 16)) {
                throw "PDB does not match the binary RSDS identity: $Path"
            }
        }
        finally { $directory.Dispose() }
    }
    finally { $reader.Dispose(); $stream.Dispose() }
}
$symbolRoot = Join-Path $buildRoot ("symbols-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $symbolRoot | Out-Null
$manifest = [ordered]@{
    schema = 1
    configuration = "Release"
    revision = (& git -C $PSScriptRoot rev-parse HEAD).Trim()
    binaries = @()
}
if ($OcrAssetManifest) {
    # Application releases consume an immutable runtime built by its separate release.
    # Record its identity, never label this build's development OCR PDB as matching it.
    $manifest.externalOcrRuntime = (Get-Content -LiteralPath $OcrAssetManifest -Raw | ConvertFrom-Json).runtime
}
foreach ($binary in Get-ChildItem -LiteralPath (Join-Path $installRoot "bin") -File) {
    if ($binary.Extension -notin @(".exe", ".dll")) { continue }
    if ($OcrAssetManifest -and $binary.Name -eq 'snow-ocr-process.exe') { continue }
    $headers = @(& $dumpbin /nologo /headers $binary.FullName 2>&1)
    if ($LASTEXITCODE -ne 0) { throw "Unable to inspect $($binary.FullName)" }
    $destination = Join-Path $symbolRoot $binary.BaseName
    New-Item -ItemType Directory -Path $destination | Out-Null
    Copy-Item -LiteralPath $binary.FullName -Destination $destination
    $record = [ordered]@{
        file = "$($binary.BaseName)/$($binary.Name)"
        sha256 = (Get-FileHash -LiteralPath $binary.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        version = $binary.VersionInfo.FileVersion
        pdb = $null
        signature = $null
        age = $null
    }
    foreach ($line in $headers) {
        if ($line -match 'Format:\s+RSDS,\s+\{(?<signature>[0-9A-Fa-f-]+)\},\s+(?<age>[0-9A-Fa-f]+),\s+(?<path>.+\.pdb)\s*$') {
            $record.signature = $Matches.signature
            $record.age = $Matches.age
            $pdb = $Matches.path.Trim()
            if (-not [System.IO.Path]::IsPathRooted($pdb)) {
                $pdb = Join-Path $buildRoot "cargo\x86_64-pc-windows-msvc\release\$pdb"
            }
            if (Test-Path -LiteralPath $pdb -PathType Leaf) {
                Assert-PdbIdentity -Path $pdb -Signature $record.signature -Age $record.age
                Copy-Item -LiteralPath $pdb -Destination $destination
                $record.pdb = "$($binary.BaseName)/$([System.IO.Path]::GetFileName($pdb))"
            }
            break
        }
    }
    if ($binary.Name -in @("snow_shot.exe", "snow-shot-updater.exe", "snow-ocr-process.exe") -and -not $record.pdb) {
        throw "The matching PDB is missing for $($binary.Name); release symbols are incomplete."
    }
    $manifest.binaries += $record
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $symbolRoot "manifest.json") -Encoding utf8
$archive = Join-Path $buildRoot "snow-shot-symbols-windows-x64.zip"
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($symbolRoot, $archive)
$checksum = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$checksum  $([System.IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath "$archive.sha256" -Encoding ascii
$resolvedSymbols = (Resolve-Path -LiteralPath $symbolRoot).Path
if (-not $resolvedSymbols.StartsWith($buildRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to remove a symbols directory outside the build tree."
}
Remove-Item -LiteralPath $resolvedSymbols -Recurse -Force
Write-Output "Release symbols: $archive"
