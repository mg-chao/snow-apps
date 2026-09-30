#include "snow_shot/presentation/screenshotautofiltercontroller.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_visual_region_detector.h"
#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QPainter>
#include <QTimer>
#include <cmath>
#include <memory>

namespace {
constexpr const char* kCategories[] = {"text", "text_in_box", "image",     "avatar",
                                       "icon", "message_box", "text_block"};
QString detectionError() {
    return QCoreApplication::translate("ScreenshotAutoFilterController",
                                       "Region identification failed. Try Auto Filter again.");
}
ScreenshotExportJobHandle detect(QObject* receiver, QImage image,
                                 ScreenshotAutoFilterController::Completion completion) {
    auto regions = std::make_shared<QList<SnowCanvasAutoFilterRegion>>();
    auto job = ScreenshotExportCoordinator::shared().submit(
        receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [image = std::move(image),
         regions](const ScreenshotExportCancellation& cancellation) mutable {
            if (cancellation.isCancellationRequested()) {
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled,
                                                           {});
            }
            image = image.convertToFormat(QImage::Format_BGR888);
            if (cancellation.isCancellationRequested()) {
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled,
                                                           {});
            }
            SnowDetectedRegions* raw = nullptr;
            const int status = snow_detect_visual_regions(
                image.constBits(), static_cast<size_t>(image.sizeInBytes()),
                static_cast<uint32_t>(image.width()), static_cast<uint32_t>(image.height()),
                static_cast<size_t>(image.bytesPerLine()), &raw);
            const std::unique_ptr<SnowDetectedRegions, decltype(&snow_detected_regions_release)>
                result(raw, snow_detected_regions_release);
            if (cancellation.isCancellationRequested()) {
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled,
                                                           {});
            }
            if (status != 0) {
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Render,
                                                           {});
            }
            size_t count = 0;
            const SnowDetectedRegion* data = snow_detected_regions_data(result.get(), &count);
            for (size_t i = 0; i < count; ++i) {
                const auto& region = data[i];
                if (region.category < std::size(kCategories)) {
                    regions->append({static_cast<quint64>(i + 1),
                                     QRectF(region.x, region.y, region.width, region.height),
                                     QString::fromLatin1(kCategories[region.category])});
                }
            }
            return ScreenshotExportTaskResult{};
        },
        [regions, completion](ScreenshotExportTaskResult result) mutable {
            if (result.failureStage != ScreenshotExportFailureStage::Cancelled) {
                completion(std::move(*regions), result.succeeded() ? QString() : detectionError());
            }
        });
    if (!job.isValid()) {
        completion({}, detectionError());
    }
    return job;
}
} // namespace

void ScreenshotAutoFilterController::detectRegions(QImage image, Completion completion) {
    static_cast<void>(
        detect(QCoreApplication::instance(), std::move(image), std::move(completion)));
}

class ScreenshotAutoFilterVisual final : public QWidget {
  public:
    ScreenshotAutoFilterVisual(SnowCanvasWidget* canvas, ScreenshotAutoFilterController* controller)
        : QWidget(canvas), m_canvas(canvas), m_controller(controller) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        canvas->installEventFilter(this);
        setGeometry(canvas->rect());
    }

    void updateFrame() {
        QRegion dirty;
        const QTransform transform = m_canvas->canvasToViewTransform();
        const QRectF bounds = transform.mapRect(m_controller->currentBounds());
        if (m_controller->detecting()) {
            const qreal phase = static_cast<qreal>(m_controller->elapsed() % 1400) / 1400.0;
            const qreal y = bounds.top() + phase * (bounds.height() + 60.0) - 30.0;
            dirty += QRectF(bounds.left(), y - 31, bounds.width(), 44)
                         .intersected(bounds)
                         .toAlignedRect();
        }
        if (m_controller->flashing()) {
            for (const auto& region : m_controller->flashRegions()) {
                dirty += transform.mapRect(region.bounds).adjusted(-2, -2, 2, 2).toAlignedRect();
            }
        }
        update(dirty | m_previousFrame);
        m_previousFrame = dirty;
    }

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Resize) {
            setGeometry(m_canvas->rect());
        }
        return false;
    }
    void paintEvent(QPaintEvent*) override {
        if (!m_controller || m_canvas->canvasTool() != SnowCanvasTool::AutoFilter) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QTransform transform = m_canvas->canvasToViewTransform();
        const QRectF bounds = transform.mapRect(m_controller->currentBounds());
        painter.setClipRect(bounds);
        if (m_controller->detecting()) {
            const qreal phase = static_cast<qreal>(m_controller->elapsed() % 1400) / 1400.0;
            const qreal y = bounds.top() + phase * (bounds.height() + 60.0) - 30.0;
            QLinearGradient glow(0, y - 30, 0, y + 12);
            glow.setColorAt(0, QColor(64, 150, 255, 0));
            glow.setColorAt(0.70, QColor(64, 150, 255, 45));
            glow.setColorAt(0.76, QColor(96, 220, 255, 145));
            glow.setColorAt(1, QColor(64, 150, 255, 0));
            painter.fillRect(QRectF(bounds.left(), y - 30, bounds.width(), 42), glow);
        }
        if (m_controller->flashing()) {
            painter.setPen(QPen(QColor(QStringLiteral("#ff4d4f")), 1));
            painter.setBrush(QColor(255, 77, 79, 51));
            for (const auto& region : m_controller->flashRegions()) {
                painter.drawRect(transform.mapRect(region.bounds));
            }
        }
    }

  private:
    QRegion m_previousFrame;
    SnowCanvasWidget* m_canvas;
    QPointer<ScreenshotAutoFilterController> m_controller;
};

ScreenshotAutoFilterController::ScreenshotAutoFilterController(std::function<QRectF()> bounds,
                                                               Source source, QObject* parent,
                                                               Detector detector,
                                                               std::function<qint64()> clock)
    : QObject(parent), m_bounds(std::move(bounds)), m_source(std::move(source)),
      m_detector(std::move(detector)), m_clock(std::move(clock)) {
    m_elapsed.start();
    m_timer = new QTimer(this);
    m_timer->setInterval(16);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        if (m_busy || flashing()) {
            refresh();
        } else if (!m_flashRegions.isEmpty()) {
            m_flashRegions.clear();
            refresh();
        }
    });
}

SnowCanvasWidget* ScreenshotAutoFilterController::canvas() const {
    for (const auto& item : m_canvases) {
        if (item) {
            return item;
        }
    }
    return nullptr;
}
bool ScreenshotAutoFilterController::active() const {
    return canvas() && canvas()->canvasTool() == SnowCanvasTool::AutoFilter;
}
bool ScreenshotAutoFilterController::available() const {
    if (!canvas()) {
        return false;
    }
    const auto record = canvas()->autoFilterRegions();
    return record && record->sourceBounds == m_bounds() && !record->regions.isEmpty();
}
void ScreenshotAutoFilterController::attachCanvas(SnowCanvasWidget* item) {
    if (!item || m_canvases.contains(item)) {
        return;
    }
    m_canvases.append(item);
    item->installEventFilter(this);
    m_visuals.append(new ScreenshotAutoFilterVisual(item, this));
    connect(item, &SnowCanvasWidget::autoFilterInteractionStarting, this,
            &ScreenshotAutoFilterController::validate, Qt::DirectConnection);
    connect(item, &SnowCanvasWidget::autoFilterRegionsChanged, this, [this]() {
        m_flashUntil = 0;
        m_flashRegions.clear();
        refresh();
    });
    connect(item, &SnowCanvasWidget::activeToolChanged, this, [this]() {
        if (active()) {
            validate();
        } else {
            m_flashUntil = 0;
            m_flashRegions.clear();
            refresh();
        }
    });
    refresh();
}
void ScreenshotAutoFilterController::refresh() {
    const bool visible = active() && (m_busy || flashing());
    if (visible && !m_timer->isActive()) {
        m_timer->start();
    } else if (!visible) {
        m_timer->stop();
    }
    for (const auto& visual : m_visuals) {
        if (visual) {
            visual->setVisible(visible);
            if (visible) {
                visual->raise();
                static_cast<ScreenshotAutoFilterVisual*>(visual.data())->updateFrame();
            }
        }
    }
    updateAvailability();
}
void ScreenshotAutoFilterController::resetSession() {
    ++m_session;
    m_detectionJob.cancel();
    m_detectionJob = {};
    m_busy = false;
    m_flashUntil = 0;
    m_flashRegions.clear();
    refresh();
}
void ScreenshotAutoFilterController::validate() {
    auto* target = canvas();
    if (!target || !active()) {
        return;
    }
    const QRectF bounds = m_bounds();
    if (bounds.isEmpty()) {
        return;
    }
    const auto record = target->autoFilterRegions();
    if (record && record->sourceBounds != bounds) {
        if (!target->setAutoFilterRegions(std::nullopt)) {
            return;
        }
    } else if (record) {
        refresh();
        return;
    }
    if (m_busy) {
        refresh();
        return;
    }
    m_busy = true;
    const quint64 generation = target->autoFilterGeneration();
    const quint64 session = m_session;
    refresh();
    const QPointer<ScreenshotAutoFilterController> receiver(this);
    m_source([receiver, generation, session, bounds](QImage image) {
        if (!receiver || receiver->m_session != session) {
            return;
        }
        const QSize pixels = image.size();
        auto completion = [receiver, generation, session, bounds,
                           pixels](QList<SnowCanvasAutoFilterRegion> regions, QString error) {
            if (receiver) {
                receiver->finish(session, generation, bounds, pixels, std::move(regions), error);
            }
        };
        if (receiver->m_detector) {
            receiver->m_detector(std::move(image), std::move(completion));
        } else {
            receiver->m_detectionJob = detect(receiver, std::move(image), std::move(completion));
        }
    });
}
void ScreenshotAutoFilterController::finish(quint64 session, quint64 generation, QRectF bounds,
                                            QSize pixels, QList<SnowCanvasAutoFilterRegion> regions,
                                            const QString& error) {
    if (session != m_session) {
        return;
    }
    m_detectionJob = {};
    const qint64 arrived = elapsed();
    m_busy = false;
    auto* target = canvas();
    if (!target || generation != target->autoFilterGeneration() || target->autoFilterRegions()) {
        refresh();
        return;
    }
    if (!error.isEmpty() || pixels.isEmpty()) {
        const QString message =
            error.isEmpty() ? tr("Region identification failed. Try Auto Filter again.") : error;
        if (active()) {
            emit detectionFailed(message);
        } else {
            qWarning() << message;
        }
        refresh();
        return;
    }
    for (auto& region : regions) {
        region.bounds = QRectF(bounds.x() + region.bounds.x() * bounds.width() / pixels.width(),
                               bounds.y() + region.bounds.y() * bounds.height() / pixels.height(),
                               region.bounds.width() * bounds.width() / pixels.width(),
                               region.bounds.height() * bounds.height() / pixels.height())
                            .intersected(bounds);
    }
    regions.removeIf([](const auto& region) { return region.bounds.isEmpty(); });
    if (target->setAutoFilterRegions(SnowCanvasAutoFilterRecord{bounds, regions}) && active()) {
        m_flashRegions = std::move(regions);
        m_flashUntil = arrived + 300;
        QTimer::singleShot(static_cast<int>(qMax<qint64>(0, m_flashUntil - elapsed())),
                           Qt::PreciseTimer, this, &ScreenshotAutoFilterController::refresh);
    }
    refresh();
}
void ScreenshotAutoFilterController::fillCategory(const QString& category) {
    if (available()) {
        canvas()->fillAutoFilterCategory(category);
    }
}

ScreenshotAutoFilterController::~ScreenshotAutoFilterController() {
    ++m_session;
    m_detectionJob.cancel();
    for (const auto& visual : m_visuals) {
        delete visual.data();
    }
}
void ScreenshotAutoFilterController::updateAvailability() {
    const bool next = available();
    if (m_available != next) {
        m_available = next;
        emit availabilityChanged(next);
    }
}
bool ScreenshotAutoFilterController::eventFilter(QObject*, QEvent* event) {
    if (event->type() == QEvent::Paint || event->type() == QEvent::Resize) {
        updateAvailability();
    }
    return false;
}
