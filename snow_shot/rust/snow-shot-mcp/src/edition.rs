//! Compile-time product identity and the reduced Mini MCP contract.
pub const MINI: bool = cfg!(feature = "mini");
pub const PRODUCT: &str = if MINI { "snow-shot-mini" } else { "snow-shot" };
pub const PRODUCT_NAME: &str = if MINI { "Snow Shot Mini" } else { "Snow Shot" };
pub const MCP_NAME: &str = if MINI {
    "snow-shot-mini-mcp"
} else {
    "snow-shot-mcp"
};
pub const REGISTRY_NAME: &str = if MINI { "SnowShotMini" } else { "SnowShot" };
pub const APP_NAME: &str = if MINI { "snow_shot_mini" } else { "snow_shot" };

pub fn method_enabled(name: &str) -> bool {
    !MINI
        || !matches!(
            name,
            "snow_shot_screenshot_translate"
                | "snow_shot_models_list"
                | "snow_shot_models_update"
                | "snow_shot_credentials_set"
                | "snow_shot_translation_catalog"
                | "snow_shot_translation_start"
        )
}

pub fn input_enabled(name: &str, input: &serde_json::Value) -> bool {
    if !method_enabled(name) {
        return false;
    }
    if !MINI {
        return true;
    }
    match name {
        "snow_shot_screenshot_recognize" | "snow_shot_document_recognize" => {
            input.get("kind").and_then(serde_json::Value::as_str) == Some("text")
        }
        "snow_shot_pinned_edit" => {
            let action = input.get("action").and_then(serde_json::Value::as_str);
            action != Some("translate")
                && (action != Some("recognize")
                    || input
                        .get("payload")
                        .and_then(|p| p.get("kind"))
                        .and_then(serde_json::Value::as_str)
                        == Some("text"))
        }
        _ => true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn retained_ids_and_recognition_modes_follow_the_compiled_edition() {
        assert!(method_enabled("snow_shot_document_recognize"));
        assert_eq!(method_enabled("snow_shot_translation_start"), !MINI);
        for kind in ["table", "qr", "latex", "markdown", "html"] {
            assert_eq!(
                input_enabled("snow_shot_document_recognize", &json!({"kind":kind})),
                !MINI
            );
            assert_eq!(
                input_enabled(
                    "snow_shot_pinned_edit",
                    &json!({"action":"recognize","payload":{"kind":kind}})
                ),
                !MINI
            );
        }
        assert!(input_enabled(
            "snow_shot_document_recognize",
            &json!({"kind":"text"})
        ));
    }
}
