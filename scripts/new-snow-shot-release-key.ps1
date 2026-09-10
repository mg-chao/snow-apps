[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PrivateKeyPath,
    [Parameter(Mandatory)][string]$PublicKeysPath,
    [string]$KeyId = 'snow-shot-2026-01'
)
$ErrorActionPreference = 'Stop'
$PrivateKeyPath = [IO.Path]::GetFullPath($PrivateKeyPath)
$PublicKeysPath = [IO.Path]::GetFullPath($PublicKeysPath)
$repoPrefix = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if ($PrivateKeyPath.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The private signing key must be stored outside the repository.'
}
if ($PrivateKeyPath.Equals($PublicKeysPath, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The private and public key output paths must be different.'
}
if (Test-Path -LiteralPath $PrivateKeyPath) { throw 'The private key already exists; refusing to replace it.' }
if (Test-Path -LiteralPath $PublicKeysPath) { throw 'The public key file already exists; refusing to replace it.' }
$privateDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($PrivateKeyPath))
if ((Test-Path -LiteralPath $privateDirectory) -and @(Get-ChildItem -LiteralPath $privateDirectory -Force).Count -gt 0) {
    throw 'Use an empty, dedicated directory for the private signing key.'
}
$null = New-Item -ItemType Directory -Force -Path $privateDirectory
$acl = [Security.AccessControl.DirectorySecurity]::new()
$acl.SetAccessRuleProtection($true, $false)
$identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
$acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
    $identity, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
Set-Acl -LiteralPath $privateDirectory -AclObject $acl
$rsa = [Security.Cryptography.RSA]::Create(3072)
try {
    [IO.File]::WriteAllText($PrivateKeyPath, $rsa.ExportPkcs8PrivateKeyPem(), [Text.UTF8Encoding]::new($false))
    $parameters = $rsa.ExportParameters($false)
    $public = @{ keys = @(@{ id = $KeyId; modulus = [Convert]::ToBase64String($parameters.Modulus);
        exponent = [Convert]::ToBase64String($parameters.Exponent) }) } | ConvertTo-Json -Depth 5
    [IO.File]::WriteAllText($PublicKeysPath, $public + "`n", [Text.UTF8Encoding]::new($false))
} finally { $rsa.Dispose() }
Write-Output "Created signing key '$KeyId'. Back up the private key securely; never commit it."
