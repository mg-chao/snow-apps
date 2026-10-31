// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::theme::{AlgorithmChain, ComponentThemeConfig, ThemeConfig, TokenPatch};

fn resolve(options: ButtonStyleOptions) -> ButtonStyle {
    ButtonStyle::resolve(&ResolvedTheme::default(), options)
}

#[test]
fn button_style_matches_reference_default_geometry_and_colors() {
    let style = resolve(Default::default());
    assert_eq!(style.metrics.height, 32.);
    assert_eq!(style.metrics.font_size, 14.);
    assert_eq!(style.metrics.padding_inline, 15.);
    assert_eq!(style.metrics.radius, 6.);
    assert_eq!(style.metrics.icon_gap, 8.);
    assert_eq!(style.metrics.font_weight, 400.);
    assert_eq!(style.rest.background, Color::WHITE);
    assert_eq!(style.rest.border, Color::rgb(0xd9d9d9));
    assert_eq!(style.rest.text, Color::BLACK.with_alpha(0.88));
    assert_eq!(style.hover.text, Color::rgb(0x4096ff));
    assert_eq!(style.hover.border, style.hover.text);
    assert_eq!(style.active.text, Color::rgb(0x0958d9));
    assert_eq!(style.shadow[0].y, 2.);
    assert_eq!(style.shadow[0].color, Color::BLACK.with_alpha(0.02));
    assert_eq!(style.focus_width, 3.);
    assert_eq!(style.focus_offset, 1.);
    assert_eq!(style.focus_color, Color::rgb(0x91caff));
    assert_eq!(style.loading_opacity, 0.65);
}

#[test]
fn button_style_sizes_shapes_and_icon_metrics_follow_reference() {
    for (size, height, font_size, padding, radius) in [
        (Size::Small, 24., 14., 7., 4.),
        (Size::Medium, 32., 14., 15., 6.),
        (Size::Large, 40., 16., 15., 8.),
    ] {
        let options = ButtonStyleOptions {
            size,
            ..Default::default()
        };
        let style = resolve(options);
        assert_eq!(
            (
                style.metrics.height,
                style.metrics.font_size,
                style.metrics.padding_inline,
                style.metrics.radius
            ),
            (height, font_size, padding, radius)
        );
        let square = resolve(ButtonStyleOptions {
            shape: ButtonShape::Square,
            ..options
        });
        assert_eq!(square.metrics, style.metrics);
        let circle = resolve(ButtonStyleOptions {
            shape: ButtonShape::Circle,
            ..options
        });
        assert_eq!(circle.metrics.min_width, height);
        assert_eq!(circle.metrics.padding_inline, 0.);
        assert_eq!(circle.metrics.radius, height / 2.);
        let round = resolve(ButtonStyleOptions {
            shape: ButtonShape::Round,
            ..options
        });
        assert_eq!(round.metrics.radius, height);
        assert_eq!(round.metrics.padding_inline, padding);
        let icon = resolve(ButtonStyleOptions {
            icon_only: true,
            ..options
        });
        assert_eq!(icon.metrics.min_width, height);
        assert_eq!(icon.metrics.padding_inline, 0.);
        assert_eq!(icon.metrics.icon_size, font_size);
    }
}

#[test]
fn button_style_primary_danger_and_default_solid_and_filled_are_distinct() {
    let theme = ResolvedTheme::default();
    let primary = ButtonStyle::resolve(
        &theme,
        ButtonStyleOptions {
            color: ButtonColor::Primary,
            variant: ButtonVariant::Solid,
            ..Default::default()
        },
    );
    assert_eq!(primary.rest.background, Color::rgb(0x1677ff));
    assert_eq!(primary.hover.background, Color::rgb(0x4096ff));
    assert_eq!(primary.active.background, Color::rgb(0x0958d9));
    assert_eq!(primary.rest.text, Color::WHITE);
    assert_eq!(primary.rest.border, Color::TRANSPARENT);
    let solid = ButtonStyle::resolve(
        &theme,
        ButtonStyleOptions {
            variant: ButtonVariant::Solid,
            ..Default::default()
        },
    );
    assert_eq!(solid.rest.background, Color::BLACK);
    assert_eq!(solid.rest.text, Color::WHITE);
    let danger = ButtonStyle::resolve(
        &theme,
        ButtonStyleOptions {
            color: ButtonColor::Danger,
            variant: ButtonVariant::Filled,
            ..Default::default()
        },
    );
    assert_eq!(danger.rest.text, theme.tokens.color_error);
    assert_eq!(danger.hover.text, danger.rest.text);
    assert_eq!(
        danger.hover.background,
        theme.tokens.color_error_bg_filled_hover
    );
    assert_eq!(danger.active.background, theme.tokens.color_error_bg_active);
    assert!(danger.shadow.is_empty());
}

#[test]
fn button_style_every_preset_uses_its_native_palette_for_every_variant() {
    let theme = ResolvedTheme::default();
    for preset in PresetColor::ALL {
        let palette = theme.tokens.palettes.get(preset);
        for variant in ButtonVariant::ALL {
            let style = ButtonStyle::resolve(
                &theme,
                ButtonStyleOptions {
                    color: ButtonColor::Preset(preset),
                    variant,
                    ..Default::default()
                },
            );
            match variant {
                ButtonVariant::Solid => {
                    assert_eq!(style.rest.background, palette.tones[5]);
                    assert_eq!(style.hover.background, palette.hover);
                    assert_eq!(style.active.background, palette.active);
                    assert_eq!(style.rest.text, Color::WHITE);
                }
                ButtonVariant::Filled => {
                    assert_eq!(style.rest.background, palette.tones[0]);
                    assert_eq!(style.hover.background, palette.tones[1]);
                    assert_eq!(style.active.background, palette.tones[2]);
                    assert_eq!(style.hover.text, palette.tones[5]);
                }
                ButtonVariant::Text => {
                    assert_eq!(style.rest.background, Color::TRANSPARENT);
                    assert_eq!(style.hover.background, palette.tones[0]);
                    assert_eq!(style.active.background, palette.tones[2]);
                    assert_eq!(style.hover.text, palette.hover);
                }
                ButtonVariant::Link => {
                    assert_eq!(style.hover.background, Color::TRANSPARENT);
                    assert_eq!(style.rest.text, palette.tones[5]);
                    assert_eq!(style.active.text, palette.active);
                }
                ButtonVariant::Outlined | ButtonVariant::Dashed => {
                    assert_eq!(style.rest.border, palette.tones[5]);
                    assert_eq!(style.hover.border, palette.hover);
                    assert_eq!(style.active.border, palette.active);
                    assert_eq!(
                        style.line_type == LineType::Dashed,
                        variant == ButtonVariant::Dashed
                    );
                }
            }
        }
    }
}

#[test]
fn button_style_disabled_states_have_no_shadow_or_hover_response() {
    for color in [
        ButtonColor::Default,
        ButtonColor::Primary,
        ButtonColor::Danger,
        ButtonColor::Preset(PresetColor::Purple),
    ] {
        for variant in ButtonVariant::ALL {
            let style = resolve(ButtonStyleOptions {
                color,
                variant,
                disabled: true,
                ..Default::default()
            });
            assert_eq!(style.rest, style.hover);
            assert_eq!(style.rest, style.active);
            assert_eq!(style.rest.text, Color::BLACK.with_alpha(0.25));
            assert_eq!(style.focus_width, 0.);
            assert!(style.shadow.is_empty());
            if variant.is_unbordered() {
                assert_eq!(style.rest.background, Color::TRANSPARENT);
                assert_eq!(style.rest.border, Color::TRANSPARENT);
            } else {
                assert_eq!(style.rest.background, Color::BLACK.with_alpha(0.04));
                assert_eq!(style.rest.border, Color::rgb(0xd9d9d9));
            }
        }
    }
}

#[test]
fn button_style_ghost_preserves_colored_outline_and_ignores_text_variant() {
    let ghost = resolve(ButtonStyleOptions {
        color: ButtonColor::Primary,
        variant: ButtonVariant::Solid,
        ghost: true,
        ..Default::default()
    });
    assert_eq!(ghost.rest.background, Color::TRANSPARENT);
    assert_eq!(ghost.hover.background, Color::TRANSPARENT);
    assert_eq!(ghost.rest.border, Color::rgb(0x1677ff));
    assert_eq!(ghost.rest.text, ghost.rest.border);
    assert!(ghost.shadow.is_empty());
    let ghost = resolve(ButtonStyleOptions {
        ghost: true,
        ..Default::default()
    });
    assert_eq!(ghost.rest.text, Color::WHITE);
    assert_eq!(ghost.rest.border, Color::WHITE);
    let text = ButtonStyleOptions {
        variant: ButtonVariant::Text,
        ..Default::default()
    };
    assert_eq!(
        resolve(text),
        resolve(ButtonStyleOptions {
            ghost: true,
            ..text
        })
    );
}

#[test]
fn button_style_component_overrides_inherit_and_cache_with_local_globals() {
    let mut parent = ThemeConfig::default();
    parent.components.set::<ButtonTheme>(ButtonTokenPatch {
        icon_gap: Some(12.),
        ..Default::default()
    });
    let mut local = ThemeConfig::default();
    local.components.set_theme(
        ComponentThemeConfig::<ButtonTheme>::new(ButtonTokenPatch {
            padding_inline: Some(21.),
            ..Default::default()
        })
        .with_tokens(TokenPatch {
            color_primary: Some(Color::rgb(0x123456)),
            control_height: Some(38.),
            ..Default::default()
        }),
    );
    let theme = ThemeConfig::merged(&parent, &local).resolve();
    let first = theme.component::<ButtonTheme>();
    assert!(Arc::ptr_eq(&first, &theme.component::<ButtonTheme>()));
    let style = ButtonStyle::resolve(
        &theme,
        ButtonStyleOptions {
            color: ButtonColor::Primary,
            variant: ButtonVariant::Solid,
            ..Default::default()
        },
    );
    assert_eq!(style.rest.background, Color::rgb(0x123456));
    assert_eq!(style.metrics.height, 38.);
    assert_eq!(style.metrics.padding_inline, 21.);
    assert_eq!(style.metrics.icon_gap, 12.);
    assert_eq!(theme.tokens.control_height, 32.);
    assert_eq!(theme.tokens.color_primary, Color::rgb(0x1677ff));
}

#[test]
fn button_style_component_algorithms_drive_geometry_colors_and_contrast() {
    let mut config = ThemeConfig::default();
    config.components.set_theme(
        ComponentThemeConfig::<ButtonTheme>::default()
            .with_algorithms(AlgorithmChain::dark_compact()),
    );
    let style = ButtonStyle::resolve(
        &config.resolve(),
        ButtonStyleOptions {
            variant: ButtonVariant::Solid,
            ..Default::default()
        },
    );
    assert_eq!(style.metrics.height, 28.);
    assert_eq!(style.metrics.font_size, 12.);
    // Component algorithms inherit the resolved parent's explicit text base.
    // Its black base remains authoritative even through the dark algorithm.
    assert_eq!(style.rest.text, Color::WHITE);
    assert_eq!(style.rest.background, Color::BLACK.with_alpha(0.95));
}

#[test]
fn button_style_legacy_link_uses_link_palette_and_explicit_default_stays_neutral() {
    let mut config = ThemeConfig::default();
    config.tokens.color_link = Some(Color::rgb(0x8250df));
    let theme = config.resolve();
    let (color, variant) = ButtonType::Link.appearance();
    let link = ButtonStyle::resolve(
        &theme,
        ButtonStyleOptions {
            color,
            variant,
            ..Default::default()
        },
    );
    assert_eq!(link.rest.text, Color::rgb(0x8250df));
    let neutral = ButtonStyle::resolve(
        &theme,
        ButtonStyleOptions {
            variant: ButtonVariant::Link,
            ..Default::default()
        },
    );
    assert_eq!(neutral.rest.text, theme.tokens.color_text);
}

#[test]
fn button_style_solid_contrast_rounds_translucent_composites_at_half_alpha() {
    // Independently executed upstream isBright/AggregationColor yields black
    // for rgba(0, 0, 0, 0.5): its white composite rounds 127.5 to byte 128.
    // Preserve that boundary while retaining the opaque luminance decision.
    for (background, expected) in [
        (Color::BLACK.with_alpha(0.), Color::BLACK),
        (Color::BLACK.with_alpha(0.499), Color::BLACK),
        (Color::BLACK.with_alpha(0.5), Color::BLACK),
        (Color::BLACK.with_alpha(0.501), Color::WHITE),
        (Color::rgb(0xc0c0c0), Color::WHITE),
        (Color::rgb(0xc1c1c1), Color::BLACK),
        (Color::WHITE, Color::BLACK),
    ] {
        let mut config = ThemeConfig::default();
        config.tokens.color_bg_solid = Some(background);
        let style = ButtonStyle::resolve(
            &config.resolve(),
            ButtonStyleOptions {
                variant: ButtonVariant::Solid,
                ..Default::default()
            },
        );
        for state in [style.rest, style.hover, style.active] {
            assert_eq!(state.text, expected, "solid background {background:?}");
        }
    }
}
