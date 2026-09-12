//! JSON serialization, preview rendering, and crop export.

use crate::error::{Error, Result};
use crate::grid::{BgrImage, Image};
use crate::model::Region;
use std::collections::BTreeMap;

pub fn regions_to_jsonable(source: &str, bgr: &BgrImage, regions: &[Region]) -> serde_json::Value {
    let mut counts = BTreeMap::new();
    for region in regions {
        *counts.entry(&region.kind).or_insert(0usize) += 1;
    }
    serde_json::json!({
        "source_image": source,
        "width": bgr.width(),
        "height": bgr.height(),
        "coordinate_system": "origin at top-left, x grows right, y grows down, units are pixels",
        "regions": regions,
        "region_count": regions.len(),
        "type_counts": counts,
    })
}

pub fn draw_regions(bgr: &BgrImage, regions: &[Region], thickness: i32) -> Result<BgrImage> {
    let mut output = bgr.image.clone();
    for region in regions {
        let color = match region.kind.as_str() {
            "text" => [220, 120, 30],
            "text_block" => [150, 160, 0],
            "text_in_box" => [255, 180, 80],
            "image" => [0, 140, 255],
            "avatar" => [40, 200, 70],
            "icon" => [200, 0, 220],
            "message_box" => [40, 80, 255],
            _ => [0, 255, 255],
        };
        draw_rectangle(&mut output, region.rect, thickness, color);
    }
    Ok(BgrImage::from_grid(output))
}

fn draw_rectangle(image: &mut Image, rect: crate::geometry::Rect, thickness: i32, color: [u8; 3]) {
    let x0 = rect.x;
    let y0 = rect.y;
    let x1 = rect.x2() - 1;
    let y1 = rect.y2() - 1;
    if x1 < 0 || y1 < 0 || x0 >= image.w || y0 >= image.h {
        return;
    }
    let thickness = thickness.max(1);
    // Positive OpenCV rectangle thickness is centered on the requested
    // boundary. For the normal thickness=2 path this paints one pixel on the
    // outside and one on the inside, including the outward extension for
    // one-pixel-wide rectangles.
    let outer = thickness / 2;
    let inner = thickness / 2;
    let paint_horizontal = |image: &mut Image, y_start: i32, y_end: i32| {
        for y in y_start.max(0)..=y_end.min(image.h - 1) {
            for x in x0.max(0)..=x1.min(image.w - 1) {
                image.set(x, y, color);
            }
        }
    };
    let paint_vertical = |image: &mut Image, x_start: i32, x_end: i32| {
        for x in x_start.max(0)..=x_end.min(image.w - 1) {
            for y in y0.max(0)..=y1.min(image.h - 1) {
                image.set(x, y, color);
            }
        }
    };
    paint_horizontal(image, y0 - outer, y0 + inner);
    paint_horizontal(image, y1 - inner, y1 + outer);
    paint_vertical(image, x0 - outer, x0 + inner);
    paint_vertical(image, x1 - inner, x1 + outer);
}

/// Export exact, unresized PNG slices and an index in detection order.
pub fn write_crops(
    bgr: &BgrImage,
    regions: &[Region],
    directory: &std::path::Path,
) -> Result<Vec<serde_json::Value>> {
    std::fs::create_dir_all(directory)?;
    let mut entries = Vec::new();
    for (index, region) in regions.iter().enumerate() {
        if region.rect.x < 0
            || region.rect.y < 0
            || region.rect.x2() > bgr.image.w
            || region.rect.y2() > bgr.image.h
        {
            return Err(Error::InvalidImage(format!(
                "region {index} is outside the source image"
            )));
        }
        let file = format!("{index:04}_{}.png", region.kind);
        let path = directory.join(&file);
        let crop = BgrImage::from_grid(bgr.image.crop(region.rect));
        crop.to_rgb8().save(&path)?;
        let mut entry = serde_json::to_value(region)?;
        entry["region_index"] = index.into();
        entry["file"] = file.into();
        entries.push(entry);
    }
    std::fs::write(
        directory.join("index.json"),
        serde_json::to_string_pretty(&serde_json::json!({"crops": entries}))?,
    )?;
    Ok(entries)
}
