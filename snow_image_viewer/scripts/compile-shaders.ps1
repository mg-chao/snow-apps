param(
    [string]$QsbPath = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$shaderRoot = Join-Path $projectRoot "resources\shaders"

if (-not $QsbPath) {
    $qsbCommand = Get-Command qsb.exe -ErrorAction SilentlyContinue
    if ($qsbCommand) {
        $QsbPath = $qsbCommand.Source
    }
    if (-not $QsbPath -and $env:QTDIR) {
        $candidate = Join-Path $env:QTDIR "bin\qsb.exe"
        if (Test-Path -LiteralPath $candidate) { $QsbPath = $candidate }
    }
}
if (-not $QsbPath -or -not (Test-Path $QsbPath)) {
    throw "A Qt 6.11.x qsb executable is required. Pass it with -QsbPath."
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
