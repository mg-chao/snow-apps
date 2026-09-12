//! Detection orchestration and final semantic classification.

use crate::color::{background, boundary_contrast, foreground, local_foreground, surface_fraction};
use crate::error::{Error, Result};
use crate::geometry::{Rect, containment, iou};
use crate::grid::{BgrImage, Image, Mask};
use crate::icons::{dot_icons, icons};
use crate::mask::{dilate, or, rasterize, suppress};
use crate::media::{
    detect_images, detect_logos, refine_avatars, reject_glyphs, square_badges, textures,
};
use crate::model::{Region, region};
use crate::selection::{merge, nms, sort_regions};
use crate::surfaces::{estimate_surfaces, outlined_controls};
use crate::text::{paragraphs, split_styled_lines, split_surface_text, text_lines};

fn finalize(
    texts: &[Rect],
    images: &[Rect],
    avatars: &[Rect],
    icons: &[Rect],
    boxes: &[Rect],
    w: i32,
    h: i32,
) -> Vec<Region> {
    let avatars: Vec<_> = avatars
        .iter()
        .copied()
        .filter(|r| !images.iter().any(|im| containment(*r, *im) > 0.6))
        .collect();
    let mut icons: Vec<_> = icons
        .iter()
        .copied()
        .filter(|r| {
            !images.iter().any(|im| containment(*r, *im) > 0.6)
                && !avatars.iter().any(|a| iou(*r, *a) > 0.5)
        })
        .collect();
    let mut regions = Vec::new();
    for &t in texts {
        if images.iter().any(|im| containment(t, *im) >= 0.72)
            || avatars.iter().any(|a| containment(t, *a) >= 0.72)
        {
            continue;
        }
        let aspect = f64::from(t.w) / f64::from(t.h.max(1));
        if t.y < (0.12 * f64::from(h)) as i32
            && t.x > (0.82 * f64::from(w)) as i32
            && (0.7..=1.45).contains(&aspect)
            && t.w.max(t.h) <= (0.04 * f64::from(h)) as i32
        {
            icons.push(t);
            continue;
        }
        regions.push(region(
            t,
            if boxes.iter().any(|b| containment(t, *b) >= 0.8) {
                "text_in_box"
            } else {
                "text"
            },
        ));
    }
    for (items, kind) in [
        (images, "image"),
        (avatars.as_slice(), "avatar"),
        (icons.as_slice(), "icon"),
        (boxes, "message_box"),
    ] {
        regions.extend(items.iter().map(|r| region(*r, kind)));
    }
    let mut regions = nms(regions, 0.8);
    sort_regions(&mut regions);
    regions
}

/// Pixel evidence shared by detection stages. Constructed once from a validated input.
struct PreparedImage {
    bgr: Image,
    bg: [f64; 3],
    local_bg: Image,
    surfaces: Vec<Rect>,
    controls: Vec<Rect>,
    border: Mask,
    fg: Mask,
    local_fg: Mask,
}

/// Media candidates and the masks that prevent them from being read as text.
struct MediaCandidates {
    images: Vec<Rect>,
    image_mask: Mask,
    avatar_cands: Vec<Rect>,
    badge_mask: Mask,
    dots: Vec<Rect>,
}

/// Detect regions in a nonempty owned BGR image, without modifying the input.
/// Coordinates are half-open source-pixel bounds. Nested text blocks are retained.
pub fn detect_regions(bgr: &BgrImage) -> Result<Vec<Region>> {
    let prepared = prepare_image(bgr)?;
    let media = detect_media(&prepared)?;
    detect_text_and_classify(prepared, media)
}

fn prepare_image(source: &BgrImage) -> Result<PreparedImage> {
    if source.width() == 0 || source.height() == 0 {
        return Err(Error::InvalidImage(
            "expected a non-empty H x W x 3 uint8 BGR image".into(),
        ));
    }
    let bgr: Image = source.image.clone();
    let bg = background(&bgr);
    let (local_bg, surfaces, border) = estimate_surfaces(&bgr)?;
    let (controls, outlines) = outlined_controls(&bgr)?;
    let border = or(&border, &outlines);
    let fg = foreground(&bgr, bg, 24.);
    let local_fg = suppress(&local_foreground(&bgr, &local_bg, 20), &border);
    Ok(PreparedImage {
        bgr,
        bg,
        local_bg,
        surfaces,
        controls,
        border,
        fg,
        local_fg,
    })
}

fn detect_media(prepared: &PreparedImage) -> Result<MediaCandidates> {
    let PreparedImage {
        bgr,
        bg,
        surfaces,
        fg,
        local_fg,
        ..
    } = prepared;
    let (w, h) = (bgr.w, bgr.h);
    let bg = *bg;
    let mut images = detect_images(bgr, fg)?;
    images.retain(|r| surface_fraction(bgr, *r) < 0.65);
    let raw_badges = square_badges(bgr, fg, bg, &rasterize(&images, w, h))?;
    images.extend(textures(bgr, local_fg)?.into_iter().filter(|r| {
        raw_badges
            .iter()
            .filter(|a| containment(**a, *r) > 0.8)
            .count()
            < 2
    }));
    images.extend(detect_logos(bgr, fg)?.into_iter().filter(|r| {
        f64::from(r.y) < 0.06 * f64::from(h) && !surfaces.iter().any(|s| containment(*r, *s) > 0.8)
    }));
    let images = merge(images, 0.2);
    let image_mask = dilate(&rasterize(&images, w, h), 3)?;
    let avatar_cands: Vec<_> = raw_badges
        .into_iter()
        .filter(|r| {
            !images.iter().any(|im| containment(*r, *im) > 0.6)
                && !surfaces.iter().any(|s| {
                    containment(*r, *s) > 0.9
                        && s.area() < 4 * r.area()
                        && f64::from(s.w) > 1.6 * f64::from(s.h)
                })
        })
        .collect();
    let mut accepted = reject_glyphs(&avatar_cands, local_fg, surfaces)?;
    let glyphs: Vec<_> = avatar_cands
        .iter()
        .copied()
        .filter(|r| !accepted.contains(r))
        .collect();
    accepted.extend(surfaces.iter().copied().filter(|s| {
        (18..=90).contains(&s.w.min(s.h))
            && f64::from(s.w.max(s.h)) <= 1.2 * f64::from(s.w.min(s.h))
            && !images.iter().any(|r| containment(*s, *r) > 0.7)
            && !glyphs.iter().any(|r| containment(*s, *r) > 0.7)
    }));
    let avatar_cands = merge(accepted, 0.3);
    let badge_mask = dilate(&rasterize(&avatar_cands, w, h), 3)?;
    let dots = dot_icons(local_fg)?;
    Ok(MediaCandidates {
        images,
        image_mask,
        avatar_cands,
        badge_mask,
        dots,
    })
}

fn detect_text_and_classify(
    prepared: PreparedImage,
    media: MediaCandidates,
) -> Result<Vec<Region>> {
    let PreparedImage {
        bgr,
        local_bg,
        surfaces,
        controls,
        border,
        local_fg,
        ..
    } = prepared;
    let MediaCandidates {
        images,
        image_mask,
        avatar_cands,
        badge_mask,
        dots,
    } = media;
    let (w, h) = (bgr.w, bgr.h);
    let text_suppress = or(
        &or(&or(&image_mask, &badge_mask), &border),
        &rasterize(&dots, w, h),
    );
    let (texts, ink, char_h) = text_lines(&bgr, &local_bg, &text_suppress)?;
    let mut boxes: Vec<_> = surfaces
        .iter()
        .copied()
        .filter(|s| {
            s.w > 2 * s.h
                && f64::from(s.h) >= 1.5 * f64::from(char_h)
                && f64::from(s.w) < 0.4 * f64::from(w)
                && f64::from(s.h) < 0.2 * f64::from(h)
                && s.area() > 2000
                && boundary_contrast(&bgr, *s) >= 20.
                && !images.iter().any(|r| containment(*s, *r) > 0.7)
                && surface_fraction(&bgr, *s) < 0.97
        })
        .collect();
    boxes.extend(controls);
    let boxes = merge(boxes, 0.8);
    let text_mask = dilate(&rasterize(&texts, w, h), 3)?;
    let sup = or(&or(&image_mask, &text_mask), &badge_mask);
    let (avatars, extra_icons) = refine_avatars(&avatar_cands, &texts, w, h);
    let mut icons = icons(&local_fg, &sup, &text_mask, &texts)?;
    icons.extend(extra_icons);
    icons.extend(dots);
    let blocks = paragraphs(&texts);
    let texts = split_styled_lines(&split_surface_text(&texts, &surfaces, &ink), &bgr, &ink)?;
    let mut regions = finalize(&texts, &images, &avatars, &icons, &boxes, w, h);
    regions.extend(blocks.into_iter().map(|r| region(r, "text_block")));
    let mut regions = nms(regions, 0.8);
    sort_regions(&mut regions);
    Ok(regions)
}
