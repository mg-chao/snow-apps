[CmdletBinding()]
param(
    [string]$Executable = 'build/snow-shot-msvc-release/snow_shot/Release/snow-shot-updater.exe',
    [string]$Manifest = 'snow_shot/rust/snow-shot-updater/Cargo.toml',
    [string]$Pdb,
    [string]$CargoProfileDirectory =
        'build/snow-shot-msvc-release/cargo/x86_64-pc-windows-msvc/release-size',
    [string]$Output,
    [long]$BaselineBytes = 9460736
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-RepositoryPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

$executablePath = Resolve-RepositoryPath $Executable
$manifestPath = Resolve-RepositoryPath $Manifest
$cargoProfilePath = Resolve-RepositoryPath $CargoProfileDirectory
if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "Updater executable was not found: $executablePath"
}
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Updater Cargo manifest was not found: $manifestPath"
}
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path (Split-Path -Parent $executablePath) 'snow-shot-updater-size-report.json'
}
$outputPath = Resolve-RepositoryPath $Output

$bytes = [System.IO.File]::ReadAllBytes($executablePath)
if ($bytes.Length -lt 64 -or [System.Text.Encoding]::ASCII.GetString($bytes, 0, 2) -cne 'MZ') {
    throw "Updater is not a valid PE image: $executablePath"
}
$peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
if ($peOffset -lt 0 -or $peOffset + 24 -gt $bytes.Length -or
    [System.Text.Encoding]::ASCII.GetString($bytes, $peOffset, 4) -cne "PE`0`0") {
    throw "Updater has an invalid PE header: $executablePath"
}
$sectionCount = [BitConverter]::ToUInt16($bytes, $peOffset + 6)
$optionalHeaderBytes = [BitConverter]::ToUInt16($bytes, $peOffset + 20)
$sectionOffset = $peOffset + 24 + $optionalHeaderBytes
$sections = @()
for ($index = 0; $index -lt $sectionCount; ++$index) {
    $offset = $sectionOffset + 40 * $index
    if ($offset + 40 -gt $bytes.Length) { throw 'Updater section table is truncated.' }
    $nameBytes = $bytes[$offset..($offset + 7)]
    $name = [System.Text.Encoding]::ASCII.GetString($nameBytes).TrimEnd([char]0)
    $sections += [pscustomobject][ordered]@{
        name = $name
        virtualBytes = [BitConverter]::ToUInt32($bytes, $offset + 8)
        rawBytes = [BitConverter]::ToUInt32($bytes, $offset + 16)
    }
}

$imports = @()
$rsdsPdb = $null
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($dumpbin) {
    $dependentOutput = @(& $dumpbin.Source /nologo /dependents $executablePath 2>&1)
    if ($LASTEXITCODE -eq 0) {
        $imports = @($dependentOutput |
            ForEach-Object { [string]$_ } |
            Where-Object { $_ -match '^\s+([^\s]+\.dll)\s*$' } |
            ForEach-Object { $Matches[1] } |
            Sort-Object -Unique)
    }
    $headerOutput = @(& $dumpbin.Source /nologo /headers $executablePath 2>&1)
    if ($LASTEXITCODE -eq 0) {
        foreach ($line in $headerOutput) {
            if ([string]$line -match 'Format:\s+RSDS,\s+\{[0-9A-Fa-f-]+\},\s+[0-9]+,\s+(?<path>.+\.pdb)\s*$') {
                $rsdsPdb = $Matches.path.Trim()
                break
            }
        }
    }
}

$cargoTree = @(& cargo tree --locked --offline --manifest-path $manifestPath `
    --edges normal,build --prefix depth --format '{p} {f}' 2>&1)
if ($LASTEXITCODE -ne 0) { throw 'cargo tree failed while creating the updater size report.' }

$pdbPath = if (-not [string]::IsNullOrWhiteSpace($Pdb)) {
    Resolve-RepositoryPath $Pdb
} elseif ($rsdsPdb -and [System.IO.Path]::IsPathRooted($rsdsPdb) -and
          (Test-Path -LiteralPath $rsdsPdb -PathType Leaf)) {
    [System.IO.Path]::GetFullPath($rsdsPdb)
} elseif ($rsdsPdb) {
    Join-Path $cargoProfilePath ([System.IO.Path]::GetFileName($rsdsPdb))
} else {
    Join-Path $cargoProfilePath 'snow_shot_updater.pdb'
}
if (-not (Test-Path -LiteralPath $pdbPath -PathType Leaf)) {
    $besideExecutable = [System.IO.Path]::ChangeExtension($executablePath, '.pdb')
    if (Test-Path -LiteralPath $besideExecutable -PathType Leaf) {
        $pdbPath = $besideExecutable
    }
}
$pdbBytes = if (Test-Path -LiteralPath $pdbPath -PathType Leaf) {
    (Get-Item -LiteralPath $pdbPath).Length
} else { $null }
$totalBytes = (Get-Item -LiteralPath $executablePath).Length
$changeBytes = $totalBytes - $BaselineBytes
$changePercent = if ($BaselineBytes -eq 0) { $null } else {
    [Math]::Round(($changeBytes * 100.0) / $BaselineBytes, 3)
}

$report = [pscustomobject][ordered]@{
    schema = 1
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    executable = $executablePath
    totalBytes = $totalBytes
    totalMiB = [Math]::Round($totalBytes / 1MB, 3)
    baselineBytes = $BaselineBytes
    changeBytes = $changeBytes
    changePercent = $changePercent
    sections = $sections
    importedDlls = $imports
    cargoFeatureTree = $cargoTree
    pdb = [pscustomobject][ordered]@{
        path = $pdbPath
        bytes = $pdbBytes
        miB = if ($null -eq $pdbBytes) { $null } else { [Math]::Round($pdbBytes / 1MB, 3) }
    }
}

$outputDirectory = Split-Path -Parent $outputPath
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $outputPath -Encoding utf8NoBOM

$direction = if ($changeBytes -lt 0) { 'smaller' } else { 'larger' }
$summaryFormat = "snow-shot-updater: {0:N0} bytes ({1:N3} MiB), {2:N3}% {3} than the " +
    "{4:N0}-byte baseline; PDB: {5}; report: {6}"
Write-Host ($summaryFormat -f
    $totalBytes, ($totalBytes / 1MB), [Math]::Abs($changePercent), $direction,
    $BaselineBytes, $(if ($null -eq $pdbBytes) { 'missing' } else { "${pdbBytes} bytes" }),
    $outputPath)
