use snow_draw_engine_core::{ColorRgba8, CornerRadii, EngineConfig, Point, arrow::StrokeStyle};
use snow_draw_engine_document::{
    ElementId, ElementMeta, FillStyle, HighlightShape, RectangleData, RectangleElementKind,
    SerialNumberData, TextData, TextLayoutSize, Transaction,
};
use snow_draw_engine_editor::{EditorSession, EditorViewportState};
use snow_draw_engine_model::DocumentModel;
use snow_draw_engine_scene::{DocumentSceneCache, ViewportComposer};
use std::{hint::black_box, time::Instant};

fn main() {
    println!("kind,elements,median_us,p95_us,order_plan_builds,relationship_index_builds");
    for count in [100, 1_000, 10_000] {
        for labels in [false, true] {
            let mut model = DocumentModel::new();
            let mut tx = Transaction::new("benchmark fixture");
            let id = |index| ElementId {
                index,
                generation: 1,
            };
            for index in 0..count {
                let center = Point::new(
                    f64::from(index % 100) * 8.0 - 400.0,
                    f64::from(index / 100) * 8.0 - 400.0,
                );
                if labels && index % 2 == 0 {
                    tx.insert_serial_number(
                        id(index),
                        ElementMeta::default(),
                        SerialNumberData {
                            center,
                            diameter: 4.0,
                            text_element_id: Some(id(index + 1)),
                            ..SerialNumberData::default()
                        },
                    );
                } else if labels {
                    tx.insert_text(
                        id(index),
                        ElementMeta::default(),
                        TextData {
                            center,
                            text: "label".to_owned(),
                            layout: TextLayoutSize::new(6.0, 4.0),
                            ..TextData::default()
                        },
                    );
                } else {
                    tx.insert_rectangle(
                        id(index),
                        ElementMeta::default(),
                        RectangleData {
                            rectangle_kind: RectangleElementKind::Rectangle,
                            highlight_shape: HighlightShape::Rectangle,
                            center,
                            width: 4.0,
                            height: 4.0,
                            rotation: 0.0,
                            fill: ColorRgba8::default(),
                            fill_style: FillStyle::Solid,
                            stroke: ColorRgba8::default(),
                            stroke_width: 0.0,
                            stroke_style: StrokeStyle::Solid,
                            corner_radii: CornerRadii::default(),
                            opacity: 1.0,
                        },
                    );
                }
            }
            model.apply_transaction(tx).unwrap();
            let mut cache = DocumentSceneCache::new();
            cache.sync(&model, None);
            let mut session = EditorSession::new(EngineConfig::default()).unwrap();
            let mut composer = ViewportComposer::new();
            let mut view = EditorViewportState::default();
            view.set_surface_size(1024, 1024);
            let mut samples = Vec::new();
            for frame in 0..23 {
                // Move the camera without changing topology or binding relationships.
                view.camera.center.x = f64::from(frame % 2);
                let start = Instant::now();
                composer.refresh(&cache, &model, &mut session, &view);
                black_box(composer.current_cursor());
                if frame >= 3 {
                    samples.push(start.elapsed().as_secs_f64() * 1e6);
                }
            }
            samples.sort_by(f64::total_cmp);
            println!(
                "{},{count},{:.3},{:.3},{},{}",
                if labels { "labels" } else { "ordinary" },
                samples[10],
                samples[18],
                cache.order_plan_build_count(),
                model.relation_index_build_count()
            );
        }
    }
}
