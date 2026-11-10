#![cfg(target_os = "windows")]
use base64::Engine as _;
use gpui_windows::gpu_extensions::OffscreenGpuHost;
use snow_draw_engine_gpui::{
    CanvasDocument, CanvasPresentation, CanvasSession, CanvasSnapshot, GpuExportFormat,
    GpuExportJob, engine::*,
};
use std::{
    task::Poll,
    time::{Duration, Instant},
};

fn fixture(
    head: Arrowhead,
    pattern: &str,
    shape: &str,
    outline_width: f64,
    alpha: u8,
) -> (CanvasDocument, CanvasSession) {
    let mut document = CanvasDocument::default();
    let mut session = CanvasSession::new(&mut document).unwrap();
    session.set_surface(&mut document, 256, 256).unwrap();
    session
        .set_camera(
            &mut document,
            Camera {
                center: Point::new(128.0, 128.0),
                zoom: 1.0,
            },
        )
        .unwrap();
    let points = if shape == "elbow" {
        vec![[31.3, 65.7], [100.2, 65.7], [100.2, 165.8], [190.4, 165.8]]
    } else {
        vec![[31.3, 65.7], [100.2, 112.1], [190.4, 165.8]]
    };
    let batch = serde_json::to_vec(&serde_json::json!({"version":1,"operations":[
        {"type":"arrow","points":points,"style":{
            "stroke":[200,40,20,alpha], "stroke_width":5.5,
            "arrow_outline_width":outline_width, "arrow_outline_color":[20,180,40,128],
            "arrow_type":shape, "stroke_style":pattern
        }}
    ]}))
    .unwrap();
    document
        .command(|engine| engine.apply_annotation_json(&batch))
        .unwrap();
    session.flush(&document).unwrap();
    let id = session.scene().order_ids()[0].element;
    session
        .mutate(&mut document, |engine, viewport| {
            engine.select_element_with_viewport_changes(
                viewport,
                ElementId {
                    index: id.index,
                    generation: id.generation,
                },
            )
        })
        .unwrap();
    let mut style = document
        .engine()
        .viewport_style_toolbar_state(session.viewport())
        .unwrap()
        .shape_style;
    style.start_arrowhead = Some(head);
    style.end_arrowhead = Some(head);
    session
        .mutate(&mut document, |engine, viewport| {
            engine.set_viewport_shape_style_patch(
                viewport,
                ShapeStylePatch {
                    kind: ShapeKind::Arrow,
                    style,
                    properties: snow_draw_engine_editor::SHAPE_STYLE_PROPERTY_START_ARROWHEAD
                        | snow_draw_engine_editor::SHAPE_STYLE_PROPERTY_END_ARROWHEAD,
                },
            )
        })
        .unwrap();
    session.flush(&document).unwrap();
    (document, session)
}

fn export(
    host: &OffscreenGpuHost,
    snapshot: &CanvasSnapshot,
    scale: f32,
    format: GpuExportFormat,
) -> Vec<u8> {
    let mut job = GpuExportJob::bytes(
        host.service(),
        snapshot.clone(),
        scale,
        Default::default(),
        format,
        Default::default(),
    )
    .unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        if let Poll::Ready(result) = job.poll() {
            return result.unwrap();
        }
        assert!(Instant::now() < deadline, "arrow export timed out");
        std::thread::yield_now();
    }
}

#[test]
fn arrow_outline_renders_every_head_with_solid_dashed_and_dotted_shafts() {
    let host = OffscreenGpuHost::new().unwrap();
    let heads = [
        Arrowhead::Arrow,
        Arrowhead::Bar,
        Arrowhead::Dot,
        Arrowhead::Circle,
        Arrowhead::CircleOutline,
        Arrowhead::Triangle,
        Arrowhead::TriangleOutline,
        Arrowhead::Diamond,
        Arrowhead::DiamondOutline,
        Arrowhead::CrowfootOne,
        Arrowhead::CrowfootMany,
        Arrowhead::CrowfootOneOrMany,
        Arrowhead::Square,
        Arrowhead::InvertedTriangle,
        Arrowhead::IndentedTriangle,
    ];
    for head in heads {
        for pattern in ["solid", "dashed", "dotted"] {
            for shape in ["straight", "curve", "elbow"] {
                let (document, session) = fixture(head, pattern, shape, 4.0, 255);
                let snapshot = CanvasSnapshot::capture(&document, &session).unwrap();
                let png = export(&host, &snapshot, 1.0, GpuExportFormat::Png);
                let pixels = image::load_from_memory(&png).unwrap().to_rgba8();
                assert!(
                    pixels
                        .pixels()
                        .any(|p| p[1] > p[0].saturating_add(20) && p[1] > p[2].saturating_add(20)),
                    "missing outline: {head:?} {pattern} {shape}"
                );
                assert!(
                    pixels.pixels().any(|p| p[0] > p[1].saturating_add(40)),
                    "missing foreground: {head:?} {pattern} {shape}"
                );
            }
        }
    }
}

#[test]
fn arrow_outline_dotted_crowfeet_preserve_single_alpha_in_png_and_svg() {
    let host = OffscreenGpuHost::new().unwrap();
    for head in [Arrowhead::CrowfootMany, Arrowhead::CrowfootOneOrMany] {
        for width in [0.0, 4.0] {
            let (document, session) = fixture(head, "dotted", "elbow", width, 0);
            let snapshot = CanvasSnapshot::capture(&document, &session)
                .unwrap()
                .with_presentation(CanvasPresentation {
                    clear_background: false,
                    ..Default::default()
                })
                .unwrap();
            for scale in [0.5, 1.0, 1.5, 2.0] {
                let png = export(&host, &snapshot, scale, GpuExportFormat::Png);
                let pixels = image::load_from_memory(&png).unwrap().to_rgba8();
                let max_alpha = pixels.pixels().map(|p| p[3]).max().unwrap();
                if width == 0.0 {
                    assert_eq!(max_alpha, 0);
                    continue;
                }
                assert_eq!(
                    max_alpha, 128,
                    "overlapping outline contours must receive alpha once"
                );
                let svg = String::from_utf8(export(&host, &snapshot, scale, GpuExportFormat::Svg))
                    .unwrap();
                let payload = svg
                    .split_once("data:image/png;base64,")
                    .unwrap()
                    .1
                    .split('"')
                    .next()
                    .unwrap();
                let embedded = base64::engine::general_purpose::STANDARD
                    .decode(payload)
                    .unwrap();
                assert_eq!(
                    pixels,
                    image::load_from_memory(&embedded).unwrap().to_rgba8()
                );
            }
        }
    }
}
