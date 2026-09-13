#include "snow_shot/presentation/screenshothistoryimageeditor.h"

#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/storage/capturehistoryrepository.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace {
struct HistoryImageLoad {
    ScreenshotExportJobHandle job;
    bool active = true;
};
} // namespace

ScreenshotPinnedWindow*
openScreenshotHistoryImageEditor(snow_shot::storage::CaptureHistoryRepository& repository,
                                 const QString& recordId, QScreen* screen, QObject* lifetime,
                                 std::function<void(const QString&)> reportFailure) {
    const QPointer<QObject> guardedLifetime(lifetime);
    const auto fail = [guardedLifetime, reportFailure] {
        if (guardedLifetime != nullptr && reportFailure) {
            reportFailure(QCoreApplication::translate("ScreenshotHistoryImageEditor",
                                                      "The saved screenshot could not be opened"));
        }
    };
    const auto records = repository.records();
    const auto found =
        std::find_if(records.cbegin(), records.cend(),
                     [&recordId](const auto& record) { return record.id == recordId; });
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (found == records.cend() || !found->result || screen == nullptr || lifetime == nullptr) {
        fail();
        return nullptr;
    }
    const auto record = *found;
    const QSize imageSize = record.result->imageSize;
    const auto fit = ScreenshotGeometryMapper::fitImageToAvailableGeometry(
        imageSize, screen->availableGeometry(), screen->geometry(),
        ScreenshotGeometryMapper::physicalRectForScreen(*screen), 64);
    if (!fit.valid) {
        fail();
        return nullptr;
    }
    ScreenshotPinnedWindow::Config config;
    config.screen = screen;
    config.nativeGeometry = fit.nativeGeometry;
    config.canvasSourceRect = QRectF(QPointF(), imageSize);
    config.fullResolutionScaleBasis = imageSize;
    config.automaticTextRecognition = false;
    const auto load = std::make_shared<HistoryImageLoad>();
    config.imageLoader = [&repository, record, fail, load](QObject* receiver,
                                                           ScreenshotImageLoadCallback callback) {
        auto completion = [callback, fail, load](ScreenshotExportTaskResult result) {
            if (!load->active) {
                return;
            }
            if (result.image.isNull()) {
                fail();
            }
            callback(std::move(result.image));
        };
        load->job = ScreenshotExportCoordinator::shared().submit(
            receiver, ScreenshotExportCoordinator::Priority::Foreground,
            [&repository, record](const ScreenshotExportCancellation& cancellation) {
                ScreenshotExportTaskResult result;
                if (!cancellation.isCancellationRequested()) {
                    result.image = repository.loadResultImage(record).value_or(QImage());
                }
                return result;
            },
            std::move(completion));
        if (!load->job.isValid()) {
            fail();
            callback({});
        }
    };
    auto* window = new ScreenshotPinnedWindow();
    window->setObjectName(QStringLiteral("screenshotHistoryImageEditor"));
    window->setWindowTitle(
        QCoreApplication::translate("ScreenshotHistoryImageEditor", "Edit screenshot"));
    QObject::connect(lifetime, &QObject::destroyed, window, &QWidget::close);
    QObject::connect(window, &ScreenshotPinnedWindow::closingForPersistence, window, [load] {
        load->active = false;
        load->job.cancel();
    });
    QObject::connect(window, &QObject::destroyed, [load] {
        load->active = false;
        load->job.cancel();
    });
    const QPointer<ScreenshotPinnedWindow> guarded(window);
    if (!window->present(config, [guarded](bool succeeded, QImage) {
            if (succeeded && guarded != nullptr) {
                // Leave the first-frame paint callback before showing its editing toolbar.
                QTimer::singleShot(0, guarded, [guarded] {
                    if (guarded != nullptr) {
                        guarded->setEditMode(true);
                    }
                });
            }
        })) {
        window->deleteLater();
        fail();
        return nullptr;
    }
    return window;
}
