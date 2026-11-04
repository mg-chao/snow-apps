use super::*;

fn outlined_arrow() -> ArrowDisplayItem {
    ArrowDisplayItem {
        path_commands: vec![
            PathCommand::MoveTo {
                point: [10.0, 32.0],
            },
            PathCommand::LineTo {
                point: [90.0, 32.0],
            },
        ],
        stroke: white(),
        stroke_width: 8.0,
        arrow_outline_color: ColorRgba8 {
            r: 20,
            g: 180,
            b: 40,
            a: 128,
        },
        arrow_outline_width: 4.0,
        ..Default::default()
    }
}

fn mesh_covers(mesh: &Mesh, x: f32, y: f32) -> bool {
    let p = point(x, y);
    mesh.indices.as_chunks::<3>().0.iter().any(|indices| {
        let [a, b, c] = [indices[0], indices[1], indices[2]].map(|i| {
            let [x, y] = mesh.vertices[i as usize];
            point(x, y)
        });
        let signs = [
            (b - a).cross(p - a),
            (c - b).cross(p - b),
            (a - c).cross(p - c),
        ];
        signs.iter().all(|value| *value >= -0.0001) || signs.iter().all(|value| *value <= 0.0001)
    })
}

#[test]
fn arrow_outline_is_a_boundary_band_and_reuses_geometry_for_color_edits() {
    let mut cache = GpuGeometryCache::default();
    let mut arrow = outlined_arrow();
    arrow.stroke.a = 0;
    let first = cache
        .item(&SceneDisplayItem::Arrow(arrow.clone()), 1.0, white())
        .unwrap();
    assert_eq!(first.parts.len(), 1, "transparent ink retains its outline");
    let outline = &first.parts[0];
    assert_eq!(outline.color.a, 128);
    assert_eq!(
        outline.coverage_group, 2,
        "overlapping contour strokes receive alpha once"
    );
    assert!(mesh_covers(&outline.mesh, 40.0, 5.0));
    assert!(
        !mesh_covers(&outline.mesh, 40.0, 0.0),
        "outline must not fill the ink interior"
    );
    assert!(outline.mesh.bounds[0] <= -5.9 && outline.mesh.bounds[2] >= 85.9);
    let builds = cache.builds;
    arrow.arrow_outline_color.r = 210;
    let next = cache
        .item(&SceneDisplayItem::Arrow(arrow.clone()), 1.0, white())
        .unwrap();
    assert_eq!(builds, cache.builds);
    assert!(Arc::ptr_eq(&outline.mesh, &next.parts[0].mesh));
    arrow.arrow_outline_width = 8.0;
    let widened = cache
        .item(&SceneDisplayItem::Arrow(arrow), 1.0, white())
        .unwrap();
    assert!(widened.parts[0].mesh.bounds[3] > outline.mesh.bounds[3] + 1.9);
}

#[test]
fn arrow_outline_unions_heads_and_preserves_hollow_head_interiors() {
    let mut cache = GpuGeometryCache::default();
    let mut arrow = outlined_arrow();
    arrow.arrowhead_primitives.push(ArrowheadDisplayPrimitive {
        kind: ArrowheadDisplayPrimitiveKind::Polygon,
        points: vec![[70.0, 20.0], [90.0, 32.0], [70.0, 44.0]],
        fill_mode: ArrowheadDisplayFillMode::Stroke,
        dash_mode: ArrowheadDisplayDashMode::Solid,
        center: [0.0, 0.0],
        diameter: 0.0,
        roughness_cap: None,
    });
    let geometry = cache
        .item(&SceneDisplayItem::Arrow(arrow.clone()), 1.0, white())
        .unwrap();
    assert!(
        !mesh_covers(&geometry.parts[0].mesh, 60.0, 0.0),
        "the shaft/head overlap is not a boundary"
    );
    arrow.arrowhead_primitives[0] = ArrowheadDisplayPrimitive {
        kind: ArrowheadDisplayPrimitiveKind::Circle,
        center: [80.0, 32.0],
        diameter: 24.0,
        fill_mode: ArrowheadDisplayFillMode::Background,
        dash_mode: ArrowheadDisplayDashMode::Solid,
        points: Vec::new(),
        roughness_cap: None,
    };
    let geometry = cache
        .item(&SceneDisplayItem::Arrow(arrow), 2.0, white())
        .unwrap();
    assert!(!mesh_covers(&geometry.parts[0].mesh, 70.0, 0.0));
    assert!(
        mesh_covers(&geometry.parts[0].mesh, 70.0, 7.0),
        "the inner head boundary retains an outline"
    );
}

#[test]
fn arrow_outline_eligibility_and_rough_metadata_follow_the_arrow() {
    let mut cache = GpuGeometryCache::default();
    let mut arrow = outlined_arrow();
    arrow.stroke_rendering_style = StrokeRenderingStyle::Artist;
    arrow.stroke_rendering_seed = 172;
    let geometry = cache
        .item(&SceneDisplayItem::Arrow(arrow.clone()), 1.0, white())
        .unwrap();
    assert_eq!(geometry.parts[0].rough_style, StrokeRenderingStyle::Artist);
    assert_eq!(geometry.parts[0].rough_seed, 172);
    for suppressed in 0..4 {
        let mut value = arrow.clone();
        match suppressed {
            0 => value.is_free_draw = true,
            1 => value.arrow_outline_width = 0.0,
            2 => value.arrow_outline_color.a = 0,
            _ => value.stroke_width = 0.0,
        }
        assert!(!arrow_outline_visible(&value));
        let geometry = cache
            .item(&SceneDisplayItem::Arrow(value), 1.0, white())
            .unwrap();
        assert!(geometry.parts.iter().all(|part| part.coverage_group != 2));
    }
}

#[test]
fn arrow_outline_heads_share_local_coordinates_and_reuse_meshes_after_translation() {
    let mut cache = GpuGeometryCache::default();
    for fill_mode in [
        ArrowheadDisplayFillMode::Stroke,
        ArrowheadDisplayFillMode::Background,
    ] {
        let mut arrow = outlined_arrow();
        arrow.arrowhead_primitives.push(ArrowheadDisplayPrimitive {
            kind: ArrowheadDisplayPrimitiveKind::Polygon,
            points: vec![[70.0, 20.0], [90.0, 32.0], [70.0, 44.0]],
            fill_mode,
            dash_mode: ArrowheadDisplayDashMode::Solid,
            center: [0.0; 2],
            diameter: 0.0,
            roughness_cap: None,
        });
        let normal = cache
            .item(&SceneDisplayItem::Arrow(arrow.clone()), 2.0, white())
            .unwrap();
        let offset = [1.0e9 + 0.125, -1.0e9 + 0.25];
        for command in &mut arrow.path_commands {
            match command {
                PathCommand::MoveTo { point } | PathCommand::LineTo { point } => {
                    point[0] += offset[0];
                    point[1] += offset[1];
                }
                _ => unreachable!(),
            }
        }
        for p in &mut arrow.arrowhead_primitives[0].points {
            p[0] += offset[0];
            p[1] += offset[1];
        }
        let distant = cache
            .item(&SceneDisplayItem::Arrow(arrow), 2.0, white())
            .unwrap();
        assert_eq!(normal.parts.len(), distant.parts.len());
        for (a, b) in normal.parts.iter().zip(&distant.parts) {
            assert!(
                Arc::ptr_eq(&a.mesh, &b.mesh),
                "translation rebuilt local head/outline geometry"
            );
            assert_eq!(
                [
                    b.transform[4] - a.transform[4],
                    b.transform[5] - a.transform[5]
                ],
                offset
            );
        }
    }
}

#[test]
fn arrow_outline_handles_curves_elbows_tapered_ribbons_and_dash_gaps() {
    let mut cache = GpuGeometryCache::default();
    for stroke_style in [StrokeStyle::Solid, StrokeStyle::Dashed, StrokeStyle::Dotted] {
        for width in [2.0, 8.0, 72.0] {
            for shape in 0..4 {
                let mut arrow = outlined_arrow();
                arrow.stroke_style = stroke_style;
                arrow.arrow_outline_width = width;
                match shape {
                    0 => (),
                    1 => {
                        arrow.arrow_type = snow_draw_engine_core::arrow::ArrowType::Curve;
                        arrow.path_commands = vec![
                            PathCommand::MoveTo {
                                point: [10.0, 32.0],
                            },
                            PathCommand::CubicTo {
                                control_1: [30.0, -20.0],
                                control_2: [70.0, 84.0],
                                end: [90.0, 32.0],
                            },
                        ];
                    }
                    2 => {
                        arrow.arrow_type = snow_draw_engine_core::arrow::ArrowType::Elbow;
                        arrow.path_commands = vec![
                            PathCommand::MoveTo {
                                point: [10.0, 32.0],
                            },
                            PathCommand::LineTo {
                                point: [50.0, 32.0],
                            },
                            PathCommand::LineTo {
                                point: [50.0, 60.0],
                            },
                            PathCommand::LineTo {
                                point: [90.0, 60.0],
                            },
                        ];
                    }
                    _ => {
                        arrow.arrow_shaft_type =
                            snow_draw_engine_core::arrow::ArrowShaftType::Tapered;
                        arrow.geometry =
                            Arc::new(snow_draw_engine_core::PathGeometry::from_commands(
                                1,
                                vec![
                                    PathCommand::MoveTo {
                                        point: [10.0, 32.0],
                                    },
                                    PathCommand::LineTo {
                                        point: [80.0, 24.0],
                                    },
                                    PathCommand::LineTo {
                                        point: [90.0, 32.0],
                                    },
                                    PathCommand::LineTo {
                                        point: [80.0, 40.0],
                                    },
                                ],
                                true,
                            ));
                    }
                }
                let geometry = cache
                    .item(&SceneDisplayItem::Arrow(arrow), 1.5, white())
                    .unwrap();
                assert_eq!(geometry.parts[0].coverage_group, 2);
                assert!(!geometry.parts[0].mesh.indices.is_empty());
                assert!(
                    geometry.parts[0]
                        .mesh
                        .vertices
                        .iter()
                        .flatten()
                        .all(|v| v.is_finite())
                );
            }
        }
    }
    for (style, gap) in [(StrokeStyle::Dashed, 52.0), (StrokeStyle::Dotted, 10.0)] {
        let mut arrow = outlined_arrow();
        arrow.stroke_style = style;
        let geometry = cache
            .item(&SceneDisplayItem::Arrow(arrow), 1.0, white())
            .unwrap();
        assert!(
            !mesh_covers(&geometry.parts[0].mesh, gap, 0.0),
            "outline must preserve dash gaps"
        );
    }
}

fn rectangle() -> RectangleDisplayItem {
    RectangleDisplayItem {
        center_x: 20.0,
        center_y: 30.0,
        width: 40.0,
        height: 20.0,
        fill: ColorRgba8 {
            r: 30,
            g: 80,
            b: 140,
            a: 192,
        },
        stroke: ColorRgba8 {
            r: 8,
            g: 9,
            b: 10,
            a: 128,
        },
        stroke_width: 2.0,
        ..Default::default()
    }
}

#[test]
fn pose_color_opacity_and_patterns_reuse_uploaded_geometry() {
    let mut cache = GpuGeometryCache::default();
    let mut rect = rectangle();
    let first = cache
        .item(&SceneDisplayItem::Rectangle(rect), 1.25, white())
        .unwrap();
    let builds = cache.builds;
    rect.center_x += 100.0;
    rect.rotation = 0.75;
    rect.opacity = 0.3;
    rect.fill.r = 240;
    rect.fill_style = DisplayFillStyle::CrossLine;
    let next = cache
        .item(&SceneDisplayItem::Rectangle(rect), 1.5, white())
        .unwrap();
    assert_eq!(cache.builds, builds);
    assert_eq!(next.opacity, 0.3);
    assert_eq!(next.parts[0].color.a, 192);
    assert_eq!(next.parts[1].color.a, 128);
    assert_ne!(first.parts[0].transform, next.parts[0].transform);
    for (a, b) in first.parts.iter().zip(&next.parts) {
        assert!(Arc::ptr_eq(&a.mesh, &b.mesh));
    }
    rect.width += 4.0;
    cache
        .item(&SceneDisplayItem::Rectangle(rect), 1.5, white())
        .unwrap();
    assert!(cache.builds > builds);
}

#[test]
fn shapes_arrows_labels_and_rough_strokes_have_explicit_paint_metadata() {
    let mut cache = GpuGeometryCache::default();
    for shape in [
        DisplayRectangleShape::Rectangle,
        DisplayRectangleShape::Ellipse,
        DisplayRectangleShape::Diamond,
    ] {
        let mut rect = rectangle();
        rect.shape = shape;
        rect.corner_radii = CornerRadii {
            top_left: 2.0,
            top_right: 4.0,
            bottom_left: 6.0,
            bottom_right: 3.0,
        };
        let geometry = cache
            .item(&SceneDisplayItem::Rectangle(rect), 2.0, white())
            .unwrap();
        assert_eq!(geometry.parts.len(), 2);
        assert!(geometry.parts.iter().all(|p| !p.mesh.indices.is_empty()));
    }
    let mut arrow = ArrowDisplayItem {
        path_commands: vec![
            PathCommand::MoveTo {
                point: [100.0, 100.0],
            },
            PathCommand::CubicTo {
                control_1: [110.0, 110.0],
                control_2: [120.0, 90.0],
                end: [140.0, 100.0],
            },
        ],
        stroke: white(),
        stroke_width: 2.0,
        opacity: 0.4,
        bound_text_id: Some(DisplayItemId {
            index: 4,
            generation: 1,
        }),
        label_bounds: Some(DrawRect::new(115.0, 90.0, 125.0, 110.0)),
        ..Default::default()
    };
    arrow.stroke_rendering_style = StrokeRenderingStyle::Artist;
    arrow.stroke_rendering_seed = 72;
    let geometry = cache
        .item(&SceneDisplayItem::Arrow(arrow), 1.0, white())
        .unwrap();
    assert_eq!(geometry.opacity, 0.4);
    assert_eq!(
        geometry.label_gap,
        Some(DrawRect::new(110.0, 85.0, 130.0, 115.0))
    );
    assert_eq!(geometry.parts[0].rough_seed, 72);
    assert_eq!(geometry.parts[0].rough_style, StrokeRenderingStyle::Artist);
    assert_eq!(geometry.parts[0].rough_scale, 20.0);
}

#[test]
fn masks_support_fractional_rotated_rectangles_single_points_and_round_brushes() {
    let mut cache = GpuGeometryCache::default();
    let mut filter = FilterDisplayItem {
        center_x: 10.5,
        center_y: 20.25,
        width: 25.0,
        height: 16.0,
        rotation: 0.3,
        opacity: 0.25,
        ..Default::default()
    };
    let rect = cache.filter_mask(&filter, 1.25).unwrap();
    assert_eq!(rect.color.a, 255);
    assert_eq!(rect.transform[4..], [10.5, 20.25]);
    filter.is_pen_filter = true;
    filter.stroke_width = 7.0;
    filter.points = vec![[25.0, 30.0]].into();
    let dot = cache.filter_mask(&filter, 1.25).unwrap();
    assert!(!dot.mesh.indices.is_empty());
    assert!(dot.mesh.bounds[0] < -3.0 && dot.mesh.bounds[2] > 3.0);
    filter.points = vec![[25.0, 30.0], [50.0, 40.0]].into();
    let line = cache.filter_mask(&filter, 1.25).unwrap();
    assert!(line.mesh.bounds[2] > 27.0);
}

#[test]
fn meshes_use_local_coordinates_and_quantized_device_error() {
    let mut cache = GpuGeometryCache::default();
    let mut rect = rectangle();
    rect.center_x = 999_999_000.0;
    rect.center_y = -999_999_000.0;
    rect.shape = DisplayRectangleShape::Ellipse;
    let coarse = cache
        .item(&SceneDisplayItem::Rectangle(rect), 1.0, white())
        .unwrap();
    let fine = cache
        .item(&SceneDisplayItem::Rectangle(rect), 32.0, white())
        .unwrap();
    assert!(
        coarse.parts[0]
            .mesh
            .vertices
            .iter()
            .all(|p| p[0].abs() <= 20.01 && p[1].abs() <= 10.01)
    );
    assert_eq!(coarse.parts[0].transform[4], rect.center_x);
    assert!(fine.parts[0].mesh.vertices.len() > coarse.parts[0].mesh.vertices.len());
    for scale in [0.01, 0.9, 1.0, 1.25, 32.0] {
        assert!(f64::from(curve_tolerance(scale).unwrap()) * scale <= 0.25 + 1.0e-7);
    }
}

#[test]
fn dashed_and_dotted_strokes_keep_actual_geometry_gaps() {
    let mut builder = Path::builder();
    builder.begin(point(0.0, 0.0));
    builder.line_to(point(100.0, 0.0));
    builder.end(false);
    let path = builder.build();
    for style in [StrokeStyle::Solid, StrokeStyle::Dashed, StrokeStyle::Dotted] {
        let mesh = tessellate(&path, Some((2.0, style)), 0.1).unwrap();
        assert!(!mesh.indices.is_empty());
        if style == StrokeStyle::Dashed {
            assert!(!mesh.vertices.iter().any(|p| p[0] > 11.1 && p[0] < 14.9));
        }
    }
}

#[test]
fn text_and_serial_use_shared_shaped_outlines_and_one_group_opacity() {
    let mut cache = GpuGeometryCache::default();
    let mut text = TextDisplayItem {
        center_x: 100.0,
        center_y: 80.0,
        width: 240.0,
        height: 60.0,
        text: "A q\u{301} אב".into(),
        font_size: 24.0,
        color: white(),
        stroke: ColorRgba8 {
            r: 10,
            g: 20,
            b: 30,
            a: 255,
        },
        stroke_width: 1.0,
        opacity: 0.4,
        ..Default::default()
    };
    let geometry = cache
        .item(&SceneDisplayItem::Text(text.clone()), 1.25, white())
        .unwrap();
    assert_eq!(geometry.opacity, 0.4);
    assert!(geometry.parts.len() > 4);
    assert!(geometry.parts.iter().all(|p| p.is_text && p.color.a == 255));
    let before = cache.builds;
    text.center_x += 200.0;
    text.color.r = 80;
    text.rotation = 0.4;
    let moved = cache
        .item(&SceneDisplayItem::Text(text), 1.25, white())
        .unwrap();
    assert_eq!(cache.builds, before);
    assert_eq!(moved.parts.len(), geometry.parts.len());
    let serial = cache
        .item(
            &SceneDisplayItem::SerialNumber(SerialNumberDisplayItem {
                center_x: 40.0,
                center_y: 40.0,
                diameter: 36.0,
                label: "1".into(),
                font_size: 20.0,
                color: white(),
                serial_number_type: DisplaySerialNumberType::SolidCircle,
                fill_style: DisplayFillStyle::Border,
                opacity: 0.25,
                ..Default::default()
            }),
            1.0,
            white(),
        )
        .unwrap();
    assert_eq!(serial.opacity, 0.25);
    assert_eq!(serial.parts[0].fill_style, DisplayFillStyle::Solid);
    assert!(serial.parts.iter().any(|p| p.is_text));
}

#[test]
fn indexed_meshes_do_not_truncate_at_sixteen_bit_indices() {
    let mut path = Path::builder();
    for i in 0..22_000 {
        let x = (i % 200) as f32 * 2.0;
        let y = (i / 200) as f32 * 2.0;
        path.begin(point(x, y));
        path.line_to(point(x + 1.0, y));
        path.line_to(point(x, y + 1.0));
        path.end(true);
    }
    let mesh = tessellate(&path.build(), None, 0.25).unwrap();
    assert!(mesh.vertices.len() > u16::MAX as usize);
    assert!(mesh.indices.iter().any(|&index| index > u16::MAX as u32));
    assert!(
        mesh.indices
            .iter()
            .all(|&index| (index as usize) < mesh.vertices.len())
    );
}

#[test]
fn long_stroke_tail_reuses_unchanged_chunk_meshes_and_preserves_dash_phase() {
    let commands: Vec<_> = (0..150)
        .map(|index| {
            let point = [index as f64, 0.0];
            if index == 0 {
                PathCommand::MoveTo { point }
            } else {
                PathCommand::LineTo { point }
            }
        })
        .collect();
    let geometry = snow_draw_engine_core::PathGeometry::from_commands(1, commands.clone(), false);
    let mut arrow = ArrowDisplayItem {
        geometry: Arc::new(geometry),
        stroke: white(),
        stroke_width: 2.0,
        stroke_style: StrokeStyle::Dashed,
        ..Default::default()
    };
    let mut cache = GpuGeometryCache::default();
    let before = cache
        .item(&SceneDisplayItem::Arrow(arrow.clone()), 1.0, white())
        .unwrap();
    assert_eq!(before.parts.len(), 3);
    assert!(before.parts.iter().all(|part| part.coverage_group == 1));
    let reference = tessellate(
        &command_path(commands.iter().copied(), [0.0; 2], false).unwrap(),
        Some((2.0, StrokeStyle::Dashed)),
        0.25,
    )
    .unwrap();
    let contains = |mesh: &Mesh, p: [f64; 2]| {
        mesh.indices.as_chunks::<3>().0.iter().any(|indices| {
            let p0 = mesh.vertices[indices[0] as usize].map(f64::from);
            let p1 = mesh.vertices[indices[1] as usize].map(f64::from);
            let p2 = mesh.vertices[indices[2] as usize].map(f64::from);
            let cross = |a: [f64; 2], b: [f64; 2]| {
                (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])
            };
            let c = [cross(p0, p1), cross(p1, p2), cross(p2, p0)];
            c.iter().all(|&v| v >= 0.0) || c.iter().all(|&v| v <= 0.0)
        })
    };
    for sample in 0..596 {
        let x = sample as f64 * 0.25 + 0.125;
        let chunked = before.parts.iter().any(|part| {
            contains(
                &part.mesh,
                [x - part.transform[4], 0.25 - part.transform[5]],
            )
        });
        assert_eq!(
            chunked,
            contains(&reference, [x, 0.25]),
            "dash coverage at {x}"
        );
    }
    let builds = cache.builds;
    let mut tail = commands[128..].to_vec();
    tail[21] = PathCommand::LineTo {
        point: [149.0, 4.0],
    };
    arrow.geometry = Arc::new(
        arrow
            .geometry
            .replace_tail(2, 128, [127.0, 0.0], tail, false),
    );
    let after = cache
        .item(&SceneDisplayItem::Arrow(arrow), 1.0, white())
        .unwrap();
    assert_eq!(cache.builds, builds + 1);
    for index in 0..2 {
        assert!(Arc::ptr_eq(
            &before.parts[index].mesh,
            &after.parts[index].mesh
        ));
    }
    assert!(!Arc::ptr_eq(&before.parts[2].mesh, &after.parts[2].mesh));
}

#[test]
fn pressure_contours_retain_prefix_and_union_overlapping_coverage() {
    let contour = |x: f64| {
        vec![
            PathCommand::MoveTo { point: [x, 0.0] },
            PathCommand::LineTo {
                point: [x + 10.0, 0.0],
            },
            PathCommand::LineTo {
                point: [x + 10.0, 4.0],
            },
            PathCommand::LineTo { point: [x, 4.0] },
        ]
    };
    let original = snow_draw_engine_core::PathGeometry::replace_contour_tail(
        None,
        1,
        0,
        vec![(contour(0.0), 0.0, 10.0), (contour(8.0), 10.0, 20.0)],
    );
    let mut arrow = ArrowDisplayItem {
        pressure_geometry: Some(Arc::new(original)),
        stroke: ColorRgba8 { a: 100, ..white() },
        stroke_width: 4.0,
        opacity: 0.5,
        ..Default::default()
    };
    let mut cache = GpuGeometryCache::default();
    let before = cache
        .item(&SceneDisplayItem::Arrow(arrow.clone()), 1.0, white())
        .unwrap();
    let builds = cache.builds;
    let replacement = snow_draw_engine_core::PathGeometry::replace_contour_tail(
        arrow.pressure_geometry.as_deref(),
        2,
        1,
        vec![(contour(9.0), 10.0, 21.0)],
    );
    arrow.pressure_geometry = Some(Arc::new(replacement));
    let after = cache
        .item(&SceneDisplayItem::Arrow(arrow), 1.0, white())
        .unwrap();
    assert_eq!(cache.builds, builds + 1);
    assert_eq!(after.opacity, 0.5);
    assert_eq!(after.parts.len(), 2);
    assert!(
        after
            .parts
            .iter()
            .all(|part| part.coverage_group == 1 && part.color.a == 100)
    );
    assert!(Arc::ptr_eq(&before.parts[0].mesh, &after.parts[0].mesh));
}

#[test]
fn dotted_arrowhead_caps_keep_round_dot_gaps() {
    let mut cache = GpuGeometryCache::default();
    let item = ArrowDisplayItem {
        stroke: white(),
        stroke_width: 2.0,
        stroke_style: StrokeStyle::Solid,
        arrowhead_primitives: vec![ArrowheadDisplayPrimitive {
            kind: ArrowheadDisplayPrimitiveKind::Line,
            points: vec![[0.0, 0.0], [20.0, 0.0]],
            center: [0.0; 2],
            diameter: 0.0,
            roughness_cap: None,
            fill_mode: ArrowheadDisplayFillMode::Stroke,
            dash_mode: ArrowheadDisplayDashMode::DottedCap,
        }],
        ..Default::default()
    };
    let output = cache
        .item(&SceneDisplayItem::Arrow(item), 1.0, white())
        .unwrap();
    assert_eq!(output.parts.len(), 1);
    let vertices = &output.parts[0].mesh.vertices;
    assert!(vertices.iter().any(|p| p[0] <= 1.0));
    assert!(vertices.iter().any(|p| p[0] >= 14.0));
    assert!(vertices.iter().all(|p| p[0] < 1.1 || p[0] > 3.9));
}

#[test]
fn first_stroke_chunk_subtracts_its_move_origin_before_float_conversion() {
    let mut cache = GpuGeometryCache::default();
    let prepare = |origin: [f64; 2]| ArrowDisplayItem {
        geometry: Arc::new(snow_draw_engine_core::PathGeometry::from_commands(
            1,
            vec![
                PathCommand::MoveTo { point: origin },
                PathCommand::LineTo {
                    point: [origin[0] + 10.0, origin[1] + 4.0],
                },
            ],
            false,
        )),
        stroke: white(),
        stroke_width: 2.0,
        ..Default::default()
    };
    let normal = cache
        .item(&SceneDisplayItem::Arrow(prepare([0.0, 0.0])), 1.0, white())
        .unwrap();
    let origin = [1.0e9 + 0.125, -1.0e9 + 0.25];
    let distant = cache
        .item(&SceneDisplayItem::Arrow(prepare(origin)), 1.0, white())
        .unwrap();
    assert_eq!(distant.parts[0].transform[4..], origin);
    assert_eq!(
        normal.parts[0].mesh.vertices,
        distant.parts[0].mesh.vertices
    );
    assert_eq!(normal.parts[0].mesh.indices, distant.parts[0].mesh.indices);
}
