//! Product identities are selected at compile time, never by downloaded metadata.
pub const MINI: bool = cfg!(feature = "mini");
macro_rules! identity {
    ($name:ident, $full:literal, $mini:literal) => {
        pub const $name: &str = if MINI { $mini } else { $full };
    };
}
identity!(PRODUCT, "snow-shot", "snow-shot-mini");
identity!(PRODUCT_NAME, "Snow Shot", "Snow Shot Mini");
identity!(REGISTRY_NAME, "SnowShot", "SnowShotMini");
identity!(APP_PATH, "bin/snow_shot.exe", "bin/snow_shot_mini.exe");
identity!(
    UPDATER_PATH,
    "bin/snow-shot-updater.exe",
    "bin/snow-shot-mini-updater.exe"
);
identity!(
    UPDATER_NONWINDOWS_PATH,
    "bin/snow-shot-updater",
    "bin/snow-shot-mini-updater"
);
identity!(UPDATER_NAME, "snow-shot-updater", "snow-shot-mini-updater");
identity!(
    INSTALLATION_RECORD,
    "snow-shot-installation.json",
    "snow-shot-mini-installation.json"
);
identity!(UPDATE_WORK, ".snow-shot-update", ".snow-shot-mini-update");
identity!(SHARE_PREFIX, "share/snow-shot/", "share/snow-shot-mini/");
identity!(
    PACKAGE_PREFIX,
    "setup/snow-shot_windows-x64-",
    "setup/snow-shot-mini_windows-x64-"
);
identity!(FEED_NAME, "latest-version.json", "latest-version-mini.json");
identity!(TEMP_PREFIX, "snow-shot-updater-", "snow-shot-mini-updater-");
identity!(
    MARKER_PATH,
    "bin/__data_directory",
    "bin/__mini_data_directory"
);
identity!(
    INSTALL_KEY,
    "Software\\Snow Apps\\SnowShot",
    "Software\\Snow Apps\\SnowShotMini"
);
identity!(
    UNINSTALL_KEY,
    "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SnowShot",
    "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SnowShotMini"
);

pub fn product_matches(value: Option<&serde_json::Value>) -> bool {
    match value {
        None => !MINI,
        Some(value) => value.as_str() == Some(PRODUCT),
    }
}

pub fn installation_variant(variant: &str) -> bool {
    matches!(variant, "online" | "portable") || (!MINI && variant == "offline")
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn product_binding_accepts_only_this_product_and_legacy_full_records() {
        assert!(product_matches(Some(&json!(PRODUCT))));
        assert!(!product_matches(Some(&json!(if MINI {
            "snow-shot"
        } else {
            "snow-shot-mini"
        }))));
        assert!(!product_matches(Some(&json!(null))));
        assert_eq!(product_matches(None), !MINI);
        assert_eq!(installation_variant("offline"), !MINI);
        assert!(installation_variant("online"));
        assert!(installation_variant("portable"));
    }
}
