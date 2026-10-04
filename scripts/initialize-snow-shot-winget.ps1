#Requires -Version 7.0
# Provision the client version verified with manifest schema 1.12.0 on hosted runners.
[CmdletBinding()]
param([string]$ToolDirectory = (Join-Path $PSScriptRoot '../build/winget-client'))
$ErrorActionPreference = 'Stop'
$architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
if ($architecture -notin @('x64', 'arm64')) { throw 'WinGet provisioning requires Windows x64 or ARM64.' }
$requiredVersion = [version]'1.29.380'
$command = Get-Command winget.exe -ErrorAction SilentlyContinue
$installedVersion = if ($command) { (& $command.Source --version).Trim().TrimStart('v') } else { '0.0' }
if ([version]$installedVersion -lt $requiredVersion) {
    # Install the official bundle directly: Repair first tries to register the runner's
    # pre-provisioned bundle, which can be missing its native payload on hosted images.
    $null = New-Item -ItemType Directory -Force -Path $ToolDirectory
    $release = "https://github.com/microsoft/winget-cli/releases/download/v$requiredVersion"
    $bundle = Join-Path $ToolDirectory 'Microsoft.DesktopAppInstaller.msixbundle'
    $dependencies = Join-Path $ToolDirectory 'dependencies.zip'
    Invoke-WebRequest "$release/Microsoft.DesktopAppInstaller_8wekyb3d8bbwe.msixbundle" -OutFile $bundle
    Invoke-WebRequest "$release/DesktopAppInstaller_Dependencies.zip" -OutFile $dependencies
    Expand-Archive -LiteralPath $dependencies -DestinationPath (Join-Path $ToolDirectory 'dependencies') -Force
    $dependencyPaths = @(Get-ChildItem (Join-Path $ToolDirectory "dependencies/$architecture") -Filter '*.appx' |
        Select-Object -ExpandProperty FullName)
    if ($dependencyPaths.Count -eq 0) { throw "The WinGet release has no $architecture dependencies." }
    Add-AppxPackage -Path $bundle -DependencyPath $dependencyPaths
}
$package = Get-AppxPackage Microsoft.DesktopAppInstaller | Sort-Object Version -Descending | Select-Object -First 1
$executable = if ($package) { Join-Path $package.InstallLocation 'winget.exe' } else { $command.Source }
$actualVersion = (& $executable --version).Trim().TrimStart('v')
if ($LASTEXITCODE -ne 0 -or [version]$actualVersion -lt $requiredVersion) {
    throw "WinGet $requiredVersion or newer is required; found $actualVersion."
}
Write-Output $executable
