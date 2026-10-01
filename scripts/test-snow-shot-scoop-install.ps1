#Requires -Version 7.0
param([Parameter(Mandatory)][string]$ScoopRoot)
# Exercise pinned Scoop's real install, shim, shortcut and persistence functions.
# Only OS integration boundaries (Start Menu location and persistent PATH writes)
# are redirected. No existing Scoop config, installation or user data is touched.
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'The Scoop lifecycle smoke test requires Windows.' }
$ScoopRoot = (Resolve-Path -LiteralPath $ScoopRoot).Path
$root = Join-Path ([IO.Path]::GetTempPath()) "snow-shot-scoop-install-$([guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory -Path $root
$saved = @{}
foreach ($key in @('SCOOP', 'SCOOP_GLOBAL', 'SCOOP_CACHE', 'XDG_CONFIG_HOME', 'PATH')) {
    $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
}
function Require($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Assert-FixturePath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith("$root\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escaped isolated fixture: $full"
    }
}
try {
    $env:SCOOP = Join-Path $root 'scoop'
    $env:SCOOP_GLOBAL = Join-Path $root 'global'
    $env:SCOOP_CACHE = Join-Path $root 'cache'
    $env:XDG_CONFIG_HOME = Join-Path $root 'config'
    $runtime = Join-Path $env:SCOOP 'apps/scoop/current'
    $null = New-Item -ItemType Directory -Force -Path $runtime, $env:SCOOP_CACHE, "$env:SCOOP/buckets"
    Copy-Item -Path "$ScoopRoot/*" -Destination $runtime -Recurse -Force
    Set-StrictMode -Off # Scoop itself runs without strict mode.
    foreach ($library in @('core', 'buckets', 'json', 'manifest', 'system', 'install', 'download',
        'decompress', 'shortcuts', 'psmodules', 'versions', 'depends')) {
        . "$runtime/lib/$library.ps1"
    }
    $null = set_config ARIA2_ENABLED $false
    $null = set_config USE_EXTERNAL_7ZIP $false
    function shortcut_folder($global) {
        if ($global) { throw 'Global shortcut requested by per-user fixture.' }
        return ensure (Join-Path $root 'Start Menu')
    }
    function Add-Path($Path, $Global) {
        Assert-FixturePath $Path
        if ($Global) { throw 'Global PATH requested by per-user fixture.' }
        $env:PATH = "$Path;$env:PATH"
    }
    . (Join-Path $PSScriptRoot 'snow-shot-scoop.ps1')
    Set-StrictMode -Off
    $fixtureManifest = Join-Path $root 'snowshot.json'
    $manifest = New-SnowShotScoopManifestObject '0.0.1' 'https://fixture.invalid/portable.zip' ('0' * 64) |
        ConvertTo-Json -Depth 8 | ConvertFrom-Json
    # The executable is inert: use the OS's existing executable as a valid PE fixture,
    # but never launch it. The real release ZIP is independently verified by the generator.
    foreach ($version in @('0.0.1-beta', '0.0.2-beta')) {
        $stage = Join-Path $root "stage-$version"
        $null = New-Item -ItemType Directory -Path "$stage/bin"
        Copy-Item -LiteralPath "$env:SystemRoot/System32/where.exe" -Destination "$stage/bin/snow_shot.exe"
        [IO.File]::WriteAllText("$stage/bin/__data_directory", 'portable')
        $archive = Join-Path $root "$version.zip"
        [IO.Compression.ZipFile]::CreateFromDirectory($stage, $archive)
        $manifest.version = $version
        $manifest.architecture.'64bit'.url = "https://fixture.invalid/snow-shot-$version.zip"
        $manifest.architecture.'64bit'.hash = (Get-FileHash $archive -Algorithm SHA256).Hash
        $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $fixtureManifest
        # Populate Scoop's isolated cache; download still runs its normal hash check.
        $cacheFile = cache_path 'snowshot' $version $manifest.architecture.'64bit'.url
        Assert-FixturePath $cacheFile
        Copy-Item -LiteralPath $archive -Destination $cacheFile
        install_app $fixtureManifest '64bit' $false @{}
        $current = Join-Path $env:SCOOP 'apps/snowshot/current'
        Require ((Get-Content "$current/manifest.json" -Raw | ConvertFrom-Json).version -ceq $version) 'Current version did not advance'
        Require ((Get-Content "$current/bin/__data_directory" -Raw) -ceq 'portable') 'Portable marker changed'
        $data = Join-Path $current 'bin/portable'
        Require ((Get-Item $data).LinkType -eq 'Junction') 'Portable data is not persisted'
        $shim = Get-Content "$env:SCOOP/shims/snowshot.shim" -Raw
        $expectedShim = 'path = "' + (Convert-Path "$current/bin/snow_shot.exe") + '"'
        Require ($shim.Trim() -ceq $expectedShim) 'Wrong shim target'
        Require (Test-Path "$env:SCOOP/shims/snowshot.exe") 'Missing executable shim'
        $shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut("$root/Start Menu/Snow Shot.lnk")
        Require ($shortcut.TargetPath -ieq "$current\bin\snow_shot.exe") 'Wrong shortcut target'
        if ($version -eq '0.0.1-beta') {
            [IO.File]::WriteAllText("$data/settings.json", '{"fixture":"retained"}')
            [IO.File]::WriteAllText("$data/history.fixture", 'history')
        } else {
            Require ((Get-Content "$data/settings.json" -Raw) -ceq '{"fixture":"retained"}') 'Settings lost during upgrade'
            Require ((Get-Content "$data/history.fixture" -Raw) -ceq 'history') 'History lost during upgrade'
        }
    }
    # Use the same removal primitives as scoop-uninstall without reloading its
    # real Start Menu boundary. Unlink persisted data before removing any version.
    rm_shims 'snowshot' $manifest $false '64bit'
    rm_startmenu_shortcuts $manifest $false '64bit'
    $null = unlink_current (Join-Path $env:SCOOP 'apps/snowshot/0.0.2-beta')
    foreach ($version in @('0.0.1-beta', '0.0.2-beta')) {
        $directory = Join-Path $env:SCOOP "apps/snowshot/$version"
        Assert-FixturePath $directory
        unlink_persist_data $manifest $directory
        Remove-Item -LiteralPath $directory -Recurse -Force
    }
    Require (-not (Test-Path "$env:SCOOP/shims/snowshot.exe")) 'Shim survived uninstall'
    Require (-not (Test-Path "$root/Start Menu/Snow Shot.lnk")) 'Shortcut survived uninstall'
    $persisted = Join-Path $env:SCOOP 'persist/snowshot/bin/portable'
    Require ((Get-Content "$persisted/settings.json" -Raw) -ceq '{"fixture":"retained"}') 'Uninstall removed settings'
    Require ((Get-Content "$persisted/history.fixture" -Raw) -ceq 'history') 'Uninstall removed history'
    # Changing buckets requires uninstall/reinstall of the same app name.
    # Reinstall must reconnect the data left by ordinary uninstall.
    install_app $fixtureManifest '64bit' $false @{}
    $current = Join-Path $env:SCOOP 'apps/snowshot/current'
    $data = Join-Path $current 'bin/portable'
    Require ((Get-Item $data).LinkType -eq 'Junction') 'Reinstall did not reconnect portable data'
    Require ((Get-Content "$data/settings.json" -Raw) -ceq '{"fixture":"retained"}') 'Reinstall lost settings'
    Require ((Get-Content "$data/history.fixture" -Raw) -ceq 'history') 'Reinstall lost history'
    rm_shims 'snowshot' $manifest $false '64bit'
    rm_startmenu_shortcuts $manifest $false '64bit'
    $directory = Join-Path $env:SCOOP 'apps/snowshot/0.0.2-beta'
    Assert-FixturePath $directory
    $null = unlink_current $directory
    unlink_persist_data $manifest $directory
    Remove-Item -LiteralPath $directory -Recurse -Force
    Write-Output 'Isolated Scoop install, upgrade, uninstall, and migration reinstall smoke test passed.'
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process') }
    # The root is created by this test. Remove junctions first, including on failure.
    $resolved = [IO.Path]::GetFullPath($root)
    if (-not $resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $resolved -Leaf) -notlike 'snow-shot-scoop-install-*') { throw 'Unsafe smoke cleanup path.' }
    foreach ($link in @(Get-ChildItem -LiteralPath $resolved -Recurse -Force -Attributes ReparsePoint)) {
        Assert-FixturePath $link.FullName
        [IO.Directory]::Delete($link.FullName)
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
