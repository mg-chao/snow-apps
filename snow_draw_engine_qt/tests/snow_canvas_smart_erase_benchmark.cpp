#include "snow_canvas_smart_erase.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <atomic>
#include <cmath>
#include <iostream>

namespace {
QImage fixture(QSize size, bool nonrepeating) {
    QImage image(size, QImage::Format_ARGB32);
    for (int y = 0; y < size.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const int stripe = ((x / 16 + y / 16) % 2) * 24;
            if (nonrepeating) {
                const double wave =
                    12 * std::sin(x * 0.037 + y * 0.013) + 9 * std::sin(y * 0.071 + x * 0.009);
                row[x] = qRgb(qRound(110 + 60.0 * x / size.width() + wave),
                              qRound(135 + 45.0 * y / size.height() + wave), qRound(165 + wave));
            } else {
                row[x] = qRgb(160 + stripe, 175 + stripe, 195 + stripe);
            }
        }
    }
    return image;
}
bool run(const char* name, QSize size, QSize hole, bool pen, const QString& outputDirectory,
         bool nonrepeating = false) {
    auto source = fixture(size, nonrepeating);
    SnowSceneDisplayItem raw{};
    raw.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    raw.element_id = {1, 1};
    raw.filter.filter_type = 5;
    raw.filter.strength = 0.5;
    raw.center_x = size.width() / 2.0;
    raw.center_y = size.height() / 2.0;
    raw.width = hole.width();
    raw.height = hole.height();
    raw.opacity = 1;
    SnowCanvasSceneItem item(raw);
    if (pen) {
        item.is_free_draw = 1;
        item.stroke_width = 20;
        std::vector<SnowArrowPoint> points;
        for (int i = 0; i < hole.width(); i += 4)
            points.push_back(
                {raw.center_x - hole.width() / 2.0 + i, raw.center_y + std::sin(i / 50.0) * 30});
        item.setArrowPoints(points.data(), static_cast<std::uint32_t>(points.size()));
    }
    const auto target = snow_canvas_smart_erase::path(item);
    {
        QPainter painter(&source);
        painter.fillPath(target, QColor(25, 35, 45));
    }
    const QList<SnowCanvasBaseImageSource> sources{{source, QRectF(QPointF(0, 0), size), {}}};
    std::atomic_bool cancelled{false};
    QElapsedTimer timer;
    timer.start();
    const auto result = snow_canvas_smart_erase::reconstruct(item, sources, cancelled);
    const auto elapsed = timer.elapsed();
    std::cout << name << "," << size.width() << "x" << size.height() << "," << elapsed << ","
              << result.success << ","
              << result.original.sizeInBytes() + result.filled.sizeInBytes() << '\n'
              << std::flush;
    if (!result.success)
        return false;
    if (!outputDirectory.isEmpty()) {
        const QRect inspect = target.boundingRect()
                                  .adjusted(-48, -48, 48, 48)
                                  .toAlignedRect()
                                  .intersected(source.rect());
        QImage comparison(inspect.width() * 2, inspect.height(), QImage::Format_ARGB32);
        comparison.fill(Qt::white);
        QPainter painter(&comparison);
        painter.drawImage(QPoint(0, 0), source.copy(inspect));
        painter.setClipRect(QRect(inspect.width(), 0, inspect.width(), inspect.height()));
        painter.translate(inspect.width() - inspect.x(), -inspect.y());
        painter.drawImage(QPoint(0, 0), source);
        painter.setClipPath(target);
        painter.drawImage(result.canvasRect, result.filled);
        painter.end();
        if (!comparison.save(
                QDir(outputDirectory).filePath(QString::fromLatin1(name) + QStringLiteral(".png"))))
            return false;
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString output = app.arguments().size() > 1 ? app.arguments()[1] : QString();
    if (!output.isEmpty() && !QDir().mkpath(output))
        return 1;
    std::cout << "scenario,source,elapsed_ms,success,retained_bytes\n";
    bool ok = run("texture", QSize(512, 320), QSize(96, 64), false, output);
    ok = run("4k-text", QSize(3840, 2160), QSize(192, 64), false, output) && ok;
    ok = run("8k-text", QSize(7680, 4320), QSize(192, 64), false, output) && ok;
    ok = run("large-region", QSize(3840, 2160), QSize(384, 256), false, output) && ok;
    ok = run("long-pen", QSize(7680, 4320), QSize(2400, 80), true, output) && ok;
    ok = run("nonrepeating-texture", QSize(1024, 768), QSize(192, 96), false, output, true) && ok;
    return ok ? 0 : 1;
}
