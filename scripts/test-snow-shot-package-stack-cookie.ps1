#Requires -Version 7.2
[CmdletBinding()]
param([Parameter(Mandatory)][string]$PythonExecutable)

$ErrorActionPreference = 'Stop'

function Require([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

# Import the release guard without configuring, building, or packaging artifacts.
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'package-snow-shot.ps1'), [ref]$null, [ref]$parseErrors)
Require ($parseErrors.Count -eq 0) 'The Full package script must parse.'
$definition = $ast.Find({
    param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq 'Assert-SnowArm64StackCookieSafety'
}, $false)
Require ($null -ne $definition) 'The Full package script must define its ARM64 image guard.'
# A dynamically imported script block has no source path; preserve its script directory.
$guardDefinition = $definition.Extent.Text.Replace('$PSScriptRoot', "'$($PSScriptRoot.Replace("'", "''"))'")
. ([scriptblock]::Create($guardDefinition))
foreach ($name in @('package-snow-shot.ps1', 'package-snow-shot-mini.ps1')) {
    $packageAst = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $PSScriptRoot $name), [ref]$null, [ref]$parseErrors)
    Require ($parseErrors.Count -eq 0) "The package script must parse: $name"
    $calls = @($packageAst.FindAll({
        param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
            $node.GetCommandName() -ceq 'Assert-SnowArm64StackCookieSafety'
    }, $true))
    Require ($calls.Count -eq 1) "Each edition must invoke the staged ARM64 image guard: $name"
    Require ($calls[0].Extent.Text -match '-Path \$stage\b') (
        "Each edition must audit the entire final payload stage: $name")
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('snow-package-cookie-' + [guid]::NewGuid().ToString('N'))
try {
    $null = New-Item -ItemType Directory -Path $testRoot
    # Reuse the compiled-image scanner's PE fixtures and exercise its real CLI.
    $fixtureScript = @'
import importlib.util
from pathlib import Path
import sys
spec = importlib.util.spec_from_file_location("cookie_fixture", sys.argv[1])
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
root = Path(sys.argv[2])
(root / "safe application.exe").write_bytes(fixture.fixture([0xA8C17BFD, fixture.GUARD.RET_LR]))
(root / "unsafe.exe").write_bytes(fixture.fixture([0x910183FF, fixture.GUARD.RET_LR]))
(root / "unknown.exe").write_bytes(fixture.fixture([fixture.GUARD.RET_LR], helper=[fixture.GUARD.NOP] * 8))
safe = (root / "safe application.exe").read_bytes()
unsafe = (root / "unsafe.exe").read_bytes()
for name, worker, handler in [("safe-stage", safe, safe),
                              ("unsafe-worker-stage", unsafe, safe),
                              ("unsafe-handler-stage", safe, unsafe)]:
    stage = root / name / "bin"
    runtime = stage / "assets/ocr/runtimes/version/windows-arm64"
    runtime.mkdir(parents=True)
    (stage / "snow_shot.exe").write_bytes(safe)
    (stage / "crashpad_handler.exe").write_bytes(handler)
    (runtime / "snow-ocr-process.exe").write_bytes(worker)
(root / "empty-stage").mkdir()
'@
    & $PythonExecutable -c $fixtureScript (Join-Path $PSScriptRoot 'test-arm64-stack-cookie.py') $testRoot
    Require ($LASTEXITCODE -eq 0) 'The PE fixtures must be generated.'

    function Test-GuardCase([string]$Name, [string]$Image, [string]$Cache,
        [string]$Architecture = 'arm64', [string]$ExpectedFailure) {
        $cachePath = Join-Path $testRoot 'CMakeCache.txt'
        [IO.File]::WriteAllText($cachePath, $Cache)
        $failure = $null
        try {
            Assert-SnowArm64StackCookieSafety -Path (Join-Path $testRoot $Image) `
                -BuildDirectory $testRoot -Architecture $Architecture
        } catch { $failure = $_.Exception.Message }
        if ($ExpectedFailure) {
            Require ($null -ne $failure -and $failure -like $ExpectedFailure) (
                "The package guard must reject $Name for the expected reason: $failure")
        } else {
            Require ($null -eq $failure) "The package guard rejected $Name`: $failure"
        }
        Write-Output "PASS: $Name"
    }

    $pythonPath = (Resolve-Path -LiteralPath $PythonExecutable).Path
    $internalCache = "_Python3_EXECUTABLE:INTERNAL=$pythonPath`n"
    Test-GuardCase safe-internal-cache 'safe application.exe' $internalCache
    Test-GuardCase safe-explicit-cache 'safe application.exe' "Python3_EXECUTABLE:FILEPATH=$pythonPath`n"
    Test-GuardCase corrupt-return-address 'unsafe.exe' $internalCache -ExpectedFailure 'ARM64 stack-cookie audit rejected*'
    Test-GuardCase unknown-cookie-helper 'unknown.exe' $internalCache -ExpectedFailure 'ARM64 stack-cookie audit rejected*'
    Test-GuardCase safe-payload-stage 'safe-stage' $internalCache
    Test-GuardCase safe-main-unsafe-worker 'unsafe-worker-stage' $internalCache `
        -ExpectedFailure 'ARM64 stack-cookie audit rejected*: *snow-ocr-process.exe'
    Test-GuardCase safe-main-unsafe-handler 'unsafe-handler-stage' $internalCache `
        -ExpectedFailure 'ARM64 stack-cookie audit rejected*: *crashpad_handler.exe'
    Test-GuardCase empty-payload-stage 'empty-stage' $internalCache `
        -ExpectedFailure 'ARM64 stack-cookie audit found no staged executables*'
    Test-GuardCase missing-interpreter 'safe application.exe' '' -ExpectedFailure 'ARM64 stack-cookie audit requires one available Python3*'
    Test-GuardCase unavailable-interpreter 'safe application.exe' (
        "_Python3_EXECUTABLE:INTERNAL=$(Join-Path $testRoot 'missing-python')`n") `
        -ExpectedFailure 'ARM64 stack-cookie audit requires one available Python3*'
    Test-GuardCase x64-unaffected 'missing-image.exe' '' -Architecture x64
    Remove-Item -LiteralPath (Join-Path $testRoot 'CMakeCache.txt')
    $failure = $null
    try {
        Assert-SnowArm64StackCookieSafety -Path (Join-Path $testRoot 'safe application.exe') `
            -BuildDirectory $testRoot -Architecture arm64
    } catch { $failure = $_.Exception.Message }
    Require ($failure -like 'ARM64 stack-cookie audit requires the configured CMake cache*') (
        'ARM64 packaging must reject an unconfigured build.')
    Write-Output 'PASS: missing-build-cache'
} finally {
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
