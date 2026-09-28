//! GitHub is a transport mirror, never a replacement for signed update metadata.
use crate::error::{Result, UpdateError, require};
use reqwest::Url;
use serde_json::Value;

pub const API: &str = "https://api.github.com/repos/mg-chao/snow-apps/releases";
pub const REPOSITORY: &str = "https://github.com/mg-chao/snow-apps";

pub fn error() -> UpdateError {
    UpdateError::new(
        "metadata_download_failed",
        "Could not download signed update metadata",
    )
}

pub fn version(release: &Value) -> Option<semver::Version> {
    if release.get("draft")?.as_bool()? || release.get("prerelease")?.as_bool()? {
        return None;
    }
    let text = release
        .get("tag_name")?
        .as_str()?
        .strip_prefix('v')?
        .strip_suffix("_snow-shot")?;
    let version = semver::Version::parse(text).ok()?;
    version.pre.is_empty().then_some(version)
}

pub fn asset(release: &Value, name: &str) -> Result<Url> {
    let tag = release["tag_name"].as_str().ok_or_else(error)?;
    let assets = release["assets"].as_array().ok_or_else(error)?;
    let matches: Vec<_> = assets
        .iter()
        .filter(|asset| asset["name"].as_str() == Some(name))
        .collect();
    require(
        matches.len() == 1,
        "metadata_download_failed",
        "Could not download signed update metadata",
    )?;
    let url = Url::parse(
        matches[0]["browser_download_url"]
            .as_str()
            .ok_or_else(error)?,
    )
    .map_err(|_| error())?;
    let expected =
        Url::parse(&format!("{REPOSITORY}/releases/download/{tag}/{name}")).map_err(|_| error())?;
    require(
        url == expected,
        "metadata_download_failed",
        "Could not download signed update metadata",
    )?;
    Ok(url)
}

pub fn package_name(version: &str, path: &str) -> Result<String> {
    crate::contract::parse_version(version)?;
    let suffix = path
        .strip_prefix("setup/snow-shot_windows-x64-")
        .ok_or_else(error)?;
    require(
        matches!(
            suffix,
            "online.exe"
                | "offline.exe"
                | "online-update.zip"
                | "offline-update.zip"
                | "portable.zip"
        ),
        "metadata_download_failed",
        "Could not download signed update metadata",
    )?;
    Ok(format!("snow-shot-{version}-windows-x64-{suffix}"))
}

pub fn asset_origin(url: &Url) -> bool {
    url.scheme() == "https"
        && url.host_str() == Some("github.com")
        && url.port_or_known_default() == Some(443)
        && url.username().is_empty()
        && url.password().is_none()
        && url
            .path()
            .starts_with("/mg-chao/snow-apps/releases/download/")
}

pub fn redirect_allowed(origin: &Url, target: &Url) -> bool {
    if !target.username().is_empty() || target.password().is_some() {
        return false;
    }
    if origin.scheme() == target.scheme()
        && origin.host_str() == target.host_str()
        && origin.port_or_known_default() == target.port_or_known_default()
    {
        return true;
    }
    asset_origin(origin)
        && target.scheme() == "https"
        && target.port_or_known_default() == Some(443)
        && matches!(
            target.host_str(),
            Some("release-assets.githubusercontent.com" | "objects.githubusercontent.com")
        )
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn stable_tags_and_exact_assets_only() {
        for tag in ["v2.0.0-beta_snow-shot", "v2.0.0_other", "v02.0.0_snow-shot"] {
            assert!(version(&json!({"tag_name":tag,"draft":false,"prerelease":false})).is_none());
        }
        assert!(
            version(&json!({"tag_name":"v2.0.0_snow-shot","draft":false,"prerelease":false}))
                .is_some()
        );
        let mut release = json!({"tag_name":"v2.0.0_snow-shot","assets":[{"name":"latest-version.json",
            "browser_download_url":format!("{REPOSITORY}/releases/download/v2.0.0_snow-shot/latest-version.json")}]});
        assert!(asset(&release, "latest-version.json").is_ok());
        release["assets"][0]["browser_download_url"] =
            json!("https://evil.test/latest-version.json");
        assert!(asset(&release, "latest-version.json").is_err());
        assert_eq!(
            package_name("2.0.0", "setup/snow-shot_windows-x64-online-update.zip").unwrap(),
            "snow-shot-2.0.0-windows-x64-online-update.zip"
        );
        assert!(package_name("2.0.0", "../evil.zip").is_err());
    }
    #[test]
    fn redirects_are_scoped_to_github_assets() {
        let origin = Url::parse(&format!(
            "{REPOSITORY}/releases/download/v2.0.0_snow-shot/a.zip"
        ))
        .unwrap();
        let cdn =
            Url::parse("https://release-assets.githubusercontent.com/a?token=secret").unwrap();
        assert!(redirect_allowed(&origin, &cdn));
        for url in [
            "http://release-assets.githubusercontent.com/a",
            "https://evil.test/a",
            "https://user@release-assets.githubusercontent.com/a",
        ] {
            assert!(!redirect_allowed(&origin, &Url::parse(url).unwrap()));
        }
        assert!(!redirect_allowed(
            &Url::parse("https://snowshot.top").unwrap(),
            &cdn
        ));
    }
}
