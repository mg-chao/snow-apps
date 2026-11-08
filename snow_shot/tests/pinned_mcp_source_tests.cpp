// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/app/applicationcontroller.h"
#include "snow_shot/app/mcp/mcpdocumentservice.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionbackground.h"
#include "snow_shot/presentation/screenshotselectioncomposition.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snowimageqtcodec.h"
#include "widgets/platform_compatibility.h"

#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPointer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <cstdlib>
#include <iostream>

int runPinnedMcpSourceTests(int argc, char** argv);

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        // A failed assertion cannot enter static storage shutdown while this
        // stack still owns the application's worker threads and controllers.
        std::quick_exit(EXIT_FAILURE);
    }
}

template <typename Predicate> void await(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), "pinned source request completes");
}

QImage comparable(QImage image) {
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    image.setColorSpace(QColorSpace::SRgb);
    image.setDevicePixelRatio(1);
    return image;
}

void fractionalSelectionProjection() {
    ScreenshotResultStyle style;
    style.cornerRadius = 3;
    style.regionScale = 1.25;
    style.region = QRegion(QRect(0, 0, 32, 24)).subtracted(QRect(10, 8, 12, 8));
    style.backgroundOpacityPercent = 50;
    const QRectF content(-10.25, 5.5, 40, 30);
    const auto params = pinnedStyleToContentSelection(style, content);
    require(params.region && params.selection == content.toAlignedRect() && params.radius == 0 &&
                params.backgroundOpacityPercent == 50,
            "fractional projection captures exact geometry and appearance");
    const auto expected =
        screenshotSelectionPath(content, style.region, style.cornerRadius, style.regionScale);
    const auto restored = params.resultStyle();
    const auto actual = screenshotSelectionPath(QRectF(params.selection), restored.region,
                                                restored.cornerRadius, restored.regionScale);
    require(actual == expected, "fractional origin and scale apply exactly once");
    style.region.reset();
    const auto rectangle = pinnedStyleToContentSelection(style, content);
    require(rectangle.region && rectangle.radius == 0 &&
                rectangle.region->path() ==
                    screenshotSelectionPath(content, {}, style.cornerRadius, 1),
            "fractional ordinary rectangles preserve their rounded outline");
}
} // namespace

// Uses the production document service and its ApplicationController source port.
int runPinnedMcpSourceTests(int argc, char** argv) {
    using namespace snow_shot;
    using namespace snow_shot::app::mcp;
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated pinned source storage");
    qputenv("SNOW_SHOT_MCP_DESCRIPTOR", temporary.filePath("descriptor.json").toUtf8());
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    adqt::widgets::initializePlatformCompatibility(application);
    auto& store = storage::ApplicationStorage::instance();
    require(store.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize pinned source storage");
    require(store.configuration().setValues(
                {{QStringLiteral("mcp/enabled"), true}, {QStringLiteral("tray/enabled"), false}}),
            "enable the isolated production MCP service");
    require(storage::PinToScreenSettings().setAutomaticTextRecognition(false) &&
                storage::PinToScreenSettings().setConfirmBeforeClosingWindow(false),
            "source fixture disables recognition and close confirmation");
    presentation::LanguageManager::instance().initialize();
    presentation::styles::ThemeManager::instance().initialize(application);
    fractionalSelectionProjection();
    {
        app::ApplicationController controller(application);
        auto* documents = controller.findChild<McpDocumentService*>();
        require(documents, "application owns its production document source adapter");
        int sequence = 0;
        QString caseDescription;
        const auto call = [&](const QString& method, QJsonObject params,
                              std::optional<quint64> revision = {}) {
            ScreenshotMcpRequest request;
            request.connectionId = 1;
            request.requestId = QString::number(++sequence);
            request.idempotencyKey = request.requestId;
            request.method = method;
            request.params = std::move(params);
            request.expectedRevision = revision;
            std::optional<ScreenshotMcpResponse> response;
            documents->request(request, [&](auto value) { response = std::move(value); });
            await([&] { return response.has_value(); });
            if (!response->ok) {
                std::cerr
                    << "case=" << caseDescription.toStdString()
                    << " method=" << method.toStdString() << " params="
                    << QJsonDocument(request.params).toJson(QJsonDocument::Compact).constData()
                    << " revision=" << request.expectedRevision.value_or(0)
                    << " error=" << response->errorCode.toStdString() << " details="
                    << QJsonDocument(response->errorDetails)
                           .toJson(QJsonDocument::Compact)
                           .constData()
                    << " message=" << response->errorMessage.toStdString() << '\n';
            }
            require(response->ok, "production pinned document operation succeeds");
            return std::move(*response);
        };
        for (const auto origin : {QPoint(1000, 500), QPoint(-1000, -500)}) {
            for (const qreal scale : {1.0, 1.25, 1.75, 2.0}) {
                for (const int opacity : {0, 50, 100}) {
                    caseDescription = QStringLiteral("origin=(%1,%2),scale=%3,opacity=%4")
                                          .arg(origin.x())
                                          .arg(origin.y())
                                          .arg(scale)
                                          .arg(opacity);
                    ScreenshotResultStyle style;
                    style.region = QRegion(QRect(0, 0, 32, 24)).subtracted(QRect(10, 8, 12, 8));
                    style.regionScale = scale;
                    style.cornerRadius = 3;
                    style.backgroundOpacityPercent = opacity;
                    style.borderType = opacity == 50 ? ScreenshotSelectionBorderType::Border
                                                     : ScreenshotSelectionBorderType::Shadow;
                    style.borderWidth = style.shadowWidth = 2;
                    style.borderColor = Qt::blue;
                    if (opacity == 50) {
                        QPainterPath outline;
                        outline.setFillRule(Qt::OddEvenFill);
                        outline.addRect(QRectF(0, 0, 32, 24));
                        outline.addEllipse(QRectF(10, 8, 12, 8));
                        style.region = ScreenshotRegionGeometry::fromPath(
                            outline, ScreenshotRegionType::Curve);
                    }
                    const QSize pixels(qRound(32 * scale), qRound(24 * scale));
                    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
                    image.fill(QColor(30, 170, 80));
                    image.setColorSpace(QColorSpace::SRgb);
                    const QRectF content(origin, pixels);
                    storage::PinnedWindowRecord record;
                    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    record.image = image;
                    record.canvasSourceRect = record.contentCanvasRect = content;
                    record.surfaceCanvasRect = content.adjusted(-8, -8, 8, 8);
                    record.nativeGeometry = QRect(10, 10, pixels.width(), pixels.height());
                    record.initialWindowSize = pixels;
                    record.resultStyle = encodeScreenshotResultStyle(style);
                    const bool annotatedCase =
                        origin == QPoint(1000, 500) && scale == 1.25 && opacity == 50;
                    std::unique_ptr<SnowCanvasRuntime> annotations;
                    if (annotatedCase) {
                        annotations = std::make_unique<SnowCanvasRuntime>();
                        const QJsonObject operation{
                            {QStringLiteral("type"), QStringLiteral("rectangle")},
                            {QStringLiteral("bounds"),
                             QJsonArray{origin.x() + 4, origin.y() + 4, 8, 8}}};
                        const QJsonObject transaction{
                            {QStringLiteral("version"), 1},
                            {QStringLiteral("operations"), QJsonArray{operation}}};
                        require(
                            !annotations
                                 ->applyAnnotationTransaction(QJsonDocument(transaction).toJson())
                                 .isEmpty(),
                            "pinned source owns editable annotation history");
                        record.canvasSession = annotations->serializeDocumentSession();
                    }
                    require(store.pinnedWindows().upsert(record).success &&
                                store.pinnedWindows().flush().success,
                            "persist a source with content-relative mask geometry");
                    const auto opened = call(QStringLiteral("snow_shot_document_open"),
                                             {{QStringLiteral("source"), QStringLiteral("pinned")},
                                              {QStringLiteral("source_id"), record.id}});
                    const auto id = opened.result.value(QStringLiteral("document_id")).toString();
                    require(!annotatedCase ||
                                opened.result.value(QStringLiteral("can_undo")).toBool(),
                            "actual source adapter transfers editable annotation history");
                    const auto bounds =
                        opened.result.value(QStringLiteral("selection_bounds")).toArray();
                    require(bounds.size() == 4 && bounds[0].toDouble() == origin.x() &&
                                bounds[1].toDouble() == origin.y() &&
                                bounds[2].toDouble() == pixels.width() &&
                                bounds[3].toDouble() == pixels.height(),
                            "real adapter imports the absolute content selection");
                    const auto rendered =
                        call(QStringLiteral("snow_shot_document_render"),
                             {{QStringLiteral("document_id"), id}}, opened.revision);
                    const auto actual =
                        image_codec::decode(rendered.attachment, snow::image::Format::png, nullptr);
                    const QList<CanvasExportSource> sources{{image, content}};
                    const auto background =
                        renderScreenshotSelectionBackground(sources, content, pixels, style);
                    const auto expected =
                        annotations
                            ? renderAnnotatedScreenshotSelection(*annotations, content, pixels,
                                                                 sources, style)
                            : ScreenshotResultCompositor::compose(image, style, 1, 1, background);
                    require(!actual.isNull() && comparable(actual) == comparable(expected),
                            "document output preserves scale, rounded holes, effects and opacity");
                    call(QStringLiteral("snow_shot_document_close"),
                         {{QStringLiteral("document_id"), id}}, opened.revision);
                    if (annotatedCase) {
                        controller.restorePinnedWindows();
                        QPointer<ScreenshotPinnedWindow> live;
                        await([&] {
                            for (auto* widget : QApplication::topLevelWidgets()) {
                                auto* pin = qobject_cast<ScreenshotPinnedWindow*>(widget);
                                if (pin && pin->persistenceId() == record.id &&
                                    pin->automationState().value(QStringLiteral("ready")).toBool())
                                    live = pin;
                            }
                            return !live.isNull();
                        });
                        const auto imported =
                            call(QStringLiteral("snow_shot_document_open"),
                                 {{QStringLiteral("source"), QStringLiteral("pinned")},
                                  {QStringLiteral("source_id"), record.id}});
                        const auto liveId =
                            imported.result.value(QStringLiteral("document_id")).toString();
                        require(imported.result.value(QStringLiteral("can_undo")).toBool(),
                                "live source retains editable annotations without flattening");
                        const auto liveOutput =
                            call(QStringLiteral("snow_shot_document_render"),
                                 {{QStringLiteral("document_id"), liveId}}, imported.revision);
                        require(comparable(image_codec::decode(liveOutput.attachment,
                                                               snow::image::Format::png,
                                                               nullptr)) == comparable(expected),
                                "live pinned sources share the same exact selection projection");
                        call(QStringLiteral("snow_shot_document_close"),
                             {{QStringLiteral("document_id"), liveId}}, imported.revision);
                        live->close();
                        await([&] { return !live || !live->sourcePinAvailable(); });
                    }
                    require(store.pinnedWindows().remove(record.id).success,
                            "retire pinned source fixture");
                }
            }
        }
    }
    store.shutdown();
    return EXIT_SUCCESS;
}
