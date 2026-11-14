// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use ant_design_gpui::{ConfigPatch, ConfigScope, Progress, progress::*, theme::Color};
use gpui::{App, Hsla, Styled, div, px, relative};
use serde_json::Value;

fn color(value: &Value) -> Hsla {
    let text = value.as_str().unwrap();
    if let Some(args) = text.strip_prefix("rgba(").and_then(|s| s.strip_suffix(')')) {
        let parts: Vec<f64> = args.split(',').map(|s| s.trim().parse().unwrap()).collect();
        return Color {
            r: parts[0] as u8,
            g: parts[1] as u8,
            b: parts[2] as u8,
            a: parts[3],
        }
        .into();
    }
    text.parse::<Color>().unwrap().into()
}

pub fn custom_scope(scope: &ConfigScope, cx: &mut App) -> ConfigScope {
    let mut patch = ConfigPatch::default();
    patch
        .theme
        .get_or_insert_default()
        .components
        .set::<ProgressTheme>(ProgressTokenPatch {
            default_color: Some(Color::rgb(0xbae0ff)),
            remaining_color: Some(Color::rgb(0xf5222d)),
            circle_text_color: Some(Color::rgb(0x52c41a)),
            line_border_radius: Some(3.),
            circle_text_font_size: Some(0.8),
            circle_icon_font_size: Some(1.25),
        });
    scope.child(patch, cx).unwrap()
}

pub fn component(id: usize, sample: &Value, scope: ConfigScope) -> Progress {
    let mut p = Progress::new(("sample", id))
        .scope(scope)
        .percent(sample["percent"].as_f64().unwrap_or(0.))
        .show_info(sample["showInfo"].as_bool().unwrap_or(true));
    p = p.kind(match sample["type"].as_str() {
        Some("circle") => ProgressKind::Circle,
        Some("dashboard") => ProgressKind::Dashboard,
        _ => ProgressKind::Line,
    });
    if let Some(status) = sample["status"].as_str() {
        p = p.status(match status {
            "success" => ProgressStatus::Success,
            "exception" => ProgressStatus::Exception,
            "active" => ProgressStatus::Active,
            _ => ProgressStatus::Normal,
        });
    }
    if sample["size"] == "small" {
        p = p.size(ProgressSize::Small);
    } else if let Some(size) = sample["size"].as_f64() {
        p = p.size(size as f32);
    } else if let Some(size) = sample["size"].as_array() {
        let width = if let Some(n) = size[0].as_f64() {
            Some(px(n as f32).into())
        } else {
            size[0]
                .as_str()
                .map(|s| relative(s.trim_end_matches('%').parse::<f32>().unwrap() / 100.).into())
        };
        p = p.size(ProgressSize::Dimensions {
            width,
            height: size[1].as_f64().unwrap_or(8.) as f32,
        });
    } else if sample["size"].is_object() {
        p = p.size(ProgressSize::Dimensions {
            width: sample["size"]["width"]
                .as_f64()
                .map(|v| px(v as f32).into())
                .or_else(|| {
                    sample["size"]["width"].as_str().map(|s| {
                        relative(s.trim_end_matches('%').parse::<f32>().unwrap() / 100.).into()
                    })
                }),
            height: sample["size"]["height"].as_f64().unwrap_or(8.) as f32,
        });
    }
    if !sample["strokeWidth"].is_null() {
        p = p.stroke_width(sample["strokeWidth"].as_f64().unwrap() as f32);
    }
    if let Some(cap) = sample["strokeLinecap"].as_str() {
        p = p.stroke_linecap(match cap {
            "square" => ProgressLinecap::Square,
            "butt" => ProgressLinecap::Butt,
            _ => ProgressLinecap::Round,
        });
    }
    if !sample["railColor"].is_null() {
        p = p.rail_color(color(&sample["railColor"]));
    }
    if let Some(steps) = sample["steps"].as_u64() {
        p = p.steps(steps as usize);
    } else if sample["steps"].is_object() {
        p = p.steps(ProgressSteps {
            count: sample["steps"]["count"].as_u64().unwrap() as usize,
            gap: sample["steps"]["gap"].as_f64().unwrap_or(2.) as f32,
        });
    }
    if !sample["success"].is_null() {
        let mut s = ProgressSuccess::new(sample["success"]["percent"].as_f64().unwrap_or(0.));
        if !sample["success"]["strokeColor"].is_null() {
            s = s.color(color(&sample["success"]["strokeColor"]));
        }
        p = p.success(s);
    }
    match sample["rounding"].as_str() {
        Some("floor") => p = p.rounding(f64::floor),
        Some("ceil") => p = p.rounding(f64::ceil),
        _ => (),
    }
    if let Some(degrees) = sample["gapDegree"].as_f64() {
        p = p.gap_degree(degrees as f32);
    }
    if let Some(placement) = sample["gapPlacement"].as_str() {
        p = p.gap_placement(match placement {
            "top" => ProgressGapPlacement::Top,
            "start" => ProgressGapPlacement::Start,
            "end" => ProgressGapPlacement::End,
            _ => ProgressGapPlacement::Bottom,
        });
    }
    if sample["percentPosition"].is_object() {
        p = p.percent_position(ProgressPercentPosition {
            align: match sample["percentPosition"]["align"].as_str() {
                Some("start") => ProgressAlign::Start,
                Some("center") => ProgressAlign::Center,
                _ => ProgressAlign::End,
            },
            position: if sample["percentPosition"]["type"] == "inner" {
                ProgressInfoPosition::Inner
            } else {
                ProgressInfoPosition::Outer
            },
        });
    }
    match sample["format"].as_str() {
        Some("release") => p = p.format(|n, _| format!("In progress, {n}% complete").into()),
        Some("done") => p = p.format(|n, _| format!("{n} done").into()),
        Some("badge") => {
            p = p.format_content(|_, _, _, _| div().w(px(80.)).h(px(10.)).bg(Color::WHITE))
        }
        Some("days") => p = p.format(|_, _| "3 Days".into()),
        _ => (),
    }
    let stroke = &sample["strokeColor"];
    if stroke.is_string() {
        p = p.stroke_color(color(stroke));
    } else if let Some(colors) = stroke.as_array() {
        p = p.stroke_color(ProgressStroke::steps(colors.iter().map(color)));
    } else if let Some(stops) = stroke.as_object() {
        let mut gradient = if !stroke["from"].is_null() {
            ProgressGradient::between(color(&stroke["from"]), color(&stroke["to"]))
        } else {
            ProgressGradient::new(stops.iter().filter_map(|(position, c)| {
                position
                    .trim_end_matches('%')
                    .parse::<f32>()
                    .ok()
                    .map(|n| (n / 100., color(c)))
            }))
        };
        if let Some(direction) = stroke["direction"].as_str() {
            gradient = gradient.angle(match direction {
                "to left" => 270.,
                "to top" => 0.,
                "to bottom" => 180.,
                _ => 90.,
            });
        }
        p = p.stroke_color(gradient);
    }
    if sample["semantic"] == true {
        let mut rail = div().bg(Color::rgb(0xfff1f0));
        if let Some(opacity) = sample["railOpacity"].as_f64() {
            rail = rail.opacity(opacity as f32);
        }
        p = p.styles(ProgressStyles {
            rail: rail.style().clone(),
            track: div()
                .bg(Color::rgb(0x9254de))
                .opacity(sample["trackOpacity"].as_f64().unwrap_or(1.) as f32)
                .style()
                .clone(),
            indicator: div().text_color(Color::rgb(0xd4380d)).style().clone(),
            ..Default::default()
        });
    }
    p
}
