#requires -Version 7.2
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')

$vcpkgRoot = Join-Path $script:SnowRepoRoot '.tools/vcpkg'
$vcpkg = Join-Path $vcpkgRoot 'vcpkg.exe'
if (-not (Test-Path -LiteralPath $vcpkg -PathType Leaf)) {
    throw 'Run bootstrap before verifying the native vcpkg compiler environments.'
}
$testRoot = Join-Path $script:SnowRepoRoot ('build/vcpkg-windows-environment-' + [guid]::NewGuid().ToString('N'))
$saved = @{}
foreach ($name in @('PATH', 'INCLUDE', 'LIB', 'VSINSTALLDIR', 'VCINSTALLDIR', 'VCToolsInstallDir',
        'WindowsSdkDir', 'WindowsSDKVersion', 'SNOW_MSVC_HOST_ARCHITECTURE',
        'SNOW_MSVC_TARGET_ARCHITECTURE', 'VSCMD_ARG_HOST_ARCH', 'VSCMD_ARG_TGT_ARCH', 'VCPKG_ROOT')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $null = New-Item -ItemType Directory -Force -Path $testRoot
    # Start from the other architecture's environment, matching an x64 host-tool
    # port built during ARM dependency provisioning (and the reverse on ARM).
    $hostArchitecture = Get-SnowWindowsHostArchitecture
    $parentArchitecture = if ($hostArchitecture -eq 'x64') { 'arm64' } else { 'x64' }
    Add-SnowMsvcToolsToPath -Architecture $parentArchitecture | Out-Null
    $env:VCPKG_ROOT = $vcpkgRoot
    $probe = @'
param([string]$Architecture, [string]$Runtime, [string]$RepositoryRoot, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path $RepositoryRoot 'scripts/snow-build-environment.ps1')
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
if ($compiler -notmatch "[\\/]Host(?:x64|arm64)[\\/]$Architecture[\\/]cl\.exe$") {
    throw "Compiler PATH does not match the port architecture: $compiler"
}
foreach ($directory in ($env:LIB -split ';' | Where-Object { $_ })) {
    if ($directory -notmatch "[\\/]$Architecture[\\/]*$") {
        throw "Port $Architecture inherited another architecture's LIB: $directory"
    }
}
$source = Join-Path $OutputDirectory "$Architecture-$Runtime.c"
$binary = Join-Path $OutputDirectory "$Architecture-$Runtime.exe"
$object = Join-Path $OutputDirectory "$Architecture-$Runtime.obj"
[IO.File]::WriteAllText($source, "#include <stdio.h>`nint main(void) { puts(`"host/target CRT probe`" ); return 0; }`n")
& $compiler /nologo /WX "/$Runtime" "/Fe$binary" "/Fo$object" $source
if ($LASTEXITCODE -ne 0) { throw 'Port compiler could not compile/link the CRT probe.' }
$expectedMachine = if ($Architecture -eq 'arm64') { 0xAA64 } else { 0x8664 }
if ((Get-SnowPeMachine -Path $binary) -ne $expectedMachine) { throw 'The probe PE machine does not match its port.' }
if ($Architecture -eq (Get-SnowWindowsHostArchitecture)) {
    & $binary
    if ($LASTEXITCODE -ne 0) { throw 'The native host-tool CRT probe did not execute.' }
}
Write-Output "Verified $Architecture /$Runtime compiler, CRT libraries and PE machine."
'@
    $probePath = Join-Path $testRoot 'probe.ps1'
    [IO.File]::WriteAllText($probePath, $probe, [Text.UTF8Encoding]::new($false))
    $powerShell = (Get-Command pwsh.exe -ErrorAction Stop).Source
    foreach ($architecture in @('x64', 'arm64')) {
        foreach ($variant in @('', '-static')) {
            $triplet = "$architecture-windows$variant"
            $runtime = if ($variant) { 'MT' } else { 'MD' }
            $command = 'call "{0}" -NoProfile -File "{1}" -Architecture {2} -Runtime {3} -RepositoryRoot "{4}" -OutputDirectory "{5}"' -f
                $powerShell, $probePath, $architecture, $runtime, $script:SnowRepoRoot, $testRoot
            & $vcpkg env $command "--vcpkg-root=$vcpkgRoot" "--triplet=$triplet" `
                "--host-triplet=$hostArchitecture-windows" `
                "--overlay-triplets=$(Join-Path $script:SnowRepoRoot 'cmake/vcpkg-overlay-triplets')"
            if ($LASTEXITCODE -ne 0) { throw "Native vcpkg environment regression failed: $triplet" }
        }
    }
    Write-Output 'Windows vcpkg host/target environment regression passed.'
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    $buildRoot = [IO.Path]::GetFullPath((Join-Path $script:SnowRepoRoot 'build')) + [IO.Path]::DirectorySeparatorChar
    $resolved = [IO.Path]::GetFullPath($testRoot)
    if (-not $resolved.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe native environment fixture cleanup path.'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
