#Requires -Version 7.2
[CmdletBinding()]
param([switch]$SdkSmoke)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'snow-shot-msix.ps1')
. (Join-Path $PSScriptRoot 'snow-build-environment.ps1')
function Require([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Reject([scriptblock]$Action, [string]$Message, [string]$ExpectedError = '*') {
    $rejected = $false
    try { & $Action | Out-Null } catch {
        Require ($_.Exception.Message -like $ExpectedError) "$Message Unexpected error: $($_.Exception.Message)"
        $rejected = $true
    }
    Require $rejected $Message
}
function Write-Pe([string]$Path, [string]$Architecture) {
    $bytes = [byte[]]::new(128)
    [BitConverter]::GetBytes([uint16]0x5a4d).CopyTo($bytes, 0)
    [BitConverter]::GetBytes([uint32]64).CopyTo($bytes, 0x3c)
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($bytes, 64)
    [BitConverter]::GetBytes([uint16]$(if ($Architecture -eq 'arm64') { 0xaa64 } else { 0x8664 })).CopyTo($bytes, 68)
    [IO.File]::WriteAllBytes($Path, $bytes)
}
function New-Fixture([string]$Stage, [string]$Architecture) {
    $files = @('bin/snow_shot.exe', 'bin/snow-shot-mcp.exe', 'bin/crashpad_handler.exe',
        'bin/SnowShotExplorerCommand.dll',
        'bin/audios/camera_shutter.mp3', 'share/snow-shot/licenses/LICENSES.txt',
        'bin/assets/qrcode/detect.prototxt', 'bin/assets/qrcode/detect.caffemodel',
        'bin/assets/qrcode/sr.prototxt', 'bin/assets/qrcode/sr.caffemodel',
        "bin/assets/ocr/runtimes/1.2.0/windows-$Architecture/snow-ocr-process-1.2.0-windows-$Architecture.exe",
        "bin/assets/ocr/runtimes/1.2.0/windows-$Architecture/runtime-manifest.json", 'bin/assets/ocr/models/small/model.onnx')
    foreach ($name in $files) {
        $path = Join-Path $Stage $name
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force
        if ([IO.Path]::GetExtension($name) -in @('.exe', '.dll')) { Write-Pe $path $Architecture }
        else { [IO.File]::WriteAllText($path, 'fixture') }
    }
    $runtimeFiles = @(Get-ChildItem -LiteralPath (Join-Path $Stage "bin/assets/ocr/runtimes/1.2.0/windows-$Architecture") -File | ForEach-Object {
        @{ name = $_.Name; size = $_.Length; sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $model = Get-Item -LiteralPath (Join-Path $Stage 'bin/assets/ocr/models/small/model.onnx')
    @{ schema = 1; component = '1.2.0' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Stage "bin/assets/ocr/runtimes/1.2.0/windows-$Architecture/.complete.json") -Encoding utf8NoBOM
    @{ schema = 1; component = 'small' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Stage 'bin/assets/ocr/models/small/.complete.json') -Encoding utf8NoBOM
    @{ schema = 2; default_model = 'small'; runtime = @{ version = '1.2.0'; platform = "windows-$Architecture"; files = $runtimeFiles };
        models = @(@{ type = 'small'; id = 'small'; files = @(@{ name = 'model.onnx'; size = $model.Length;
            sha256 = (Get-FileHash $model.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }) }) } |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Stage 'bin/assets/ocr/asset-manifest.json') -Encoding utf8NoBOM
}
function Write-Audit([string]$Stage, [string]$Path, [string]$Architecture, [string]$Channel = 'store') {
    @{ SchemaVersion = 1; DistributionChannel = $Channel; PackageVersion = '1.3.2'; Architecture = $Architecture;
        StaticCrt = $true; StaticQt = $true; NativeValidationRequired = $false;
        InstallFiles = (Get-SnowMsixInventory $Stage) } | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $Path -Encoding utf8NoBOM
}
$testRoot = Join-Path $script:SnowMsixRepoRoot ('build/msix-tests-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $testRoot
try {
    $identity = Read-SnowMsixIdentity
    # Exercise the real packaging assignment and CMake command together. A single
    # target must remain an array when splatted into a native command.
    $packageAst = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot 'package-snow-shot.ps1'), [ref]$null, [ref]$null)
    $targetAssignment = @($packageAst.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.AssignmentStatementAst] -and
            $node.Left.Extent.Text.EndsWith('$releaseTargets')
    }, $true))
    $buildCommand = @($packageAst.FindAll({
        param($node)
        $node -is [System.Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'cmake' -and $node.Extent.Text.Contains('@releaseTargets')
    }, $true))
    Require ($targetAssignment.Count -eq 1 -and $buildCommand.Count -eq 1) 'Packaging must have one release target selection/build command.'
    $buildInvocation = [scriptblock]::Create($targetAssignment[0].Extent.Text + "`n" + $buildCommand[0].Extent.Text)
    function Invoke-ReleaseBuildFixture([bool]$Store) {
        function cmake { [pscustomobject]@{ Arguments = @($args) } }
        $store = $Store
        $buildDirectory = 'fixture-build'
        $Parallelism = 2
        & $buildInvocation
    }
    foreach ($storeFixture in @($true, $false)) {
        $invocation = Invoke-ReleaseBuildFixture $storeFixture
        $targets = if ($storeFixture) { 'snow_shot' } else { 'snow_shot|snow_shot_mini' }
        Require (($invocation.Arguments -join '|') -ceq "--build|fixture-build|--config|Release|--target|$targets|--parallel|2") 'Packaging must pass complete target names to CMake for both channels.'
    }
    # Run the actual release hook with a recording MSIX driver. Keep compiler,
    # network, signing and publishing operations out of this workflow fixture.
    $releaseHook = @($packageAst.EndBlock.Statements | Where-Object {
        $_.Extent.Text.Contains("'package-snow-shot-msix.ps1'")
    })
    Require ($releaseHook.Count -eq 1) 'Release packaging must invoke the MSIX driver once.'
    $releaseScripts = Join-Path $testRoot 'release-scripts'
    $null = New-Item -ItemType Directory -Path $releaseScripts
    $releaseFixture = Join-Path $releaseScripts 'package-release.ps1'
    $fixtureSetup = @'
param([string]$Architecture = 'x64', [bool]$store = $false, [switch]$StageOnly,
    [string]$packageVersion = '1.3.2', [string]$packageVersionNumeric = '1.3.2',
    [switch]$SkipBuild)
$repoRoot = $PSScriptRoot
$runtimeArchivePath = Join-Path $PSScriptRoot 'verified-runtime.zip'
$Parallelism = 7
'@
    Set-Content -LiteralPath $releaseFixture -Value ($fixtureSetup + "`n" + $releaseHook[0].Extent.Text)
    @'
[CmdletBinding()]
param([string]$Architecture, [string]$OutputDirectory, [string]$ExpectedVersion,
    [hashtable]$OcrRuntimeArchives, [int]$Parallelism, [switch]$SkipBuild)
@{ Architecture = $Architecture; OutputDirectory = $OutputDirectory;
    ExpectedVersion = $ExpectedVersion; OcrRuntimeArchives = $OcrRuntimeArchives;
    Parallelism = $Parallelism; SkipBuild = [bool]$SkipBuild } |
    ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $PSScriptRoot 'calls.jsonl')
if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'fail-msix')) { throw 'Injected MSIX packaging failure.' }
'@ | Set-Content -LiteralPath (Join-Path $releaseScripts 'package-snow-shot-msix.ps1')
    $callLog = Join-Path $releaseScripts 'calls.jsonl'
    & $releaseFixture
    $calls = @(Get-Content -LiteralPath $callLog | ConvertFrom-Json)
    Require ($calls.Count -eq 1 -and $calls[0].Architecture -ceq 'x64' -and
        $calls[0].ExpectedVersion -ceq '1.3.2' -and -not $calls[0].SkipBuild -and
        $calls[0].Parallelism -eq 7 -and
        $calls[0].OutputDirectory -ceq (Join-Path $releaseScripts 'artifacts/msix/1.3.2') -and
        $calls[0].OcrRuntimeArchives.x64 -ceq (Join-Path $releaseScripts 'verified-runtime.zip')) 'Stable x64 releases must build an unsigned local MSIX using the verified runtime and a separate versioned output folder.'
    & $releaseFixture -SkipBuild
    $calls = @(Get-Content -LiteralPath $callLog | ConvertFrom-Json)
    Require ($calls.Count -eq 2 -and $calls[1].SkipBuild) 'Release repackaging must forward SkipBuild to the Store packager.'
    & $releaseFixture -Architecture arm64
    & $releaseFixture -store $true
    & $releaseFixture -StageOnly
    & $releaseFixture -packageVersion '1.3.2-beta'
    Require (@(Get-Content -LiteralPath $callLog).Count -eq 2) 'ARM64, Store stages, staging-only runs and previews must not invoke the release MSIX hook.'
    Set-Content -LiteralPath (Join-Path $releaseScripts 'fail-msix') -Value 'fixture'
    Reject { & $releaseFixture } 'MSIX failures must fail release packaging.' '*Injected MSIX packaging failure*'
    $driver = Join-Path $PSScriptRoot 'package-snow-shot-msix.ps1'
    $driverAst = [System.Management.Automation.Language.Parser]::ParseFile(
        $driver, [ref]$null, [ref]$null)
    $versionGuard = @($driverAst.EndBlock.Statements | Where-Object {
        $_.Extent.Text.StartsWith('if ($ExpectedVersion')
    })
    Require ($versionGuard.Count -eq 1) 'The Store driver must check the requested release version before creating the MSIX.'
    $checkVersion = [scriptblock]::Create($versionGuard[0].Extent.Text)
    $audit = [pscustomobject]@{ PackageVersion = '1.3.2' }
    $ExpectedVersion = '1.3.2'
    & $checkVersion
    $ExpectedVersion = ''
    & $checkVersion
    $ExpectedVersion = '1.3.3'
    Reject { & $checkVersion } 'A stale Store build must not produce an MSIX for another release.' '*does not match release version*'
    $driverParameters = [scriptblock]::Create($driverAst.ParamBlock.Extent.Text + "`n" + '$Architecture')
    Require ((& $driverParameters -IdentityPath 'fixture' -OutputDirectory 'fixture') -ceq 'x64') 'MSIX packaging must default to x64 only.'
    Require ((& $driverParameters -Architecture x64 -IdentityPath 'fixture' -OutputDirectory 'fixture') -ceq 'x64') 'Explicit x64 MSIX packaging must remain supported.'
    Reject { & $driver -Architecture arm64 } 'MSIX packaging must reject ARM64 before staging.' '*ValidateSet*'
    Reject { & $driver -Bundle } 'The removed multi-architecture bundle option must be rejected.' '*Bundle*'
    foreach ($wrapper in @('build.ps1', 'run-snow-shot.ps1')) {
        Reject { & (Join-Path $PSScriptRoot $wrapper) -Preset snow-shot-msvc-arm64-store-release } 'Build/run entry points must reject the removed ARM64 Store preset.' '*ValidateSet*'
    }
    Reject { Get-SnowWindowsTarget -Preset snow-shot-msvc-arm64-store-release } 'The removed ARM64 Store preset must be rejected.' '*Unsupported Windows preset*'
    $armTarget = Get-SnowWindowsTarget -Architecture arm64
    Require ($armTarget.ReleasePreset -ceq 'snow-shot-msvc-arm64-release' -and
        $null -eq $armTarget.StorePreset) 'Standalone ARM64 must remain supported without a Store preset.'
    foreach ($prepareOnly in @($false, $true)) {
        Reject { & (Join-Path $PSScriptRoot 'package-snow-shot.ps1') -Architecture arm64 `
            -DistributionChannel store -StageOnly -PrepareOcrRuntimeOnly:$prepareOnly
        } 'Store staging must reject ARM64 before building or preparing OCR.' '*supports x64 only*'
    }
    Reject { Assert-SnowMsixAuditedStage 'unused' 'unused' arm64 } 'MSIX auditing must reject ARM64.' '*ValidateSet*'
    Reject { Write-SnowMsixManifest 'unused' $identity '1.3.2' arm64 } 'MSIX manifests must reject ARM64.' '*ValidateSet*'
    # Exercise the top-level Store guard with real compiler architecture metadata,
    # without initializing a compiler or configuring the application dependencies.
    $architectureGuard = Join-Path $testRoot 'arm64-store.cmake'
    $root = $script:SnowMsixRepoRoot.Replace('\', '/')
    [IO.File]::WriteAllText($architectureGuard, @'
cmake_minimum_required(VERSION 4.2)
set(WIN32 TRUE)
set(CMAKE_CURRENT_SOURCE_DIR "@ROOT@")
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64)
set(CMAKE_SIZEOF_VOID_P 8)
set(SNOW_SHOT_DISTRIBUTION_CHANNEL store CACHE STRING "" FORCE)
function(project)
endfunction()
include("@ROOT@/CMakeLists.txt")
'@.Replace('@ROOT@', $root))
    $architectureError = (& cmake -P $architectureGuard 2>&1 | Out-String)
    Require ($LASTEXITCODE -ne 0 -and $architectureError -like '*Store channel supports x64 only*') 'CMake must reject manually configured ARM64 Store builds.'
    # Evaluate the real update module without a compiler or generated build tree.
    # A preprocessor macro does not set a CMake variable: Store must gate helpers
    # using the distribution channel even when SNOW_SHOT_STORE_BUILD is undefined.
    foreach ($channel in @('store', 'standalone')) {
        $module = (Join-Path $script:SnowMsixRepoRoot 'cmake/SnowShotUpdates.cmake').Replace('\', '/')
        $guardFixture = @'
cmake_minimum_required(VERSION 4.2)
set(APPLE FALSE)
set(SNOW_SHOT_DISTRIBUTION_CHANNEL "@CHANNEL@")
set(SNOW_SHOT_BUILD_UPDATE_TESTS OFF)
set(helper_built FALSE)
set(helper_installed FALSE)
function(snow_add_rust_executable)
    set(helper_built TRUE PARENT_SCOPE)
endfunction()
function(install)
    set(helper_installed TRUE PARENT_SCOPE)
endfunction()
function(add_library)
endfunction()
function(target_include_directories)
endfunction()
function(target_link_libraries)
endfunction()
function(add_custom_target)
endfunction()
function(add_custom_command)
endfunction()
function(add_dependencies)
endfunction()
include("@MODULE@")
if(SNOW_SHOT_DISTRIBUTION_CHANNEL STREQUAL "store")
    if(helper_built OR helper_installed)
        message(FATAL_ERROR "Store must not build or install the standalone updater")
    endif()
elseif(NOT helper_built OR NOT helper_installed)
    message(FATAL_ERROR "Standalone must retain its updater build/install")
endif()
'@
        $guardPath = Join-Path $testRoot "$channel-helper-guard.cmake"
        [IO.File]::WriteAllText($guardPath, $guardFixture.Replace('@CHANNEL@', $channel).Replace('@MODULE@', $module))
        & cmake -P $guardPath
        if ($LASTEXITCODE -ne 0) { throw "Updater build/install channel regression: $channel" }
    }
    Require ($identity.Name -ceq 'SnowApps.SnowShot' -and $identity.PackageFamilyName -ceq 'SnowApps.SnowShot_3pp9w6ncw7bs0') 'Partner Center identity must remain stable.'
    Require ((ConvertTo-SnowMsixVersion '1.3.2') -ceq '1.3.2.0') 'Application version must map to Store revision zero.'
    foreach ($invalid in @('1.3.2-beta', '1.3.2.1', '1.3.65536', '01.2.3', '0.2.3', '1.2', '999999999999.1.1')) {
        Reject { ConvertTo-SnowMsixVersion $invalid } "Invalid Store version accepted: $invalid"
    }
    Reject { Reset-SnowMsixDirectory -Path (Join-Path $testRoot '..') -Root $testRoot } 'Cleanup must reject escaping the work directory.'
    Reject { Set-SnowMsixTestSignature -Package 'fixture.msix' -CertificateThumbprint 'invalid' -Publisher $identity.Publisher } 'Signing requires an explicit valid thumbprint.'
    $linksRoot = Join-Path $testRoot 'link-fixture'
    $linksOutside = Join-Path $testRoot 'outside-fixture'
    $null = New-Item -ItemType Directory -Path $linksRoot, $linksOutside
    $null = New-Item -ItemType Junction -Path (Join-Path $linksRoot 'nested') -Target $linksOutside
    Reject { Get-SnowMsixInventory $linksRoot } 'Payload inventory must reject junction directories before traversing them.'
    $presets = Get-Content -LiteralPath (Join-Path $script:SnowMsixRepoRoot 'CMakePresets.json') -Raw | ConvertFrom-Json
    Require (@($presets.configurePresets | Where-Object name -Like '*arm64-store*').Count -eq 0 -and
        @($presets.buildPresets | Where-Object configurePreset -Like '*arm64-store*').Count -eq 0) 'ARM64 Store configure/build presets must not be offered.'
    $architecture = 'x64'
    $target = Get-SnowWindowsTarget -Preset snow-shot-msvc-store-release
    Require ($target.IsStatic -and $target.Architecture -ceq $architecture -and $target.DistributionChannel -ceq 'store') 'Store target must retain static architecture settings.'
    $preset = @($presets.configurePresets | Where-Object name -CEQ $target.StorePreset)[0]
    Require ($preset.cacheVariables.SNOW_SHOT_DISTRIBUTION_CHANNEL -ceq 'store' -and
        $preset.cacheVariables.SNOW_APPS_BUILD_SNOW_SHOT_MINI -ceq 'OFF' -and
        $preset.cacheVariables.SNOW_APPS_PACKAGE_SNOW_SHOT -ceq 'OFF') 'Store presets must isolate the channel and exclude Mini/NSIS.'
    $buildPreset = @($presets.buildPresets | Where-Object name -CEQ ('build-' + $target.StorePreset))[0]
    Require (($buildPreset.targets -join ',') -ceq 'snow_shot') 'Store build must only target Full.'
    $cachePath = Join-Path $testRoot "$architecture-CMakeCache.txt"
    "SNOW_SHOT_DISTRIBUTION_CHANNEL:STRING=standalone`nSNOW_APPS_BUILD_SNOW_SHOT_MINI:BOOL=ON" |
        Set-Content -LiteralPath $cachePath -Encoding utf8NoBOM
    $compatibility = Get-SnowCacheCompatibility -CachePath $cachePath -Preset $target.StorePreset
    Require ('SNOW_SHOT_DISTRIBUTION_CHANNEL' -in $compatibility.Differences.Name -and
        'SNOW_APPS_BUILD_SNOW_SHOT_MINI' -in $compatibility.Differences.Name) 'Store cache reuse must reject standalone channel and Mini.'
    $stage = Join-Path $testRoot $architecture
    New-Fixture $stage $architecture
    $noticePath = Join-Path $stage 'share/snow-shot/licenses/LICENSES.txt'
    [IO.File]::WriteAllText($noticePath, 'legitimate third-party license notice for test-helpers/benchmark')
    $audit = Join-Path $testRoot "$architecture-audit.json"
    Write-Audit $stage $audit arm64
    Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } 'An ARM64 stage audit must not be accepted for an x64 package.'
    Write-Audit $stage $audit $architecture
    $null = Assert-SnowMsixAuditedStage $stage $audit $architecture
    [IO.File]::WriteAllText($noticePath, '')
    Write-Audit $stage $audit $architecture
    Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } 'Empty consolidated notices must fail.'
    Remove-Item -LiteralPath $noticePath
    Write-Audit $stage $audit $architecture
    Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } 'Missing consolidated notices must fail.'
    [IO.File]::WriteAllText($noticePath, 'legitimate third-party license notice')
    $legacyNotice = Join-Path $stage 'share/snow-shot/licenses/LICENSE'
    [IO.File]::WriteAllText($legacyNotice, 'legacy loose notice')
    Write-Audit $stage $audit $architecture
    Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } 'Loose license files must fail.'
    Remove-Item -LiteralPath $legacyNotice
    Write-Audit $stage $audit $architecture
    $shellExtension = Join-Path $stage 'bin/SnowShotExplorerCommand.dll'
    Remove-Item -LiteralPath $shellExtension
    Write-Audit $stage $audit $architecture
    Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } 'Store stage requires the packaged Explorer command DLL.'
    Write-Pe $shellExtension 'arm64'
    Write-Audit $stage $audit $architecture
    Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } 'Explorer command architecture must match the package.'
    Write-Pe $shellExtension $architecture
    Write-Audit $stage $audit $architecture standalone
    Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } 'Standalone stage must not be accepted as Store.'
    Write-Audit $stage $audit $architecture
    $main = Join-Path $stage 'bin/snow_shot.exe'
    Add-Content -LiteralPath $main -Value 'changed'
    Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } 'Changed stage bytes must fail the audit.'
    Write-Pe $main 'arm64'
    Write-Audit $stage $audit $architecture
    Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } 'Opposite-architecture binary must fail.'
    Write-Pe $main $architecture
    $updater = Join-Path $stage 'bin/snow-shot-updater.exe'
    Write-Pe $updater $architecture
    Write-Audit $stage $audit $architecture
    Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } 'Standalone updater must never enter a Store package.'
    Remove-Item -LiteralPath $updater
    foreach ($relative in @('bin/assets/nested/unexpected.dat', 'bin/assets/nested/extra.com',
            'bin/assets/nested/extra.scr', 'share/snow-shot/licenses/third-party/native-LICENSE.txt')) {
        $disguised = Join-Path $stage $relative
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $disguised) -Force
        Write-Pe $disguised $architecture
        Require (Test-SnowMsixPeImage $disguised) 'PE discovery must ignore misleading filename extensions.'
        Write-Audit $stage $audit $architecture
        Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } "Renamed nested executable must fail the exact binary inventory: $relative"
        Remove-Item -LiteralPath $disguised
    }
    $disguised = Join-Path $stage 'bin/assets/nested/wrong-architecture.dat'
    Write-Pe $disguised 'arm64'
    Write-Audit $stage $audit $architecture
    Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } 'Architecture checks must inspect disguised native code.'
    Remove-Item -LiteralPath $disguised
    foreach ($suffix in @('.msi', '.appinstaller', '.ps1', '.cmd', '.vbs')) {
        $artifact = Join-Path $stage "bin/assets/nested/install$suffix"
        [IO.File]::WriteAllText($artifact, 'installer or script fixture')
        Write-Audit $stage $audit $architecture
        Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } "Installer/script artifact must not enter Store payload: $suffix"
        Remove-Item -LiteralPath $artifact
    }
    $nonPe = Join-Path $testRoot "$architecture-invalid-pe.dat"
    Write-Pe $nonPe $architecture
    $header = [IO.File]::OpenWrite($nonPe)
    try { $header.Position = 0x3c; $header.Write([BitConverter]::GetBytes([uint32]::MaxValue)) } finally { $header.Dispose() }
    Require (-not (Test-SnowMsixPeImage $nonPe)) 'PE discovery must validate the DOS header offset before seeking.'
    $modelPath = Join-Path $stage 'bin/assets/ocr/models/small/model.onnx'
    [IO.File]::WriteAllText($modelPath, 'altered')
    Write-Audit $stage $audit $architecture
    Reject { Assert-SnowMsixAuditedStage $stage $audit $architecture } 'Altered offline OCR model must fail its pinned hash.'
    [IO.File]::WriteAllText($modelPath, 'fixture')
    Write-Audit $stage $audit $architecture
    $null = Assert-SnowMsixAuditedStage $stage $audit $architecture
    Write-SnowMsixManifest $stage $identity '1.3.2' $architecture
    $manifest = [xml](Get-Content -LiteralPath (Join-Path $stage 'AppxManifest.xml') -Raw)
    Require ($manifest.Package.Identity.ProcessorArchitecture -ceq 'x64') 'The Store manifest must target x64.'
    $ns = [Xml.XmlNamespaceManager]::new($manifest.NameTable)
    $ns.AddNamespace('m', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
    $ns.AddNamespace('uap', 'http://schemas.microsoft.com/appx/manifest/uap/windows10')
    $ns.AddNamespace('uap2', 'http://schemas.microsoft.com/appx/manifest/uap/windows10/2')
    $ns.AddNamespace('uap3', 'http://schemas.microsoft.com/appx/manifest/uap/windows10/3')
    $ns.AddNamespace('desktop4', 'http://schemas.microsoft.com/appx/manifest/desktop/windows10/4')
    $ns.AddNamespace('desktop5', 'http://schemas.microsoft.com/appx/manifest/desktop/windows10/5')
    $ns.AddNamespace('com', 'http://schemas.microsoft.com/appx/manifest/com/windows10')
    $types = @($manifest.SelectNodes('//uap:FileType', $ns) | ForEach-Object InnerText)
    Require (($types -join ',') -ceq ((Get-SnowMsixFileExtensions) -join ',')) 'File associations must follow supportedPinFileExtensions.'
    $association = $manifest.SelectSingleNode('//uap3:FileTypeAssociation', $ns)
    Require ($association.Parameters -ceq '--edit-with-snow-shot "%1"' -and
        $association.MultiSelectModel -ceq 'Document') 'Open with and default file activation must retain typed file arguments.'
    Require ($manifest.SelectNodes('//uap2:SupportedVerbs', $ns).Count -eq 0) 'Only the native Explorer command should contribute an edit menu entry, including when Snow Shot is the default app.'
    $commands = @($manifest.SelectNodes('//desktop4:FileExplorerContextMenus/desktop5:ItemType', $ns))
    Require (($commands.Type -join ',') -ceq ($types -join ',')) 'Explorer context menus must cover exactly the supported file types without changing default associations.'
    $comClass = $manifest.SelectSingleNode('//com:SurrogateServer/com:Class', $ns)
    Require ($null -ne $comClass -and $comClass.Path -ceq 'bin\SnowShotExplorerCommand.dll' -and
        $comClass.ThreadingModel -ceq 'STA' -and
        (Test-Path -LiteralPath (Join-Path $stage $comClass.Path) -PathType Leaf)) 'Packaged COM must load the staged native Explorer command DLL.'
    foreach ($command in $commands) {
        $verb = $command.SelectSingleNode('desktop5:Verb', $ns)
        Require ($verb.Id -ceq 'EditWithSnowShot' -and $verb.Clsid -ceq $comClass.Id -and
            $verb.Clsid -ceq '3F1E90A2-7D4B-4F21-9D8A-6B0C5E2F7093') 'Each Explorer verb must activate the registered command CLSID.'
    }
    $minimum = $manifest.SelectSingleNode('//m:TargetDeviceFamily', $ns).MinVersion
    Require ($minimum -ceq '10.0.19041.0') 'The x64 Store package must retain its Windows 10 baseline.'
    $manifestText = $manifest.OuterXml
    Require ($manifestText.Contains('--autostart') -and $manifestText.Contains('SnowShotStartup') -and
        $manifestText.Contains('snow-shot-store-mcp.exe') -and $manifestText.Contains('mediumIL') -and
        $manifestText.Contains('runFullTrust') -and $manifestText.Contains('microphone') -and
        $manifestText.Contains('webcam')) 'Packaged launch integrations/capabilities must be declared.'
    $identity.PublisherDisplayName = 'Snow & Apps'
    Write-SnowMsixManifest $stage $identity '1.3.2' $architecture
    $escaped = [xml](Get-Content -LiteralPath (Join-Path $stage 'AppxManifest.xml') -Raw)
    Require ($escaped.Package.Properties.PublisherDisplayName -ceq 'Snow & Apps') 'Manifest metadata must be XML escaped.'
    $identity.PublisherDisplayName = 'Snow Apps'
    Write-SnowMsixManifest $stage $identity '1.3.2' $architecture
    if ($SdkSmoke) {
        $package = Join-Path $testRoot "$architecture.msix"
        New-SnowMsixPackage $stage $package $testRoot
        $packedIdentity = Get-SnowMsixPackageIdentity $package
        Require ($packedIdentity.ProcessorArchitecture -ceq $architecture -and $packedIdentity.Version -ceq '1.3.2.0') 'Real SDK package must retain identity and architecture.'
        $dumpPath = Join-Path $testRoot "$architecture-resources.xml"
        Invoke-SnowMsixTool 'makepri.exe' @('dump', '/if', (Join-Path $stage 'resources.pri'), '/of', $dumpPath, '/dt', 'detailed', '/o')
        $dump = Get-Content -LiteralPath $dumpPath -Raw
        Require ($dump.Contains('zh-CN') -and $dump.Contains('zh-TW') -and $dump.Contains('en-US') -and
            $dump.Contains('Images and text documents')) 'Compiled PRI must retain all language candidates.'
    }
    foreach ($language in @('en-US', 'zh-CN', 'zh-TW')) {
        $resources = [xml](Get-Content -LiteralPath (Join-Path $script:SnowMsixRepoRoot "snow_shot/packaging/msix/Strings/$language/Resources.resw") -Raw)
        Require (@($resources.root.data).Count -eq 4 -and @($resources.root.data | Where-Object { [string]::IsNullOrWhiteSpace($_.value) }).Count -eq 0) 'All manifest languages require complete resource strings.'
    }
    $assetRoot = Join-Path $script:SnowMsixRepoRoot 'snow_shot/packaging/msix/Assets'
    $source = Get-Content -LiteralPath (Join-Path $assetRoot 'source.json') -Raw | ConvertFrom-Json
    Require ($source.sha256 -ceq (Get-FileHash -LiteralPath (Join-Path $script:SnowMsixRepoRoot $source.source) -Algorithm SHA256).Hash.ToLowerInvariant()) 'Regenerate Store assets after an SVG change.'
    foreach ($item in @(@('StoreLogo.png', 50), @('Square44x44Logo.png', 44), @('Square150x150Logo.png', 150))) {
        $bytes = [IO.File]::ReadAllBytes((Join-Path $assetRoot $item[0]))
        $size = [Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($bytes, 16))
        Require ($size -eq $item[1]) 'Required Store asset dimensions must be correct.'
    }
    if ($SdkSmoke) {
        $unpack = Join-Path $testRoot 'unpacked'
        Invoke-SnowMsixTool 'makeappx.exe' @('unpack', '/p', $package, '/d', $unpack, '/o')
        Require (Test-Path -LiteralPath (Join-Path $unpack 'resources.pri')) 'Compiled localized resources must survive real package round trip.'
    }
    Write-Output 'Snow Shot MSIX packaging tests passed.'
} finally {
    $safe = Assert-SnowMsixChildPath $testRoot (Join-Path $script:SnowMsixRepoRoot 'build')
    Remove-Item -LiteralPath $safe -Recurse -Force
}
