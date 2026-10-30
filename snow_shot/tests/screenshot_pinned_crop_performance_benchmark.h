// SPDX-License-Identifier: GPL-3.0-or-later
// Included by the pinned fixture only in performance presets.
void pinnedCropPerformance() {
    using namespace pinned_crop_tests;
    for (const int extent : {1024, 4096}) {
        ScreenshotPinnedWindow window;
        auto setup = config();
        QImage source(extent, extent, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(30, 100, 170));
        setup.canvasSourceRect = QRectF(0, 0, 1024, 1024);
        setup.imageSource = ScreenshotImageSource::fromImage(source, setup.canvasSourceRect);
        setup.initialWindowSize = QSize(1024, 1024);
        setup.nativeGeometry = QRect(100, 100, 768, 768);
        Access::prepareReplacement(window, setup);
        require(Access::crop(window, {100, 100, 400, 300}), "benchmark crop fixture");
        const auto key = Access::originalImage(window).cacheKey();
        const auto* storage = Access::originalImage(window).constBits();
        const auto document = Access::drawingHistory(window);
        const auto originalRevision = Access::renderer(window).originalBackgroundRevision();
        const QPointF press = LiveAccess::contentGeometry(window).bottomRight();
        require(Access::beginCropResize(window, press, int(pinned_crop_tests::Handle::BottomRight)),
                "benchmark crop drag");
        for (int i = 0; i != 30; ++i)
            Access::moveCropResize(window, press + QPointF(i % 40, i % 25));
        const auto memory = snow::test_support::memorySnapshot();
        std::vector<qint64> samples;
        samples.reserve(300);
        QElapsedTimer timer;
        for (int i = 0; i != 300; ++i) {
            timer.start();
            Access::moveCropResize(window, press + QPointF(i % 40, i % 25));
            samples.push_back(timer.nsecsElapsed());
        }
        const auto after = snow::test_support::memorySnapshot();
        Access::endCropResize(window);
        std::sort(samples.begin(), samples.end());
        require(Access::originalImage(window).cacheKey() == key &&
                    Access::originalImage(window).constBits() == storage &&
                    Access::drawingHistory(window) == document &&
                    Access::renderer(window).originalBackgroundRevision() == originalRevision,
                "drag retains source storage, document, and original-background caches");
        std::cout << "crop_drag source=" << extent << 'x' << extent
                  << " samples=300 p50_us=" << static_cast<double>(samples[149]) / 1000.0
                  << " p95_us=" << static_cast<double>(samples[284]) / 1000.0
                  << " resident_delta_bytes="
                  << static_cast<qint64>(after.residentBytes) -
                         static_cast<qint64>(memory.residentBytes)
                  << " footprint_delta_bytes="
                  << static_cast<qint64>(after.footprintBytes) -
                         static_cast<qint64>(memory.footprintBytes)
                  << " source_storage_reused=1 original_background_cache_reused=1\n";
        auto first = exportImage(window, Access::cropSave(window));
        auto second = exportImage(window, Access::cropSave(window));
        require(first.constBits() == second.constBits(), "unchanged crop reuses export pixels");
    }
    const auto animation = snow_shot::image_codec::decodeAnimationSource(
        pinned_animation_fixtures::images().first().bytes);
    ScreenshotPinnedWindow window;
    auto setup = config();
    setup.canvasSourceRect = QRectF(0, 0, 40, 40);
    setup.initialWindowSize = QSize(40, 40);
    setup.nativeGeometry = QRect(100, 100, 40, 40);
    setup.imageSource =
        ScreenshotImageSource::fromImage(animation->firstFrame, setup.canvasSourceRect);
    setup.animationSource = animation;
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    require(window.present(setup), "benchmark animation fixture");
    auto* canvas = window.findChild<SnowCanvasWidget*>();
    translation_tests::waitUntil(
        [&] {
            static_cast<void>(renderWidget(*canvas));
            return Access::shakeContentReady(window);
        },
        "benchmark animation ready");
    Access::playback(window).pause();
    require(Access::crop(window, {0, 0, 20, 40}), "benchmark animation crop");
    const auto before = Access::playback(window).diagnostics();
    for (int i = 0; i != 300; ++i)
        require(Access::crop(window, {0, 0, 20.0 + i % 10, 40}), "benchmark animation drag update");
    const auto after = Access::playback(window).diagnostics();
    require(before.decodeRequests == after.decodeRequests &&
                before.compositedFrames == after.compositedFrames,
            "crop dragging introduces no animation decoding or frame compositing");
    Access::playback(window).nextFrame();
    translation_tests::waitUntil(
        [&] {
            static_cast<void>(renderWidget(*canvas));
            return Access::playback(window).diagnostics().displayedFrames > after.displayedFrames;
        },
        "benchmark cropped animation frame delivery");
    const auto delivered = Access::playback(window).diagnostics();
    std::cout << "crop_animation drag_updates=300 extra_decode_requests=0 extra_composited_frames=0"
              << " frame_decode_latency_us="
              << static_cast<double>(delivered.maximumDecodeLatencyNs) / 1000.0
              << " cached_bytes=" << delivered.cachedBytes << '\n';
    window.hide();
}
