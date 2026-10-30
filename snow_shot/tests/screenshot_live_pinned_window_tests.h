// SPDX-License-Identifier: GPL-3.0-or-later
// Included by screenshot_pinned_window_tests.cpp after its shared test helpers.
#include "snow_shot/presentation/livewindowsource.h"
#include "snow_shot/presentation/screenshotselectionexportuiservices.h"
#include "widgets/result.h"
#ifdef Q_OS_WIN
#include <ole2.h>
#endif

class ScreenshotLivePinnedWindowTestAccess {
  public:
    static bool allowed(const ScreenshotPinnedWindow& window, const QString& command) {
        return window.liveCommandAllowed(command);
    }
    static bool checkerboard(const ScreenshotPinnedWindow& window) {
        return window.m_checkerboardEnabled.value_or(false);
    }
    static std::shared_ptr<ScreenshotExportArtifact> artifact(ScreenshotPinnedWindow& window,
                                                              bool opacity = true) {
        return window.liveViewportArtifact(opacity);
    }
    static void command(ScreenshotPinnedWindow& window, const QString& command) {
        window.executeImageCommand(command);
    }
    static void convert(ScreenshotPinnedWindow& window) {
        window.convertLiveToImage();
    }
    static QRect contentGeometry(const ScreenshotPinnedWindow& window) {
        return window.authoritativeContentNativeGeometry();
    }
    static QRect contentView(const ScreenshotPinnedWindow& window) {
        return window.contentViewRect().toAlignedRect();
    }
    static void opacity(ScreenshotPinnedWindow& window, int percent) {
        window.setOpacityPercent(percent);
    }
    static void alwaysOnTop(ScreenshotPinnedWindow& window, bool enabled) {
        window.setAlwaysOnTop(enabled);
    }
    static adqt::widgets::AdContextMenu* currentContextMenu(const ScreenshotPinnedWindow& window) {
        return window.m_contextMenu;
    }
    static void scale(ScreenshotPinnedWindow& window, int percent) {
        window.applyScale(percent);
    }
    static void lock(ScreenshotPinnedWindow& window, bool enabled) {
        window.setLockedMode(enabled);
    }
    static bool clickThrough(ScreenshotPinnedWindow& window, bool enabled) {
        return window.setClickThroughMode(enabled);
    }
    static QImage original(const ScreenshotPinnedWindow& window) {
        return window.m_originalImage;
    }
    static QImage transformed(const ScreenshotPinnedWindow& window) {
        return window.m_transformedImage;
    }
    static QTransform transform(const ScreenshotPinnedWindow& window) {
        return window.m_imageTransform;
    }
    static QRectF contentRect(const ScreenshotPinnedWindow& window) {
        return window.contentViewRect();
    }
    static bool snapshotPending(const ScreenshotPinnedWindow& window) {
        return window.m_liveSnapshotPending;
    }
    static void appearance(ScreenshotPinnedWindow& window) {
        window.setAlwaysOnTop(false);
        window.setShowBorder(false);
        window.setShowShadow(true);
        window.setOpacityPercent(70);
    }
    static void persist(ScreenshotPinnedWindow& window) {
        window.persistNow();
    }
    static void split(ScreenshotPinnedWindow& window) {
        window.splitLiveImage();
    }
    static void edit(ScreenshotPinnedWindow& window) {
        window.editLiveImage();
    }
    static bool editButtonHidden(const ScreenshotPinnedWindow& window) {
        return window.m_editButton == nullptr || window.m_editButton->isHidden();
    }
    static bool automaticRecognition(const ScreenshotPinnedWindow& window) {
        return window.m_automaticTextRecognition;
    }
    static bool configuredAutomaticRecognition(const ScreenshotPinnedWindow& window) {
        return window.m_staticAutomaticTextRecognition;
    }
    static void recognize(ScreenshotPinnedWindow& window) {
        window.activateRecognitionMode(0, false);
    }
    static void copy(ScreenshotPinnedWindow& window) {
        window.copyCurrentViewport();
    }
};

namespace live_pinned_tests {
using Source = snow_shot::presentation::LiveWindowSource;
using State = snow_shot::presentation::LiveWindowState;
using Presentation = snow_shot::presentation::LiveWindowPresentation;
using Access = ScreenshotLivePinnedWindowTestAccess;

class FakeSource final : public Source {
  public:
    QImage frame;
    QRect region{40, 50, 160, 80};
    State currentState = State::Live;
    QString title = QStringLiteral("Project notes — Editor");
    int requests = 0;
    int shutdowns = 0;
    int initialFrameReleases = 0;
    bool deferFrames = false;
    bool nativePresentation = false;
    QVector<std::pair<QImage, FrameCompletion>> pending;
    QVector<Presentation> presentations;

    FakeSource() : frame(80, 40, QImage::Format_ARGB32_Premultiplied) {
        frame.fill(Qt::red);
    }
    void initialize(InitializationCompletion completion) override {
        completion(true, {});
    }
    QImage initialFrame() const override {
        return initialFrameReleases ? QImage() : frame;
    }
    void releaseInitialFrame() override {
        ++initialFrameReleases;
    }
    QRect initialDesktopRegion() const override {
        return region;
    }
    State state() const override {
        return currentState;
    }
    QString stateMessage() const override {
        return {};
    }
    QString windowTitle() const override {
        return title;
    }
    void requestFrame(FrameCompletion completion) override {
        ++requests;
        const QImage captured = currentState == State::Live && shutdowns == 0 ? frame : QImage();
        if (deferFrames)
            pending.push_back({captured, std::move(completion)});
        else
            completion(captured, {});
    }
    bool setPresentation(const Presentation& presentation, QString*) override {
        presentations.push_back(presentation);
        return shutdowns == 0;
    }
    bool usesNativePresentation() const override {
        return nativePresentation;
    }
    void shutdown() override {
        if (shutdowns)
            return;
        ++shutdowns;
        for (auto& request : std::exchange(pending, {}))
            request.second({}, {});
        emit stopped();
    }
    void publish(const QColor& color) {
        frame = QImage(frame.size(), frame.format());
        frame.fill(color);
        emit frameChanged(frame);
    }
    void setNativePresentation(bool native) {
        nativePresentation = native;
        emit nativePresentationChanged(native);
    }
    void setState(State state) {
        currentState = state;
        emit stateChanged(state, {});
    }
    void deliver() {
        for (auto& request : std::exchange(pending, {}))
            request.second(request.first, {});
    }
};

template <typename Predicate> void wait(Predicate predicate, const char* message) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    require(predicate(), message);
}

class ClipboardRestorer final {
  public:
    ClipboardRestorer() {
#ifdef Q_OS_WIN
        m_ole = LoadLibraryW(L"ole32.dll");
        m_window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
        require(m_window, "clipboard fixture requires a native clipboard owner");
        using Duplicate = HANDLE(WINAPI*)(HANDLE, CLIPFORMAT, UINT);
        const auto duplicate =
            m_ole ? reinterpret_cast<Duplicate>(GetProcAddress(m_ole, "OleDuplicateData"))
                  : nullptr;
        require(duplicate, "clipboard fixture requires native clipboard duplication");
        wait([&] { return OpenClipboard(m_window) != FALSE; }, "preserve the native clipboard");
        const auto close = qScopeGuard([] { CloseClipboard(); });
        UINT format = 0;
        while ((format = EnumClipboardFormats(format)) != 0) {
            if (format == CF_OWNERDISPLAY)
                continue;
            const auto data = GetClipboardData(format);
            if (!data)
                continue;
            HANDLE saved = nullptr;
            if (format == CF_ENHMETAFILE || format == CF_DSPENHMETAFILE) {
                saved = CopyEnhMetaFileW(static_cast<HENHMETAFILE>(data), nullptr);
            } else {
                const UINT duplicateFormat = format == CF_DSPBITMAP         ? CF_BITMAP
                                             : format == CF_DSPMETAFILEPICT ? CF_METAFILEPICT
                                                                            : format;
                saved = duplicate(data, static_cast<CLIPFORMAT>(duplicateFormat), 0);
            }
            require(saved, "duplicate the original native clipboard format");
            m_formats.push_back({format, saved});
        }
#else
        m_saved = std::make_unique<QMimeData>();
        if (const auto* current = QApplication::clipboard()->mimeData()) {
            for (const auto& format : current->formats())
                m_saved->setData(format, current->data(format));
            if (current->hasImage())
                m_saved->setImageData(current->imageData());
            if (current->hasUrls())
                m_saved->setUrls(current->urls());
        }
#endif
    }
    ~ClipboardRestorer() {
#ifdef Q_OS_WIN
        bool opened = false;
        for (int attempt = 0; attempt < 100 && !opened; ++attempt) {
            opened = OpenClipboard(m_window) != FALSE;
            if (!opened)
                QThread::msleep(5);
        }
        if (opened) {
            if (EmptyClipboard()) {
                for (auto& format : m_formats)
                    if (SetClipboardData(format.first, format.second))
                        format.second = nullptr;
            }
            CloseClipboard();
        }
        for (const auto& format : m_formats) {
            if (!format.second)
                continue;
            if (format.first == CF_BITMAP || format.first == CF_DSPBITMAP ||
                format.first == CF_PALETTE)
                DeleteObject(format.second);
            else if (format.first == CF_ENHMETAFILE || format.first == CF_DSPENHMETAFILE)
                DeleteEnhMetaFile(static_cast<HENHMETAFILE>(format.second));
            else {
                if (format.first == CF_METAFILEPICT || format.first == CF_DSPMETAFILEPICT) {
                    auto* picture = static_cast<METAFILEPICT*>(GlobalLock(format.second));
                    if (picture) {
                        DeleteMetaFile(picture->hMF);
                        GlobalUnlock(format.second);
                    }
                }
                GlobalFree(format.second);
            }
        }
        if (m_ole)
            FreeLibrary(m_ole);
        if (m_window)
            DestroyWindow(m_window);
#else
        QApplication::clipboard()->setMimeData(m_saved.release());
#endif
    }

  private:
#ifdef Q_OS_WIN
    HMODULE m_ole = nullptr;
    HWND m_window = nullptr;
    QVector<std::pair<UINT, HANDLE>> m_formats;
#else
    std::unique_ptr<QMimeData> m_saved;
#endif
};

class ClipboardPublicationObserver final {
  public:
    ClipboardPublicationObserver() {
#ifdef Q_OS_WIN
        m_window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
        require(m_window &&
                    SetWindowSubclass(m_window, receive, 1, reinterpret_cast<DWORD_PTR>(this)) &&
                    AddClipboardFormatListener(m_window),
                "observe complete native clipboard publications");
#else
        m_connection = QObject::connect(QApplication::clipboard(), &QClipboard::dataChanged,
                                        [&] { ++publications; });
#endif
    }
    ~ClipboardPublicationObserver() {
#ifdef Q_OS_WIN
        RemoveClipboardFormatListener(m_window);
        RemoveWindowSubclass(m_window, receive, 1);
        DestroyWindow(m_window);
#else
        QObject::disconnect(m_connection);
#endif
    }
    int publications = 0;

  private:
#ifdef Q_OS_WIN
    static LRESULT CALLBACK receive(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR, DWORD_PTR reference) {
        if (message == WM_CLIPBOARDUPDATE)
            ++reinterpret_cast<ClipboardPublicationObserver*>(reference)->publications;
        return DefSubclassProc(window, message, wParam, lParam);
    }
    HWND m_window = nullptr;
#else
    QMetaObject::Connection m_connection;
#endif
};

ScreenshotPinnedWindow::Config config(const std::shared_ptr<FakeSource>& source) {
    ScreenshotPinnedWindow::Config value;
    value.screen = QGuiApplication::primaryScreen();
    require(value.screen, "live fake fixture requires the offscreen display");
    value.nativeGeometry = source->region;
    value.initialWindowSize = source->region.size();
    value.canvasSourceRect = QRectF(QPointF(), QSizeF(source->region.size()));
    value.imageSource = ScreenshotImageSource::fromImage(source->frame, value.canvasSourceRect);
    value.liveSource = source;
    value.enableEditing = true;
    value.automaticTextRecognition = true;
    value.checkerboardEnabled = false;
    value.initialBorderVisible = false;
    value.initialShadowVisible = false;
    return value;
}

void present(ScreenshotPinnedWindow& window, const ScreenshotPinnedWindow::Config& value) {
    bool completed = false;
    bool succeeded = false;
    require(window.present(value,
                           [&](bool success, QImage) {
                               completed = true;
                               succeeded = success;
                           }),
            "fake live source presentation must start");
    window.setAttribute(Qt::WA_DeleteOnClose, false);
    wait([&] { return completed; }, "fake live presentation must complete");
    require(succeeded && window.isLiveWindow() && window.liveFrameAvailable(),
            "an initialized source must be a live pin after its first frame");
}

QImage image(const std::shared_ptr<ScreenshotExportArtifact>& artifact, QObject* receiver) {
    require(bool(artifact), "current live frame must provide an export artifact");
    bool completed = false;
    ScreenshotExportImageResult result;
    require(artifact->requestImage(receiver,
                                   [&](ScreenshotExportImageResult value) {
                                       result = std::move(value);
                                       completed = true;
                                   }),
            "live frame image export must start");
    wait([&] { return completed; }, "live frame image export must complete");
    require(result.succeeded(), "live frame export must provide pixels");
    return result.image;
}

QSize exportSize(const ScreenshotPinnedWindow& window) {
    const auto content = Access::contentGeometry(window);
    const qreal scale = snow_shot::presentation::kPinnedGeometryUnits ==
                                snow_shot::storage::PinnedGeometryUnits::LogicalPixels
                            ? window.devicePixelRatioF()
                            : 1;
    return QSize(qRound(content.width() * scale), qRound(content.height() * scale));
}

void frameSnapshotsAndTransforms() {
    auto source = std::make_shared<FakeSource>();
    ScreenshotPinnedWindow window;
    present(window, config(source));
    require(source->initialFrameReleases == 1 && source->initialFrame().isNull(),
            "successful first presentation releases the source's obsolete initialization image");
    const QRect originalGeometry = window.currentNativeGeometry();
    const QSize originalOutput = exportSize(window);
    auto frozen = Access::artifact(window);
    require(source->requests == 1, "export requests the current frame at action execution");
    source->publish(Qt::green);
    require(Access::original(window).cacheKey() == source->frame.cacheKey(),
            "identity frames install directly and share the captured pixels");
    const auto exported = image(frozen, &window);
    require(exported.size() == originalOutput &&
                exported.pixelColor(exported.rect().center()) == QColor(Qt::red) &&
                window.isLiveWindow() && source->shutdowns == 0,
            "exports freeze the requested frame at displayed dimensions and retain live mode");
    wait([&] { return Access::original(window).pixelColor(0, 0) == QColor(Qt::green); },
         "source updates must reach the pinned image renderer");
    require(window.currentNativeGeometry() == originalGeometry,
            "new source frames never move or resize the pin");
    Access::opacity(window, 50);
    Access::scale(window, 150);
    const auto scaled = image(Access::artifact(window), &window);
    require(scaled.size() == exportSize(window) && scaled.size() != originalOutput &&
                std::abs(scaled.pixelColor(scaled.rect().center()).alpha() - 128) <= 1,
            "exports apply current display scaling and the standard window opacity");
    Access::command(window, QStringLiteral("rotate_clockwise"));
    require(!Access::transform(window).isIdentity() && Access::transformed(window).isNull(),
            "rotation commits presentation geometry without materializing another image");
    require(window.isLiveWindow() && !Access::transform(window).isIdentity() &&
                source->shutdowns == 0,
            "rotation retains its live source association");
    source->publish(Qt::blue);
    source->publish(Qt::yellow);
    source->publish(Qt::cyan);
    wait([&] { return Access::original(window).pixelColor(0, 0) == QColor(Qt::cyan); },
         "coalesced live frame delivery must eventually display the newest frame");
    require(Access::transformed(window).isNull() &&
                Access::original(window).cacheKey() == source->frame.cacheKey() &&
                image(Access::artifact(window), &window).size() == exportSize(window),
            "rotated display shares capture pixels while snapshots retain displayed dimensions");
    Access::command(window, QStringLiteral("flip_horizontal"));
    Access::command(window, QStringLiteral("flip_vertical"));
    require(window.isLiveWindow() && source->shutdowns == 0,
            "both flip operations retain the live source");
    Access::command(window, QStringLiteral("reset_transform"));
    require(Access::transform(window).isIdentity() && window.isLiveWindow(),
            "resetting transformations preserves live mode");
    auto peerSource = std::make_shared<FakeSource>();
    peerSource->region.translate(220, 0);
    ScreenshotPinnedWindow peer;
    present(peer, config(peerSource));
    window.closeLiveWindow();
    require(source->shutdowns == 1 && peer.isVisible() && peerSource->shutdowns == 0,
            "closing one pin releases only its own source");
    peer.closeLiveWindow();
}

void liveFramesPaintTransforms() {
    auto source = std::make_shared<FakeSource>();
    source->frame.fill(Qt::red);
    {
        QPainter painter(&source->frame);
        painter.fillRect(QRect(40, 0, 40, 20), Qt::green);
        painter.fillRect(QRect(0, 20, 40, 20), Qt::blue);
        painter.fillRect(QRect(40, 20, 40, 20), Qt::yellow);
    }
    ScreenshotPinnedWindow window;
    present(window, config(source));
    for (int turn = 0; turn < 4; ++turn) {
        if (turn)
            Access::command(window, QStringLiteral("rotate_clockwise"));
        for (int flip = 0; flip < 2; ++flip) {
            if (flip)
                Access::command(window, QStringLiteral("flip_horizontal"));
            emit source->frameChanged(source->frame);
            const auto& renderer = ScreenshotPinnedWindowTestAccess::renderer(window);
            require(renderer.imageSourceSnapshot().materializedImage.cacheKey() ==
                        source->frame.cacheKey(),
                    "rotated live presentation retains original capture storage");
            const auto oriented = source->frame.transformed(Access::transform(window));
            QImage actual(window.size(), QImage::Format_ARGB32_Premultiplied);
            actual.fill(Qt::transparent);
            window.render(&actual);
            const auto content = Access::contentRect(window);
            for (const qreal x : {0.25, 0.75})
                for (const qreal y : {0.25, 0.75}) {
                    const QPoint displayed(qFloor(content.x() + x * content.width()),
                                           qFloor(content.y() + y * content.height()));
                    const QPoint pixel(qFloor(x * oriented.width()), qFloor(y * oriented.height()));
                    requireColorNear(actual.pixelColor(displayed), oriented.pixelColor(pixel), 0,
                                     "live canvas paints every rotation and reflection correctly");
                }
            if (flip)
                Access::command(window, QStringLiteral("flip_horizontal"));
        }
    }
    window.closeLiveWindow();
}

void rotatedLiveSaveOpensDialog() {
    const snow_shot::storage::ScreenshotSettings settings;
    const QString previousDialog = settings.saveAsFileDialog();
    const auto restore =
        qScopeGuard([&] { static_cast<void>(settings.setSaveAsFileDialog(previousDialog)); });
    require(settings.setSaveAsFileDialog(QStringLiteral("snow_shot")),
            "use the offscreen save dialog for the rotated live pin");
    auto source = std::make_shared<FakeSource>();
    ScreenshotPinnedWindow window;
    present(window, config(source));
    Access::command(window, QStringLiteral("rotate_clockwise"));
    require(Access::transformed(window).isNull(),
            "rotated live display has no materialized rotation");
    ScreenshotPinnedWindowTestAccess::saveAsFile(window);
    auto* modal =
        window.findChild<adqt::widgets::AdModal*>(QStringLiteral("screenshotSaveAsFileModal"));
    require(modal && window.property("saveDialogOpen").toBool() && source->requests > 0,
            "saving a rotated live pin requests an action-time frame and opens the dialog");
    modal->reject();
    wait([&] { return !window.property("saveDialogOpen").toBool(); },
         "cancelling the save dialog retains the live pin");
    window.closeLiveWindow();
}

void liveFramesReplacePixelsWithoutEditingSetup() {
    auto source = std::make_shared<FakeSource>();
    ScreenshotPinnedWindow window;
    present(window, config(source));
    const QRect geometry = window.currentNativeGeometry();
    auto& renderer = ScreenshotPinnedWindowTestAccess::renderer(window);
    for (const auto color : {QColor(Qt::blue), QColor(Qt::transparent), QColor(Qt::green)}) {
        source->publish(color);
        wait([&] { return Access::original(window).pixelColor(0, 0) == color; },
             "live frames replace both opaque and transparent content");
        const auto displayed = renderer.imageSourceSnapshot();
        require(displayed.opacityMetadata && displayed.opacityMetadata->entries.isEmpty(),
                "live frame installation does not scan pixel opacity on the GUI thread");
        const auto samples = renderer.baseImageSources();
        require(samples.size() == 1 &&
                    samples.front().image.cacheKey() == displayed.materializedImage.cacheKey(),
                "live sampling retains the current frame instead of the initial still image");
        QImage raster(window.size(), QImage::Format_ARGB32_Premultiplied);
        raster.fill(Qt::transparent);
        window.render(&raster);
        requireColorNear(raster.pixelColor(raster.rect().center()), color, 0,
                         "live rendering clears old pixels when transparent frames arrive");
        // The canvas clears transparency independently of the shell backdrop.
        // Compare against the ordinary renderer to cover the complete paint result.
        renderer.setImageSource(displayed);
        QImage reference(window.size(), QImage::Format_ARGB32_Premultiplied);
        reference.fill(Qt::transparent);
        window.render(&reference);
        require(imagesPixelAligned(raster, reference, {}, 0),
                "the live update path preserves the ordinary renderer's pixels and alpha");
        renderer.setPinnedLiveFrame(displayed);
        require(window.currentNativeGeometry() == geometry,
                "content-only frame replacement preserves the viewport geometry");
    }
    Access::convert(window);
    wait([&] { return !window.isLiveWindow(); }, "live streaming can return to editable content");
    const auto converted = renderer.imageSourceSnapshot();
    require(converted.imageIsOpaque(converted.materializedImage),
            "conversion restores ordinary image metadata and editing setup");
    window.close();
}

void deferredCopyRetainsItsActionTimeFrame() {
    const ClipboardRestorer restoreClipboard;
    const snow_shot::storage::ScreenshotSettings settings;
    const auto previousActions = settings.autoSaveAfterExecutionActions();
    const bool previousCopyFile = settings.copyImageFileToClipboard();
    const auto restoreSettings = qScopeGuard([&] {
        static_cast<void>(settings.setAutoSaveAfterExecutionActions(previousActions));
        static_cast<void>(settings.setCopyImageFileToClipboard(previousCopyFile));
    });
    require(settings.setAutoSaveAfterExecutionActions({}) &&
                settings.setCopyImageFileToClipboard(false),
            "configure an image-only clipboard publication");
    auto source = std::make_shared<FakeSource>();
    source->deferFrames = true;
    ScreenshotPinnedWindow window;
    present(window, config(source));
    const QSize expectedSize = exportSize(window);
    ClipboardPublicationObserver clipboard;
    Access::copy(window);
    require(source->pending.size() == 1, "actual Copy requests one deferred source frame");
    auto artifact = ScreenshotPinnedWindowTestAccess::exportArtifact(window);
    require(artifact && !artifact->isCancelled(), "actual Copy retains its frozen export");
    Access::scale(window, 150);
    Access::command(window, QStringLiteral("rotate_clockwise"));
    source->publish(Qt::blue);
    require(ScreenshotPinnedWindowTestAccess::exportArtifact(window) == artifact &&
                !artifact->isCancelled(),
            "live resizing, zoom, and rotation do not cancel an already captured Copy request");
    source->deliver();
    wait(
        [&] {
            return !ScreenshotPinnedWindowTestAccess::exportArtifact(window) &&
                   clipboard.publications > 0;
        },
        "actual deferred Copy publishes after later presentation changes");
    const auto copied = exportedClipboardImage();
    require(clipboard.publications == 1 && copied.size() == expectedSize &&
                copied.pixelColor(0, 0) == QColor(Qt::red) && source->requests == 1 &&
                window.isLiveWindow(),
            "actual Copy publishes the original frame and display dimensions exactly once");
    window.closeLiveWindow();
}

void stoppedSourceClosesOnlyItsPin() {
    auto source = std::make_shared<FakeSource>();
    auto peerSource = std::make_shared<FakeSource>();
    peerSource->region.translate(220, 0);
    ScreenshotPinnedWindow window;
    ScreenshotPinnedWindow peer;
    present(window, config(source));
    present(peer, config(peerSource));
    source->shutdown();
    require(!window.isVisible() && !window.liveFrameAvailable() && source->shutdowns == 1 &&
                peer.isVisible() && peer.liveFrameAvailable() && peerSource->shutdowns == 0,
            "explicit source shutdown closes its pin and leaves other live sources running");
    source->shutdown();
    require(source->shutdowns == 1 && peer.isVisible() && peerSource->shutdowns == 0,
            "source shutdown is idempotent through pin cleanup");
    peer.closeLiveWindow();
}

void statusFitsConstrainedContent() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalTheme = themes.config();
    const auto restoreTheme = qScopeGuard([&] { themes.setConfig(originalTheme); });
    themes.setColorScheme(adqt::theme::ThemeScheme::Light);
    auto source = std::make_shared<FakeSource>();
    ScreenshotPinnedWindow window;
    present(window, config(source));
    const auto verifyStatus = [&](const QString& summary, bool requireText) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCoreApplication::processEvents();
        const auto* host =
            window.findChild<QWidget*>(QStringLiteral("screenshotPinnedLiveStatusView"));
        const auto* result = window.findChild<adqt::widgets::AdResult*>(
            QStringLiteral("screenshotPinnedLiveStatus"));
        require(host && host->isVisible() && result, "unavailable content has a status view");
        require(host->geometry() == Access::contentView(window),
                "the status follows the pin without changing its content geometry");
        require(host->accessibleName() == source->title &&
                    host->accessibleDescription() == result->subTitle(),
                "every presentation retains the full source title and state for accessibility");
        bool visibleText = false;
        for (const auto* label : host->findChildren<QLabel*>()) {
            if (!label->isVisible() ||
                (label->text() != result->subTitle() && label->text() != summary))
                continue;
            require(label->visibleRegion().contains(label->rect()),
                    "the current state text is fully visible inside the pin");
            require(label->heightForWidth(label->width()) <= label->height(),
                    "the current state text has enough height for all wrapped lines");
            visibleText = true;
        }
        if (requireText)
            require(visibleText, "small pins prioritize readable state text over the title");
        if (!visibleText) {
            const auto* icon =
                host->findChild<QLabel*>(QStringLiteral("screenshotPinnedLiveStatusCompactIcon"));
            require(icon && icon->isVisible() && !icon->pixmap().isNull() &&
                        icon->visibleRegion().contains(icon->rect()),
                    "pins too small for text retain a fully visible state icon");
        }
        return visibleText;
    };
    for (const auto state : {State::Minimized, State::Closed, State::Unavailable}) {
        const QString summary = state == State::Minimized ? QStringLiteral("Minimized")
                                : state == State::Closed  ? QStringLiteral("Closed")
                                                          : QStringLiteral("Unavailable");
        source->setState(state);
        for (const int percent : {100, 50, 10, 200, 500, 100}) {
            Access::scale(window, percent);
            const QRect content = Access::contentView(window);
            verifyStatus(summary, content.width() >= 100 && content.height() >= 50);
            if (percent == 500)
                require(window.findChild<adqt::widgets::AdResult*>()->isVisible(),
                        "growing the pin restores the complete Result");
        }
        Access::command(window, QStringLiteral("rotate_clockwise"));
        verifyStatus(summary, Access::contentView(window).width() >= 100);
        Access::command(window, QStringLiteral("reset_transform"));
        source->title = QString(600, QChar(u'W'));
        source->setState(state);
        verifyStatus(summary, true);
        source->title = QStringLiteral("Project notes — Editor");
    }
    source->setState(State::Minimized);
    const QRect geometry = Access::contentGeometry(window);
    themes.setColorScheme(adqt::theme::ThemeScheme::Dark);
    verifyStatus(QStringLiteral("Minimized"), true);
    auto largerFont = themes.config();
    largerFont.fontSize = 28;
    themes.setConfig(largerFont);
    verifyStatus(QStringLiteral("Minimized"), false);
    themes.setConfig(originalTheme);
    verifyStatus(QStringLiteral("Minimized"), true);
    require(Access::contentGeometry(window) == geometry,
            "theme and font changes reflow the status without resizing the pin");

    class ExpandedStatusTranslator final : public QTranslator {
      public:
        bool isEmpty() const override {
            return false;
        }
        QString translate(const char* context, const char* text, const char*, int) const override {
            if (QByteArray(context) != "ScreenshotPinnedWindow")
                return {};
            if (QByteArray(text) == "Minimized")
                return QStringLiteral("Translated state");
            return QString(600, QChar(u'W'));
        }
    } expanded;
    QCoreApplication::installTranslator(&expanded);
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&window, &languageChange);
    verifyStatus(QStringLiteral("Translated state"), true);
    QCoreApplication::removeTranslator(&expanded);
    QCoreApplication::sendEvent(&window, &languageChange);

    for (const auto* locale : {"zh_CN", "zh_TW"}) {
        QTranslator translator;
        require(
            translator.load(QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QLatin1String(locale))),
            "load the complete application catalog for constrained status text");
        QCoreApplication::installTranslator(&translator);
        const auto removeTranslator =
            qScopeGuard([&] { QCoreApplication::removeTranslator(&translator); });
        QCoreApplication::sendEvent(&window, &languageChange);
        const QString summary = QCoreApplication::translate("ScreenshotPinnedWindow", "Minimized");
        require(summary != QStringLiteral("Minimized"), "compact status captions are translated");
        verifyStatus(summary, true);
    }
    QCoreApplication::sendEvent(&window, &languageChange);
    verifyStatus(QStringLiteral("Minimized"), true);
    source->setState(State::Live);
    require(
        !window.findChild<QWidget*>(QStringLiteral("screenshotPinnedLiveStatusView"))->isVisible(),
        "source recovery hides compact status content as well as the Result");
    window.closeLiveWindow();
}

void statusLayoutFollowsContent() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalTheme = themes.config();
    const auto restoreTheme = qScopeGuard([&] { themes.setConfig(originalTheme); });
    themes.setColorScheme(adqt::theme::ThemeScheme::Light);
    auto source = std::make_shared<FakeSource>();
    source->region = QRect(80, 90, 480, 400);
    source->title = QStringLiteral("Project notes <draft> — A long source window title in Editor");
    ScreenshotPinnedWindow window;
    present(window, config(source));
    const auto verifyLayout = [&] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCoreApplication::processEvents();
        const auto* result = window.findChild<adqt::widgets::AdResult*>(
            QStringLiteral("screenshotPinnedLiveStatus"));
        require(result, "unavailable source retains its Result content");
        const auto* host = result->parentWidget();
        require(host->isVisible(), "unavailable source displays its status");
        wait(
            [&] {
                return !result->isVisible() ||
                       result->height() == result->heightForWidth(result->width());
            },
            "the Result settles after queued theme and nested layout updates");
        const QWidget* content = nullptr;
        for (const auto* child :
             host->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
            if (child->isVisible()) {
                require(!content, "exactly one status presentation is visible");
                content = child;
            }
        require(content && host->rect().contains(content->geometry()),
                "the visible status presentation fits entirely inside the content");
        if (result->isVisible())
            require(result->height() == result->heightForWidth(result->width()),
                    "the Result settles at its wrapped height");
        require(host->geometry() == Access::contentView(window),
                "status host follows the current pinned content rectangle after resize");
        const QPointF center = QRectF(content->geometry()).center();
        const QPointF expected = QRectF(host->rect()).center();
        require(qAbs(center.x() - expected.x()) <= 1 && qAbs(center.y() - expected.y()) <= 1,
                "the wrapped Result remains centered horizontally and vertically");
        require(host->testAttribute(Qt::WA_TransparentForMouseEvents),
                "the status does not intercept pin dragging or resizing");
        return result;
    };
    QImage minimizedIcon;
    for (const auto state : {State::Minimized, State::Closed, State::Unavailable}) {
        source->setState(state);
        for (const int percent : {100, 60, 150, 80}) {
            const QSize before = window.size();
            Access::scale(window, percent);
            const auto* result = verifyLayout();
            require(result->title() == source->title, "the source title remains plain text");
            if (percent != 100)
                require(window.size() != before, "the fixture exercises a real pin resize");
        }
        auto& controller = ScreenshotPinnedWindowTestAccess::nativeController(window);
        const QRect beforeResize = controller.committedGeometry();
        require(controller.beginResize(screenshot_pinned_resize_geometry::DragHandle::Right),
                "begin resizing the unavailable live pin");
        require(controller
                    .updateResize(beforeResize.adjusted(0, 0, 80, 0),
                                  screenshot_pinned_resize_geometry::DragHandle::Right,
                                  source->region.size(), 0.1, 5.0)
                    .has_value(),
                "accept an interactive resize while the source state stays unchanged");
        require(ScreenshotPinnedWindowTestAccess::finishNativeInteraction(window) &&
                    controller.committedGeometry().size() != beforeResize.size(),
                "commit the interactive pin resize without a source-state notification");
        const auto* result = verifyLayout();
        const QImage icon = result->icon().pixmap(64, 64).toImage();
        require(!icon.isNull(), "the custom status icon renders through the icon pack");
        if (state == State::Minimized)
            minimizedIcon = icon;
        else if (state == State::Closed)
            require(icon != minimizedIcon, "closed and minimized sources have distinct artwork");
        themes.setColorScheme(adqt::theme::ThemeScheme::Dark);
        verifyLayout();
        require(result->icon().pixmap(64, 64).toImage() != icon,
                "custom status icons follow the current theme");
        themes.setColorScheme(adqt::theme::ThemeScheme::Light);

        const QString previewDirectory = qEnvironmentVariable("SNOW_TEST_LIVE_STATUS_PREVIEW");
        if (!previewDirectory.isEmpty()) {
            Access::scale(window, 100);
            verifyLayout();
            require(QDir().mkpath(previewDirectory),
                    "create the requested status preview directory");
            require(
                window.grab().save(
                    QDir(previewDirectory)
                        .filePath(QStringLiteral("status-%1.png").arg(static_cast<int>(state)))),
                "save the requested status preview");
        }
    }
    source->title.clear();
    source->setState(State::Minimized);
    require(verifyLayout()->title() == QStringLiteral("Untitled window"),
            "untitled sources receive a readable heading");
    class StatusTranslator final : public QTranslator {
      public:
        bool isEmpty() const override {
            return false;
        }
        QString translate(const char* context, const char* text, const char*, int) const override {
            return QByteArray(context) == "ScreenshotPinnedWindow"
                       ? QStringLiteral("Translated: ") + QString::fromUtf8(text)
                       : QString();
        }
    } translator;
    QCoreApplication::installTranslator(&translator);
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&window, &languageChange);
    const auto* translated = verifyLayout();
    require(translated->title() == QStringLiteral("Translated: Untitled window") &&
                translated->subTitle().startsWith(QStringLiteral("Translated: ")),
            "visible Result headings and descriptions retranslate and recenter");
    QCoreApplication::removeTranslator(&translator);
    source->setState(State::Live);
    require(!translated->isVisible(), "source recovery hides the entire Result");
    window.closeLiveWindow();
}

void statusAndCapabilityGates() {
    auto source = std::make_shared<FakeSource>();
    ScreenshotPinnedWindow window;
    present(window, config(source));
    const QStringList frameCommands{QStringLiteral("copy_to_clipboard"),
                                    QStringLiteral("save_as_file"),
                                    QStringLiteral("convert_to_image"),
                                    QStringLiteral("split_image"), QStringLiteral("image_editing")};
    const QStringList independent{
        QStringLiteral("toggle_click_through"), QStringLiteral("toggle_lock"),
        QStringLiteral("always_on_top"),        QStringLiteral("show_border"),
        QStringLiteral("show_shadow"),          QStringLiteral("close_window"),
        QStringLiteral("rotate_clockwise"),     QStringLiteral("increase_scale")};
    for (const auto state : {State::Minimized, State::Closed, State::Unavailable}) {
        source->setState(state);
        auto* menu = ScreenshotPinnedWindowTestAccess::contextMenu(window);
        materializePinnedMenuTree(window);
        for (const QString& command : frameCommands)
            require(!Access::allowed(window, command), "source status disables frame operations");
        for (const QString& command : independent)
            require(Access::allowed(window, command), "source status retains window operations");
        for (const char* name :
             {"screenshotPinnedCopyAction", "screenshotPinnedSaveAsFileAction",
              "screenshotPinnedConvertToImageAction", "screenshotPinnedSplitImageAction",
              "screenshotPinnedImageEditingAction"}) {
            auto* action = menu->findChild<QAction*>(QString::fromLatin1(name));
            require(action && !action->isEnabled(),
                    "all materialized menu frame operations track source status");
        }
        const auto* result = window.findChild<adqt::widgets::AdResult*>(
            QStringLiteral("screenshotPinnedLiveStatus"));
        require(result && result->parentWidget()->isVisible() && !result->subTitle().isEmpty() &&
                    window.isVisible(),
                "source status remains visible within the retained pin");
        require(result->title() == source->title && !result->icon().isNull(),
                "the Result identifies the source window and uses a custom status icon");
        if (state == State::Minimized)
            require(result->subTitle() ==
                        QStringLiteral(
                            "The window is minimized. The latest content cannot be displayed."),
                    "minimized sources show the specified message");
        if (state == State::Closed)
            require(
                result->subTitle() ==
                    QStringLiteral("The window is closed. The latest content cannot be displayed."),
                "closed sources show the specified message");
        const int requests = source->requests;
        Access::convert(window);
        Access::split(window);
        Access::edit(window);
        require(source->requests == requests && window.isLiveWindow(),
                "disabled image operations cannot execute through direct handlers");
    }
    source->setState(State::Live);
    for (const QString& command : frameCommands)
        require(Access::allowed(window, command),
                "restored source reenables every frame operation");
    for (const QString& command :
         {QStringLiteral("destroy_window"), QStringLiteral("drawing_mode"),
          QStringLiteral("thumbnail"), QStringLiteral("hide_to_top"), QStringLiteral("print"),
          QStringLiteral("upload_to_cloud"), QStringLiteral("copy_original_image"),
          QStringLiteral("close_all"), QStringLiteral("close_others")}) {
        require(!Access::allowed(window, command),
                "nonwhitelisted commands are rejected centrally");
        require(!window.automationAction(command),
                "automation cannot execute nonwhitelisted commands");
    }
    auto* menu = ScreenshotPinnedWindowTestAccess::contextMenu(window);
    materializePinnedMenuTree(window);
    for (const char* name :
         {"screenshotPinnedCopyAction", "screenshotPinnedSaveAsFileAction",
          "screenshotPinnedConvertToImageAction", "screenshotPinnedSplitImageAction",
          "screenshotPinnedImageEditingAction"}) {
        auto* action = menu->findChild<QAction*>(QString::fromLatin1(name));
        require(action && action->isEnabled(),
                "source recovery reenables materialized frame actions");
    }
    for (const QString& name : {QStringLiteral("screenshotPinnedDrawingAction"),
                                QStringLiteral("screenshotPinnedDestroyAction"),
                                QStringLiteral("screenshotPinnedPrintAction"),
                                QStringLiteral("screenshotPinnedHideToTopAction"),
                                QStringLiteral("screenshotPinnedThumbnailAction"),
                                QStringLiteral("screenshotPinnedCopyOriginalAction"),
                                QStringLiteral("screenshotPinnedGroupAction"),
                                QStringLiteral("screenshotPinnedLoadContentAction"),
                                QStringLiteral("screenshotPinnedBatchActionsAction"),
                                QStringLiteral("screenshotPinnedHideAllWindowsToTopAction"),
                                QStringLiteral("screenshotPinnedCloseAllWindowsAction")})
        require(!menu->findChild<QAction*>(name), "nonwhitelisted features have no menu entry");
    Access::lock(window, true);
    source->publish(Qt::blue);
    wait([&] { return Access::original(window).pixelColor(0, 0) == QColor(Qt::blue); },
         "locked pins continue displaying source updates");
    require(window.automationState().value(QStringLiteral("locked")).toBool(),
            "live lock follows ordinary pin state");
    window.closeLiveWindow();
}

struct MenuAppearance {
    QString name;
    QString text;
    adqt::icons::IconRef icon;
    bool separator = false;
    bool checkable = false;
    QList<MenuAppearance> children;
    bool operator==(const MenuAppearance&) const = default;
};

MenuAppearance menuAppearance(adqt::widgets::AdContextMenu* menu, QAction* action) {
    MenuAppearance appearance{action->objectName(),     action->text(),
                              menu->actionIcon(action), action->isSeparator(),
                              action->isCheckable(),    {}};
    if (auto* child = qobject_cast<adqt::widgets::AdContextMenu*>(action->menu()))
        for (auto* entry : child->actions())
            appearance.children.append(menuAppearance(child, entry));
    return appearance;
}

void contextMenuMatchesOrdinaryContent() {
    auto source = std::make_shared<FakeSource>();
    ScreenshotPinnedWindow window;
    present(window, config(source));
    auto* menu = ScreenshotPinnedWindowTestAccess::contextMenu(window);
    const auto named = [](QMenu* parent, const char* name) {
        auto* action = parent->findChild<QAction*>(QString::fromLatin1(name));
        if (!action)
            std::cerr << "Missing shared menu action: " << name << '\n';
        require(action, "the shared menu exposes its named action");
        return action;
    };
    auto* process = named(menu, "screenshotPinnedProcessImageMenu");
    require(!process->icon().isNull() && process->menu() && process->menu()->actions().isEmpty(),
            "live image processing has the standard icon and populates only when opened");
    auto* management = named(menu, "screenshotPinnedWindowManagementAction");
    require(!management->icon().isNull() && management->menu() &&
                management->menu()->actions().isEmpty(),
            "live window management uses the standard icon and lazy submenu");
    auto* convert = named(menu, "screenshotPinnedConvertToImageAction");
    require(!convert->icon().isNull(), "the live-only conversion action has an image icon");
    materializePinnedMenuTree(window);
    const QList<const char*> common{
        "screenshotPinnedCopyAction",       "screenshotPinnedSaveAsFileAction",
        "screenshotPinnedProcessImageMenu", "screenshotPinnedClickThroughAction",
        "screenshotPinnedLockAction",       "screenshotPinnedCloseAction"};
    QList<MenuAppearance> before;
    for (const char* name : common) {
        auto* action = named(menu, name);
        require(!action->icon().isNull(), "shared live root actions have standard icons");
        before.append(menuAppearance(menu, action));
    }
    const auto managementIcon = menu->actionIcon(management);
    const auto managementRows = menuAppearance(menu, management).children;
    require(managementRows.size() == 3, "live management retains its supported appearance actions");

    const auto checkSeparators = [](auto&& self, QMenu* current) -> void {
        bool previousSeparator = true;
        for (auto* action : current->actions()) {
            require(!action->isSeparator() || !previousSeparator,
                    "capability filtering leaves no leading or adjacent menu separators");
            previousSeparator = action->isSeparator();
            if (action->menu())
                self(self, action->menu());
        }
        require(!previousSeparator, "capability filtering leaves no trailing menu separator");
    };
    checkSeparators(checkSeparators, menu);
    Access::opacity(window, 75);
    Access::scale(window, 125);
    require(named(menu, "screenshotPinnedOpacityReadoutAction")->text() ==
                    QStringLiteral("Current: 75%") &&
                named(menu, "screenshotPinnedScaleReadoutAction")->text() ==
                    QStringLiteral("Current: 125%"),
            "open live opacity and scale menus update their standard current-value readouts");
    require(!named(menu, "screenshotPinnedOpacityReadoutAction")->isEnabled() &&
                !named(menu, "screenshotPinnedScaleReadoutAction")->isEnabled(),
            "live current-value readouts remain informational");
    Access::opacity(window, 100);
    Access::scale(window, 100);

    convert->trigger();
    wait([&] { return !window.isLiveWindow(); }, "the menu converts the live pin in place");
    menu = ScreenshotPinnedWindowTestAccess::contextMenu(window);
    require(!menu->findChild<QAction*>(QStringLiteral("screenshotPinnedConvertToImageAction")),
            "an ordinary image has no live-only conversion entry");
    materializePinnedMenuTree(window);
    for (qsizetype index = 0; index < common.size(); ++index)
        require(menuAppearance(menu, named(menu, common[index])) == before[index],
                "shared actions retain the same labels, icons, hierarchy and separators after "
                "conversion");
    management = named(menu, "screenshotPinnedWindowManagementAction");
    const auto after = menuAppearance(menu, management);
    require(menu->actionIcon(management) == managementIcon &&
                after.children.first(3) == managementRows,
            "common management controls share the ordinary menu presentation");
    require(menu->findChild<QAction*>(QStringLiteral("screenshotPinnedDrawingAction")) &&
                menu->findChild<QAction*>(QStringLiteral("screenshotPinnedDestroyAction")) &&
                menu->findChild<QAction*>(QStringLiteral("screenshotPinnedBatchActionsAction")) &&
                ScreenshotPinnedWindowTestAccess::batchTargets(window).contains(&window),
            "conversion restores ordinary image capabilities in the shared menu");
    window.close();
}

void retiredMenuDoesNotReceiveCurrentState() {
    auto source = std::make_shared<FakeSource>();
    ScreenshotPinnedWindow window;
    present(window, config(source));
    materializePinnedMenuTree(window);
    QPointer<adqt::widgets::AdContextMenu> retired =
        ScreenshotPinnedWindowTestAccess::contextMenu(window);
    auto* oldReadout =
        retired->findChild<QAction*>(QStringLiteral("screenshotPinnedOpacityReadoutAction"));
    require(oldReadout, "the materialized live menu has an opacity readout");
    const QString oldText = oldReadout->text();
    retired->popupAt(window.mapToGlobal(QPoint(10, 10)));
    retired->hide();
    require(retired && retired->isRetiring(), "the closed popup awaits deferred deletion");
    auto* current = ScreenshotPinnedWindowTestAccess::contextMenu(window);
    require(current != retired, "reopening creates a fresh menu before the old one is deleted");
    Access::opacity(window, 75);
    require(oldReadout->text() == oldText,
            "state changes never update action references belonging to a retired popup");
    materializePinnedMenuTree(window);
    auto* readout =
        current->findChild<QAction*>(QStringLiteral("screenshotPinnedOpacityReadoutAction"));
    require(readout && readout->text() == QStringLiteral("Current: 75%"),
            "a newly opened lazy submenu reads current state");
    // Popup retirement queues deleteLater after the action dispatch has unwound.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(!retired, "the retired menu is released independently of the current popup");
    Access::opacity(window, 50);
    require(readout->text() == QStringLiteral("Current: 50%"),
            "current menu state remains bound after deletion of its predecessor");
    window.closeLiveWindow();
}

void splittingAndEditingRetainTheirLiveSource() {
    auto source = std::make_shared<FakeSource>();
    ScreenshotPinnedWindow window;
    present(window, config(source));
    Access::scale(window, 150);
    const QSize output = exportSize(window);
    ScreenshotPinnedImageSnapshot split;
    bool splitReceived = false;
    QObject::connect(&window, &ScreenshotPinnedWindow::splitRequested, &window,
                     [&](const ScreenshotPinnedImageSnapshot& value) {
                         split = value;
                         splitReceived = true;
                     });
    materializePinnedMenuTree(window);
    auto* menu = ScreenshotPinnedWindowTestAccess::contextMenu(window);
    auto* splitAction =
        menu->findChild<QAction*>(QStringLiteral("screenshotPinnedSplitImageAction"));
    require(splitAction && splitAction->isEnabled(),
            "live splitting is available in the shared menu");
    splitAction->trigger();
    source->publish(Qt::blue);
    require(splitReceived && split.source.isValid() && split.outputSize == output,
            "splitting imports the current live frame using the existing snapshot contract");
    const auto splitImage =
        image(std::make_shared<ScreenshotExportArtifact>(split.source), &window);
    require(splitImage.size() == output && splitImage.pixelColor(0, 0) == QColor(Qt::red),
            "split input retains its action-time pixels while the source continues updating");
    bool editReceived = false;
    snow_shot::presentation::ImageEditingSource edited;
    QObject::connect(&window, &ScreenshotPinnedWindow::imageEditingSnapshotRequested, &window,
                     [&](const snow_shot::presentation::ImageEditingSource& value) {
                         edited = value;
                         editReceived = true;
                     });
    auto* editAction =
        menu->findChild<QAction*>(QStringLiteral("screenshotPinnedImageEditingAction"));
    require(editAction && editAction->isEnabled(), "live editing is available in the shared menu");
    editAction->trigger();
    source->publish(Qt::green);
    wait([&] { return editReceived; }, "editing imports its current live frame asynchronously");
    require(edited.flattened && edited.pixelSize == output && edited.image.size() == output &&
                edited.image.pixelColor(0, 0) == QColor(Qt::blue) && window.isLiveWindow() &&
                window.isVisible() && source->shutdowns == 0,
            "editing receives the requested pixels and retains the original live pin");
    window.closeLiveWindow();
}

void conversionAndPersistence() {
    auto source = std::make_shared<FakeSource>();
    ScreenshotPinnedWindow window;
    snow_shot::presentation::PinnedWindowSelectionController selection;
    auto value = config(source);
    int writes = 0;
    value.persistenceWriter = [&](const snow_shot::storage::PinnedWindowRecord&) { ++writes; };
    value.selectionController = &selection;
    present(window, value);
    require(!selection.isSelectable(&window), "live pins do not join ordinary pin multiselection");
    Access::appearance(window);
    Access::scale(window, 125);
    Access::command(window, QStringLiteral("rotate_clockwise"));
    require(!Access::transform(window).isIdentity(),
            "conversion fixture commits its rotation before requesting a snapshot");
    require(Access::clickThrough(window, true), "live pins support ordinary click-through");
    Access::lock(window, true);
    Access::persist(window);
    require(writes == 0, "live pins never call the recoverable image writer");
    const auto before = window.automationState();
    const QRect beforeGeometry = window.currentNativeGeometry();
    Access::convert(window);
    wait([&] { return !window.isLiveWindow(); }, "explicit conversion produces an ordinary image");
    const auto after = window.persistenceSnapshot();
    require(
        source->shutdowns == 1 && window.currentNativeGeometry() == beforeGeometry &&
            before.value(QStringLiteral("always_on_top")).toBool() == after.alwaysOnTop &&
            before.value(QStringLiteral("show_border")).toBool() == after.showBorder &&
            before.value(QStringLiteral("show_shadow")).toBool() == after.showShadow &&
            before.value(QStringLiteral("click_through")).toBool() == after.clickThroughMode &&
            before.value(QStringLiteral("locked")).toBool() == after.lockedMode &&
            before.value(QStringLiteral("opacity_percent")).toInt() == after.opacityPercent &&
            before.value(QStringLiteral("click_through_opacity_percent")).toInt() ==
                after.clickThroughOpacityPercent,
        "conversion releases capture while preserving geometry and every window appearance state");
    require(after.imageTransform.isIdentity() && writes > 0,
            "converted pixels bake transforms once and enable standard persistence");
    Access::lock(window, false);
    require(Access::clickThrough(window, false),
            "converted image retains ordinary click-through controls");
    require(selection.isSelectable(&window), "conversion enables ordinary pin multiselection");
    require(Access::allowed(window, QStringLiteral("drawing_mode")),
            "conversion enables the full ordinary pinned-image feature set");
    window.close();
}

void conversionRestoresOrdinaryContentFeatures() {
    class Recognition final : public ScreenshotOcrRecognitionPort {
      public:
        QVector<ScreenshotOcrRequest> requests;
        RequestToken recognize(ScreenshotOcrRequest request, QObject*, Completion) override {
            requests.push_back(std::move(request));
            return static_cast<RequestToken>(requests.size());
        }
        void cancel(RequestToken) override {}
        bool reprioritize(RequestToken, ScreenshotOcrRequestPriority) override {
            return false;
        }
    } recognition;
    auto source = std::make_shared<FakeSource>();
    ScreenshotPinnedWindow window;
    auto value = config(source);
    int providerCalls = 0;
    value.recognitionProvider = [&] {
        ++providerCalls;
        return ScreenshotPinnedRecognitionProviders{&recognition, nullptr, nullptr};
    };
    present(window, value);
    require(
        providerCalls == 0 && !ScreenshotPinnedWindowTestAccess::recognition(window) &&
            Access::editButtonHidden(window),
        "live pins do not initialize ordinary recognition providers, sessions, or edit controls");
    source->publish(Qt::blue);
    wait([&] { return Access::original(window).pixelColor(0, 0) == QColor(Qt::blue); },
         "conversion fixture advances beyond its creation frame");
    Access::scale(window, 150);
    const auto output = exportSize(window);
    Access::convert(window);
    wait(
        [&] {
            return !window.isLiveWindow() &&
                   ScreenshotPinnedWindowTestAccess::recognitionReady(window) &&
                   !Access::editButtonHidden(window);
        },
        "conversion initializes ordinary recognition and restores the annotation button");
    require(
        providerCalls > 0 && recognition.requests.isEmpty() &&
            !Access::automaticRecognition(window) && Access::configuredAutomaticRecognition(window),
        "conversion retains recognition preferences without running automatic creation effects");
    Access::recognize(window);
    wait([&] { return recognition.requests.size() == 1; },
         "manual OCR remains executable after conversion");
    require(recognition.requests.front().image.size() == output &&
                recognition.requests.front().image.pixelColor(0, 0) == QColor(Qt::blue),
            "converted OCR targets the captured current frame at its converted pixel dimensions");
    window.close();
}

void configuredShortcutsRespectCapabilities() {
    using Manager = snow_shot::presentation::WindowShortcutManager;
    snow_shot::storage::PinToScreenShortcutSettings settings;
    const auto previous = settings.allShortcuts();
    auto configured = previous;
    const QStringList forbidden{QStringLiteral("destroy_window"), QStringLiteral("thumbnail_mode"),
                                QStringLiteral("hide_to_top"),
                                QStringLiteral("copy_original_content"),
                                QStringLiteral("drawing_mode")};
    constexpr auto modifiers = Qt::ControlModifier | Qt::ShiftModifier;
    configured[QStringLiteral("toggle_lock")] =
        Manager::shortcutBindingsFromKeyCombinations({QKeyCombination(modifiers, Qt::Key_F13)});
    configured[QStringLiteral("copy_to_clipboard")] =
        Manager::shortcutBindingsFromKeyCombinations({QKeyCombination(modifiers, Qt::Key_F19)});
    for (int i = 0; i < forbidden.size(); ++i)
        configured[forbidden[i]] = Manager::shortcutBindingsFromKeyCombinations(
            {QKeyCombination(modifiers, static_cast<Qt::Key>(Qt::Key_F14 + i))});
    require(settings.setAllShortcutsAtomic(configured),
            "configure real live pin shortcut bindings");
    const auto restore = qScopeGuard([&] {
        require(settings.setAllShortcutsAtomic(previous),
                "restore live test shortcut configuration");
    });
    class Clipboard final : public ScreenshotPinnedClipboard {
      public:
        int writes = 0;
        QMimeData sentinel;
        const QMimeData* mimeData() const override {
            return &sentinel;
        }
        std::optional<ScreenshotClipboardContentSnapshot> snapshot(qreal) override {
            return {};
        }
        void setMimeData(std::unique_ptr<QMimeData>) override {
            ++writes;
        }
    };
    auto source = std::make_shared<FakeSource>();
    ScreenshotPinnedWindow window;
    auto clipboard = std::make_unique<Clipboard>();
    auto* recorded = clipboard.get();
    ScreenshotPinnedWindowTestAccess::setClipboard(window, std::move(clipboard));
    present(window, config(source));
    const auto key = [&](Qt::Key value) {
        PhysicalKeyEvent pressed(QEvent::KeyPress, value, modifiers);
        QCoreApplication::sendEvent(&window, &pressed);
        PhysicalKeyEvent released(QEvent::KeyRelease, value, modifiers);
        QCoreApplication::sendEvent(&window, &released);
    };
    key(Qt::Key_F13);
    require(window.automationState().value(QStringLiteral("locked")).toBool(),
            "an allowed configured shortcut executes through the actual keyboard dispatcher");
    key(Qt::Key_F13);
    require(!window.automationState().value(QStringLiteral("locked")).toBool(),
            "allowed lock shortcut toggles normally in live mode");
    for (int i = 0; i < forbidden.size(); ++i) {
        key(static_cast<Qt::Key>(Qt::Key_F14 + i));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const auto state = window.automationState();
        require(window.isVisible() && window.isLiveWindow() && source->shutdowns == 0 &&
                    !state.value(QStringLiteral("thumbnail")).toBool() &&
                    !state.value(QStringLiteral("hide_to_top")).toBool() &&
                    !state.value(QStringLiteral("editing")).toBool() && recorded->writes == 0,
                "configured forbidden shortcuts cannot close, thumbnail, hide, copy original, or "
                "edit live pins");
    }
    source->setState(State::Minimized);
    const int requests = source->requests;
    key(Qt::Key_F19);
    require(source->requests == requests, "minimized source disables configured Copy shortcuts");
    key(Qt::Key_F13);
    require(window.automationState().value(QStringLiteral("locked")).toBool(),
            "window commands remain executable by shortcut while the source is minimized");
    window.closeLiveWindow();
}

void conversionRejectsObsoleteFrames() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        auto source = std::make_shared<FakeSource>();
        ScreenshotPinnedWindow window;
        present(window, config(source));
        source->deferFrames = true;
        Access::convert(window);
        require(Access::snapshotPending(window) && source->pending.size() == 1,
                "conversion fixture holds one asynchronous action-time frame");
        if (scenario == 0)
            Access::command(window, QStringLiteral("rotate_clockwise"));
        else if (scenario == 1)
            Access::scale(window, 150);
        else
            source->setState(State::Minimized);
        source->deliver();
        wait([&] { return !Access::snapshotPending(window); },
             "obsolete conversion snapshot must finish without replacing current live state");
        require(
            window.isLiveWindow() && source->shutdowns == 0,
            "late conversion frames cannot replace newer geometry, transforms, or source status");
        window.closeLiveWindow();
    }
}

void groupLifecycle() {
    using snow_shot::presentation::PinnedWindowGroupManager;
    for (int scenario = 0; scenario < 3; ++scenario) {
        PinnedWindowGroupManager groups;
        const auto originalGroup = groups.activeGroupId();
        const auto other = groups.createGroup(QStringLiteral("Live fixture %1").arg(scenario));
        require(other.has_value(), "create unrelated live pin fixture group");
        auto source = std::make_shared<FakeSource>();
        ScreenshotPinnedWindow window;
        auto value = config(source);
        value.groupManager = &groups;
        value.groupId = originalGroup;
        if (scenario == 2) {
            require(groups.setActiveGroup(*other), "activate live deletion fixture group");
            value.groupId = *other;
        }
        int records = 0;
        value.persistenceWriter = [&](const snow_shot::storage::PinnedWindowRecord&) { ++records; };
        present(window, value);
        const auto unrelated = groups.createGroup(QStringLiteral("Unrelated %1").arg(scenario));
        require(unrelated && window.isVisible() && source->shutdowns == 0,
                "creating unrelated groups leaves live pins running");
        if (scenario == 0)
            require(groups.setActiveGroup(*other), "switch away from the live pin group");
        else if (scenario == 1)
            require(groups.moveWindow(&window, *other), "moving a live pin closes it");
        else
            require(groups.deleteSpecifiedGroup(*other), "delete the live pin group");
        wait([&] { return !window.isVisible(); }, "group lifecycle closes a live pin");
        require(source->shutdowns == 1 && records == 0,
                "group lifecycle releases capture without producing a recoverable record");
        require(groups.setActiveGroup(originalGroup), "restore fixture active group");
        require(groups.deleteEmptyGroups(), "remove empty live fixture groups");
    }
}

void productionCreationUsesPlainBackdrop() {
    auto source = std::make_shared<FakeSource>();
    ScreenshotSelectionExportUiServices services;
    bool completed = false;
    bool succeeded = false;
    require(services.presentLiveWindow(source, QGuiApplication::primaryScreen(), source->region,
                                       source->region.size(),
                                       [&](bool success, QImage) {
                                           succeeded = success;
                                           completed = true;
                                       }),
            "the production live pin creation service must accept an initialized source");
    wait([&] { return completed; }, "production live pin creation must finish");
    require(succeeded && !source->presentations.isEmpty(),
            "production live pin creation must bind its native presentation owner");
    auto* window = qobject_cast<ScreenshotPinnedWindow*>(source->presentations.back().owner);
    require(window && window->isLiveWindow() && !Access::checkerboard(*window),
            "blank live crop pixels must use the same plain backdrop as native DWM display");
    source->publish(Qt::transparent);
    wait([&] { return Access::original(*window).pixelColor(0, 0).alpha() == 0; },
         "the live crop may become fully blank without changing its backdrop policy");
    require(!Access::checkerboard(*window), "blank live frames must not enable a checkerboard");
    Access::convert(*window);
    wait([&] { return !window->isLiveWindow(); }, "a blank but available live frame may convert");
    require(!Access::checkerboard(*window), "conversion must preserve the plain live backdrop");
    window->close();
}

void liveBackdropFollowsThemeChanges() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalTheme = themes.config();
    const auto restoreTheme = qScopeGuard([&] { themes.setConfig(originalTheme); });
    themes.setColorScheme(adqt::theme::ThemeScheme::Light);
    auto source = std::make_shared<FakeSource>();
    source->frame.fill(Qt::transparent);
    ScreenshotPinnedWindow window;
    present(window, config(source));
    QCoreApplication::processEvents();
    const QRect geometry = window.currentNativeGeometry();
    QColor previous = source->presentations.back().backgroundColor;
    for (const auto scheme : {adqt::theme::ThemeScheme::Dark, adqt::theme::ThemeScheme::Light}) {
        const auto before = source->presentations.size();
        themes.setColorScheme(scheme);
        QColor expected = themes.resolveTheme(&window).colorBgContainer;
        expected.setAlpha(255);
        require(expected != previous, "the live backdrop fixture changes its resolved theme color");
        wait(
            [&] {
                return source->presentations.size() > before &&
                       source->presentations.back().backgroundColor == expected;
            },
            "blank native crop regions receive the current resolved theme background");
        require(window.currentNativeGeometry() == geometry && window.isLiveWindow() &&
                    source->shutdowns == 0,
                "theme updates preserve live content, placement, and capture ownership");
        previous = expected;
    }
    window.closeLiveWindow();
}

void presentationFollowsOwnerGeometryAndLifetime() {
    auto source = std::make_shared<FakeSource>();
    Presentation retained;
    {
        ScreenshotPinnedWindow window;
        present(window, config(source));
        QCoreApplication::processEvents();
        require(!source->presentations.isEmpty(), "the live pin publishes its presentation");
        retained = source->presentations.back();
        const auto content = [&] {
            return window.currentNativeGeometry().marginsRemoved(retained.nativeContentMargins);
        };
        require(content() == Access::contentGeometry(window),
                "presentation margins describe the pin's native content, including decoration");
        const QSize before = content().size();
        Access::scale(window, 150);
        QCoreApplication::processEvents();
        require(content().size() != before && content() == Access::contentGeometry(window),
                "a retained presentation follows resized content without stale desktop bounds");
        Access::opacity(window, 80);
        QCoreApplication::processEvents();
        require(source->presentations.back().nativeContentMargins == retained.nativeContentMargins,
                "appearance refreshes preserve the owner-relative placement contract");
        window.closeLiveWindow();
    }
    require(retained.owner.isNull(),
            "retained presentations cannot keep a dangling pointer to a destroyed pin");
}

void nativePresentationPreservesShell() {
    auto source = std::make_shared<FakeSource>();
    source->region = QRect(80, 90, 480, 400);
    ScreenshotPinnedWindow window;
    auto value = config(source);
    value.initialBorderVisible = true;
    value.initialShadowVisible = true;
    present(window, value);
    PaintEventCounter paints(window);
    auto* border = window.findChild<QWidget*>(QStringLiteral("screenshotPinnedBorder"));
    require(border, "live pin has its ordinary decoration layer");
    for (const bool native : {false, true, false, true}) {
        source->setNativePresentation(native);
        QCoreApplication::processEvents();
        require(Access::contentGeometry(window) == source->region,
                "switching live presentation preserves the selected desktop content rectangle");
        QImage raster((QSizeF(window.size()) * window.devicePixelRatioF()).toSize(),
                      QImage::Format_ARGB32_Premultiplied);
        raster.setDevicePixelRatio(window.devicePixelRatioF());
        raster.fill(Qt::transparent);
        window.render(&raster);
        const int margin = qRound(snow_shot::presentation::pinnedShadowMargin() *
                                  (snow_shot::presentation::kPinnedGeometryUnits ==
                                           snow_shot::storage::PinnedGeometryUnits::LogicalPixels
                                       ? window.devicePixelRatioF()
                                       : 1));
        requireColorNear(raster.pixelColor(margin, raster.height() / 2),
                         border->property("borderColor").value<QColor>(), 0,
                         "native live content retains the shell's visible border");
        if (!native)
            continue;
        const QRect geometry = window.geometry();
        const auto render = [&] {
            raster.fill(Qt::transparent);
            window.render(&raster);
        };
        const auto pixel = [&](QPoint point) {
            return raster.pixelColor((QPointF(point) * window.devicePixelRatioF()).toPoint());
        };
        ScreenshotPinnedWindowTestAccess::observePointerOffscreen(window, true);
        auto* close = window.findChild<QWidget*>(QStringLiteral("screenshotPinnedCloseButton"));
        require(close && close->isVisible(), "native live pins show their hover controls");
        render();
        require(pixel(close->mapTo(&window, close->rect().center())).alpha() > 0,
                "native live pins paint controls above their transparent content hole");
        const QRect content = Access::contentView(window);
        const QPoint start = content.topLeft() + QPoint(30, 50);
        const QPoint end = content.center();
        const QPoint sample = (start + end) / 2;
        require(pixel(sample).alpha() == 0, "native content starts with a transparent Qt shell");
        const int requests = source->requests;
        const int paintsBeforeDrag = paints.count();
        QMouseEvent press(QEvent::MouseButtonPress, start, window.mapToGlobal(start),
                          Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &press);
        QMouseEvent move(QEvent::MouseMove, end, window.mapToGlobal(end), Qt::NoButton,
                         Qt::RightButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &move);
        QCoreApplication::processEvents();
        require(paints.count() > paintsBeforeDrag,
                "dragging a native crop schedules a repaint of the visible shell");
        render();
        require(pixel(sample).alpha() == 0x33 && window.geometry() == geometry,
                "native live pins paint the crop preview without moving their shell");
        const QPoint smallerEnd = start + (end - start) / 3;
        const QPoint smallerSample = (start + smallerEnd) / 2;
        QMouseEvent shrink(QEvent::MouseMove, smallerEnd, window.mapToGlobal(smallerEnd),
                           Qt::NoButton, Qt::RightButton, Qt::NoModifier);
        const int paintsBeforeShrink = paints.count();
        QApplication::sendEvent(&window, &shrink);
        QCoreApplication::processEvents();
        require(paints.count() > paintsBeforeShrink,
                "shrinking a native crop schedules a repaint of its old and new selection");
        render();
        require(pixel(sample).alpha() == 0 && pixel(smallerSample).alpha() == 0x33,
                "shrinking a native crop clears its old preview and paints its new bounds");
        source->setNativePresentation(false);
        QCoreApplication::processEvents();
        render();
        require(pixel(smallerSample).alpha() == 255 && pixel(smallerSample).blue() > 0,
                "an active crop preview follows a switch to raster presentation");
        source->setNativePresentation(true);
        QCoreApplication::processEvents();
        render();
        require(pixel(sample).alpha() == 0 && pixel(smallerSample).alpha() == 0x33,
                "an active crop preview returns to the native shell without duplicate painting");
        QKeyEvent cancel(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        const int paintsBeforeCancel = paints.count();
        QApplication::sendEvent(&window, &cancel);
        QCoreApplication::processEvents();
        require(paints.count() > paintsBeforeCancel,
                "cancelling a native crop schedules a repaint to erase its preview");
        render();
        require(pixel(smallerSample).alpha() == 0 && source->requests == requests,
                "cancelling a native live crop restores the transparent content hole");
    }
    window.closeLiveWindow();
}
void batchAndShakeRespectLiveCapabilities() {
    IsolatedPinnedStorage isolated;
    using PinAccess = ScreenshotPinnedWindowTestAccess;
    const snow_shot::storage::PinToScreenSettings settings;
    const bool previousShake = settings.shakeWindowToHideOthers();
    const auto restore =
        qScopeGuard([&] { static_cast<void>(settings.setShakeWindowToHideOthers(previousShake)); });
    require(settings.setShakeWindowToHideOthers(true), "enable the mixed live-pin shake fixture");

    auto source = std::make_shared<FakeSource>();
    ScreenshotPinnedWindow live;
    present(live, config(source));
    ScreenshotPinnedWindow ordinary;
    ScreenshotPinnedWindow peer;
    auto ordinaryConfig = config(source);
    ordinaryConfig.liveSource.reset();
    ordinaryConfig.enableEditing = false;
    ordinaryConfig.automaticTextRecognition = false;
    for (auto* window : {&ordinary, &peer}) {
        window->setAttribute(Qt::WA_DeleteOnClose, false);
        require(window->present(ordinaryConfig), "present an ordinary pin beside the live pin");
        wait([&] { return PinAccess::shakeContentReady(*window); },
             "ordinary pins publish their first content frame");
    }
    const auto targets = PinAccess::batchTargets(ordinary);
    require(targets.size() == 2 && targets.contains(&ordinary) && targets.contains(&peer) &&
                !targets.contains(&live),
            "ordinary batch actions exclude restricted live pins");
    materializePinnedMenuTree(ordinary);
    auto* menu = PinAccess::contextMenu(ordinary);
    auto* lock = menu->findChild<QAction*>(QStringLiteral("screenshotPinnedBatchEnableLockAction"));
    auto* unlock =
        menu->findChild<QAction*>(QStringLiteral("screenshotPinnedBatchDisableLockAction"));
    require(lock && unlock, "ordinary pins retain the batch mode actions");
    lock->trigger();
    require(ordinary.automationState().value(QStringLiteral("locked")).toBool() &&
                peer.automationState().value(QStringLiteral("locked")).toBool() &&
                !live.automationState().value(QStringLiteral("locked")).toBool(),
            "batch mode changes preserve live-pin state");
    unlock->trigger();

    require(PinAccess::beginShakeDrag(live), "live pins retain ordinary window dragging");
    require(!PinAccess::shakeArmed(live),
            "restricted live pins do not initiate group hide gestures");
    PinAccess::finishShakeDrag(live);
    require(PinAccess::beginShakeDrag(ordinary), "begin a shake with a live peer present");
    for (int leg = 1; leg <= 6; ++leg)
        PinAccess::sampleShake(ordinary, QPointF((leg % 2) ? 64 : 0, 0), leg * 80);
    PinAccess::finishShakeDrag(ordinary);
    wait([&] { return !peer.isVisible(); }, "shaking an ordinary pin hides ordinary peers");
    require(PinAccess::shakeMessage(ordinary) && live.isVisible() && source->shutdowns == 0,
            "a live peer neither fails the hide gesture nor loses its capture session");
    live.closeLiveWindow();
    ordinary.close();
}
} // namespace live_pinned_tests

void pinnedLiveWindowTests() {
    live_pinned_tests::liveFramesPaintTransforms();
    live_pinned_tests::rotatedLiveSaveOpensDialog();
    live_pinned_tests::liveFramesReplacePixelsWithoutEditingSetup();
    live_pinned_tests::nativePresentationPreservesShell();
    live_pinned_tests::contextMenuMatchesOrdinaryContent();
    live_pinned_tests::retiredMenuDoesNotReceiveCurrentState();
    live_pinned_tests::presentationFollowsOwnerGeometryAndLifetime();
    live_pinned_tests::frameSnapshotsAndTransforms();
    live_pinned_tests::deferredCopyRetainsItsActionTimeFrame();
    live_pinned_tests::stoppedSourceClosesOnlyItsPin();
    live_pinned_tests::statusAndCapabilityGates();
    live_pinned_tests::statusFitsConstrainedContent();
    live_pinned_tests::statusLayoutFollowsContent();
    live_pinned_tests::splittingAndEditingRetainTheirLiveSource();
    live_pinned_tests::conversionAndPersistence();
    live_pinned_tests::conversionRestoresOrdinaryContentFeatures();
    live_pinned_tests::configuredShortcutsRespectCapabilities();
    live_pinned_tests::conversionRejectsObsoleteFrames();
    live_pinned_tests::groupLifecycle();
    live_pinned_tests::batchAndShakeRespectLiveCapabilities();
    live_pinned_tests::productionCreationUsesPlainBackdrop();
    live_pinned_tests::liveBackdropFollowsThemeChanges();
}
