#Requires -Version 7.2

function Test-SnowOcrNativeValidationHost {
    param([ValidateSet('x64', 'arm64')][string]$Architecture = 'x64', [switch]$RequireNative)
    $hostArchitecture = Get-SnowWindowsHostArchitecture
    if ($hostArchitecture -ceq $Architecture) { return $true }
    if ($RequireNative) { throw "Native OCR validation requires a windows-$Architecture host; this host is windows-$hostArchitecture." }
    return $false
}

function New-SnowOcrRuntimeReleaseReport {
    param([Parameter(Mandatory)][string]$RuntimeVersion,
        [Parameter(Mandatory)][string]$ArchivePath, [Parameter(Mandatory)][string]$UploadUrl,
        [Parameter(Mandatory)][object[]]$Files, [Parameter(Mandatory)][bool]$NativeProbePassed,
        [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64')

    $hostArchitecture = Get-SnowWindowsHostArchitecture
    if ($NativeProbePassed) {
        $null = Test-SnowOcrNativeValidationHost -Architecture $Architecture -RequireNative
    }
    $item = Get-Item -LiteralPath $ArchivePath
    if ($RuntimeVersion -cnotmatch '^\d+\.\d+\.\d+$' -or
        $item.Name -cne "snow-ocr-runtime-$RuntimeVersion-windows-$Architecture.zip") {
        throw 'OCR runtime report archive identity differs from its version/platform.'
    }
    $hash = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $platform = "windows-$Architecture"
    return [ordered]@{
        SchemaVersion = 1
        RuntimeVersion = $RuntimeVersion
        Platform = $platform
        Protocol = 5
        NativeValidationRequired = (-not $NativeProbePassed)
        NativeValidation = [ordered]@{
            Platform = $platform
            HostPlatform = "windows-$hostArchitecture"
            Passed = $NativeProbePassed
            Artifacts = @([ordered]@{ Name = $item.Name; Bytes = $item.Length; Sha256 = $hash })
        }
        UploadUrl = $UploadUrl
        Archive = [ordered]@{ name = $item.Name; size = $item.Length; sha256 = $hash; url = $UploadUrl }
        Files = $Files
    }
}

function Invoke-SnowOcrModelSetValidation {
    param([Parameter(Mandatory)][string]$Executable,
        [Parameter(Mandatory)][string]$Detector, [Parameter(Mandatory)][string]$Recognizer,
        [Parameter(Mandatory)][string]$Dictionary,
        [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64', [switch]$RequireNative)

    # Cross packaging verifies the pinned bytes, while native package validation
    # initializes the actual bundled models. Never execute a target under emulation.
    if (-not (Test-SnowOcrNativeValidationHost -Architecture $Architecture -RequireNative:$RequireNative)) { return $false }
    Assert-SnowPeArchitecture -Path $Executable -Architecture $Architecture
    foreach ($path in @($Detector, $Recognizer, $Dictionary)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "OCR model validation input is missing: $path" }
    }
    $arguments = @('--validate-model-set', "`"$Detector`"", "`"$Recognizer`"", "`"$Dictionary`"")
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru -WindowStyle Hidden
    try {
        if (-not $process.WaitForExit(60000)) { $process.Kill(); throw 'Native OCR model initialization timed out.' }
        if ($process.ExitCode -ne 0) { throw "Native OCR model initialization failed ($($process.ExitCode))." }
    } finally { $process.Dispose() }
    return $true
}
