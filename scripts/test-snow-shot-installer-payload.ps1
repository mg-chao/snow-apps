[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][Alias('Helper')][string]$Extractor,
    [Parameter(Mandatory = $true)][string]$Encoder,
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [string]$ExecutableFixture
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-installer-payload.ps1')
$extractorPath = (Resolve-Path -LiteralPath $Extractor).Path
$encoderPath = (Resolve-Path -LiteralPath $Encoder).Path
$fixturePath = if ($ExecutableFixture) { (Resolve-Path -LiteralPath $ExecutableFixture).Path } else { $extractorPath }
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('snow-installer-payload-' + [guid]::NewGuid().ToString('N'))
$junctions = [Collections.Generic.List[string]]::new()
$checks = [Collections.Generic.List[string]]::new()

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Invoke-Extractor([string]$ArchivePath, [string]$Destination, [string]$Digest,
    [string]$Entry = '', [switch]$SelectEntry) {
    $start = [Diagnostics.ProcessStartInfo]::new($extractorPath)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    foreach ($argument in @($ArchivePath, $Destination, $Digest)) {
        $start.ArgumentList.Add($argument)
    }
    if ($Entry -or $SelectEntry) { $start.ArgumentList.Add($Entry) }
    $process = [Diagnostics.Process]::Start($start)
    try {
        Require ($process.WaitForExit(30000)) 'The native payload decoder must finish within 30 seconds.'
        return $process.ExitCode
    }
    finally {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}

function Get-Digest([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Require-Payload([string]$Source, [string]$Destination) {
    $files = @(Get-ChildItem -LiteralPath $Source -Recurse -File -Force)
    $actual = @(Get-ChildItem -LiteralPath $Destination -Recurse -File -Force)
    Require ($files.Count -eq $actual.Count) 'The decoder must preserve the complete file inventory.'
    foreach ($file in $files) {
        $relative = [IO.Path]::GetRelativePath($Source, $file.FullName)
        $target = Join-Path $Destination $relative
        Require (Test-Path -LiteralPath $target -PathType Leaf) "Missing extracted file: $relative"
        Require ((Get-Digest $file.FullName) -ceq (Get-Digest $target)) "Extracted bytes differ: $relative"
        Require (((Get-Item -LiteralPath $target -Force).Attributes -band [IO.FileAttributes]::Temporary) -eq 0) (
            "Installed files must not retain temporary staging attributes: $relative")
    }
    Require (@(Get-ChildItem -LiteralPath $Destination -Recurse -File -Filter '*.snow-payload-*.tmp' -Force).Count -eq 0) (
        'Extraction must not leave temporary output files.')
}

try {
    $source = Join-Path $testRoot 'source'
    $null = New-Item -ItemType Directory -Path (Join-Path $source 'bin') -Force
    $null = New-Item -ItemType Directory -Path (Join-Path $source '雪/empty-directory') -Force
    Copy-Item -LiteralPath $fixturePath -Destination (Join-Path $source 'bin/application.exe')
    [IO.File]::WriteAllText((Join-Path $source '雪/unicode-文件-😀.txt'),
        "Payload Unicode: 雪 文件 😀`n", [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllBytes((Join-Path $source 'empty.txt'), [byte[]]::new(0))
    $bytes = [byte[]]::new(262144)
    [Random]::new(1729).NextBytes($bytes)
    [IO.File]::WriteAllBytes((Join-Path $source 'random.bin'), $bytes)
    $first = New-SnowShotInstallerPayload -Stage $source -Archive (Join-Path $testRoot 'first.7z') `
        -Architecture $Architecture -Encoder $encoderPath
    Require ($first.FileCount -eq 4) 'The archive creator must report all input files.'
    Require ($first.Methods -match 'LZMA2') 'The payload must use LZMA2.'
    Get-ChildItem -LiteralPath $source -Recurse -Force | ForEach-Object {
        $_.LastWriteTimeUtc = [DateTime]::UtcNow.AddDays(-7)
    }
    $second = New-SnowShotInstallerPayload -Stage $source -Archive (Join-Path $testRoot 'second.7z') `
        -Architecture $Architecture -Encoder $encoderPath
    Require ($first.Sha256 -ceq $second.Sha256) 'Payload archive bytes must ignore source timestamps.'
    $checks.Add('Deterministic pinned executable-aware archive creation')

    $destination = Join-Path $testRoot 'installed'
    Require ((Invoke-Extractor $first.Archive $destination $first.Sha256.ToUpperInvariant()) -eq 0) (
        'The native decoder must accept the matching archive SHA-256.')
    Require-Payload $source $destination
    Require (Test-Path -LiteralPath (Join-Path $destination '雪/empty-directory') -PathType Container) (
        'Empty directories must survive extraction.')
    $replacement = Join-Path $destination '雪/unicode-文件-😀.txt'
    [IO.File]::WriteAllText($replacement, 'previous version')
    (Get-Item -LiteralPath $replacement).IsReadOnly = $true
    Require ((Invoke-Extractor $first.Archive $destination $first.Sha256) -eq 0) (
        'An existing read-only owned file must be replaced safely.')
    Require-Payload $source $destination
    $checks.Add('Exact Unicode, empty file/directory, overwrite and attributes round trip')

    $selectedDestination = Join-Path $testRoot 'selected-output'
    Require ((Invoke-Extractor $first.Archive $selectedDestination $first.Sha256 'bin/application.exe') -eq 0) (
        'The bootstrap extraction mode must decode the requested file.')
    Require (@(Get-ChildItem -LiteralPath $selectedDestination -Recurse -File).Count -eq 1) (
        'Bootstrap extraction must write only the selected file.')
    Require ((Get-Digest (Join-Path $selectedDestination 'bin/application.exe')) -ceq
        (Get-Digest (Join-Path $source 'bin/application.exe'))) 'Bootstrap file bytes must match the stage.'
    foreach ($selection in @('', 'missing.exe', '../escape.txt', '雪/empty-directory')) {
        $missingDestination = Join-Path $testRoot ('missing-output-' + [guid]::NewGuid().ToString('N'))
        Require ((Invoke-Extractor $first.Archive $missingDestination $first.Sha256 $selection -SelectEntry) -eq 4) (
            'Invalid, absent or directory bootstrap selections must fail validation.')
        Require (-not (Test-Path -LiteralPath $missingDestination)) 'Selection failure must precede writes.'
    }
    $checks.Add('Single-entry bootstrap extraction validates selection and writes no unrelated files')

    $wrongHashDestination = Join-Path $testRoot 'wrong-hash'
    Require ((Invoke-Extractor $first.Archive $wrongHashDestination ('0' * 64)) -eq 2) (
        'A wrong archive digest must fail with the integrity exit code.')
    Require (-not (Test-Path -LiteralPath $wrongHashDestination)) 'Digest failure must precede output creation.'
    $truncated = Join-Path $testRoot 'truncated.7z'
    $archiveBytes = [IO.File]::ReadAllBytes($first.Archive)
    [IO.File]::WriteAllBytes($truncated, $archiveBytes[0..([int]($archiveBytes.Length / 2))])
    $truncatedDestination = Join-Path $testRoot 'truncated-output'
    Require ((Invoke-Extractor $truncated $truncatedDestination $first.Sha256) -eq 2) (
        'Truncation must fail the embedded expected digest.')
    Require ((Invoke-Extractor $truncated $truncatedDestination (Get-Digest $truncated)) -ne 0) (
        'A malformed archive must fail decoding even if its supplied hash matches.')
    $corrupt = Join-Path $testRoot 'corrupt.7z'
    $archiveBytes[50] = $archiveBytes[50] -bxor 0x40
    [IO.File]::WriteAllBytes($corrupt, $archiveBytes)
    Require ((Invoke-Extractor $corrupt (Join-Path $testRoot 'corrupt-output') (Get-Digest $corrupt)) -ne 0) (
        'A corrupt compressed stream must not report success.')
    $checks.Add('Digest, truncation and corrupt-stream rejection')

    foreach ($unsafeName in @('../escape.txt', 'C:/escape.txt', 'NUL.txt', 'trailing.')) {
        $unsafe = Join-Path $testRoot ('unsafe-' + [guid]::NewGuid().ToString('N') + '.7z')
        Copy-Item -LiteralPath $first.Archive -Destination $unsafe
        $renameOutput = @(& $encoderPath rn -sccUTF-8 -bso0 -bsp0 $unsafe empty.txt $unsafeName 2>&1)
        Require ($LASTEXITCODE -eq 0) "Unable to construct the unsafe-name archive fixture: $renameOutput"
        $unsafeDestination = Join-Path $testRoot ('unsafe-output-' + [guid]::NewGuid().ToString('N'))
        Require ((Invoke-Extractor $unsafe $unsafeDestination (Get-Digest $unsafe)) -eq 4) (
            "Unsafe archive path must fail validation: $unsafeName")
        Require (-not (Test-Path -LiteralPath $unsafeDestination)) 'All entry paths must be checked before writes.'
        Require ((Invoke-Extractor $unsafe $unsafeDestination (Get-Digest $unsafe) 'bin/application.exe') -eq 4) (
            'Single-entry mode must still reject unsafe unselected archive entries.')
    }
    $checks.Add('Traversal, absolute path, device-name and trailing-dot archive rejection')

    $lockedDestination = Join-Path $testRoot 'locked-output'
    $null = New-Item -ItemType Directory -Path (Join-Path $lockedDestination 'bin') -Force
    $lockedPath = Join-Path $lockedDestination 'bin/application.exe'
    [IO.File]::WriteAllText($lockedPath, 'unchanged locked version')
    $locked = [IO.File]::Open($lockedPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try {
        Require ((Invoke-Extractor $first.Archive $lockedDestination $first.Sha256) -eq 5) (
            'A locked output must return a failed installation result.')
    }
    finally { $locked.Dispose() }
    Require ([IO.File]::ReadAllText($lockedPath) -ceq 'unchanged locked version') (
        'A failed replacement must preserve the previous owned file bytes.')
    Require (@(Get-ChildItem -LiteralPath $lockedDestination -Recurse -File -Filter '*.snow-payload-*.tmp').Count -eq 0) (
        'Failed replacement must remove its temporary file.')
    $checks.Add('Locked-output failure preserves existing bytes and cleans temporary files')

    $outside = Join-Path $testRoot 'outside'
    $reparseDestination = Join-Path $testRoot 'reparse-output'
    $null = New-Item -ItemType Directory -Path $outside, $reparseDestination -Force
    $destinationJunction = Join-Path $reparseDestination 'bin'
    $null = New-Item -ItemType Junction -Path $destinationJunction -Target $outside
    $junctions.Add($destinationJunction)
    Require ((Invoke-Extractor $first.Archive $reparseDestination $first.Sha256) -eq 5) (
        'Extraction must reject a reparse-point descendant of the chosen installation root.')
    Require (@(Get-ChildItem -LiteralPath $outside -Force).Count -eq 0) 'Reparse failure must not write outside the root.'
    $sourceJunction = Join-Path $source 'reparse-input'
    $null = New-Item -ItemType Junction -Path $sourceJunction -Target $outside
    $junctions.Add($sourceJunction)
    $reparseRejected = $false
    try {
        $null = New-SnowShotInstallerPayload -Stage $source -Archive (Join-Path $testRoot 'reparse-source.7z') `
            -Architecture $Architecture -Encoder $encoderPath
    }
    catch { $reparseRejected = $_.Exception.Message -match 'reparse points' }
    Require $reparseRejected 'The creator must reject staged reparse points before compression.'
    Remove-Item -LiteralPath $sourceJunction -Force
    $checks.Add('Input and destination-descendant reparse rejection')

    foreach ($name in @('trailing.', 'trailing ')) {
        $invalidStage = Join-Path $testRoot ('invalid-source-' + [guid]::NewGuid().ToString('N'))
        $null = New-Item -ItemType Directory -Path $invalidStage
        [IO.File]::WriteAllText(('\\?\' + (Join-Path $invalidStage $name)), 'invalid path fixture')
        $rejected = $false
        try {
            $null = New-SnowShotInstallerPayload -Stage $invalidStage -Archive (Join-Path $testRoot 'invalid.7z') `
                -Architecture $Architecture -Encoder $encoderPath
        }
        catch { $rejected = $_.Exception.Message -match 'unsupported path' }
        Require $rejected "The creator must reject an actual trailing-dot/space input path: $name"
    }
    $checks.Add('Creator rejects actual Windows trailing-dot/space files')
    $oversizedStage = Join-Path $testRoot 'oversized-source'
    $null = New-Item -ItemType Directory -Path $oversizedStage
    $oversizedFile = [IO.File]::Create((Join-Path $oversizedStage 'too-large.bin'))
    try { $oversizedFile.SetLength(256MB + 1) }
    finally { $oversizedFile.Dispose() }
    $oversizedRejected = $false
    try {
        $null = New-SnowShotInstallerPayload -Stage $oversizedStage -Archive (Join-Path $testRoot 'too-large.7z') `
            -Architecture $Architecture -Encoder $encoderPath
    }
    catch { $oversizedRejected = $_.Exception.Message -match '256 MiB decoder block contract' }
    Require $oversizedRejected 'Oversized files must fail before creating a payload that exceeds decoder limits.'
    Require (-not (Test-Path -LiteralPath (Join-Path $testRoot 'too-large.7z'))) (
        'Oversized payload rejection must precede compression.')
    $checks.Add('Creator file/block memory contract enforced before compression')
    Write-Host ("Installer payload checks passed: {0}." -f ($checks -join '; '))
}
finally {
    foreach ($junction in $junctions) {
        if (Test-Path -LiteralPath $junction) { Remove-Item -LiteralPath $junction -Force }
    }
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedRoot = [IO.Path]::GetFullPath($testRoot)
        $temporaryPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') +
            [IO.Path]::DirectorySeparatorChar
        Require ($resolvedRoot.StartsWith($temporaryPrefix, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedRoot) -match '^snow-installer-payload-[0-9a-f]{32}$') (
            'Test cleanup must remain within its temporary GUID directory.')
        Remove-Item -LiteralPath ('\\?\' + $resolvedRoot) -Recurse -Force
    }
}
