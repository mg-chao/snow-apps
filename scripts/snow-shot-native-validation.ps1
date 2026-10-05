# Production publication requires native execution evidence bound to the final package bytes.
function Assert-SnowShotNativeValidation {
    param([Parameter(Mandatory)]$Manifest, [Parameter(Mandatory)][string]$ArtifactPath,
        [Parameter(Mandatory)][ValidateSet('windows-x64', 'windows-arm64')][string]$Platform,
        [string]$ProofDirectory = '')
    $proof = $Manifest.PSObject.Properties['NativeValidation']
    $value = if ($proof) { $proof.Value } else { $null }
    if ($ProofDirectory) {
        $external = Join-Path $ProofDirectory "$([IO.Path]::GetFileName($ArtifactPath)).native-validation.json"
        if (Test-Path -LiteralPath $external -PathType Leaf) {
            $value = Get-Content -LiteralPath $external -Raw | ConvertFrom-Json
        }
    }
    if (-not $value -or $value.Passed -isnot [bool] -or $value.Passed -ne $true -or
        $value.Platform -cne $Platform -or $value.HostPlatform -cne $Platform) {
        throw "Native validation is required for $([IO.Path]::GetFileName($ArtifactPath)) on $Platform. Cross packaging and static audit cannot satisfy this gate."
    }
    $matches = @($value.Artifacts | Where-Object { $_.Name -ceq [IO.Path]::GetFileName($ArtifactPath) })
    $item = Get-Item -LiteralPath $ArtifactPath
    $hash = (Get-FileHash -LiteralPath $ArtifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($matches.Count -ne 1 -or $matches[0].Bytes -ne $item.Length -or $matches[0].Sha256 -cne $hash) {
        throw "Native validation does not match the exact published bytes: $($item.Name)."
    }
    return $value
}
