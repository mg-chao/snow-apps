#Requires -Version 7.2
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$fixtureRoot = Join-Path $repoRoot ("build/onnxruntime-directml-relocation-tests-" + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $fixtureRoot
$helper = (Join-Path $repoRoot 'cmake/vcpkg-overlay-ports/onnxruntime/relocate-directml-reference.cmake').Replace('\', '/')
$fixture = Join-Path $fixtureRoot 'relocation.cmake'

try {
    $fixtureContents = @'
cmake_minimum_required(VERSION 3.30)
include("@HELPER@")

foreach(_architecture IN ITEMS x64 arm64)
    set(_import "E:/Cache.With+[Literal]/packages/Microsoft.AI.DirectML.1.15.4/bin/${_architecture}-win/DirectML.lib")
    set(_unrelated "E:/OtherCache/packages/Microsoft.AI.DirectML.1.15.4/bin/${_architecture}-win/DirectML.lib")
    string(TOUPPER "${_architecture}" _architecture_upper)
    string(REPLACE "/${_architecture}-win/" "/${_architecture_upper}-win/" _ort_import "${_import}")
    string(TOLOWER "${_import}" _lower_import)
    string(TOUPPER "${_import}" _upper_import)
    foreach(_path IN ITEMS "${_import}" "${_ort_import}" "${_lower_import}" "${_upper_import}")
        set(_before "set(TargetCaseSensitive \"KeepCASE;${_path};${_path};${_unrelated};\${_IMPORT_PREFIX}/lib/DirectML.lib\")\n")
        set(_expected "set(TargetCaseSensitive \"KeepCASE;\${_IMPORT_PREFIX}/lib/DirectML.lib;\${_IMPORT_PREFIX}/lib/DirectML.lib;${_unrelated};\${_IMPORT_PREFIX}/lib/DirectML.lib\")\n")
        snow_onnxruntime_relocate_directml_reference(_actual "${_before}" "${_import}")
        if(NOT _actual STREQUAL _expected)
            message(FATAL_ERROR "${_architecture} DirectML relocation changed unrelated values or retained the build-tree path: ${_actual}")
        endif()
        snow_onnxruntime_relocate_directml_reference(_second "${_actual}" "${_import}")
        if(NOT _second STREQUAL _actual)
            message(FATAL_ERROR "DirectML relocation must be idempotent.")
        endif()
    endforeach()
    set(_different_architecture "${_unrelated};E:/Cache.With+[Literal]/packages/Microsoft.AI.DirectML.1.15.4/bin/arm64ec-win/DirectML.lib")
    snow_onnxruntime_relocate_directml_reference(_unchanged "${_different_architecture}" "${_import}")
    if(NOT _unchanged STREQUAL _different_architecture)
        message(FATAL_ERROR "DirectML relocation modified a different import library.")
    endif()
    message(STATUS "PASS: ${_architecture} import relocation preserves unrelated values and supports Windows casing")
endforeach()
'@
    [IO.File]::WriteAllText($fixture, $fixtureContents.Replace('@HELPER@', $helper) + "`n", [Text.UTF8Encoding]::new($false))
    & cmake -P $fixture
    if ($LASTEXITCODE -ne 0) { throw 'DirectML import relocation regression failed.' }
} finally {
    $resolvedFixtureRoot = [IO.Path]::GetFullPath($fixtureRoot)
    $expectedParent = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build')).TrimEnd('\') + '\'
    if (-not $resolvedFixtureRoot.StartsWith($expectedParent, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe DirectML fixture cleanup: $resolvedFixtureRoot"
    }
    Remove-Item -LiteralPath $resolvedFixtureRoot -Recurse -Force
}
