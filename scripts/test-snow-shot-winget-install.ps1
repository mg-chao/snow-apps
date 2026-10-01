#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Tag = 'v1.1.5-beta',
    [string]$PreviousTag = 'v1.1.4-beta',
    [string]$Winget = 'winget.exe',
    [switch]$AllowUnrecognizedRelease,
    [ValidateSet('Full', 'Mini')][string]$Edition = 'Full'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Installs real release packages. Never run against a developer's existing installation.
if ($env:RUNNER_ENVIRONMENT -ne 'github-hosted' -or $env:RUNNER_OS -ne 'Windows') {
    throw 'Real installation tests require a disposable GitHub-hosted Windows runner.'
}
. (Join-Path $PSScriptRoot 'snow-shot-winget.ps1')
$product = Get-SnowShotEdition $Edition
$installerVariant = if ($Edition -eq 'Mini') { 'online' } else { 'offline' }
$version = Get-SnowShotWingetVersion $Tag
$previousVersion = Get-SnowShotWingetVersion $PreviousTag
if ($version -eq $previousVersion) { throw 'The upgrade fixture requires two different releases.' }
$registryPaths = @(
    "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$($product.Registry)",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\$($product.Registry)"
)
if (@($registryPaths | Where-Object { Test-Path -LiteralPath $_ }).Count) {
    throw 'Refusing to change an existing Snow Shot installation.'
}
$output = Join-Path $PSScriptRoot "../build/winget-install-test/$($product.Product)"
$null = New-Item -ItemType Directory -Force -Path $output
Start-Transcript -Path (Join-Path $output 'installation.log')
$app = $null
$reputationPolicy = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\System'
$savedReputationPolicy = $null
Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class WinGetWindowDiagnostics {
    private delegate bool Callback(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr window, Callback callback, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    public static string Read(IntPtr window) {
        var lines = new List<string>();
        Callback callback = (child, data) => {
            var text = new StringBuilder(2048);
            GetWindowText(child, text, text.Capacity);
            if (text.Length > 0) lines.Add(text.ToString());
            return true;
        };
        callback(window, IntPtr.Zero);
        EnumChildWindows(window, callback, IntPtr.Zero);
        return string.Join(Environment.NewLine, lines);
    }
    public static void ConfirmVerifiedFile(IntPtr window, string fileName) {
        if (Read(window).IndexOf(fileName, StringComparison.OrdinalIgnoreCase) < 0)
            throw new InvalidOperationException("The launch warning does not identify the verified installer.");
        IntPtr runButton = IntPtr.Zero;
        EnumChildWindows(window, (child, data) => {
            var text = new StringBuilder(2048);
            GetWindowText(child, text, text.Capacity);
            if (text.ToString().Replace("&", "").Trim() == "Run") runButton = child;
            return true;
        }, IntPtr.Zero);
        if (runButton == IntPtr.Zero || !PostMessage(runButton, 0x00F5, IntPtr.Zero, IntPtr.Zero))
            throw new InvalidOperationException("Could not acknowledge the verified installer's Run button.");
    }
}
'@
function Invoke-WingetBounded([string[]]$Arguments) {
    Write-Host "WinGet: $($Arguments -join ' ')"
    $info = [Diagnostics.ProcessStartInfo]::new($Winget)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in ($Arguments + @('--disable-interactivity', '--verbose-logs'))) {
        $info.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds(180)
    $dialog = $false
    while (-not $process.WaitForExit(1000)) {
        if ([DateTime]::UtcNow -ge $deadline) { break }
        $process.Refresh()
        if ($AllowUnrecognizedRelease -and $process.MainWindowTitle -eq 'Open File - Security Warning' -and
            $Arguments[0] -in @('install', 'upgrade')) {
            $manifestIndex = [Array]::IndexOf($Arguments, '--manifest')
            if ($manifestIndex -lt 0) { throw 'Installer consent requires a validated local manifest.' }
            $manifest = Get-Content -LiteralPath (Join-Path $Arguments[$manifestIndex + 1] "$($product.Winget).installer.yaml") -Raw
            $packageVersion = [regex]::Match($manifest, "(?m)^PackageVersion: '([^']+)'$").Groups[1].Value
            $expectedHash = [regex]::Match($manifest, '(?m)^    InstallerSha256: ([A-Fa-f0-9]{64})$').Groups[1].Value
            $fileName = "$($product.Product)-$packageVersion-windows-x64-$installerVariant.exe"
            $cachedInstaller = Join-Path $env:TEMP "WinGet/$($product.Winget).$packageVersion/$fileName"
            if ((Get-FileHash -LiteralPath $cachedInstaller -Algorithm SHA256).Hash -ine $expectedHash) {
                throw 'Refusing consent: the cached installer hash does not match the manifest.'
            }
            Write-Host ([WinGetWindowDiagnostics]::Read($process.MainWindowHandle))
            [WinGetWindowDiagnostics]::ConfirmVerifiedFile($process.MainWindowHandle, $fileName)
            Write-Host "Acknowledged Windows launch warning for verified $fileName."
            continue
        }
        if ($process.MainWindowTitle -eq 'Window Dialog') {
            if ($process.WaitForExit(2000)) { break }
            Write-Host ([WinGetWindowDiagnostics]::Read($process.MainWindowHandle))
            Add-Type -AssemblyName System.Windows.Forms
            $bounds = [Windows.Forms.Screen]::PrimaryScreen.Bounds
            $bitmap = [Drawing.Bitmap]::new($bounds.Width, $bounds.Height)
            $graphics = [Drawing.Graphics]::FromImage($bitmap)
            try {
                $graphics.CopyFromScreen($bounds.Location, [Drawing.Point]::Empty, $bounds.Size)
                $bitmap.Save((Join-Path $output 'blocking-dialog.png'), [Drawing.Imaging.ImageFormat]::Png)
            } finally { $graphics.Dispose(); $bitmap.Dispose() }
            $dialog = $true
            break
        }
        if ([DateTime]::UtcNow -ge $deadline) { break }
    }
    if (-not $process.HasExited) {
        Get-CimInstance Win32_Process | Where-Object { $_.Name -match 'winget|snow.?shot' } |
            Select-Object Name, ProcessId, ParentProcessId, CommandLine | Format-List | Out-Host
        Get-Process | Where-Object { $_.MainWindowTitle } |
            Select-Object ProcessName, Id, MainWindowTitle | Format-Table | Out-Host
        $process.Kill($true)
        $process.WaitForExit()
        Write-Host $stdout.GetAwaiter().GetResult()
        Write-Host $stderr.GetAwaiter().GetResult()
        if ($dialog) { throw 'WinGet displayed an interactive dialog during unattended installation; see the captured dialog text.' }
        throw "WinGet timed out after 180 seconds: $($Arguments -join ' ')"
    }
    Write-Host $stdout.GetAwaiter().GetResult()
    Write-Host $stderr.GetAwaiter().GetResult()
    return $process.ExitCode
}
function Invoke-WingetChecked([string[]]$Arguments) {
    $result = Invoke-WingetBounded $Arguments
    if ($result -ne 0) { throw "WinGet failed ($result): $($Arguments -join ' ')" }
}
function Assert-InstalledVersion([string]$Expected) {
    $entries = @($registryPaths | Where-Object { Test-Path -LiteralPath $_ } |
        ForEach-Object { Get-ItemProperty -LiteralPath $_ })
    if ($entries.Count -ne 1 -or $entries[0].DisplayVersion -cne $Expected -or
        $entries[0].DisplayName -cne $product.Name -or $entries[0].Publisher -cne 'Snow Apps') {
        throw "Installed registration does not match Snow Shot $Expected."
    }
    Invoke-WingetChecked @('list', '--name', $product.Name, '--exact', '--accept-source-agreements')
}
try {
    $current = New-SnowShotWingetManifest $Tag $output $Edition
    $previous = New-SnowShotWingetManifest $PreviousTag $output $Edition
    Invoke-WingetChecked @('validate', '--manifest', $current)
    Invoke-WingetChecked @('validate', '--manifest', $previous)
    if ($AllowUnrecognizedRelease) {
        # Explicit test-only consent for unsigned releases. The hosted VM is disposable;
        # preserve/restore its reputation policy and keep hashes and antivirus enabled.
        $existingPolicy = Get-ItemProperty -LiteralPath $reputationPolicy -ErrorAction SilentlyContinue
        $property = if ($existingPolicy) { $existingPolicy.PSObject.Properties['EnableSmartScreen'] } else { $null }
        $savedReputationPolicy = @{ Present = ($null -ne $property); Value = if ($property) { $property.Value } else { 0 } }
        if (-not (Test-Path -LiteralPath $reputationPolicy)) { $null = New-Item -Path $reputationPolicy -Force }
        $null = New-ItemProperty -LiteralPath $reputationPolicy -Name EnableSmartScreen -Value 0 -PropertyType DWord -Force
        Write-Host 'Temporarily allowing unsigned release fixtures in this disposable VM; hash verification remains enabled.'
    }
    & $Winget settings --enable LocalManifestFiles
    if ($LASTEXITCODE -ne 0) { throw 'Could not enable local manifests in the disposable runner.' }
    $installDirectory = Join-Path $env:RUNNER_TEMP "$($product.Name) custom installation"
    Invoke-WingetChecked @('install', '--manifest', $previous, '--silent', '--location',
        $installDirectory, '--accept-package-agreements', '--accept-source-agreements')
    Assert-InstalledVersion $previousVersion
    $executable = Join-Path $installDirectory "bin/$($product.Executable).exe"
    if (-not (Test-Path -LiteralPath $executable)) { throw 'Custom installation directory was ignored.' }
    if (Get-Process $product.Executable -ErrorAction SilentlyContinue) { throw 'Silent installation launched Snow Shot.' }
    $userData = Join-Path $env:APPDATA "$($product.Registry)/$($product.Executable)"
    $null = New-Item -ItemType Directory -Force -Path $userData
    $sentinel = Join-Path $userData 'winget-preservation-test.txt'
    $sentinelValue = [guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllText($sentinel, $sentinelValue)

    $app = Start-Process -FilePath $executable -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 5
    if ($app.HasExited) { throw 'The installed app did not remain running for the refusal test.' }
    $before = (Get-FileHash -LiteralPath $executable).Hash
    $result = Invoke-WingetBounded @('upgrade', '--manifest', $current, '--silent',
        '--accept-package-agreements', '--accept-source-agreements')
    if ($result -eq 0 -or $app.HasExited -or (Get-FileHash -LiteralPath $executable).Hash -cne $before) {
        throw 'Upgrade failed to preserve the running application.'
    }
    Assert-InstalledVersion $previousVersion
    Stop-Process -Id $app.Id
    $app.WaitForExit()
    $app = $null

    Invoke-WingetChecked @('upgrade', '--manifest', $current, '--silent',
        '--accept-package-agreements', '--accept-source-agreements')
    Assert-InstalledVersion $version
    if (-not (Test-Path -LiteralPath $executable)) { throw 'Upgrade did not preserve the custom directory.' }
    if ([IO.File]::ReadAllText($sentinel) -cne $sentinelValue) { throw 'Upgrade changed user data.' }
    if (Get-Process $product.Executable -ErrorAction SilentlyContinue) { throw 'Silent upgrade launched Snow Shot.' }
    $registration = @($registryPaths | Where-Object { Test-Path -LiteralPath $_ } |
        ForEach-Object { Get-ItemProperty -LiteralPath $_ })[0]
    if ($Edition -eq 'Full' -and $version -eq '1.1.5-beta' -and -not $registration.PSObject.Properties['QuietUninstallString']) {
        # This immutable historical release predates the quiet registration fix.
        # Validate its supported NSIS removal directly; a separate CPack integration
        # test verifies that newly built installers register WinGet's quiet command.
        Write-Host 'Legacy 1.1.5-beta has no QuietUninstallString; verifying its NSIS /S removal directly.'
        $uninstaller = $registration.UninstallString.Trim('"')
        $removal = Start-Process -FilePath $uninstaller -ArgumentList '/S' -WindowStyle Hidden -PassThru
        if (-not $removal.WaitForExit(30000) -or $removal.ExitCode -ne 0) { throw 'Legacy NSIS removal failed.' }
    } else {
        if (-not $registration.PSObject.Properties['QuietUninstallString']) { throw 'New installers must register QuietUninstallString.' }
        Invoke-WingetChecked @('uninstall', '--id', $product.Winget, '--exact', '--silent')
    }
    # NSIS may finish removal in a copied child after its original process exits.
    $removalDeadline = [DateTime]::UtcNow.AddSeconds(30)
    while (((Test-Path -LiteralPath $executable) -or @($registryPaths | Where-Object { Test-Path -LiteralPath $_ }).Count) -and
        [DateTime]::UtcNow -lt $removalDeadline) { Start-Sleep -Milliseconds 100 }
    if ((Test-Path -LiteralPath $executable) -or
        @($registryPaths | Where-Object { Test-Path -LiteralPath $_ }).Count) {
        throw 'Uninstall left the executable or registration behind.'
    }
    if ([IO.File]::ReadAllText($sentinel) -cne $sentinelValue) { throw 'Uninstall changed user data.' }
    Write-Output 'PASS: real WinGet install, detection, running-app refusal, upgrade, uninstall, and data preservation.'
} finally {
    if ($app -and -not $app.HasExited) { Stop-Process -Id $app.Id }
    if ($savedReputationPolicy) {
        if ($savedReputationPolicy.Present) {
            $null = New-ItemProperty -LiteralPath $reputationPolicy -Name EnableSmartScreen `
                -Value $savedReputationPolicy.Value -PropertyType DWord -Force
        } else {
            Remove-ItemProperty -LiteralPath $reputationPolicy -Name EnableSmartScreen -ErrorAction SilentlyContinue
        }
        Write-Host 'Restored the disposable VM reputation policy.'
    }
    $logDirectory = Join-Path $env:LOCALAPPDATA 'Packages/Microsoft.DesktopAppInstaller_8wekyb3d8bbwe/LocalState/DiagOutputDir'
    if (Test-Path -LiteralPath $logDirectory) {
        Copy-Item -LiteralPath $logDirectory -Destination (Join-Path $output 'winget-logs') -Recurse -Force
    }
    Stop-Transcript
}
