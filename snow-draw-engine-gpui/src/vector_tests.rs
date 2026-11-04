use super::*;

#[test]
#[ignore = "manual release-only CPU font resolution profile"]
fn profile_shared_text_resolution() {
    use std::time::Instant;
    let start = Instant::now();
    let mut shared = shared_text();
    let discovery_ms = start.elapsed().as_secs_f64() * 1000.0;
    let mut document = crate::CanvasDocument::default();
    let session = crate::CanvasSession::new(&mut document).unwrap();
    let mut style = document
        .engine()
        .viewport_style_toolbar_state(session.viewport())
        .unwrap()
        .text_style;
    style.font_size = 10.0;
    let start = Instant::now();
    let font = shared.resolve_font(&style).unwrap();
    let first_face_ms = start.elapsed().as_secs_f64() * 1000.0;
    let start = Instant::now();
    for _ in 0..1000 {
        std::hint::black_box(shared.resolve_font(&style));
    }
    let primary_ms = start.elapsed().as_secs_f64() * 1000.0;
    let mut database = shared.fontdb.clone();
    drop(shared);
    let start = Instant::now();
    for _ in 0..1000 {
        std::hint::black_box(resolve_fallback('漢', &[font], &mut database));
    }
    let fallback_ms = start.elapsed().as_secs_f64() * 1000.0;
    let mut vectors = VectorContext::default();
    let start = Instant::now();
    for index in 0..1000 {
        std::hint::black_box(vectors.shape_line(&format!("{index:04} 漢A"), &style));
    }
    let unique_mixed_ms = start.elapsed().as_secs_f64() * 1000.0;
    let start = Instant::now();
    for index in 0..1000 {
        std::hint::black_box(vectors.shape_line(&format!("{index:04} 漢A"), &style));
    }
    let cached_mixed_ms = start.elapsed().as_secs_f64() * 1000.0;
    eprintln!(
        "font_profile discovery_ms={discovery_ms:.3} first_face_ms={first_face_ms:.3} primary1000_ms={primary_ms:.3} fallback1000_ms={fallback_ms:.3} unique_mixed1000_ms={unique_mixed_ms:.3} cached_mixed1000_ms={cached_mixed_ms:.3}"
    );
}
#[test]
fn svg_fills_have_distinct_patterns_and_document_unique_definitions() {
    let mut cache = VectorContext::default();
    let mut contents = Vec::new();
    for (index, style) in [
        DisplayFillStyle::Border,
        DisplayFillStyle::Solid,
        DisplayFillStyle::Line,
        DisplayFillStyle::CrossLine,
    ]
    .into_iter()
    .enumerate()
    {
        let item = SceneDisplayItem::Rectangle(RectangleDisplayItem {
            width: 48.0,
            height: 48.0,
            opacity: 1.0,
            fill: ColorRgba8 {
                r: 40,
                g: 100,
                b: 190,
                a: 255,
            },
            fill_style: style,
            stroke_width: 0.0,
            ..Default::default()
        });
        contents.push(cache.export_svg_content(&item, index).unwrap());
    }
    assert!(contents[0].contains("fill=\"none\""));
    assert!(contents[1].contains("fill=\"#2864be\""));
    assert!(!contents[0].contains("<defs>"));
    assert!(!contents[1].contains("<defs>"));
    assert!(contents[2].contains("M-2 2 L2 -2 M0 8 L8 0 M6 10 L10 6"));
    assert!(contents[3].contains("M0 0 L8 8 M0 8 L8 0"));
    for (index, content) in contents.iter().enumerate().skip(2) {
        assert_eq!(content.matches("<pattern ").count(), 1);
        assert!(content.contains(&format!("id=\"item-{index}-0\"")));
        assert!(content.contains(&format!("url(#item-{index}-0)")));
    }
}

#[test]
fn each_text_line_fill_has_its_own_svg_pattern_definition() {
    let mut cache = VectorContext::default();
    let item = SceneDisplayItem::Text(TextDisplayItem {
        text: "first\nsecond".into(),
        width: 120.0,
        height: 80.0,
        font_size: 24.0,
        fill: ColorRgba8 {
            r: 40,
            g: 100,
            b: 190,
            a: 255,
        },
        fill_style: DisplayFillStyle::Line,
        ..Default::default()
    });
    let content = cache.export_svg_content(&item, 7).unwrap();
    assert_eq!(content.matches("<pattern ").count(), 2);
    for index in 0..2 {
        assert_eq!(
            content.matches(&format!("id=\"item-7-{index}\"")).count(),
            1
        );
        assert_eq!(content.matches(&format!("url(#item-7-{index})")).count(), 1);
    }
    assert!(!content.contains("<text"));
}

#[test]
fn hebrew_visual_carets_hit_tests_and_selection_follow_rtl_glyph_order() {
    let mut cache = VectorContext::default();
    let mut document = crate::CanvasDocument::default();
    let session = crate::CanvasSession::new(&mut document).unwrap();
    let style = document
        .engine()
        .viewport_style_toolbar_state(session.viewport())
        .unwrap()
        .text_style;
    for text in ["אבג".to_string(), "אבג".repeat(400)] {
        let line = cache.shape_line(&text, &style);
        assert!(line.caret_x(0) > line.caret_x(2));
        assert!(line.caret_x(2) > line.caret_x(4));
        assert!(line.caret_x(0) > line.width * 0.99);
        assert!(line.caret_x(text.len()).abs() < 0.1);
        for (range, left, right) in line.clusters.iter() {
            assert_eq!(line.byte_at_x(left * 0.8 + right * 0.2), range.end);
            assert_eq!(line.byte_at_x(left * 0.2 + right * 0.8), range.start);
            let selected = line.selection_segments(range.clone());
            assert_eq!(selected.len(), 1);
            assert!((selected[0].0 - left).abs() < 0.01 && (selected[0].1 - right).abs() < 0.01);
        }
    }
    let mixed = cache.shape_line("aאבb", &style);
    let aleph = mixed
        .clusters
        .iter()
        .find(|(r, _, _)| r.start == 1)
        .unwrap();
    let bet = mixed
        .clusters
        .iter()
        .find(|(r, _, _)| r.start == 3)
        .unwrap();
    assert!(
        bet.1 < aleph.1,
        "Hebrew glyph boxes must reverse logical order inside LTR text"
    );
    assert_eq!(mixed.selection_segments(1..5), vec![(bet.1, aleph.2)]);
}
#[test]
fn bidi_selection_uses_cluster_boxes_and_combining_marks_keep_origins() {
    let mut cache = VectorContext::default();
    let mut document = crate::CanvasDocument::default();
    let session = crate::CanvasSession::new(&mut document).unwrap();
    let mut style = document
        .engine()
        .viewport_style_toolbar_state(session.viewport())
        .unwrap()
        .text_style;
    style.font_size = 24.0;
    let line = cache.shape_line("aאבb", &style);
    let selected = line.selection_segments(1..5);
    let width = selected.iter().map(|(a, b)| b - a).sum::<f64>();
    let hebrew: Vec<_> = line
        .clusters
        .iter()
        .filter(|(r, _, _)| r.start >= 1 && r.end <= 5)
        .collect();
    assert_eq!(hebrew.len(), 2);
    let expected = hebrew.iter().map(|(_, a, b)| b - a).sum::<f64>();
    assert!((width - expected).abs() < 0.01);
    assert!(selected[0].0 >= line.clusters[0].2 - 0.01);
    for text in [
        "q\u{0301}z",
        "a\u{0323}\u{0301}z",
        "office",
        "مرحبا بالعالم",
        "aש\u{05b0}\u{05c1}בb",
        "بش\u{0651}\u{064e}ب",
        "हिन्दी",
    ] {
        let line = cache.shape_line(text, &style);
        for (range, a, b) in line.clusters.iter() {
            assert!(text.is_char_boundary(range.start) && text.is_char_boundary(range.end));
            assert!(a.is_finite() && b.is_finite() && b >= a);
        }
        assert!(line.width > 0.0);
        assert!(line.carets.iter().any(|(i, _)| *i == text.len()));
        let selected = line.selection_segments(0..text.len());
        assert!(
            (selected.iter().map(|(a, b)| b - a).sum::<f64>() - line.width).abs() < 0.1,
            "cluster coverage for {text:?}"
        );
    }
    let combining = cache.shape_line("q\u{0301}z", &style);
    assert!(combining.clusters[0].1.abs() < 0.1);
}
#[test]
fn arrowhead_and_text_outline_keep_color_alpha() {
    let mut cache = VectorContext::default();
    let arrow = ArrowDisplayItem {
        stroke: ColorRgba8 {
            r: 20,
            g: 40,
            b: 60,
            a: 128,
        },
        arrowhead_primitives: vec![ArrowheadDisplayPrimitive {
            kind: ArrowheadDisplayPrimitiveKind::Circle,
            center: [20.0, 20.0],
            diameter: 10.0,
            fill_mode: ArrowheadDisplayFillMode::Stroke,
            points: Vec::new(),
            roughness_cap: None,
            dash_mode: ArrowheadDisplayDashMode::Solid,
        }],
        ..Default::default()
    };
    let xml = cache
        .export_svg_content(&SceneDisplayItem::Arrow(arrow), 0)
        .unwrap();
    assert!(xml.contains("fill-opacity=\"0.5019607843137255\""));
    let xml = cache
        .export_svg_content(
            &SceneDisplayItem::Text(TextDisplayItem {
                text: "Outline".into(),
                width: 100.0,
                height: 40.0,
                font_size: 24.0,
                color: ColorRgba8 {
                    r: 255,
                    g: 255,
                    b: 255,
                    a: 255,
                },
                stroke: ColorRgba8 {
                    r: 0,
                    g: 0,
                    b: 0,
                    a: 255,
                },
                stroke_width: 3.0,
                ..Default::default()
            }),
            0,
        )
        .unwrap();
    assert!(xml.contains("stroke-width=\"3\""));
    assert!(xml.contains("paint-order=\"stroke fill\""));
}

#[test]
fn arrow_outline_serializer_rejects_lossy_vectors_and_preserves_plain_arrows() {
    let mut vectors = VectorContext::default();
    let mut arrow = ArrowDisplayItem {
        stroke_width: 2.0,
        arrow_outline_width: 8.0,
        arrow_outline_color: ColorRgba8 {
            r: 20,
            g: 40,
            b: 60,
            a: 128,
        },
        ..Default::default()
    };
    assert!(matches!(
        vectors.export_svg_content(&SceneDisplayItem::Arrow(arrow.clone()), 0),
        Err(CanvasError::Unsupported(_))
    ));
    arrow.arrow_outline_color.a = 0;
    assert!(
        vectors
            .export_svg_content(&SceneDisplayItem::Arrow(arrow.clone()), 0)
            .is_ok()
    );
    arrow.arrow_outline_color.a = 128;
    arrow.arrow_outline_width = 0.0;
    assert!(
        vectors
            .export_svg_content(&SceneDisplayItem::Arrow(arrow), 0)
            .is_ok()
    );
}

#[test]
fn wrapping_preserves_long_unicode_input_and_respects_grapheme_boundaries() {
    let mut cache = VectorContext::default();
    let mut document = crate::CanvasDocument::default();
    let session = crate::CanvasSession::new(&mut document).unwrap();
    let style = document
        .engine()
        .viewport_style_toolbar_state(session.viewport())
        .unwrap()
        .text_style;
    let text = "one שלום e\u{301}你好 👩‍👩‍👦 ".repeat(100);
    let layout = cache.layout(&text, &style, 160.0);
    assert_eq!(
        layout
            .lines
            .iter()
            .map(|line| line.text.as_ref())
            .collect::<String>(),
        text
    );
    assert!(layout.lines.len() > 100);
    for line in &layout.lines {
        assert!(line.width <= 160.1 || line.text.graphemes(true).count() == 1);
        assert!(text.is_char_boundary(line.start));
    }
    let natural = cache.layout(&text, &style, 1.0e8);
    assert_eq!(natural.lines.len(), 1);
    for text in ["你\u{0301}z", "aש\u{05b0}\u{05c1}בb", "بش\u{0651}\u{064e}ب"] {
        let line = cache.shape_line(text, &style);
        let first = line
            .clusters
            .iter()
            .map(|(_, left, _)| *left)
            .fold(f64::INFINITY, f64::min);
        assert!(
            first.abs() < 0.1,
            "fallback cluster origin {text:?}: {first}"
        );
    }
}

#[test]
fn solid_serial_badges_ignore_border_fill_and_choose_contrasting_text() {
    let mut cache = VectorContext::default();
    let item = SerialNumberDisplayItem {
        center_x: 30.0,
        center_y: 30.0,
        diameter: 40.0,
        font_size: 18.0,
        label: "1".into(),
        serial_number_type: DisplaySerialNumberType::SolidCircle,
        color: ColorRgba8 {
            r: 255,
            g: 245,
            b: 100,
            a: 255,
        },
        fill_style: DisplayFillStyle::Border,
        ..Default::default()
    };
    let content = cache
        .export_svg_content(&SceneDisplayItem::SerialNumber(item), 0)
        .unwrap();
    assert!(content.contains("fill=\"#fff564\""));
    assert!(content.contains("fill-opacity=\"0.8784313725490196\""));
}

#[test]
fn serial_svg_applies_opacity_to_the_complete_badge() {
    let mut cache = VectorContext::default();
    let content = cache
        .export_svg_content(
            &SceneDisplayItem::SerialNumber(SerialNumberDisplayItem {
                diameter: 40.0,
                font_size: 18.0,
                label: "1".into(),
                serial_number_type: DisplaySerialNumberType::SolidCircle,
                opacity: 0.5,
                ..Default::default()
            }),
            0,
        )
        .unwrap();
    assert!(content.starts_with("<g opacity=\"0.5\">"));
    assert_eq!(content.matches("opacity=\"0.5\"").count(), 1);
    assert!(content.ends_with("</g></g>"));
}

#[test]
fn long_text_exports_the_shared_positioned_outlines() {
    let mut cache = VectorContext::default();
    let mut document = crate::CanvasDocument::default();
    let session = crate::CanvasSession::new(&mut document).unwrap();
    let style = document
        .engine()
        .viewport_style_toolbar_state(session.viewport())
        .unwrap()
        .text_style;
    for text in [
        "Canvas e\u{301} שלום 你好 😀 ".repeat(100),
        "office a\u{0323}\u{0301} ".repeat(180),
        "مرحبا بالعالم ".repeat(100),
        "a\tb\r".repeat(600),
    ] {
        let metric = cache.shape_line(&text, &style);
        assert_outlined_positions(&cache, &metric, &style);
        assert_eq!(
            metric
                .clusters
                .iter()
                .map(|(range, _, _)| range.len())
                .sum::<usize>(),
            text.len()
        );
    }
    for family in [
        "monospace",
        "sans-serif",
        "Consolas, monospace",
        "'Times New Roman', serif",
    ] {
        let style = TextStyle {
            font_family: Some(family.into()),
            ..style.clone()
        };
        let text = "family list wrapping ".repeat(120);
        let metric = cache.shape_line(&text, &style);
        assert_outlined_positions(&cache, &metric, &style);
        assert!((metric.caret_x(text.len()) - metric.width).abs() < 0.01);
    }
}
fn assert_outlined_positions(cache: &VectorContext, line: &TextLine, style: &TextStyle) {
    let content = cache.outlined_text_span(line, style, 0.0, style.font_size);
    let starts: std::collections::HashSet<_> = content
        .split('M')
        .skip(1)
        .filter_map(|contour| contour.split(['L', 'Q', 'C', 'Z', '"']).next())
        .collect();
    let mut contours = 0;
    for glyph in line.glyphs.iter() {
        if let Some(path) = cache.glyph_path(glyph.font, glyph.id) {
            for event in path.iter() {
                if let lyon::path::Event::Begin { at } = event {
                    contours += 1;
                    let x = glyph.x + f64::from(at.x) * glyph.scale;
                    let y = style.font_size + glyph.y - f64::from(at.y) * glyph.scale;
                    assert!(starts.contains(format!("{x} {y}").as_str()));
                }
            }
        }
    }
    assert!(contours > 0);
    assert_eq!(content.matches('M').count(), contours);
    assert!(!content.contains("<text"));
}

#[test]
fn independent_consumers_share_font_ids_glyph_placement_and_carets() {
    let mut interaction = VectorContext::default();
    let mut renderer = VectorContext::default();
    let mut document = crate::CanvasDocument::default();
    let session = crate::CanvasSession::new(&mut document).unwrap();
    let mut style = document
        .engine()
        .viewport_style_toolbar_state(session.viewport())
        .unwrap()
        .text_style;
    style.font_size = 23.25;
    let first = interaction.shape_line("shared layout q\u{301} office אב 文", &style);
    let mut recolored = style.clone();
    recolored.color = ColorRgba8 {
        r: 200,
        g: 30,
        b: 80,
        a: 120,
    };
    let second = renderer.shape_line(&first.text, &recolored);
    assert!(Arc::ptr_eq(&first.glyphs, &second.glyphs));
    assert!(Arc::ptr_eq(&first.carets, &second.carets));
    assert!(Arc::ptr_eq(&first.clusters, &second.clusters));
    assert!(Arc::ptr_eq(&first.text, &second.text));
    assert!(
        first
            .glyphs
            .iter()
            .any(|glyph| renderer.glyph_path(glyph.font, glyph.id).is_some())
    );
    let third = VectorContext::default().shape_line(&first.text, &style);
    assert!(Arc::ptr_eq(&first.glyphs, &third.glyphs));
}

#[test]
fn shared_line_cache_evicts_bounded_entries_without_invalidating_live_layout() {
    let (select_fallback, font_bytes) = crate::font_cache::fallback_resolver();
    let mut cache = SharedTextCache {
        fontdb: Default::default(),
        select_fallback,
        font_bytes,
        fallbacks: Default::default(),
        lines: HashMap::new(),
        order: VecDeque::new(),
        bytes: 0,
        color_glyphs: HashMap::new(),
    };
    let line = TextLine {
        text: "retained".into(),
        width: 10.0,
        start: 0,
        carets: vec![(0, 0.0), (8, 10.0)].into(),
        clusters: Default::default(),
        ink_reserve: 0.0,
        glyphs: Default::default(),
    };
    for index in 0..5000 {
        cache.store_line(
            LineKey {
                family: "Segoe UI".into(),
                text: index.to_string().into(),
                size: 16.0_f64.to_bits(),
                bold: false,
                italic: false,
            },
            line.clone(),
        );
    }
    assert_eq!(cache.lines.len(), 4096);
    assert_eq!(cache.order.len(), 4096);
    assert!(cache.bytes <= 16 * 1024 * 1024);
    assert_eq!(line.caret_x(8), 10.0);
}

#[cfg(target_os = "windows")]
#[test]
fn color_fonts_are_detected_before_vector_export_and_share_native_metadata() {
    let item = SceneDisplayItem::Text(TextDisplayItem {
        text: "😀".into(),
        font_family: Some("Segoe UI Emoji".into()),
        font_size: 48.0,
        ..Default::default()
    });
    let mut cache = VectorContext::default();
    assert!(cache.item_uses_color_fonts(&item));
    assert!(
        !cache.item_uses_color_fonts(&SceneDisplayItem::Text(TextDisplayItem {
            text: "ordinary text".into(),
            ..Default::default()
        }))
    );
}
