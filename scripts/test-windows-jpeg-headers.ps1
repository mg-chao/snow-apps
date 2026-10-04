[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$QtJpegIncludeDirectory,
    [ValidateSet("x64", "arm64")]
    [string[]]$Architecture = @("x64", "arm64")
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "snow-build-environment.ps1")

$qtJpegInclude = (Resolve-Path -LiteralPath $QtJpegIncludeDirectory).Path
if (-not (Test-Path -LiteralPath (Join-Path $qtJpegInclude "jconfig.h") -PathType Leaf)) {
    throw "Qt's bundled JPEG headers were not found under $qtJpegInclude."
}
$fixture = Join-Path $script:SnowRepoRoot "snow_image/tests/jpeg_header_compatibility_tests.cpp"
$outputRoot = Join-Path $script:SnowRepoRoot "build/windows-jpeg-header-tests/$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$unwrappedFixture = Join-Path $outputRoot "jpeg_header_unwrapped.cpp"
$unwrappedSource = [IO.File]::ReadAllText($fixture).Replace(
    '#include "../src/codecs/jpeg_headers.h"',
    "#include <cstddef>`n#include <cstdio>`n#include <jpeglib.h>")
[IO.File]::WriteAllText($unwrappedFixture, $unwrappedSource)

foreach ($targetArchitecture in $Architecture) {
    $msvcTools = Add-SnowMsvcToolsToPath -Architecture $targetArchitecture
    $compiler = Join-Path $msvcTools "cl.exe"
    $target = Get-SnowWindowsTarget -Architecture $targetArchitecture
    $vcpkgInclude = Join-Path (Join-Path $target.InstalledRoot $target.Triplet) "include"
    $arguments = @("/nologo", "/c", "/std:c++20", "/EHsc", "/W4", "/WX",
        "/I$(Join-Path $script:SnowRepoRoot 'snow_memory/include')")

    # This is the original include sequence, without the private boundary.
    # It must reject the RPC/JPEG type conflict before the corrected fixture.
    $beforeLog = Join-Path $outputRoot "unwrapped-$targetArchitecture.log"
    $beforeOutput = & $compiler @arguments "/I$qtJpegInclude" /DSNOW_TEST_QT_BUNDLED_JPEG=1 `
        $unwrappedFixture "/Fo$(Join-Path $outputRoot "unwrapped-$targetArchitecture.obj")" 2>&1
    $beforeExitCode = $LASTEXITCODE
    $beforeOutput | Set-Content -LiteralPath $beforeLog -Encoding utf8
    if ($beforeExitCode -eq 0 -or ($beforeOutput -join "`n") -notmatch "C2371") {
        throw "The unwrapped JPEG include sequence did not reproduce C2371. See $beforeLog."
    }

    & $compiler @arguments "/I$qtJpegInclude" /DSNOW_TEST_QT_BUNDLED_JPEG=1 `
        $fixture "/Fo$(Join-Path $outputRoot "qt-jpeg-$targetArchitecture.obj")"
    if ($LASTEXITCODE -ne 0) {
        throw "The bundled Qt JPEG ABI fixture failed for $targetArchitecture."
    }
    & $compiler @arguments "/I$vcpkgInclude" `
        $fixture "/Fo$(Join-Path $outputRoot "vcpkg-jpeg-$targetArchitecture.obj")"
    if ($LASTEXITCODE -ne 0) {
        throw "The vcpkg JPEG ABI fixture failed for $targetArchitecture."
    }
    Write-Output "JPEG header compatibility passed for ${targetArchitecture}: Qt int, vcpkg byte, RPC byte."
}
Write-Output "Fixture objects and original failure logs: $outputRoot"
