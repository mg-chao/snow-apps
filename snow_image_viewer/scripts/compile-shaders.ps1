param(
    [string]$QsbPath = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$shaderRoot = Join-Path $projectRoot "resources\shaders"
$qtToolchain = Get-Content -LiteralPath (Join-Path $projectRoot "../scripts/qt-toolchain.json") -Raw |
    ConvertFrom-Json
if (($qtToolchain.schemaVersion -isnot [int] -and $qtToolchain.schemaVersion -isnot [long]) -or
    $qtToolchain.schemaVersion -ne 1 -or
    $qtToolchain.qtVersion -isnot [string] -or $qtToolchain.qtVersion -notmatch '^6\.\d+\.\d+$') {
    throw "The Qt toolchain manifest must specify schema version 1 and a valid Qt 6 version."
}

function Resolve-QtShaderBakerPath {
    if ($env:QTDIR) {
        $candidate = Join-Path $env:QTDIR "bin\qsb.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    $qsbCommand = Get-Command qsb.exe -ErrorAction SilentlyContinue
    if ($qsbCommand) { return $qsbCommand.Source }
    return ""
}
if (-not $QsbPath) { $QsbPath = Resolve-QtShaderBakerPath }
if (-not $QsbPath -or -not (Test-Path -LiteralPath $QsbPath -PathType Leaf)) {
    throw "A Qt $($qtToolchain.qtVersion) qsb executable is required. Pass it with -QsbPath."
}
$qsbVersion = & $QsbPath --version 2>&1
if ($LASTEXITCODE -ne 0 -or
    ($qsbVersion -join "`n").Trim() -cne "qsb $($qtToolchain.qtVersion)") {
    throw "The shader baker must match Qt $($qtToolchain.qtVersion). Selected: $QsbPath ($qsbVersion)."
}

foreach ($shader in @("image.vert", "image.frag", "image_array.frag",
                      "navigation.vert", "navigation.frag", "edit_resize.vert",
                      "edit_resize.frag", "edit_resize_array.frag")) {
    $source = Join-Path $shaderRoot $shader
    $destination = "$source.qsb"
    & $QsbPath --glsl "300 es,330" --hlsl 50 --msl 12 -o $destination $source
    if ($LASTEXITCODE -ne 0) {
        throw "qsb failed for $shader."
    }
}
