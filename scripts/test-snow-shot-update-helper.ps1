[CmdletBinding()]
param(
    [string]$HelperPath = 'build/snow-shot-msvc-release/snow_shot/Release/snow-shot-updater.exe',
    [ValidateSet('None', 'Cancel', 'Approve')][string]$ElevationAction = 'None'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$helper = (Resolve-Path -LiteralPath $HelperPath).Path
$testElevation = $ElevationAction -ne 'None'
$registrationPath = 'Software\Snow Apps\SnowShot'
$uninstallPath = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\SnowShot'
$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry32)
$existingEmptyKeys = @{}
if ($testElevation) {
    if ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run the elevation canary from a non-elevated shell.'
    }
    foreach ($path in @($registrationPath, $uninstallPath)) {
        $existing = $registry.OpenSubKey($path)
        if ($null -ne $existing) {
            $empty = $existing.ValueCount -eq 0 -and $existing.SubKeyCount -eq 0
            $existing.Dispose()
            if (-not $empty) { throw "Refusing to replace an existing per-user registration: $path" }
            $existingEmptyKeys[$path] = $true
        }
    }
}
$testRoot = Join-Path $repo "build/update-helper-tests-$([guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory -Path $testRoot
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
$null = Set-SnowBuildEnvironment -Preset 'snow-shot-msvc-release'
$fixture = Join-Path $testRoot 'fixture.exe'
& cl /nologo /std:c++20 /W4 /WX /O2 /MT /DUNICODE /D_UNICODE "/Fe:$fixture" "/Fo:$testRoot/fixture.obj" `
    (Join-Path $repo 'snow_shot/tests/update_helper_fixture.cpp') /link advapi32.lib
if ($LASTEXITCODE -ne 0) { throw 'Helper fixture compilation failed.' }
function Write-CanaryFile([string]$Path, [string]$Content) {
    [IO.File]::WriteAllText($Path, $Content, [Text.UTF8Encoding]::new($false))
}
function Wait-CanaryFile([string]$Path) {
    $deadline = [DateTime]::UtcNow.AddSeconds($(if ($testElevation) { 180 } else { 45 }))
    while (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        if ($Path.EndsWith('.canary-handoff.txt') -and $null -ne $parent -and $parent.HasExited) {
            throw "Helper parent exited before handoff with code $($parent.ExitCode): $Path"
        }
        if ([DateTime]::UtcNow -gt $deadline) { throw "Helper canary timed out: $Path" }
        Start-Sleep -Milliseconds 100
    }
    return Get-Content -LiteralPath $Path -Raw
}
$variants = if ($testElevation) { @('offline') } else { @('portable', 'online', 'offline') }
$modes = switch ($ElevationAction) { 'Cancel' { @('elevation-cancel') } 'Approve' { @('recover') } default { @('cancel', 'recover') } }
foreach ($variant in $variants) {
    foreach ($mode in $modes) {
        $root = Join-Path $testRoot "$variant $mode"
        $bin = Join-Path $root 'bin'
        $backup = Join-Path $root '.snow-shot-update/backup/bin'
        $null = New-Item -ItemType Directory -Path $bin, $backup
        Copy-Item -LiteralPath $fixture -Destination (Join-Path $bin 'snow_shot.exe')
        Copy-Item -LiteralPath $helper -Destination (Join-Path $bin 'snow-shot-updater.exe')
        $owned = @('snow_shot.exe', 'snow-shot-updater.exe') | ForEach-Object {
            $path = Join-Path $bin $_
            @{ path = "bin/$_"; size = (Get-Item -LiteralPath $path).Length; sha256 = (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() }
        }
        Write-CanaryFile (Join-Path $root 'snow-shot-installation.json') (@{
            schema = 1; variant = $variant; version = '1.0.0-alpha'; files = @($owned)
        } | ConvertTo-Json -Depth 5)
        Write-CanaryFile (Join-Path $root 'user-note.txt') 'preserve user data'
        Write-CanaryFile (Join-Path $bin 'recovered.txt') 'incomplete replacement'
        $saved = Join-Path $backup 'recovered.txt'
        Write-CanaryFile $saved 'previous payload'
        $journal = Join-Path $root '.snow-shot-update/journal.json'
        Write-CanaryFile $journal (@{
            schema = 1; state = 'applying'; previousVersion = '1.0.0-alpha'; version = '1.0.0-beta'
            files = @(@{ path = 'bin/recovered.txt'; existed = $true; size = (Get-Item -LiteralPath $saved).Length; sha256 = (Get-FileHash -LiteralPath $saved).Hash.ToLowerInvariant() })
        } | ConvertTo-Json -Depth 5)
        $parent = $null
        $originalAcl = $null
        $registered = $false
        $results = $root
        try {
            if ($testElevation) {
                $results = Join-Path $testRoot 'telemetry'
                $null = New-Item -ItemType Directory -Path $results
                $key = $registry.CreateSubKey($registrationPath)
                try { $key.SetValue('', $root) } finally { $key.Dispose() }
                $registered = $true
                $originalAcl = Get-Acl -LiteralPath $root
                $acl = [Security.AccessControl.DirectorySecurity]::new()
                $acl.SetAccessRuleProtection($true, $false)
                foreach ($entry in @(
                    @([Security.Principal.WindowsIdentity]::GetCurrent().User, 'ReadAndExecute'),
                    @([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'), 'FullControl'),
                    @([Security.Principal.SecurityIdentifier]::new('S-1-5-18'), 'FullControl'))) {
                    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($entry[0], $entry[1], 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
                }
                Set-Acl -LiteralPath $root -AclObject $acl
                $denied = $false
                try { Write-CanaryFile (Join-Path $root '.permission-probe') 'must not be writable' }
                catch [UnauthorizedAccessException] { $denied = $true }
                if (-not $denied) { throw 'Elevation test root is still writable without UAC.' }
                Write-Output "UAC CANARY: Please $ElevationAction the updater prompt now."
            }
            $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $bin 'snow_shot.exe'))
            $start.UseShellExecute = $false
            $start.CreateNoWindow = $true
            foreach ($arg in @($root, (Join-Path $bin 'snow-shot-updater.exe'), $mode)) { $start.ArgumentList.Add($arg) }
            $start.Environment['SNOW_SHOT_HELPER_CANARY_RESULTS'] = $results
            $parent = [Diagnostics.Process]::Start($start)
            $parentElevation = Wait-CanaryFile (Join-Path $results '.canary-parent-elevation.txt')
            if ($testElevation -and $parentElevation -cne 'standard') { throw 'The fixture parent unexpectedly started elevated; UAC cannot be tested.' }
            $handoff = Wait-CanaryFile (Join-Path $results '.canary-handoff.txt')
            if ($mode -eq 'elevation-cancel') {
                if ($handoff -notlike 'failed:*permission was declined*') { throw "Unexpected UAC cancellation result: $handoff" }
            } elseif ($handoff -cne 'ready') { throw "Helper refused a valid parent: $handoff" }
            if ($mode -in @('cancel', 'elevation-cancel')) {
                Start-Sleep -Milliseconds 500
                if ($parent.HasExited -or -not (Test-Path -LiteralPath $journal) -or
                    (Get-Content -Raw (Join-Path $bin 'recovered.txt')) -cne 'incomplete replacement' -or
                    (Test-Path -LiteralPath (Join-Path $results '.canary-restarted.txt'))) {
                    throw 'Cancellation changed files, exited the parent, or relaunched.'
                }
            } else {
                if (-not $parent.WaitForExit(10000) -or $parent.ExitCode -ne 0) { throw 'Parent did not hand off cleanly.' }
                if ((Wait-CanaryFile (Join-Path $results '.canary-result.txt')) -cne 'success' -or
                    (Wait-CanaryFile (Join-Path $results '.canary-restarted.txt')) -cne [Security.Principal.WindowsIdentity]::GetCurrent().User.Value -or
                    (Wait-CanaryFile (Join-Path $results '.canary-restarted-elevation.txt')) -cne $parentElevation -or
                    (Test-Path -LiteralPath $journal) -or
                    (Get-Content -Raw (Join-Path $bin 'recovered.txt')) -cne 'previous payload') {
                    throw 'Recovery or original-user relaunch failed.'
                }
            }
            if ((Get-Content -Raw (Join-Path $root 'user-note.txt')) -cne 'preserve user data') { throw 'User data changed.' }
            Write-Output "PASS: actual helper $variant $mode; user data preserved"
        } finally {
            if ($null -ne $parent) {
                if (-not $parent.HasExited) { $parent.Kill(); $parent.WaitForExit() }
                $parent.Dispose()
            }
            if ($null -ne $originalAcl) {
                # Each root is newly created and originally inherits only the test directory ACL.
                # icacls resets only its DACL; Set-Acl can request unavailable SACL privileges.
                & icacls $root /reset | Out-Null
                if ($LASTEXITCODE -ne 0) { Write-Warning "Could not restore test directory ACL: $root" }
            }
            if ($registered) {
                foreach ($path in @($registrationPath, $uninstallPath)) {
                    if ($existingEmptyKeys.ContainsKey($path)) {
                        $key = $registry.OpenSubKey($path, $true)
                        if ($null -ne $key) {
                            try { foreach ($value in $key.GetValueNames()) { $key.DeleteValue($value) } }
                            finally { $key.Dispose() }
                        }
                    } else { $registry.DeleteSubKeyTree($path, $false) }
                }
                Write-Output 'Removed only temporary per-user canary registration; original machine installation was untouched.'
            }
        }
    }
}
$registry.Dispose()
Write-Output "Helper canary artifacts: $testRoot"
