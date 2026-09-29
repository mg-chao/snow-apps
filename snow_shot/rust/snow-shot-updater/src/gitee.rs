//! Gitee transports the same signed release as GitHub.
use crate::error::{Result, UpdateError, require};
use reqwest::Url;
use serde_json::Value;

pub fn error() -> UpdateError {
    UpdateError::new(
        "metadata_download_failed",
        "Could not download signed update metadata",
    )
}

pub fn version(release: &Value) -> Option<semver::Version> {
    if release.get("draft").and_then(Value::as_bool) == Some(true) {
        return None;
    }
    let text = release
        .get("tag_name")?
        .as_str()?
        .strip_prefix('v')?
        .strip_suffix("_snow-shot")?;
    semver::Version::parse(text).ok()
}

pub fn attachment<'a>(files: &'a Value, name: &str) -> Result<&'a Value> {
    let matches: Vec<_> = files
        .as_array()
        .ok_or_else(error)?
        .iter()
        .filter(|file| file["name"].as_str() == Some(name))
        .collect();
    require(
        matches.len() == 1,
        "metadata_download_failed",
        "Could not download signed update metadata",
    )?;
    Ok(matches[0])
}

pub fn asset(files: &Value, tag: &str, name: &str) -> Result<Url> {
    let file = attachment(files, name)?;
    let url = Url::parse(file["browser_download_url"].as_str().ok_or_else(error)?)
        .map_err(|_| error())?;
    let expected = Url::parse(&format!(
        "https://gitee.com/mg-chao/snow-apps/releases/download/{tag}/{name}"
    ))
    .map_err(|_| error())?;
    require(
        url == expected,
        "metadata_download_failed",
        "Could not download signed update metadata",
    )?;
    Ok(url)
}

pub fn redirect_allowed(origin: &Url, target: &Url) -> bool {
    origin.scheme() == "https"
        && origin.host_str() == Some("gitee.com")
        && origin
            .path()
            .starts_with("/mg-chao/snow-apps/releases/download/")
        && target.scheme() == "https"
        && target.port_or_known_default() == Some(443)
        && target.username().is_empty()
        && target.password().is_none()
        && matches!(target.host_str(), Some("gitee.com" | "foruda.gitee.com"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn tags_and_attachment_urls_are_repository_scoped() {
        assert!(version(&json!({"tag_name":"v2.0.0-beta_snow-shot"})).is_some());
        assert!(version(&json!({"tag_name":"v2.0.0_snow-shot","draft":true})).is_none());
        let files = json!([{"name":"latest-version.json","browser_download_url":
            "https://gitee.com/mg-chao/snow-apps/releases/download/v2.0.0_snow-shot/latest-version.json"}]);
        assert!(asset(&files, "v2.0.0_snow-shot", "latest-version.json").is_ok());
        assert!(asset(&files, "v2.0.0_snow-shot", "missing").is_err());
        for url in [
            "https://evil.test/mg-chao/snow-apps/releases/download/v2.0.0_snow-shot/latest-version.json",
            "https://gitee.com/other/repo/attach_files/123/download/latest-version.json",
            "http://gitee.com/mg-chao/snow-apps/releases/download/v2.0.0_snow-shot/latest-version.json",
        ] {
            let bad = json!([{"name":"latest-version.json","browser_download_url":url}]);
            assert!(asset(&bad, "v2.0.0_snow-shot", "latest-version.json").is_err());
        }
    }
}
