#Requires -Version 7.2
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
. (Join-Path $PSScriptRoot 'snow-shot-ocr-native-validation.ps1')
. (Join-Path $PSScriptRoot 'snow-shot-native-validation.ps1')

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$root = Join-Path $script:SnowRepoRoot ('build/ocr-runtime-report-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
try {
    $files = @([pscustomobject]@{ name = 'fixture.exe'; size = 128; sha256 = 'a' * 64 })
    function Get-SnowWindowsHostArchitecture { return $script:FixtureHostArchitecture }
    foreach ($architecture in @('x64', 'arm64')) {
        $archive = Join-Path $root "snow-ocr-runtime-1.0.9-windows-$architecture.zip"
        [IO.File]::WriteAllText($archive, 'exact prepared runtime archive bytes')
        $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
        foreach ($hostArchitecture in @('x64', 'arm64')) {
            $script:FixtureHostArchitecture = $hostArchitecture
            foreach ($passed in @($false, $true)) {
                $report = $null
                $rejected = $false
                try {
                    $report = New-SnowOcrRuntimeReleaseReport -RuntimeVersion '1.0.9' -Architecture $architecture `
                        -ArchivePath $archive -UploadUrl 'https://example.test/runtime.zip' -Files $files -NativeProbePassed $passed
                } catch { $rejected = $true }
                if ($passed -and $hostArchitecture -cne $architecture) {
                    Require $rejected 'A cross host cannot assert that it completed a native OCR probe.'
                    continue
                }
                Require (-not $rejected) 'A truthful OCR runtime report was rejected.'
                Require ($report.NativeValidationRequired -is [bool] -and
                    $report.NativeValidationRequired -eq (-not $passed) -and
                    $report.NativeValidation.Passed -is [bool] -and $report.NativeValidation.Passed -eq $passed -and
                    $report.NativeValidation.Platform -ceq "windows-$architecture" -and
                    $report.NativeValidation.HostPlatform -ceq "windows-$hostArchitecture") 'Runtime proof must describe the actual native probe and physical host.'
                $proof = $report.NativeValidation.Artifacts[0]
                Require ($proof.Name -ceq [IO.Path]::GetFileName($archive) -and
                    $proof.Bytes -eq (Get-Item -LiteralPath $archive).Length -and $proof.Sha256 -ceq $hash -and
                    $report.Archive.name -ceq $proof.Name -and $report.Archive.size -eq $proof.Bytes -and
                    $report.Archive.sha256 -ceq $proof.Sha256) 'Proof must bind the exact archive bytes written to the runtime report.'
                $path = Join-Path $root 'runtime.manifest.json'
                $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $path -Encoding utf8NoBOM
                $saved = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
                $accepted = $true
                try { $null = Assert-SnowShotNativeValidation -Manifest $saved -ArtifactPath $archive -Platform "windows-$architecture" }
                catch { $accepted = $false }
                Require ($accepted -eq $passed) 'The production exact-byte proof gate must reject unprobed/cross reports and accept completed native probes.'
                if ($passed) {
                    [IO.File]::AppendAllText($archive, 'changed')
                    $accepted = $true
                    try { $null = Assert-SnowShotNativeValidation -Manifest $saved -ArtifactPath $archive -Platform "windows-$architecture" }
                    catch { $accepted = $false }
                    Require (-not $accepted) 'Runtime proof cannot validate different archive bytes.'
                    [IO.File]::WriteAllText($archive, 'exact prepared runtime archive bytes')
                }
            }
        }
    }
    Write-Output 'OCR runtime report native provenance and exact-byte proof tests passed.'
} finally {
    $allowed = [IO.Path]::GetFullPath((Join-Path $script:SnowRepoRoot 'build')).TrimEnd('\') + '\'
    $resolved = [IO.Path]::GetFullPath($root)
    if (-not $resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe runtime report fixture cleanup.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
