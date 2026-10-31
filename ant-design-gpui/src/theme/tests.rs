// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use serde_json::{Value, json};
use std::{
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    time::Duration,
};

pub(crate) trait FixtureValue {
    fn fixture_value(&self) -> Value;
}
pub(crate) trait FromFixture: Sized {
    fn from_fixture(value: &Value) -> Self;
}
pub(crate) fn parse_fixture<T: FromFixture>(value: &Value) -> T {
    T::from_fixture(value)
}

impl FixtureValue for f64 {
    fn fixture_value(&self) -> Value {
        json!(self)
    }
}
impl FromFixture for f64 {
    fn from_fixture(value: &Value) -> Self {
        value.as_f64().expect("numeric token")
    }
}
impl FixtureValue for bool {
    fn fixture_value(&self) -> Value {
        json!(self)
    }
}
impl FromFixture for bool {
    fn from_fixture(value: &Value) -> Self {
        value.as_bool().expect("boolean token")
    }
}
impl FixtureValue for Color {
    fn fixture_value(&self) -> Value {
        json!({"r":self.r,"g":self.g,"b":self.b,"a":self.a})
    }
}
impl FromFixture for Color {
    fn from_fixture(value: &Value) -> Self {
        Self {
            r: value["r"].as_u64().expect("red") as u8,
            g: value["g"].as_u64().expect("green") as u8,
            b: value["b"].as_u64().expect("blue") as u8,
            a: value["a"].as_f64().expect("alpha"),
        }
    }
}
impl<T: FixtureValue> FixtureValue for Option<T> {
    fn fixture_value(&self) -> Value {
        self.as_ref()
            .map(FixtureValue::fixture_value)
            .unwrap_or(Value::Null)
    }
}
impl<T: FromFixture> FromFixture for Option<T> {
    fn from_fixture(value: &Value) -> Self {
        if value.is_null() {
            None
        } else {
            Some(T::from_fixture(value))
        }
    }
}
impl FixtureValue for Duration {
    fn fixture_value(&self) -> Value {
        json!(self.as_secs_f64())
    }
}
impl FromFixture for Duration {
    fn from_fixture(value: &Value) -> Self {
        Self::from_secs_f64(value.as_f64().expect("duration"))
    }
}
impl FixtureValue for CubicBezier {
    fn fixture_value(&self) -> Value {
        json!([self.x1, self.y1, self.x2, self.y2])
    }
}
impl FromFixture for CubicBezier {
    fn from_fixture(value: &Value) -> Self {
        Self::new(
            parse_fixture(&value[0]),
            parse_fixture(&value[1]),
            parse_fixture(&value[2]),
            parse_fixture(&value[3]),
        )
    }
}
impl FixtureValue for FontStack {
    fn fixture_value(&self) -> Value {
        Value::Array(
            self.families()
                .iter()
                .map(|family| Value::String(family.to_string()))
                .collect(),
        )
    }
}
impl FromFixture for FontStack {
    fn from_fixture(value: &Value) -> Self {
        Self::new(
            value
                .as_array()
                .expect("font families")
                .iter()
                .map(|family| family.as_str().expect("font family")),
        )
    }
}
impl FixtureValue for ShadowList {
    fn fixture_value(&self) -> Value {
        Value::Array(self.iter().map(|shadow|json!({"x":shadow.x,"y":shadow.y,"blur":shadow.blur,"spread":shadow.spread,"color":shadow.color.fixture_value(),"inset":shadow.inset})).collect())
    }
}
impl FromFixture for ShadowList {
    fn from_fixture(value: &Value) -> Self {
        value
            .as_array()
            .expect("shadow list")
            .iter()
            .map(|shadow| Shadow {
                x: parse_fixture(&shadow["x"]),
                y: parse_fixture(&shadow["y"]),
                blur: parse_fixture(&shadow["blur"]),
                spread: parse_fixture(&shadow["spread"]),
                color: parse_fixture(&shadow["color"]),
                inset: parse_fixture(&shadow["inset"]),
            })
            .collect()
    }
}
impl FixtureValue for LineType {
    fn fixture_value(&self) -> Value {
        json!(match self {
            Self::Solid => "solid",
            Self::Dashed => "dashed",
            Self::Dotted => "dotted",
        })
    }
}
impl FromFixture for LineType {
    fn from_fixture(value: &Value) -> Self {
        match value.as_str().expect("line type") {
            "solid" => Self::Solid,
            "dashed" => Self::Dashed,
            "dotted" => Self::Dotted,
            _ => panic!("unsupported line type"),
        }
    }
}
impl FixtureValue for TextDecoration {
    fn fixture_value(&self) -> Value {
        json!(match self {
            Self::None => "none",
            Self::Underline => "underline",
            Self::LineThrough => "line-through",
        })
    }
}
impl FromFixture for TextDecoration {
    fn from_fixture(value: &Value) -> Self {
        match value.as_str().expect("decoration") {
            "none" => Self::None,
            "underline" => Self::Underline,
            "line-through" => Self::LineThrough,
            _ => panic!("unsupported decoration"),
        }
    }
}

pub(crate) fn palette_fixture(
    palettes: &PresetPalettes,
    object: &mut serde_json::Map<String, Value>,
) {
    for color in PresetColor::ALL {
        let palette = palettes.get(color);
        for (index, tone) in palette.tones.iter().enumerate() {
            object.insert(
                format!("{}{index}", color.name(), index = index + 1),
                tone.fixture_value(),
            );
        }
        object.insert(
            format!("{}Hover", color.name()),
            palette.hover.fixture_value(),
        );
        object.insert(
            format!("{}Active", color.name()),
            palette.active.fixture_value(),
        );
    }
}
pub(crate) fn palette_patch_from_fixture(object: &Value) -> PresetPalettesPatch {
    let mut patch = PresetPalettesPatch::default();
    for color in PresetColor::ALL {
        let entry = patch.get_mut(color);
        for (index, tone) in entry.tones.iter_mut().enumerate() {
            let canonical = format!("{}{index}", color.name(), index = index + 1);
            let legacy = format!("{}-{index}", color.name(), index = index + 1);
            if let Some(value) = object.get(&canonical).or_else(|| object.get(&legacy)) {
                *tone = Some(parse_fixture(value));
            }
        }
        if let Some(value) = object.get(format!("{}Hover", color.name())) {
            entry.hover = Some(parse_fixture(value));
        }
        if let Some(value) = object.get(format!("{}Active", color.name())) {
            entry.active = Some(parse_fixture(value));
        }
    }
    patch
}

#[test]
fn theme_default_dimensions_and_disabled_motion() {
    let theme = ThemeConfig::default().resolve();
    assert_eq!(theme.tokens.font_size, 14.0);
    assert_eq!(theme.tokens.control_height, 32.0);
    assert_eq!(theme.tokens.line_width_focus, 3.0);
    assert!(theme.seed.fixture().is_object());
    assert!(theme.map.fixture().is_object());
    assert!(theme.tokens.fixture().is_object());
    assert!(SeedTokenPatch::from_fixture(&json!({})).is_empty());
    assert!(TokenPatch::from_fixture(&json!({})).is_empty());
    let disabled = ThemeConfig {
        seed: SeedTokenPatch {
            motion: Some(false),
            focus_outline: Some(false),
            ..Default::default()
        },
        ..Default::default()
    }
    .resolve();
    assert_eq!(disabled.tokens.motion_duration_fast, Duration::ZERO);
    assert_eq!(disabled.tokens.line_width_focus, 0.0);
}

fn assert_fixture(actual: &Value, expected: &Value, path: &str) {
    match (actual, expected) {
        (Value::Number(left), Value::Number(right)) => {
            let left = left.as_f64().unwrap();
            let right = right.as_f64().unwrap();
            assert!(
                (left - right).abs() <= 1e-10 * right.abs().max(1.0),
                "{path}: actual {left}, expected {right}"
            );
        }
        (Value::Object(left), Value::Object(right)) => {
            assert_eq!(
                left.keys().collect::<std::collections::BTreeSet<_>>(),
                right.keys().collect::<std::collections::BTreeSet<_>>(),
                "{path}: token inventory differs"
            );
            for (key, value) in right {
                assert_fixture(&left[key], value, &format!("{path}.{key}"));
            }
        }
        (Value::Array(left), Value::Array(right)) => {
            assert_eq!(left.len(), right.len(), "{path}: array length differs");
            for (index, (left, right)) in left.iter().zip(right).enumerate() {
                assert_fixture(left, right, &format!("{path}[{index}]"));
            }
        }
        _ => assert_eq!(actual, expected, "{path}"),
    }
}

#[test]
fn theme_matches_independent_upstream_seed_map_and_alias_fixtures() {
    let fixture: Value =
        serde_json::from_str(include_str!("../../fixtures/theme/reference.json")).unwrap();
    assert_eq!(
        fixture["provenance"]["revision"],
        "f8f5595ab5e891fff6a2a06c5b3b98af643d008e"
    );
    for case in fixture["cases"].as_array().unwrap() {
        let chain =
            AlgorithmChain::new(case["input"]["algorithms"].as_array().unwrap().iter().map(
                |name| match name.as_str().unwrap() {
                    "default" => BuiltinAlgorithm::Default,
                    "dark" => BuiltinAlgorithm::Dark,
                    "compact" => BuiltinAlgorithm::Compact,
                    _ => panic!("unknown algorithm"),
                },
            ));
        let theme = ThemeConfig {
            seed: SeedTokenPatch::from_fixture(&case["input"]["seed"]),
            tokens: TokenPatch::from_fixture(&case["input"]["overrides"]),
            algorithms: Some(chain),
            ..Default::default()
        }
        .resolve();
        let name = case["name"].as_str().unwrap();
        assert_fixture(
            &theme.seed.fixture(),
            &case["seed"],
            &format!("{name}.seed"),
        );
        assert_fixture(&theme.map.fixture(), &case["map"], &format!("{name}.map"));
        assert_fixture(
            &theme.tokens.fixture(),
            &case["tokens"],
            &format!("{name}.tokens"),
        );
    }
}

#[test]
fn theme_nested_patches_reset_and_cross_seed_precedence() {
    let parent = ThemeConfig {
        tokens: TokenPatch {
            color_primary: Some(Color::rgb(0xff0000)),
            color_fill_secondary: Some(Color::rgb(0x334455)),
            ..Default::default()
        },
        algorithms: Some(AlgorithmChain::dark()),
        ..Default::default()
    };
    let local = ThemeConfig {
        seed: SeedTokenPatch {
            color_primary: Some(Color::rgb(0x008844)),
            ..Default::default()
        },
        ..Default::default()
    };
    let merged = ThemeConfig::merged(&parent, &local).resolve();
    assert_eq!(merged.seed.color_primary, Color::rgb(0x008844));
    assert_eq!(merged.tokens.color_fill_content, Color::rgb(0x334455));
    let reset = ThemeConfig::merged(
        &parent,
        &ThemeConfig {
            inherit: false,
            ..Default::default()
        },
    )
    .resolve();
    assert_eq!(reset, ThemeConfig::default().resolve());
    let automatic = ThemeConfig::merged(
        &ThemeConfig {
            seed: SeedTokenPatch {
                color_bg_base: Some(Some(Color::rgb(0x334455))),
                ..Default::default()
            },
            ..Default::default()
        },
        &ThemeConfig {
            seed: SeedTokenPatch {
                color_bg_base: Some(None),
                ..Default::default()
            },
            ..Default::default()
        },
    )
    .resolve();
    assert_eq!(automatic.tokens.color_bg_base, Color::WHITE);
}

#[test]
fn theme_custom_algorithms_keep_order_and_identity() {
    let algorithm: Arc<dyn MappingAlgorithm> =
        Arc::new(|seed: &SeedTokens, previous: Option<&MapTokens>| {
            let mut map = previous
                .cloned()
                .unwrap_or_else(|| BuiltinAlgorithm::Default.derive(seed, None));
            map.color_fill_secondary = Color::rgb(0x2468ac);
            map.control_height += 3.0;
            map
        });
    let first = AlgorithmChain::dark_compact().then_custom(algorithm.clone());
    let same = AlgorithmChain::dark_compact().then_custom(algorithm);
    assert_eq!(first, same);
    let theme = ThemeConfig {
        algorithms: Some(first),
        ..Default::default()
    }
    .resolve();
    assert_eq!(theme.tokens.control_height, 31.0);
    assert_eq!(theme.tokens.color_fill_content, Color::rgb(0x2468ac));
    let replacement = AlgorithmChain::default().then_custom(Arc::new(
        |seed: &SeedTokens, _: Option<&MapTokens>| BuiltinAlgorithm::Default.derive(seed, None),
    ));
    assert_ne!(same, replacement);
}

#[derive(Clone, Debug, Default, PartialEq)]
struct ProbePatch {
    size: Option<f64>,
    color: Option<Color>,
}
#[derive(Clone, Debug, PartialEq)]
struct ProbeTokens {
    size: f64,
    color: Color,
}
struct Probe;
static PROBE_RESOLVES: AtomicUsize = AtomicUsize::new(0);
impl ComponentTheme for Probe {
    type Tokens = ProbeTokens;
    type Patch = ProbePatch;
    fn default_tokens(global: &AliasTokens) -> Self::Tokens {
        PROBE_RESOLVES.fetch_add(1, Ordering::SeqCst);
        ProbeTokens {
            size: global.control_height,
            color: global.color_primary,
        }
    }
    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        ProbePatch {
            size: local.size.or(parent.size),
            color: local.color.or(parent.color),
        }
    }
    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        if let Some(size) = patch.size {
            tokens.size = size;
        }
        if let Some(color) = patch.color {
            tokens.color = color;
        }
    }
}

#[test]
fn theme_component_extensions_inherit_override_and_cache() {
    PROBE_RESOLVES.store(0, Ordering::SeqCst);
    let mut parent = ThemeConfig::default();
    parent.components.set::<Probe>(ProbePatch {
        size: Some(45.0),
        color: None,
    });
    let mut local = ThemeConfig::default();
    local.components.set::<Probe>(ProbePatch {
        size: None,
        color: Some(Color::rgb(0x123456)),
    });
    let theme = ThemeConfig::merged(&parent, &local).resolve();
    let first = theme.component::<Probe>();
    let second = theme.component::<Probe>();
    assert_eq!(
        *first,
        ProbeTokens {
            size: 45.0,
            color: Color::rgb(0x123456)
        }
    );
    assert!(Arc::ptr_eq(&first, &second));
    assert_eq!(PROBE_RESOLVES.load(Ordering::SeqCst), 1);
    let clone = theme.clone();
    assert!(Arc::ptr_eq(&first, &clone.component::<Probe>()));
    let mut custom = ThemeConfig::default();
    custom.components.set_theme(
        ComponentThemeConfig::<Probe>::default()
            .with_tokens(TokenPatch {
                control_height: Some(40.0),
                ..Default::default()
            })
            .with_algorithms(AlgorithmChain::compact()),
    );
    assert_eq!(custom.resolve().component::<Probe>().size, 36.0);
    assert_eq!(custom.resolve().tokens.control_height, 32.0);
    // Public Arc snapshots may be forked by consumers; a cache must never return
    // tokens derived from another copy after Arc::make_mut.
    let mut editable = ThemeConfig::default().resolve();
    let original = editable.clone();
    assert_eq!(editable.component::<Probe>().size, 32.0);
    Arc::make_mut(&mut editable.tokens).control_height = 99.0;
    assert_eq!(editable.component::<Probe>().size, 99.0);
    assert_eq!(original.component::<Probe>().size, 32.0);
    let mut direct = ThemeConfig::default();
    direct
        .components
        .set_theme(ComponentThemeConfig::<Probe>::default().without_algorithms());
    assert_eq!(
        ThemeConfig::merged(&custom, &direct)
            .resolve()
            .component::<Probe>()
            .size,
        40.0
    );
    assert_eq!(
        ThemeConfig::merged(&custom, &direct)
            .resolve()
            .tokens
            .control_height,
        32.0
    );
}

#[test]
fn theme_rejects_invalid_values_without_partial_resolution() {
    for seed in [
        SeedTokenPatch {
            font_size: Some(0.0),
            ..Default::default()
        },
        SeedTokenPatch {
            control_height: Some(f64::NAN),
            ..Default::default()
        },
        SeedTokenPatch {
            motion_unit: Some(-0.1),
            ..Default::default()
        },
        SeedTokenPatch {
            motion_unit: Some(f64::MAX),
            ..Default::default()
        },
        SeedTokenPatch {
            control_height: Some(f64::from(f32::MAX) * 2.0),
            ..Default::default()
        },
        SeedTokenPatch {
            font_size: Some(f64::from(f32::MAX)),
            ..Default::default()
        },
        SeedTokenPatch {
            color_primary: Some(Color {
                a: 2.0,
                ..Color::BLACK
            }),
            ..Default::default()
        },
        SeedTokenPatch {
            motion_ease_out: Some(CubicBezier::new(-1.0, 0.0, 1.0, 1.0)),
            ..Default::default()
        },
        SeedTokenPatch {
            motion_ease_out: Some(CubicBezier::new(0.0, f64::MAX, 1.0, 1.0)),
            ..Default::default()
        },
    ] {
        assert!(
            ThemeConfig {
                seed,
                ..Default::default()
            }
            .try_resolve()
            .is_err()
        );
    }
    assert!(
        ThemeConfig {
            tokens: TokenPatch {
                padding: Some(f64::INFINITY),
                ..Default::default()
            },
            ..Default::default()
        }
        .try_resolve()
        .is_err()
    );
    for tokens in [
        TokenPatch {
            padding: Some(f64::MAX),
            ..Default::default()
        },
        TokenPatch {
            font_size_sm: Some(f64::MIN_POSITIVE),
            ..Default::default()
        },
        TokenPatch {
            line_height: Some(0.0),
            ..Default::default()
        },
        TokenPatch {
            line_height: Some(f64::from(f32::MAX)),
            font_height: Some(1.0),
            ..Default::default()
        },
    ] {
        assert!(
            ThemeConfig {
                tokens,
                ..Default::default()
            }
            .try_resolve()
            .is_err()
        );
    }
    let invalid_algorithm = AlgorithmChain::default().then_custom(Arc::new(
        |seed: &SeedTokens, _: Option<&MapTokens>| {
            let mut map = BuiltinAlgorithm::Default.derive(seed, None);
            map.line_height = f64::NAN;
            map
        },
    ));
    assert!(
        ThemeConfig {
            algorithms: Some(invalid_algorithm),
            ..Default::default()
        }
        .try_resolve()
        .is_err()
    );
    let zero_font = AlgorithmChain::default().then_custom(Arc::new(
        |seed: &SeedTokens, _: Option<&MapTokens>| {
            let mut map = BuiltinAlgorithm::Default.derive(seed, None);
            map.font_size = 0.0;
            map
        },
    ));
    assert!(
        ThemeConfig {
            algorithms: Some(zero_font),
            ..Default::default()
        }
        .try_resolve()
        .is_err()
    );
}

#[test]
fn theme_palette_patches_merge_by_family_and_tone() {
    let mut parent = TokenPatch::default();
    parent.palettes.get_mut(PresetColor::Blue).tones[0] = Some(Color::rgb(0x123456));
    parent.palettes.get_mut(PresetColor::Pink).active = Some(Color::rgb(0x654321));
    let mut local = TokenPatch::default();
    local.palettes.get_mut(PresetColor::Blue).tones[1] = Some(Color::rgb(0xabcdef));
    local.palettes.get_mut(PresetColor::Blue).hover = Some(Color::rgb(0x987654));
    let resolved = ThemeConfig {
        tokens: TokenPatch::merged(&parent, &local),
        ..Default::default()
    }
    .resolve();
    let blue = resolved.tokens.palettes.get(PresetColor::Blue);
    assert_eq!(blue.tones[0], Color::rgb(0x123456));
    assert_eq!(blue.tones[1], Color::rgb(0xabcdef));
    assert_eq!(blue.hover, Color::rgb(0x987654));
    assert_eq!(
        resolved.tokens.palettes.get(PresetColor::Pink).active,
        Color::rgb(0x654321)
    );
}

#[test]
fn theme_color_font_and_palette_bridges_are_deterministic() {
    assert_eq!("#abc".parse::<Color>().unwrap(), Color::rgb(0xaabbcc));
    assert_eq!("#abcd".parse::<Color>().unwrap(), Color::rgba(0xaabbccdd));
    assert!("#非".parse::<Color>().is_err());
    assert!("#12345".parse::<Color>().is_err());
    let color = Color::rgba(0x4280ad80);
    let native = color.to_rgba();
    assert!((f64::from(native.a) - color.a).abs() < 1e-7);
    let _: gpui::Fill = color.into();
    let fonts = FontStack::new([
        "-apple-system",
        "BlinkMacSystemFont",
        "Segoe UI",
        "sans-serif",
        "Test CJK",
    ])
    .to_gpui_font();
    if cfg!(any(target_os = "macos", target_os = "ios")) {
        assert_eq!(fonts.family.as_ref(), ".SystemUIFont");
        assert_eq!(
            fonts.fallbacks.unwrap().fallback_list(),
            ["Segoe UI", "Test CJK"]
        );
    } else {
        assert_eq!(fonts.family.as_ref(), "Segoe UI");
        assert_eq!(
            fonts.fallbacks.unwrap().fallback_list(),
            [".SystemUIFont", "Test CJK"]
        );
    }
    assert_eq!(
        FontStack::default().to_gpui_font().family.as_ref(),
        ".SystemUIFont"
    );
    let theme = ThemeConfig::default().resolve();
    assert_eq!(
        theme.tokens.palettes.get(PresetColor::Blue).tone(6),
        Some(Color::rgb(0x1677ff))
    );
    assert!(
        theme
            .tokens
            .palettes
            .get(PresetColor::Blue)
            .tone(0)
            .is_none()
    );
    assert!(
        theme
            .tokens
            .palettes
            .get(PresetColor::Blue)
            .tone(11)
            .is_none()
    );
}

#[test]
fn font_stack_preserves_supported_platform_family_precedence() {
    // Browser CSS ignores Apple-only family aliases on other platforms. Turning
    // them into a native system font there masks Ant's first supported family.
    let default = ThemeConfig::default()
        .resolve()
        .tokens
        .font_family
        .to_gpui_font();
    assert_eq!(
        default.family.as_ref(),
        if cfg!(any(target_os = "macos", target_os = "ios")) {
            ".SystemUIFont"
        } else {
            "Segoe UI"
        }
    );
    let generic = FontStack::new(["system-ui", "Segoe UI"]).to_gpui_font();
    assert_eq!(generic.family.as_ref(), ".SystemUIFont");
    assert_eq!(generic.fallbacks.unwrap().fallback_list(), ["Segoe UI"]);
    let explicit = FontStack::new(["Test UI", "Test UI", "sans-serif"]).to_gpui_font();
    assert_eq!(explicit.family.as_ref(), "Test UI");
    assert_eq!(
        explicit.fallbacks.unwrap().fallback_list(),
        [".SystemUIFont"]
    );
}
