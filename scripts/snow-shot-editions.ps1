# Shared product identities. Original defaults preserve historical release backfills.
function Get-SnowShotEdition([ValidateSet('Full', 'Mini')][string]$Edition = 'Full',
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64') {
    $feedSuffix = if ($Architecture -eq 'arm64') { '-windows-arm64' } else { '' }
    if ($Edition -eq 'Mini') {
        return [pscustomobject]@{ Edition = 'Mini'; Product = 'snow-shot-mini'; Name = 'Snow Shot Mini';
            Executable = 'snow_shot_mini'; Registry = 'SnowShotMini'; Scoop = 'snowshot-mini';
            Winget = 'mg-chao.snow-shot-mini'; Feed = "latest-version-mini$feedSuffix.json";
            Variants = @('online', 'portable'); Component = 'SnowShotMini'; Marker = '__mini_data_directory' }
    }
    return [pscustomobject]@{ Edition = 'Full'; Product = 'snow-shot'; Name = 'Snow Shot';
        Executable = 'snow_shot'; Registry = 'SnowShot'; Scoop = 'snowshot';
        Winget = 'mg-chao.snow-shot'; Feed = "latest-version$feedSuffix.json";
        Variants = @('online', 'offline', 'portable'); Component = 'SnowShot'; Marker = '__data_directory' }
}

function Get-SnowShotReleaseArchitectures($Release, [string]$Version) {
    $architectures = @('x64')
    if (@($Release.assets | Where-Object { $_.name -match 'windows-arm64' }).Count) {
        $architectures += 'arm64'
        foreach ($architecture in $architectures) {
        foreach ($edition in @('Full', 'Mini')) {
            $product = Get-SnowShotEdition $edition $architecture
            $names = @($product.Feed)
            foreach ($variant in $product.Variants) {
                $base = "$($product.Product)-$Version-windows-$architecture-$variant"
                $names += if ($variant -eq 'portable') { "$base.zip" } else { @("$base.exe", "$base-update.zip") }
            }
            foreach ($name in $names) {
                if (@($Release.assets | Where-Object { $_.name -ceq $name }).Count -ne 1) {
                    throw "Windows release is missing or duplicates $name."
                }
            }
        }
        }
    }
    return $architectures
}

function Get-SnowShotReleaseEditions($Release, [string]$Version) {
    $mini = @($Release.assets | Where-Object { $_.name -like 'snow-shot-mini-*' -or $_.name -like 'latest-version-mini*.json' })
    if ($mini.Count -eq 0) { return @('Full') }
    # A partial paired release must not submit only one edition to a package manager.
    foreach ($edition in @('Full', 'Mini')) {
        $product = Get-SnowShotEdition $edition
        $names = @($product.Feed)
        foreach ($variant in $product.Variants) {
            if ($variant -eq 'portable') { $names += "$($product.Product)-$Version-windows-x64-portable.zip" }
            else { $names += @("$($product.Product)-$Version-windows-x64-$variant.exe", "$($product.Product)-$Version-windows-x64-$variant-update.zip") }
        }
        if (@($Release.assets | Where-Object { $_.name -like '*-macos-arm64.dmg' }).Count) {
            $names += "$($product.Product)-$Version-macos-arm64.dmg"
        }
        foreach ($name in $names) {
            if (@($Release.assets | Where-Object { $_.name -ceq $name }).Count -ne 1) {
                throw "Paired release is missing or duplicates $name."
            }
        }
    }
    return @('Full', 'Mini')
}
