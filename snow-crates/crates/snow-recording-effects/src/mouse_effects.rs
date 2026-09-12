use crate::mouse_hook::ObservedMouseButton;
use crate::surface::{RgbaSurface, Surface};
use std::collections::VecDeque;
pub const CLICK_ANIMATION_MS: u64 = 450;
pub const CLICK_QUEUE_DEPTH: usize = 128;
#[derive(Clone, Copy)]
pub struct RenderClick {
    pub timestamp_ms: u64,
    pub x: i32,
    pub y: i32,
    pub button: ObservedMouseButton,
}
pub fn scale_point(x: i32, y: i32, source_size: (u32, u32), output_size: (u32, u32)) -> (i32, i32) {
    (
        scale_coordinate(x, source_size.0, output_size.0),
        scale_coordinate(y, source_size.1, output_size.1),
    )
}

pub fn scale_coordinate(value: i32, source_extent: u32, output_extent: u32) -> i32 {
    if source_extent == 0 {
        return value;
    }
    ((i64::from(value) * i64::from(output_extent) + i64::from(source_extent) / 2)
        / i64::from(source_extent))
    .clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32
}

pub fn draw_clicks(
    rgba: &mut [u8],
    output_size: (u32, u32),
    clicks: &VecDeque<RenderClick>,
    timestamp_ms: u64,
    color: [u8; 4],
    source_size: (u32, u32),
) {
    draw_clicks_to(
        &mut RgbaSurface {
            pixels: rgba,
            dimensions: output_size,
        },
        clicks,
        timestamp_ms,
        color,
        source_size,
    );
}

pub fn draw_clicks_to(
    surface: &mut impl Surface,
    clicks: &VecDeque<RenderClick>,
    timestamp_ms: u64,
    color: [u8; 4],
    source_size: (u32, u32),
) {
    let output_size = surface.size();
    for click in clicks {
        let Some(age) = timestamp_ms.checked_sub(click.timestamp_ms) else {
            continue;
        };
        if age > CLICK_ANIMATION_MS {
            continue;
        }
        let progress = age as f32 / CLICK_ANIMATION_MS as f32;
        let radius = 7 + (progress * 25.0).round() as i32;
        let alpha = (f32::from(color[3]) * (1.0 - progress)).round() as u8;
        let (x, y) = scale_point(click.x, click.y, source_size, output_size);
        let thickness = match click.button {
            ObservedMouseButton::Left => 3,
            ObservedMouseButton::Right | ObservedMouseButton::Middle => 2,
        };
        draw_circle_outline_rgba(
            surface,
            x,
            y,
            radius,
            [color[0], color[1], color[2], alpha],
            thickness,
        );
    }
}

fn draw_circle_outline_rgba(
    surface: &mut impl Surface,
    center_x: i32,
    center_y: i32,
    radius: i32,
    color: [u8; 4],
    thickness: i32,
) {
    let inner = radius.saturating_sub(thickness).max(0);
    let outer_squared = radius * radius;
    let inner_squared = inner * inner;
    for y in -radius..=radius {
        for x in -radius..=radius {
            let distance = x * x + y * y;
            if distance <= outer_squared && distance >= inner_squared {
                surface.blend_pixel(center_x + x, center_y + y, color);
            }
        }
    }
}

#[cfg(test)]
mod timing_tests {
    use super::*;
    #[test]
    fn future_click_does_not_appear_in_an_earlier_output_slot() {
        let mut pixels = vec![0; 100 * 100 * 4];
        let clicks = VecDeque::from([RenderClick {
            timestamp_ms: 101,
            x: 50,
            y: 50,
            button: ObservedMouseButton::Left,
        }]);
        draw_clicks(&mut pixels, (100, 100), &clicks, 100, [255; 4], (100, 100));
        assert!(pixels.iter().all(|pixel| *pixel == 0));
        draw_clicks(&mut pixels, (100, 100), &clicks, 101, [255; 4], (100, 100));
        assert!(pixels.iter().any(|pixel| *pixel != 0));
    }
}
