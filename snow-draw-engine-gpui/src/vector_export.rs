//! Portable vector serialization. This module never rasterizes document content.
use std::io::Write;

use base64::Engine as _;

use crate::{
    CanvasCancellation, CanvasError, CanvasSnapshot,
    engine::{SceneDisplayItem, StrokeRenderingStyle},
    vector::{VectorContext, color},
};

pub(crate) fn supports(snapshot: &CanvasSnapshot, vectors: &mut VectorContext) -> bool {
    snapshot.presentation.is_default()
        && snapshot.background.as_ref().is_none_or(portable_source)
        && !snapshot.scene.decoration().spotlight.active
        && snapshot.scene.decoration().watermark.text_len == 0
        && snapshot.scene.nodes().all(|(_, node)| {
            let item = node.item.as_ref();
            let portable_geometry = match item {
                SceneDisplayItem::Filter(_) | SceneDisplayItem::Magnifier(_) => false,
                SceneDisplayItem::Rectangle(item) => {
                    item.stroke_rendering_style == StrokeRenderingStyle::Architect
                }
                SceneDisplayItem::Arrow(item) => {
                    item.stroke_rendering_style == StrokeRenderingStyle::Architect
                        && !crate::gpu_geometry::arrow_outline_visible(item)
                }
                SceneDisplayItem::SpeechBubble(item) => {
                    item.path.stroke_rendering_style == StrokeRenderingStyle::Architect
                        && !crate::gpu_geometry::arrow_outline_visible(&item.path)
                }
                SceneDisplayItem::SerialNumber(item) => {
                    item.stroke_rendering_style == StrokeRenderingStyle::Architect
                }
                _ => true,
            };
            portable_geometry && !vectors.item_uses_color_fonts(item)
        })
}

fn portable_source(source: &crate::SourceImage) -> bool {
    source.layers().map_or_else(
        || source.raster_pixels().is_some(),
        |layers| layers.iter().all(|layer| portable_source(&layer.image)),
    )
}

pub(crate) fn svg(
    snapshot: &CanvasSnapshot,
    scale: f32,
    cancel: &CanvasCancellation,
) -> Result<String, CanvasError> {
    cancel.check()?;
    let mut vectors = VectorContext::default();
    if !supports(snapshot, &mut vectors) {
        return Err(CanvasError::Unsupported(
            "vector serialization of GPU effects or color glyphs",
        ));
    }
    let [width, height] = crate::export::dimensions(snapshot, scale)?;
    let frame = snapshot.viewport;
    vectors.set_clear_color(frame.clear_color);
    let mut output = crate::export::BoundedOutput::default();
    write!(output, r#"<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}"><rect width="100%" height="100%" fill="{}" fill-opacity="{}"/>"#,
        color(frame.clear_color), f64::from(frame.clear_color.a) / 255.0).map_err(error)?;
    let zoom = frame.camera.zoom * f64::from(scale);
    let tx = (f64::from(frame.surface.width) / 2.0 - frame.camera.center.x * frame.camera.zoom)
        * f64::from(scale);
    let ty = (f64::from(frame.surface.height) / 2.0 - frame.camera.center.y * frame.camera.zoom)
        * f64::from(scale);
    write!(
        output,
        r#"<g transform="matrix({zoom} 0 0 {zoom} {tx} {ty})">"#
    )
    .map_err(error)?;
    if let Some(source) = &snapshot.background {
        if let Some(layers) = source.layers() {
            for (index, layer) in layers.iter().enumerate() {
                cancel.check()?;
                if let Some(clip) = layer.coverage {
                    write!(output, r#"<defs><clipPath id="source-{index}" clipPathUnits="userSpaceOnUse"><rect x="{}" y="{}" width="{}" height="{}"/></clipPath></defs><g clip-path="url(#source-{index})">"#,
                        clip.x, clip.y, clip.width, clip.height).map_err(error)?;
                }
                write_image(&mut output, &layer.image, layer.placement(), cancel)?;
                if layer.coverage.is_some() {
                    output.write_all(b"</g>").map_err(error)?;
                }
            }
        } else {
            write_image(
                &mut output,
                source,
                crate::CanvasRect {
                    x: source.origin().x,
                    y: source.origin().y,
                    width: f64::from(source.width()) * source.units_per_pixel(),
                    height: f64::from(source.height()) * source.units_per_pixel(),
                },
                cancel,
            )?;
        }
    }
    for (index, id) in snapshot.scene.order_ids().iter().enumerate() {
        cancel.check()?;
        let node = snapshot
            .scene
            .node(*id)
            .ok_or(crate::engine::ErrorCode::InvalidState)?;
        let portable = vectors.export_svg_content(&node.item, index)?;
        output.write_all(portable.as_bytes()).map_err(error)?;
    }
    output.write_all(b"</g></svg>").map_err(error)?;
    cancel.check()?;
    String::from_utf8(output.bytes).map_err(error)
}

fn error(error: impl std::fmt::Display) -> CanvasError {
    CanvasError::InvalidFile(error.to_string())
}

fn write_image(
    output: &mut crate::export::BoundedOutput,
    source: &crate::SourceImage,
    placement: crate::CanvasRect,
    cancel: &CanvasCancellation,
) -> Result<(), CanvasError> {
    let pixels = source.raster_pixels().ok_or(CanvasError::Unsupported(
        "noncanonical nested SVG image layers",
    ))?;
    let mut png = crate::export::BoundedOutput::default();
    let mut encoder = png::Encoder::new(&mut png, source.width(), source.height());
    encoder.set_color(png::ColorType::Rgba);
    encoder.set_depth(png::BitDepth::Eight);
    encoder
        .write_header()
        .map_err(error)?
        .write_image_data(pixels.as_raw())
        .map_err(error)?;
    cancel.check()?;
    let encoded = base64::engine::general_purpose::STANDARD.encode(png.bytes);
    write!(output, r#"<image x="{}" y="{}" width="{}" height="{}" preserveAspectRatio="none" href="data:image/png;base64,{encoded}"/>"#,
        placement.x, placement.y, placement.width, placement.height).map_err(error)
}
