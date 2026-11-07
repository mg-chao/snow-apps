// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Artwork: Ant Design 6.6.7 components/empty/simple.tsx (MIT); see THIRD_PARTY_NOTICES.md.

use crate::{
    icons::{IconColorModel, IconDescriptor, IconPalette, IconRef, IconRenderOptions, icon},
    theme::{AliasTokens, Color},
};
use gpui::{IntoElement, Window, div, prelude::*, px, size};

static SHADOW: IconDescriptor = IconDescriptor {
    key: "select/empty-shadow",
    color_model: IconColorModel::Monochrome,
    svg: br#"<svg xmlns="http://www.w3.org/2000/svg" width="64" height="41" viewBox="0 0 64 41"><ellipse transform="translate(0 1)" fill="currentColor" cx="32" cy="33" rx="32" ry="7"/></svg>"#,
};
static BODY: IconDescriptor = IconDescriptor {
    key: "select/empty-body",
    color_model: IconColorModel::TwoTone,
    svg: br#"<svg xmlns="http://www.w3.org/2000/svg" width="64" height="41" viewBox="0 0 64 41"><g transform="translate(0 1)" fill="none" stroke="currentColor"><path d="M55 12.8 44.9 1.3Q44 0 42.9 0H21.1q-1.2 0-2 1.3L9 12.8V22h46z"/><path d="M41.6 16c0-1.7 1-3 2.2-3H55v18.1c0 2.2-1.3 3.9-3 3.9H12c-1.7 0-3-1.7-3-3.9V13h11.2c1.2 0 2.2 1.3 2.2 3s1 2.9 2.2 2.9h14.8c1.2 0 2.2-1.4 2.2-3" fill="__ADGPUI_SECONDARY__"/></g></svg>"#,
};
fn solid(foreground: Color, background: Color) -> Color {
    let channel = |f: u8, b: u8| {
        (f64::from(f) * foreground.a + f64::from(b) * (1. - foreground.a)).round() as u8
    };
    Color {
        r: channel(foreground.r, background.r),
        g: channel(foreground.g, background.g),
        b: channel(foreground.b, background.b),
        a: 1.,
    }
}
pub(super) fn illustration(g: &AliasTokens, window: &Window) -> impl IntoElement {
    let dimensions = size(px(64.), px((g.control_height_lg * 0.875) as f32));
    let palette = IconPalette {
        text: solid(g.color_fill_tertiary, g.color_bg_container).to_rgba(),
        primary: solid(g.color_fill, g.color_bg_container).to_rgba(),
        secondary: solid(g.color_fill_quaternary, g.color_bg_container).to_rgba(),
    };
    let options = IconRenderOptions {
        // GPUI's icon raster uses explicit dimensions. Preserve SVG's meet
        // aspect ratio inside the reference's fixed 64px-wide image box.
        size: size(dimensions.height * (64. / 41.), dimensions.height),
        scale_factor: window.scale_factor(),
        colors: palette,
        ..Default::default()
    };
    div()
        .relative()
        .w(dimensions.width)
        .h(dimensions.height)
        .flex_shrink_0()
        .opacity(g.opacity_image as f32)
        .child(
            icon(IconRef::custom(&SHADOW).expect("static illustration key"))
                .render_options(options)
                .absolute()
                .left((dimensions.width - options.size.width) / 2.),
        )
        .child(
            icon(IconRef::custom(&BODY).expect("static illustration key"))
                .render_options(options)
                .absolute()
                .left((dimensions.width - options.size.width) / 2.),
        )
}
