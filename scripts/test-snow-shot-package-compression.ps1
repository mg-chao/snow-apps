[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

# Import the real helper without executing the package script's release actions.
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot "package-snow-shot.ps1"), [ref]$null, [ref]$parseErrors)
Require ($parseErrors.Count -eq 0) "The package script must parse."
$definition = $ast.Find({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq "New-DeterministicZip"
}, $false)
Require ($null -ne $definition) "The package script must define New-DeterministicZip."
. ([scriptblock]::Create($definition.Extent.Text))

$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) (
    "snow-package-compression-" + [guid]::NewGuid().ToString("N"))
try {
    $source = Join-Path $testRoot "source"
    $null = New-Item -ItemType Directory -Path (Join-Path $source "nested") -Force
    $random = [Random]::new(1729)
    $bytes = [byte[]]::new(65536)
    $random.NextBytes($bytes)
    [System.IO.File]::WriteAllBytes((Join-Path $source "nested/data.bin"), $bytes)
    [System.IO.File]::WriteAllText((Join-Path $source "nested/unicode-雪.txt"),
        ("Release payload: capture, history and settings.`n" * 8192),
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllBytes((Join-Path $source "empty.txt"), [byte[]]::new(0))
    $first = Join-Path $testRoot "first.zip"
    $second = Join-Path $testRoot "second.zip"
    New-DeterministicZip -SourceDirectory $source -Destination $first
    Get-ChildItem -LiteralPath $source -File -Recurse | ForEach-Object {
        $_.LastWriteTimeUtc = [DateTime]::UtcNow.AddDays(-7)
    }
    New-DeterministicZip -SourceDirectory $source -Destination $second
    Require ((Get-FileHash -LiteralPath $first).Hash -ceq
        (Get-FileHash -LiteralPath $second).Hash) "ZIP output must ignore source timestamps."

    Add-Type -AssemblyName System.IO.Compression
    $stream = [System.IO.File]::OpenRead($first)
    $archive = [System.IO.Compression.ZipArchive]::new(
        $stream, [System.IO.Compression.ZipArchiveMode]::Read, $false)
    try {
        Require ($archive.Entries.Count -eq 3) "Every payload file must occur exactly once."
        foreach ($entry in $archive.Entries) {
            Require (-not $entry.FullName.Contains('\')) "ZIP names must use forward slashes."
            Require ($entry.LastWriteTime.DateTime -eq [DateTime]::new(2000, 1, 1)) (
                "ZIP entries must use the reproducible epoch.")
            $expectedPath = Join-Path $source $entry.FullName
            Require (Test-Path -LiteralPath $expectedPath -PathType Leaf) (
                "ZIP must preserve Unicode paths and file names.")
            $input = $entry.Open()
            $output = [System.IO.MemoryStream]::new()
            try {
                $input.CopyTo($output)
                $expectedHash = (Get-FileHash -LiteralPath $expectedPath -Algorithm SHA256).Hash
                $actualHash = [Convert]::ToHexString(
                    [System.Security.Cryptography.SHA256]::HashData($output.ToArray()))
                Require ($actualHash -ceq $expectedHash) "Decompressed bytes must match the source."
            }
            finally { $output.Dispose(); $input.Dispose() }
            if ($entry.Length -gt 0) {
                Require ($entry.CompressedLength -gt 0) "Nonempty files must remain readable."
            }
        }
        $textEntry = $archive.GetEntry("nested/unicode-雪.txt")
        Require ($textEntry.CompressedLength -lt $textEntry.Length / 10) (
            "Release text must be compressed, not stored verbatim.")
    }
    finally { $archive.Dispose(); $stream.Dispose() }
    Write-Host "Package ZIP round-trip and reproducibility checks passed."
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
        $temporaryPrefix = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd(
            [System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
        Require ($resolvedTestRoot.StartsWith($temporaryPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) "Cleanup must stay in the temporary directory."
        Require ((Split-Path -Leaf $resolvedTestRoot) -match '^snow-package-compression-[0-9a-f]{32}$') (
            "Cleanup must target this test's GUID directory.")
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
