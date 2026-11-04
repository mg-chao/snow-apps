use super::*;
use crate::{
    CanvasDocument, CanvasRect, CanvasSession, DrawingSnapshot, ImageSourceLayer, SourceImage,
};
use image::Rgba;

#[test]
fn packed_band_schedule_preserves_order_and_bounds_working_segments() {
    let tiles = [
        ExportTile {
            source: [10, 20, 2500, 400],
            destination: [0, 50],
        },
        ExportTile {
            source: [1, 2, 25, 30],
            destination: [40, 120],
        },
    ];
    validate_tiles([3000, 1000], [2500, 500], &tiles).unwrap();
    let segments = band_segments([2500, 500], 128, 128, Some(&tiles));
    assert_eq!(segments.len(), 4);
    assert_eq!(segments[0].source, [10, 98, 1024, 128]);
    assert_eq!(segments[2].source, [2058, 98, 452, 128]);
    assert_eq!(segments[3].source, [1, 10, 25, 22]);
    assert_eq!(segments[3].destination, [40, 0]);
    assert!(
        segments
            .iter()
            .all(|segment| segment.source[2] <= SEGMENT_WIDTH && segment.source[3] <= 128)
    );
    assert!(
        validate_tiles(
            [30, 20],
            [30, 20],
            &[ExportTile {
                source: [u32::MAX, 0, 2, 2],
                destination: [0, 0]
            }]
        )
        .is_err()
    );
}

#[test]
fn region_frame_keeps_fractional_output_grid_and_premultiplied_readback_contract() {
    let frame = FrameView {
        surface: SurfaceSize {
            width: 101,
            height: 67,
        },
        camera: Camera {
            center: Point::new(27.5, -12.5),
            zoom: 0.75,
        },
        ..Default::default()
    };
    let region = region_frame(frame, 1.25, [40, 20, 51, 31]);
    let world = view_to_canvas(Point::new(0.0, 0.0), &region.camera, region.surface);
    let expected = view_to_canvas(Point::new(32.0, 16.0), &frame.camera, frame.surface);
    assert!((world.x - expected.x).abs() < 1.0e-10 && (world.y - expected.y).abs() < 1.0e-10);
    let mut pixels = [0, 0, 0, 0, 50, 25, 100, 128, 5, 15, 25, 255];
    unpremultiply(&mut pixels);
    assert_eq!(pixels, [0, 0, 0, 0, 100, 50, 199, 128, 5, 15, 25, 255]);
}

#[test]
fn encoder_rejects_incomplete_or_cancelled_bands_and_streams_lossless_svg() {
    let (send, receive) = mpsc::sync_channel(2);
    send.send(vec![40, 50, 60, 128, 70, 80, 90, 255]).unwrap();
    drop(send);
    let mut output = Vec::new();
    encode(
        receive,
        [2, 1],
        GpuExportFormat::Svg,
        &mut output,
        &Default::default(),
    )
    .unwrap();
    let svg = String::from_utf8(output).unwrap();
    let data = svg
        .split("base64,")
        .nth(1)
        .unwrap()
        .split('"')
        .next()
        .unwrap();
    use base64::Engine as _;
    let png = base64::engine::general_purpose::STANDARD
        .decode(data)
        .unwrap();
    assert_eq!(
        image::load_from_memory(&png)
            .unwrap()
            .into_rgba8()
            .into_raw(),
        [40, 50, 60, 128, 70, 80, 90, 255]
    );
    let (send, receive) = mpsc::sync_channel(1);
    drop(send);
    assert!(matches!(
        encode_png(receive, [2, 1], Vec::new(), &Default::default()),
        Err(CanvasError::Cancelled)
    ));
    let cancelled = CanvasCancellation::default();
    cancelled.cancel();
    let (_, receive) = mpsc::sync_channel(1);
    assert!(matches!(
        encode(
            receive,
            [2, 1],
            GpuExportFormat::Png,
            Vec::new(),
            &cancelled
        ),
        Err(CanvasError::Cancelled)
    ));
}

#[test]
fn deferred_session_updates_interaction_transform_without_publishing_geometry() {
    let mut document = CanvasDocument::default();
    let mut session = CanvasSession::new(&mut document).unwrap();
    session.set_deferred_composition(true);
    let before = document
        .engine()
        .retained_scene_diagnostics(session.viewport())
        .unwrap();
    for n in 0..20 {
        session.set_surface(&mut document, 100 + n, 80).unwrap();
        session
            .set_camera(
                &mut document,
                Camera {
                    center: Point::new(f64::from(n), 50.0),
                    zoom: 2.0,
                },
            )
            .unwrap();
    }
    assert_eq!(session.scene().frame().surface.width, 119);
    assert_eq!(session.scene().frame().camera.center.x, 19.0);
    let deferred = document
        .engine()
        .retained_scene_diagnostics(session.viewport())
        .unwrap();
    assert_eq!(before.publications, deferred.publications);
    let snapshot = CanvasSnapshot::capture(&document, &session).unwrap();
    assert_eq!(snapshot.viewport(), session.scene().frame());
    session.flush(&document).unwrap();
    let after = document
        .engine()
        .retained_scene_diagnostics(session.viewport())
        .unwrap();
    assert_eq!(after.publications - before.publications, 1);
    assert_eq!(after.owners_composed, before.owners_composed);
}

fn finish<T: Send + 'static>(job: &mut GpuExportJob<T>) -> Result<T, CanvasError> {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);
    loop {
        if let Poll::Ready(result) = job.poll() {
            return result;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "GPU export did not complete"
        );
        assert!(job.readbacks.len() + job.spare_readbacks.len() <= 2 && job.textures.len() <= 4);
        assert!(
            job.band
                .as_ref()
                .is_none_or(|band| band.as_raw().len() <= MAX_BAND_BYTES as usize)
        );
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
}

#[test]
fn gpu_export_streams_multiple_bands_with_bounded_staging_and_straight_alpha() {
    let host = gpui_windows::gpu_extensions::OffscreenGpuHost::new().unwrap();
    let source =
        SourceImage::new(RgbaImage::from_pixel(1025, 260, Rgba([100, 50, 200, 128]))).unwrap();
    let snapshot = CanvasSnapshot::source_only(source);
    let service = host.service();
    let mut job = GpuExportJob::bytes(
        service.clone(),
        snapshot.clone(),
        1.0,
        Default::default(),
        GpuExportFormat::Png,
        Default::default(),
    )
    .unwrap();
    let bytes = finish(&mut job).unwrap();
    let image = image::load_from_memory(&bytes).unwrap().into_rgba8();
    assert_eq!(image.dimensions(), (1025, 260));
    for point in [(0, 0), (255, 127), (256, 128), (1024, 259)] {
        let pixel = image.get_pixel(point.0, point.1).0;
        assert!(
            pixel
                .into_iter()
                .zip([100_u8, 50, 200, 128])
                .all(|(a, b)| a.abs_diff(b) <= 2),
            "{point:?}: {pixel:?}"
        );
    }
    assert_eq!(service.diagnostics().pending_readback_bytes, 0);
    let mut packed = GpuExportJob::packed_png(
        service,
        snapshot,
        1.0,
        [20, 10],
        vec![
            ExportTile {
                source: [1000, 250, 10, 10],
                destination: [0, 0],
            },
            ExportTile {
                source: [256, 128, 5, 5],
                destination: [15, 5],
            },
        ],
        Default::default(),
        Default::default(),
    )
    .unwrap();
    let image = image::load_from_memory(&finish(&mut packed).unwrap())
        .unwrap()
        .into_rgba8();
    assert_eq!(image.get_pixel(12, 4).0, [0, 0, 0, 0]);
    assert_eq!(image.get_pixel(15, 5)[3], 128);
}

#[test]
fn gpu_save_preserves_visual_and_donor_composites_without_cpu_projection() {
    let host = gpui_windows::gpu_extensions::OffscreenGpuHost::new().unwrap();
    let base = SourceImage::new(RgbaImage::from_pixel(32, 24, Rgba([10, 50, 100, 255]))).unwrap();
    let overlay = SourceImage::new(RgbaImage::from_pixel(8, 8, Rgba([200, 20, 30, 255]))).unwrap();
    let source = SourceImage::from_layers(&[
        ImageSourceLayer::new(base),
        ImageSourceLayer {
            image: overlay,
            canvas_rect: Some(CanvasRect::new(4.0, 6.0, 8.0, 8.0).unwrap()),
            coverage: Some(CanvasRect::new(5.0, 7.0, 6.0, 6.0).unwrap()),
            smart_erase_source: false,
        },
    ])
    .unwrap()
    .unwrap();
    let mut document = CanvasDocument::default();
    document.set_background(Some(source));
    let mut save = GpuSavePreparation::new(
        host.service(),
        document.persistence_snapshot().unwrap(),
        Default::default(),
    )
    .unwrap();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);
    let prepared = loop {
        if let Poll::Ready(result) = save.poll() {
            break result.unwrap();
        }
        assert!(std::time::Instant::now() < deadline);
        std::thread::sleep(std::time::Duration::from_millis(1));
    };
    let restored = DrawingSnapshot::decode(&prepared.encode().unwrap())
        .unwrap()
        .into_document()
        .unwrap();
    let background = restored.background().unwrap();
    assert_eq!(
        background.raster_pixels().unwrap().get_pixel(7, 9).0,
        [200, 20, 30, 255]
    );
    assert_eq!(
        background.smart_erase_pixels().get_pixel(7, 9).0,
        [10, 50, 100, 255]
    );
    assert_eq!(
        background.raster_pixels().unwrap().get_pixel(4, 6).0,
        [10, 50, 100, 255]
    );
}

#[test]
fn svg_retains_vectors_and_layer_coverage_and_strict_smart_erase_rejects_missing_results() {
    let mut document = CanvasDocument::default();
    let mut session = CanvasSession::new(&mut document).unwrap();
    session.set_surface(&mut document, 128, 96).unwrap();
    let base = SourceImage::new(RgbaImage::from_pixel(128, 96, Rgba([20, 40, 80, 255]))).unwrap();
    let overlay = SourceImage::new(RgbaImage::from_pixel(4, 4, Rgba([200, 40, 80, 128]))).unwrap();
    document.set_background(
        SourceImage::from_layers(&[
            ImageSourceLayer::new(base),
            ImageSourceLayer {
                image: overlay,
                canvas_rect: Some(CanvasRect::new(10.0, 12.0, 24.0, 12.0).unwrap()),
                coverage: Some(CanvasRect::new(12.0, 14.0, 10.0, 8.0).unwrap()),
                smart_erase_source: false,
            },
        ])
        .unwrap(),
    );
    let batch = serde_json::to_vec(
        &serde_json::json!({"version":1,"label":"vector export","operations":[
            {"type":"rectangle","bounds":[10,10,70,50],"style":{"fill":[220,20,30,255]}},
            {"type":"text","bounds":[12,12,80,40],"text":"Portable glyphs"}
        ]}),
    )
    .unwrap();
    document
        .command(|engine| engine.apply_annotation_json(&batch))
        .unwrap();
    crate::prepare_text_metrics(&mut document, &mut session).unwrap();
    let snapshot = CanvasSnapshot::capture(&document, &session).unwrap();
    let host = gpui_windows::gpu_extensions::OffscreenGpuHost::new().unwrap();
    let mut job = GpuExportJob::bytes(
        host.service(),
        snapshot,
        1.0,
        Default::default(),
        GpuExportFormat::Svg,
        Default::default(),
    )
    .unwrap();
    let svg = String::from_utf8(finish(&mut job).unwrap()).unwrap();
    assert_eq!(svg.matches("<image ").count(), 2);
    assert!(
        svg.contains("clipPathUnits=\"userSpaceOnUse\"")
            && svg.contains("preserveAspectRatio=\"none\"")
    );
    assert!(!svg.contains("<text") && svg.contains("<path"));
    assert_eq!(host.service().diagnostics().pending_readback_bytes, 0);
    let batch = serde_json::to_vec(&serde_json::json!({"version":1,"label":"missing result","operations":[
        {"type":"rectangle_filter","bounds":[30,30,20,20],"style":{"filter":"gaussian_blur","strength":0.5}}
    ]})).unwrap();
    document
        .command(|engine| engine.apply_annotation_json(&batch))
        .unwrap();
    session.flush(&document).unwrap();
    let filter_id = session
        .scene()
        .nodes()
        .find_map(|(_, node)| {
            if let SceneDisplayItem::Filter(filter) = node.item.as_ref() {
                Some(ElementId {
                    index: filter.id.index,
                    generation: filter.id.generation,
                })
            } else {
                None
            }
        })
        .unwrap();
    session
        .mutate(&mut document, |engine, viewport| {
            engine.select_element_with_viewport_changes(viewport, filter_id)
        })
        .unwrap();
    let mut style = document
        .engine()
        .viewport_style_toolbar_state(session.viewport())
        .unwrap()
        .filter_style;
    style.filter_type = CanvasFilterType::SmartErase;
    session
        .mutate(&mut document, |engine, viewport| {
            engine.set_viewport_filter_style(viewport, style, FILTER_STYLE_PROPERTY_ALL)
        })
        .unwrap();
    let snapshot = CanvasSnapshot::capture(&document, &session).unwrap();
    assert!(matches!(
        validate_smart_erase(&snapshot, &Default::default()),
        Err(CanvasError::Unsupported(_))
    ));
    let filter = snapshot
        .scene
        .nodes()
        .find_map(|(_, node)| {
            if let SceneDisplayItem::Filter(filter) = node.item.as_ref() {
                Some(filter)
            } else {
                None
            }
        })
        .unwrap();
    let mut service = crate::SmartEraseService::default();
    service
        .solve(
            filter,
            snapshot.background.as_ref().unwrap(),
            &Default::default(),
        )
        .unwrap();
    validate_smart_erase(&snapshot, &service.snapshot()).unwrap();
}

#[test]
fn svg_entry_points_agree_and_rough_styles_use_gpu_composites() {
    #[derive(Clone, Default)]
    struct SharedWriter(Arc<std::sync::Mutex<Vec<u8>>>);
    impl Write for SharedWriter {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            self.0.lock().unwrap().extend_from_slice(bytes);
            Ok(bytes.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }
    let host = gpui_windows::gpu_extensions::OffscreenGpuHost::new().unwrap();
    let mut document = CanvasDocument::default();
    let mut session = CanvasSession::new(&mut document).unwrap();
    session.set_surface(&mut document, 96, 72).unwrap();
    session
        .set_camera(
            &mut document,
            Camera {
                center: Point::new(48.0, 36.0),
                zoom: 1.0,
            },
        )
        .unwrap();
    let batch = serde_json::to_vec(&serde_json::json!({"version":1,"operations":[
        {"type":"rectangle","bounds":[14,12,60,40],"style":{"fill":[220,20,30,255],"stroke":[30,40,150,255],"stroke_width":3}}
    ]})).unwrap();
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
    for rendering_style in [
        StrokeRenderingStyle::Architect,
        StrokeRenderingStyle::Artist,
        StrokeRenderingStyle::Cartoonist,
    ] {
        let mut style = document
            .engine()
            .viewport_style_toolbar_state(session.viewport())
            .unwrap()
            .shape_style;
        style.stroke_rendering_style = rendering_style;
        session
            .mutate(&mut document, |engine, viewport| {
                engine.set_viewport_shape_style_patch(
                    viewport,
                    ShapeStylePatch {
                        kind: ShapeKind::Rectangle,
                        style,
                        properties:
                            snow_draw_engine_editor::SHAPE_STYLE_PROPERTY_STROKE_RENDERING_STYLE,
                    },
                )
            })
            .unwrap();
        let snapshot = CanvasSnapshot::capture(&document, &session).unwrap();
        let vector = rendering_style == StrokeRenderingStyle::Architect;
        assert_eq!(
            vector_snapshot(&snapshot, GpuExportFormat::Svg).is_some(),
            vector
        );
        let mut bytes = GpuExportJob::bytes(
            host.service(),
            snapshot.clone(),
            1.0,
            Default::default(),
            GpuExportFormat::Svg,
            Default::default(),
        )
        .unwrap();
        let bytes = finish(&mut bytes).unwrap();
        let output = SharedWriter::default();
        let mut writer = GpuExportJob::to_writer(
            host.service(),
            snapshot.clone(),
            1.0,
            Default::default(),
            GpuExportFormat::Svg,
            output.clone(),
            Default::default(),
        )
        .unwrap();
        finish(&mut writer).unwrap();
        assert_eq!(
            bytes,
            *output.0.lock().unwrap(),
            "writer SVG diverged for {rendering_style:?}"
        );
        let path = std::env::temp_dir().join(format!(
            "snow-gpu-svg-{}-{}.svg",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let mut file = GpuExportJob::to_path(
            host.service(),
            snapshot,
            1.0,
            Default::default(),
            GpuExportFormat::Svg,
            path.clone(),
            Default::default(),
        )
        .unwrap();
        finish(&mut file).unwrap();
        let saved = std::fs::read(&path).unwrap();
        std::fs::remove_file(path).unwrap();
        assert_eq!(bytes, saved, "path SVG diverged for {rendering_style:?}");
        let svg = String::from_utf8(bytes).unwrap();
        assert_eq!(svg.contains("<image "), !vector);
        assert!(
            !svg.contains("feDisplacementMap"),
            "SVG must preserve the GPU rough rendering"
        );
    }
}

#[test]
fn arrow_outline_svg_falls_back_to_the_correct_gpu_pixels_for_every_entry_point() {
    use base64::Engine as _;

    #[derive(Clone, Default)]
    struct SharedWriter(Arc<std::sync::Mutex<Vec<u8>>>);
    impl Write for SharedWriter {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            self.0.lock().unwrap().extend_from_slice(bytes);
            Ok(bytes.len())
        }
        fn flush(&mut self) -> std::io::Result<()> {
            Ok(())
        }
    }
    let host = gpui_windows::gpu_extensions::OffscreenGpuHost::new().unwrap();
    for (width, alpha, raster) in [(0.0, 128, false), (4.0, 0, false), (4.0, 128, true)] {
        let mut document = CanvasDocument::default();
        let mut session = CanvasSession::new(&mut document).unwrap();
        session.set_surface(&mut document, 96, 72).unwrap();
        session
            .set_camera(
                &mut document,
                Camera {
                    center: Point::new(48.0, 36.0),
                    zoom: 1.0,
                },
            )
            .unwrap();
        let batch = serde_json::to_vec(&serde_json::json!({"version":1,"operations":[
            {"type":"arrow","points":[[16,36],[80,36]],"style":{
                "stroke":[200,40,20,0],"stroke_width":8,
                "arrow_outline_width":width,"arrow_outline_color":[20,180,40,alpha]
            }}
        ]}))
        .unwrap();
        document
            .command(|engine| engine.apply_annotation_json(&batch))
            .unwrap();
        session.flush(&document).unwrap();
        let snapshot = CanvasSnapshot::capture(&document, &session).unwrap();
        assert_eq!(
            vector_snapshot(&snapshot, GpuExportFormat::Svg).is_none(),
            raster
        );
        assert_eq!(
            crate::vector_export::svg(&snapshot, 1.0, &Default::default()).is_err(),
            raster
        );
        let mut job = GpuExportJob::bytes(
            host.service(),
            snapshot.clone(),
            1.0,
            Default::default(),
            GpuExportFormat::Svg,
            Default::default(),
        )
        .unwrap();
        let bytes = finish(&mut job).unwrap();
        let svg = std::str::from_utf8(&bytes).unwrap();
        assert_eq!(svg.contains("data:image/png;base64,"), raster);
        if !raster {
            continue;
        }
        let payload = svg
            .split_once("data:image/png;base64,")
            .unwrap()
            .1
            .split('"')
            .next()
            .unwrap();
        let png = base64::engine::general_purpose::STANDARD
            .decode(payload)
            .unwrap();
        let mut job = GpuExportJob::bytes(
            host.service(),
            snapshot.clone(),
            1.0,
            Default::default(),
            GpuExportFormat::Png,
            Default::default(),
        )
        .unwrap();
        let direct = finish(&mut job).unwrap();
        let embedded = image::load_from_memory(&png).unwrap().to_rgba8();
        assert_eq!(
            embedded,
            image::load_from_memory(&direct).unwrap().to_rgba8()
        );
        // The ordinary document viewport clears to white. The partial-alpha
        // outline must composite over that backdrop, retaining the empty center.
        assert_eq!(embedded.get_pixel(48, 40).0, [137, 217, 147, 255]);
        assert_eq!(embedded.get_pixel(48, 36).0, [255; 4]);
        let output = SharedWriter::default();
        let mut job = GpuExportJob::to_writer(
            host.service(),
            snapshot.clone(),
            1.0,
            Default::default(),
            GpuExportFormat::Svg,
            output.clone(),
            Default::default(),
        )
        .unwrap();
        finish(&mut job).unwrap();
        assert_eq!(bytes, *output.0.lock().unwrap());
        let path = std::env::temp_dir().join(format!(
            "snow-arrow-outline-{}-{}.svg",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let mut job = GpuExportJob::to_path(
            host.service(),
            snapshot,
            1.0,
            Default::default(),
            GpuExportFormat::Svg,
            path.clone(),
            Default::default(),
        )
        .unwrap();
        finish(&mut job).unwrap();
        let saved = std::fs::read(&path).unwrap();
        std::fs::remove_file(path).unwrap();
        assert_eq!(saved, bytes);
    }
}

#[test]
fn export_staging_reserves_the_shared_budget_before_native_submission() {
    let host = gpui_windows::gpu_extensions::OffscreenGpuHost::new().unwrap();
    let service = host.service();
    let budget = 16 * 1024 * 1024;
    let configured = GpuCanvas::with_memory_budget(service.clone(), budget).unwrap();
    let memory = crate::gpu_memory::GpuMemory::shared(&service, None, budget).unwrap();
    let mut document = CanvasDocument::default();
    let mut session = CanvasSession::new(&mut document).unwrap();
    session.set_surface(&mut document, 128, 128).unwrap();
    let snapshot = CanvasSnapshot::capture(&document, &session).unwrap();
    let mut job = GpuExportJob::bytes(
        service.clone(),
        snapshot,
        1.0,
        Default::default(),
        GpuExportFormat::Png,
        Default::default(),
    )
    .unwrap();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while job.textures.is_empty() {
        assert!(matches!(job.poll(), Poll::Pending));
        assert!(std::time::Instant::now() < deadline);
        std::thread::yield_now();
    }
    assert!(job.readbacks.is_empty());
    let tile = &job.textures.front().unwrap().0;
    let bytes = tile.region.width as usize * tile.region.height as usize * 4;
    let held = memory.reserve(budget - memory.used() - bytes + 1).unwrap();
    match job.poll() {
        Poll::Ready(Err(CanvasError::Gpu(error))) => assert!(error.contains("budget")),
        _ => panic!("staging escaped the shared budget"),
    }
    assert_eq!(service.diagnostics().pending_readback_bytes, 0);
    assert!(memory.peak() <= budget);
    drop(held);
    drop(job);
    drop(configured);
    while service.poll_deferred().unwrap() {
        assert!(std::time::Instant::now() < deadline);
        std::thread::yield_now();
    }
    assert_eq!(memory.used(), 0);
}

#[test]
fn color_font_svg_routing_cannot_bypass_gpu_compositing() {
    let mut document = CanvasDocument::default();
    let mut session = CanvasSession::new(&mut document).unwrap();
    session.set_surface(&mut document, 100, 96).unwrap();
    let batch = serde_json::to_vec(&serde_json::json!({"version":1,"operations":[
        {"type":"text","bounds":[10,10,80,76],"text":"😀","style":{"font_family":"Segoe UI Emoji","font_size":40}}
    ]})).unwrap();
    document
        .command(|engine| engine.apply_annotation_json(&batch))
        .unwrap();
    let snapshot = CanvasSnapshot::capture(&document, &session).unwrap();
    assert!(
        vector_snapshot(&snapshot, GpuExportFormat::Svg).is_none(),
        "native color glyphs require GPU composition"
    );
    assert!(matches!(
        crate::vector_export::svg(&snapshot, 1.0, &Default::default()),
        Err(CanvasError::Unsupported(_))
    ));
}

#[test]
fn export_reuses_two_budgeted_staging_slots_and_cancellation_retires_them() {
    let host = gpui_windows::gpu_extensions::OffscreenGpuHost::new().unwrap();
    let service = host.service();
    let budget = 16 * 1024 * 1024;
    let configured = GpuCanvas::with_memory_budget(service.clone(), budget).unwrap();
    let memory = crate::gpu_memory::GpuMemory::shared(&service, None, budget).unwrap();
    let snapshot = CanvasSnapshot::source_only(
        SourceImage::new(RgbaImage::from_pixel(1024, 512, Rgba([40, 90, 170, 255]))).unwrap(),
    );
    let mut job = GpuExportJob::bytes(
        service.clone(),
        snapshot.clone(),
        1.0,
        Default::default(),
        GpuExportFormat::Png,
        Default::default(),
    )
    .unwrap();
    let image = image::load_from_memory(&finish(&mut job).unwrap())
        .unwrap()
        .into_rgba8();
    assert_eq!(image.get_pixel(0, 0).0, [40, 90, 170, 255]);
    assert_eq!(image.get_pixel(1023, 511).0, [40, 90, 170, 255]);
    let diagnostics = service.diagnostics();
    assert!(
        diagnostics.readback_allocations <= 2,
        "uniform bands allocated extra staging slots: {diagnostics:?}"
    );
    assert!(
        diagnostics.reused_readbacks >= 14,
        "uniform bands did not reuse staging: {diagnostics:?}"
    );
    assert_eq!(diagnostics.pending_readback_bytes, 0);
    assert!(memory.peak() <= budget);
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while service.poll_deferred().unwrap() {
        assert!(std::time::Instant::now() < deadline);
        std::thread::yield_now();
    }
    assert_eq!(memory.used(), 0, "completed job retained GPU allocations");

    let cancel = CanvasCancellation::default();
    let mut cancelled = GpuExportJob::bytes(
        service.clone(),
        snapshot,
        1.0,
        Default::default(),
        GpuExportFormat::Png,
        cancel.clone(),
    )
    .unwrap();
    while cancelled.readbacks.is_empty() {
        assert!(matches!(cancelled.poll(), Poll::Pending));
        assert!(std::time::Instant::now() < deadline);
        std::thread::yield_now();
    }
    cancel.cancel();
    assert!(matches!(
        cancelled.poll(),
        Poll::Ready(Err(CanvasError::Cancelled))
    ));
    while service.poll_deferred().unwrap() {
        assert!(std::time::Instant::now() < deadline);
        std::thread::yield_now();
    }
    assert_eq!(service.diagnostics().pending_readback_bytes, 0);
    assert_eq!(
        memory.used(),
        0,
        "cancelled staging allocations were stranded"
    );
    drop(configured);
}
