#include "ui/viewer_window.h"
#include "render/rhi_image_window.h"
#include "ui/edit_size_format_window.h"
#include "widgets/button.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace snow::image_viewer {
class ViewerWindowTestAccess {
  public:
    static void prepare(ViewerWindow& window, const QString& path) {
        window.currentImage_.filePath = path;
        window.currentImage_.sourceSize = QSize(8, 8);
        window.currentImage_.pixels = QImage(8, 8, QImage::Format_RGBA8888);
        window.currentImage_.pixels.fill(Qt::white);
        window.performanceOptions_.forceCpu = true;
        window.performanceTestActive_ = true;
        window.performanceScenarioPhase_ = 2;
        window.performanceRequestId_ = 42;
        window.performanceTimer_.start();
        window.updateImageControls();
    }

    static bool hasEditor(const ViewerWindow& window) {
        return window.editingActive_ && window.editWindow_ && window.editSession_;
    }

    static void emitMetrics(ViewerWindow& window, quint64 requestId = 42) {
        window.rhiWindow_->editPerformanceStageCompleted(requestId,
                                                         QStringLiteral("test.editor_stage"), 123);
        window.rhiWindow_->editResizeResourceCacheResult(requestId, true);
        window.rhiWindow_->editResizeResourceCacheResult(requestId, false);
    }

    static qsizetype stageCount(const ViewerWindow& window,
                                const QString& stage = QStringLiteral("test.editor_stage")) {
        return window.performanceTimings_.value(stage).size();
    }

    static void emitVisualFrame(ViewerWindow& window, quint64 requestId) {
        window.rhiWindow_->editVisualFrameSubmitted(requestId);
    }

    static int hitCount(const ViewerWindow& window) {
        return window.performanceResourceCacheHits_;
    }

    static int missCount(const ViewerWindow& window) {
        return window.performanceResourceCacheMisses_;
    }

    static void setProfiling(ViewerWindow& window, bool enabled) {
        window.performanceTestActive_ = enabled;
    }
};
} // namespace snow::image_viewer

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::_Exit(1);
    }
}

snow::image_viewer::EditSizeFormatWindow* visibleEditor() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* editor = qobject_cast<snow::image_viewer::EditSizeFormatWindow*>(widget);
            editor && editor->isVisible()) {
            return editor;
        }
    }
    return nullptr;
}

void editorMetricsStayBoundedAcrossSessions(const QString& path) {
    using snow::image_viewer::ViewerWindow;
    using snow::image_viewer::ViewerWindowTestAccess;

    ViewerWindow window;
    ViewerWindowTestAccess::prepare(window, path);
    auto* editButton =
        window.findChild<adqt::widgets::AdButton*>(QStringLiteral("editSizeFormatButton"));
    require(editButton && editButton->isEnabled(), "valid image enables the editor button");
    const auto requireCounts = [&](int expected) {
        require(ViewerWindowTestAccess::stageCount(window) == expected,
                "each active editor GPU stage must be recorded exactly once");
        require(ViewerWindowTestAccess::hitCount(window) == expected &&
                    ViewerWindowTestAccess::missCount(window) == expected,
                "each active editor cache result must be counted exactly once");
    };

    ViewerWindowTestAccess::emitMetrics(window);
    requireCounts(0);
    constexpr int sessionCount = 24;
    for (int session = 0; session < sessionCount; ++session) {
        editButton->click();
        require(ViewerWindowTestAccess::hasEditor(window), "open a valid image editor session");
        QPointer<snow::image_viewer::EditSizeFormatWindow> editor = visibleEditor();
        require(editor != nullptr, "editor session has its top-level window");
        const auto sessions = window.findChildren<snow::image_viewer::EditPipelineController*>(
            QString(), Qt::FindDirectChildrenOnly);
        require(sessions.size() == 1, "viewer owns only the current editor pipeline");
        QPointer<snow::image_viewer::EditPipelineController> pipeline = sessions.front();
        ViewerWindowTestAccess::emitMetrics(window);
        requireCounts(session + 1);

        ViewerWindowTestAccess::emitMetrics(window, 43);
        requireCounts(session + 1);
        ViewerWindowTestAccess::setProfiling(window, false);
        ViewerWindowTestAccess::emitMetrics(window);
        requireCounts(session + 1);
        ViewerWindowTestAccess::setProfiling(window, true);

        editor->close();
        require(!ViewerWindowTestAccess::hasEditor(window), "close the image editor session");
        if (session + 1 == sessionCount) {
            // Closing uses deferred deletion. Metrics must stop immediately, even
            // while the old editor objects are still waiting for the event loop.
            ViewerWindowTestAccess::emitMetrics(window);
            requireCounts(session + 1);
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(editor == nullptr, "closed editor releases its window");
        require(pipeline == nullptr, "closed editor releases its pipeline");
    }
    ViewerWindowTestAccess::emitMetrics(window);
    requireCounts(sessionCount);
    ViewerWindowTestAccess::setProfiling(window, false);
}

void closedPipelineIgnoresReopenedEditorFrames(const QString& path) {
    using snow::image_viewer::EditPipelineController;
    using snow::image_viewer::ViewerWindow;
    using snow::image_viewer::ViewerWindowTestAccess;

    ViewerWindow window;
    ViewerWindowTestAccess::prepare(window, path);
    auto* editButton =
        window.findChild<adqt::widgets::AdButton*>(QStringLiteral("editSizeFormatButton"));
    require(editButton && editButton->isEnabled(), "valid image enables the editor button");
    editButton->click();
    const auto initialPipelines =
        window.findChildren<EditPipelineController*>(QString(), Qt::FindDirectChildrenOnly);
    require(initialPipelines.size() == 1, "first editor owns one pipeline");
    QPointer<EditPipelineController> closedPipeline = initialPipelines.front();
    closedPipeline->setGpuSource(path);
    QPointer<snow::image_viewer::EditSizeFormatWindow> closedEditor = visibleEditor();
    require(closedEditor != nullptr, "first editor window is visible");
    closedEditor->close();
    require(!ViewerWindowTestAccess::hasEditor(window) && closedPipeline,
            "closing ends the session before deferred pipeline deletion");

    editButton->click();
    auto* currentEditor = visibleEditor();
    require(currentEditor && currentEditor != closedEditor,
            "reopen creates a new editor before the closed one is deleted");
    const auto pipelines =
        window.findChildren<EditPipelineController*>(QString(), Qt::FindDirectChildrenOnly);
    require(pipelines.size() == 2, "deferred deletion leaves both pipelines temporarily alive");
    auto* currentPipeline = pipelines.back();
    require(currentPipeline != closedPipeline, "reopened editor owns a different pipeline");
    currentPipeline->setGpuSource(path);
    const auto requestId = currentPipeline->requestEdit(currentEditor->settings());
    require(requestId != 0 && requestId == closedPipeline->latestRequestId(),
            "independent editor sessions can reuse the same request id");

    QObject observations;
    int closedFrames = 0;
    int currentFrames = 0;
    QObject::connect(closedPipeline, &EditPipelineController::visualReady, &observations,
                     [&](quint64) { ++closedFrames; });
    QObject::connect(currentPipeline, &EditPipelineController::visualReady, &observations,
                     [&](quint64) { ++currentFrames; });
    ViewerWindowTestAccess::emitVisualFrame(window, requestId);
    require(closedFrames == 0, "closed pipeline must not consume the reopened editor's GPU frame");
    require(currentFrames == 1, "current pipeline consumes its GPU frame exactly once");
    require(ViewerWindowTestAccess::stageCount(window,
                                               QStringLiteral("edit.request_to_visual_frame")) == 1,
            "reopened editor records one completion while the closed pipeline still exists");
    ViewerWindowTestAccess::setProfiling(window, false);
    currentEditor->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(closedPipeline == nullptr && closedEditor == nullptr,
            "deferred deletion reclaims the closed editor and pipeline");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "create editor lifecycle test directory");
    QApplication::setOrganizationName(QStringLiteral("Snow Image Viewer Tests"));
    QApplication::setApplicationName(QStringLiteral("editor-lifecycle"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    // A viewer document remains valid when its source disappears. This keeps
    // lifecycle coverage independent of codec workers and GPU availability.
    const QString path = directory.filePath(QStringLiteral("removed.png"));
    if (app.arguments().contains(QStringLiteral("--reopen-only")))
        closedPipelineIgnoresReopenedEditorFrames(path);
    else
        editorMetricsStayBoundedAcrossSessions(path);
    std::cout << "snow_image_viewer editor lifecycle tests passed\n";
    return 0;
}
