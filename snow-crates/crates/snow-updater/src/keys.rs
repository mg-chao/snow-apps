//! Compile-time trusted release keys.
//!
//! The canonical key file lives beside the application that ships this
//! helper (`snow_shot/resources/update-trusted-keys.json`); the publisher
//! verifies the private key matches it before every release. Embedding via
//! `include_str!` keeps a single canonical copy, and cargo records the file
//! in dep-info so key rotation triggers a rebuild.

pub const TRUSTED_KEYS_JSON: &str = include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../snow_shot/resources/update-trusted-keys.json"
));

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compiled_trusted_keys_parse() {
        let parsed: serde_json::Value = serde_json::from_str(TRUSTED_KEYS_JSON)
            .expect("the checked-in trusted-keys document must be valid JSON");
        let keys = parsed["keys"].as_array().expect("a keys array");
        assert!(!keys.is_empty(), "at least one trusted key is embedded");
        for key in keys {
            assert!(key["id"].as_str().is_some_and(|id| !id.is_empty()));
            assert!(key["modulus"].as_str().is_some() && key["exponent"].as_str().is_some());
        }
    }
}
