use super::*;
use lyon::algorithms::hit_test::hit_test_path;

fn paint(vertices: Vec<[f32; 2]>, indices: Vec<u32>, origin: [f64; 2]) -> Paint {
    Paint {
        mesh: Arc::new(Mesh::from_vertices(vertices, indices).unwrap()),
        color: white(),
        fill_style: DisplayFillStyle::Solid,
        transform: translation(origin),
        rough_style: StrokeRenderingStyle::Architect,
        rough_seed: 0,
        rough_scale: 0.0,
        is_text: false,
        color_glyph: None,
        glyph_tint: false,
        coverage_group: 0,
    }
}

fn square(origin: [f64; 2], size: f32) -> Paint {
    paint(
        vec![[0.0, 0.0], [size, 0.0], [size, size], [0.0, size]],
        vec![0, 1, 2, 0, 2, 3],
        origin,
    )
}

fn contains(path: &Path, x: f32, y: f32) -> bool {
    hit_test_path(
        &point(x, y),
        path.iter(),
        lyon::path::FillRule::NonZero,
        0.01,
    )
}

#[test]
fn arrow_outline_unions_duplicate_reversed_and_overlapping_triangles() {
    let mesh = paint(
        vec![[0.0, 0.0], [6.0, 0.0], [3.0, 6.0], [3.0, 2.0]],
        // Shared directed edges, reversed duplicates, and a degenerate face are
        // valid coverage. Only the outer triangle belongs to the ink boundary.
        vec![0, 1, 2, 0, 1, 3, 2, 1, 0, 0, 0, 1],
        [0.0; 2],
    );
    let path = ink_contour(&[mesh], &[], [0.0; 2], 0.01).unwrap();
    assert!(contains(&path, 3.0, 3.0));
    assert!(!contains(&path, 0.0, 3.0));
    assert_eq!(
        path.iter()
            .filter(|e| matches!(e, Event::Begin { .. }))
            .count(),
        1
    );
    assert_eq!(
        path.iter()
            .filter(|e| matches!(e, Event::Line { .. }))
            .count(),
        2
    );
}

#[test]
fn arrow_outline_union_preserves_touching_components_without_internal_seams() {
    let path = ink_contour(
        &[
            square([0.0, 0.0], 4.0),
            square([4.0, 0.0], 4.0),
            square([8.0, 4.0], 2.0),
        ],
        &[],
        [0.0; 2],
        0.01,
    )
    .unwrap();
    for (x, y) in [(2.0, 2.0), (4.0, 2.0), (6.0, 2.0), (9.0, 5.0)] {
        assert!(contains(&path, x, y));
    }
    assert!(!contains(&path, 7.0, 5.0));
    assert!(
        !path
            .iter()
            .any(|event| matches!(event, Event::Line { from, to }
        if from.x == 4.0 && to.x == 4.0)),
        "shared square edge is not a contour"
    );
}

#[test]
fn arrow_outline_hollow_heads_subtract_only_prior_ink() {
    let hole = ArrowheadDisplayPrimitive {
        kind: ArrowheadDisplayPrimitiveKind::Polygon,
        points: vec![[5.0, -5.0], [15.0, -5.0], [15.0, 15.0], [5.0, 15.0]],
        fill_mode: ArrowheadDisplayFillMode::Background,
        dash_mode: ArrowheadDisplayDashMode::Solid,
        center: [0.0; 2],
        diameter: 0.0,
        roughness_cap: None,
    };
    let subsequent = ArrowheadDisplayPrimitive {
        fill_mode: ArrowheadDisplayFillMode::Stroke,
        ..hole.clone()
    };
    let path = ink_contour(
        &[square([0.0; 2], 10.0)],
        &[
            (&hole, vec![]),
            (&subsequent, vec![square([6.0, 2.0], 2.0)]),
        ],
        [0.0; 2],
        0.01,
    )
    .unwrap();
    assert!(contains(&path, 2.0, 5.0));
    assert!(contains(&path, 7.0, 3.0));
    assert!(!contains(&path, 7.0, 7.0));
    assert!(!contains(&path, 12.0, 5.0));
}

#[test]
fn arrow_outline_rejects_unrepresentable_coverage_before_polygon_overlay() {
    let huge = square([f64::MAX, 0.0], 1.0);
    assert!(matches!(
        ink_contour(&[huge], &[], [0.0; 2], 0.1),
        Err(CanvasError::Engine(ErrorCode::InvalidArgument))
    ));
}
