#requires -Version 7.2
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$fixtureRoot = Join-Path $repoRoot ("build/qt-shader-contract-" + [guid]::NewGuid().ToString("N"))
$savedVariables = @{}
$savedExitCode = $global:LASTEXITCODE
foreach ($name in @("SNOW_TEST_QSB_LOG", "SNOW_TEST_QSB_VERSION", "SNOW_TEST_QSB_EXIT_CODE", "QTDIR")) {
    $savedVariables[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

try {
    $viewerRoot = Join-Path $fixtureRoot "snow_image_viewer"
    $fixtureScripts = Join-Path $viewerRoot "scripts"
    $toolchainRoot = Join-Path $fixtureRoot "scripts"
    $shaderRoot = Join-Path $viewerRoot "resources/shaders"
    New-Item -ItemType Directory -Path $fixtureScripts, $toolchainRoot, $shaderRoot -Force | Out-Null
    $compileScript = Join-Path $fixtureScripts "compile-shaders.ps1"
    Copy-Item -LiteralPath (Join-Path $repoRoot "snow_image_viewer/scripts/compile-shaders.ps1") `
        -Destination $compileScript
    $manifestPath = Join-Path $toolchainRoot "qt-toolchain.json"
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "qt-toolchain.json") -Destination $manifestPath
    $manifestText = Get-Content -LiteralPath $manifestPath -Raw
    $version = ($manifestText | ConvertFrom-Json).qtVersion
    $log = Join-Path $fixtureRoot "qsb-calls.jsonl"
    $env:SNOW_TEST_QSB_LOG = $log
    $env:SNOW_TEST_QSB_VERSION = "qsb $version"
    $env:SNOW_TEST_QSB_EXIT_CODE = "0"
    $fakeQsb = Join-Path $fixtureRoot "qsb fixture.ps1"
    Set-Content -LiteralPath $fakeQsb -Value @'
$call = @($args)
[IO.File]::AppendAllText($env:SNOW_TEST_QSB_LOG, (ConvertTo-Json -InputObject $call -Compress) + "`n")
$global:LASTEXITCODE = [int]$env:SNOW_TEST_QSB_EXIT_CODE
if ($call.Count -eq 1 -and $call[0] -ceq "--version") {
    Write-Output $env:SNOW_TEST_QSB_VERSION
}
'@
    $shaders = @("image.vert", "image.frag", "image_array.frag", "navigation.vert",
        "navigation.frag", "edit_resize.vert", "edit_resize.frag", "edit_resize_array.frag")
    foreach ($shader in $shaders) {
        Set-Content -LiteralPath (Join-Path $shaderRoot $shader) -Value "shader source fixture"
    }
    & $compileScript -QsbPath $fakeQsb
    $calls = @(Get-Content -LiteralPath $log | ForEach-Object { ,(ConvertFrom-Json -NoEnumerate -InputObject $_) })
    Require ($calls.Count -eq 9) "A matching Qt qsb must receive one version check and all eight shader recipes."
    Require ($calls[0].Count -eq 1 -and $calls[0][0] -ceq "--version") `
        "The qsb version must be checked before any shader command."
    for ($index = 0; $index -lt $shaders.Count; $index++) {
        $source = Join-Path $shaderRoot $shaders[$index]
        $expected = @("--glsl", "300 es,330", "--hlsl", "50", "--msl", "12", "-o", "$source.qsb", $source)
        $actual = $calls[$index + 1]
        Require ($actual.Count -eq $expected.Count) "Every shader recipe must retain its complete argument list."
        for ($argument = 0; $argument -lt $expected.Count; $argument++) {
            Require ($actual[$argument] -ceq $expected[$argument]) `
                "The recipe for $($shaders[$index]) changed at argument $argument."
        }
    }

    foreach ($invalid in @("mismatch", "missing", "version-failed", "manifest-invalid", "manifest-missing")) {
        Remove-Item -LiteralPath $log
        Set-Content -LiteralPath $manifestPath -Value $manifestText
        $env:SNOW_TEST_QSB_VERSION = "qsb $version"
        $env:SNOW_TEST_QSB_EXIT_CODE = "0"
        $selectedQsb = $fakeQsb
        switch ($invalid) {
            "mismatch" { $env:SNOW_TEST_QSB_VERSION = "qsb 6.11.1" }
            "missing" { $selectedQsb = Join-Path $fixtureRoot "missing-qsb.exe" }
            "version-failed" { $env:SNOW_TEST_QSB_EXIT_CODE = "1" }
            "manifest-invalid" { Set-Content -LiteralPath $manifestPath -Value '{"schemaVersion":2,"qtVersion":"6.12.0"}' }
            "manifest-missing" { Remove-Item -LiteralPath $manifestPath }
        }
        $rejected = $false
        try { & $compileScript -QsbPath $selectedQsb }
        catch { $rejected = $true }
        Require $rejected "Invalid shader toolchain case '$invalid' must be rejected."
        if (Test-Path -LiteralPath $log) {
            $failedCalls = @(Get-Content -LiteralPath $log | ForEach-Object { ,(ConvertFrom-Json -NoEnumerate -InputObject $_) })
            Require ($failedCalls.Count -eq 1 -and $failedCalls[0].Count -eq 1 -and
                $failedCalls[0][0] -ceq "--version") `
                "Invalid shader toolchain case '$invalid' must perform no shader commands."
        }
        else {
            # Keep cleanup identical for errors that occur before invoking qsb.
            New-Item -ItemType File -Path $log | Out-Null
        }
    }

    # Exercise default selection without invoking an actual shader baker.
    $scriptAst = [Management.Automation.Language.Parser]::ParseFile($compileScript, [ref]$null, [ref]$null)
    $resolver = $scriptAst.Find({
        param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq "Resolve-QtShaderBakerPath"
    }, $false)
    Require ($null -ne $resolver) "The shader baker resolver must be independently testable."
    Invoke-Expression $resolver.Extent.Text
    $script:fixturePathLookups = 0
    $script:fixturePathHasQsb = $true
    function Get-Command {
        param([string]$Name, [string]$ErrorAction)
        Require ($Name -ceq "qsb.exe") "The shader resolver must only query qsb on PATH."
        $script:fixturePathLookups++
        if ($script:fixturePathHasQsb) { return [pscustomobject]@{ Source = $fakeQsb } }
    }
    $env:QTDIR = Join-Path $fixtureRoot "selected Qt kit"
    $selectedBin = Join-Path $env:QTDIR "bin"
    New-Item -ItemType Directory -Path $selectedBin -Force | Out-Null
    $selectedBaker = Join-Path $selectedBin "qsb.exe"
    New-Item -ItemType File -Path $selectedBaker | Out-Null
    Require ((Resolve-QtShaderBakerPath) -ceq $selectedBaker -and $script:fixturePathLookups -eq 0) `
        "The selected Qt kit's shader baker must take precedence over PATH."
    Remove-Item -LiteralPath $selectedBaker
    Require ((Resolve-QtShaderBakerPath) -ceq $fakeQsb -and $script:fixturePathLookups -eq 1) `
        "A kit without qsb may use the version-checked PATH fallback."
    $script:fixturePathHasQsb = $false
    Require ([string]::IsNullOrEmpty((Resolve-QtShaderBakerPath))) `
        "Missing default shader tooling must remain unavailable."
    Write-Output "Qt shader toolchain contract tests passed."
}
finally {
    $global:LASTEXITCODE = $savedExitCode
    foreach ($name in $savedVariables.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedVariables[$name], "Process")
    }
    $resolvedFixture = [IO.Path]::GetFullPath($fixtureRoot)
    $resolvedBuild = [IO.Path]::GetFullPath((Join-Path $repoRoot "build")) + [IO.Path]::DirectorySeparatorChar
    if (-not $resolvedFixture.StartsWith($resolvedBuild, [StringComparison]::OrdinalIgnoreCase)) {
        throw "The shader fixture cleanup path escapes the build directory: $resolvedFixture"
    }
    if (Test-Path -LiteralPath $resolvedFixture) {
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}
