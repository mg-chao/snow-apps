#include "../src/presentation/pinned/pinnedimagepresenter.h"
#include "../src/presentation/pinned/pinnedwindowhost.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_view.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QMouseEvent>
#include <cstdlib>
#include <iostream>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

using namespace snow_shot::presentation;

namespace snow_shot::presentation {
class PinnedWindowHostTestAccess {
  public:
    static void setBackend(PinnedWindowHost& host,
                           std::unique_ptr<PinnedImagePresentationBackend> backend) {
        host.m_presenter = std::make_unique<PinnedImagePresenter>(std::move(backend));
    }
};
} // namespace snow_shot::presentation

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class ImageSurface final : public PinnedImageSurface {
  public:
    explicit ImageSurface(const QSize& size) : m_image(size, QImage::Format_ARGB32_Premultiplied) {}
    QImage& image() override {
        return m_image;
    }

  private:
    QImage m_image;
};

class Backend final : public PinnedImagePresentationBackend {
  public:
    int allocations = 0;
    int publications = 0;
    int moves = 0;
    bool failAllocate = false;
    bool failPublish = false;
    bool failMove = false;
    QRect actual;
    QImage frame;
    QRegion dirty;
    std::unique_ptr<PinnedImageSurface> allocate(const QSize& pixels) override {
        ++allocations;
        return failAllocate ? nullptr : std::make_unique<ImageSurface>(pixels);
    }
    bool publish(WId, const QRect& pixels, PinnedImageSurface& surface, quint8,
                 const QRegion& dirtyPixels) override {
        ++publications;
        if (failPublish)
            return false;
        actual = pixels;
        frame = surface.image().copy();
        frame.setDevicePixelRatio(1);
        dirty = dirtyPixels;
        return true;
    }
    bool move(WId, const QRect& pixels) override {
        ++moves;
        if (failMove)
            return false;
        actual = pixels;
        return true;
    }
};

class PixelRenderer final : public SnowCanvasCustomRenderer {
  public:
    QImage source;
    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.setTransform(context.canvasToViewTransform, true);
        painter.drawImage(QPointF(), source);
    }
};

QImage pattern(const QSize& size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            image.setPixelColor(x, y,
                                x == 0 ? QColor(Qt::red)
                                       : (x == size.width() - 1
                                              ? QColor(Qt::green)
                                              : QColor((x & 1) ? 255 : 0, (y & 1) ? 255 : 0, 128,
                                                       (x % 7) ? 255 : 128)));
    return image;
}

void exactPixels() {
    for (qreal ratio : {1.0, 1.25, 1.5, 1.75, 2.0}) {
        for (int width : {1000, 1001}) {
            auto backend = std::make_unique<Backend>();
            auto* observed = backend.get();
            PinnedImagePresenter presenter(std::move(backend));
            SnowCanvasView view;
            PixelRenderer renderer;
            renderer.source = pattern(QSize(width, 23));
            require(view.setSurfaceMetrics(renderer.source.size(), ratio), "set physical viewport");
            view.setCanvasContentVisible(false);
            view.setClearBackgroundEnabled(false);
            view.setCustomRenderer(&renderer);
            require(view.setViewportCamera(view.width() * ratio / 2.0, view.height() * ratio / 2.0,
                                           1.0 / ratio),
                    "set pixel-aligned camera");
            const QRect geometry(-1923, -117, width, 23);
            int paints = 0;
            const auto paint = [&](QPainter& painter, const QRegion& dirty) {
                ++paints;
                return view.render(painter, dirty);
            };
            require(presenter.present(1, geometry, ratio, 255, {}, paint), "publish first frame");
            require(observed->actual == geometry, "preserve negative native origin and odd size");
            require(observed->frame == renderer.source,
                    "100% image must preserve every source pixel");
            for (int repeat = 0; repeat < 4; ++repeat) {
                require(presenter.present(1, geometry, ratio, 255, QRegion(view.rect()), paint),
                        "repaint");
                require(observed->actual == geometry && observed->frame == renderer.source,
                        "repaints must not move, rescale or corrupt pixels");
            }
            require(observed->allocations == 1, "reuse the presentation bitmap");
            const int beforeMove = paints;
            require(presenter.move(1, geometry.translated(1, -1)), "move in physical pixels");
            require(paints == beforeMove, "moving must not render image content");
            view.setCustomRenderer(nullptr);
        }
    }
}

void failedPublication() {
    auto backend = std::make_unique<Backend>();
    auto* observed = backend.get();
    PinnedImagePresenter presenter(std::move(backend));
    const QRect original(3, 5, 17, 19);
    const auto paint = [](QPainter& painter, const QRegion& dirty) {
        painter.fillRect(dirty.boundingRect(), Qt::blue);
        return true;
    };
    observed->failAllocate = true;
    require(!presenter.present(1, original, 1.5, 255, {}, paint) && !presenter.hasFrame(),
            "failed first allocation must not commit geometry");
    observed->failAllocate = false;
    require(presenter.present(1, original, 1.5, 255, {}, paint), "publish initial frame");
    const QImage lastFrame = observed->frame;
    observed->failPublish = true;
    require(!presenter.present(1, QRect(7, 9, 19, 21), 1.25, 255, {}, paint),
            "reject failed resize");
    require(presenter.committedGeometry() == original && observed->frame == lastFrame,
            "failed resize must retain the last published geometry and pixels");
    require(!presenter.present(1, original, 1.5, 255, QRegion(1, 1, 1, 1), paint),
            "reject failed repaint");
    observed->failPublish = false;
    require(presenter.present(1, original, 1.5, 255, QRegion(1, 1, 1, 1), paint), "retry repaint");
    require(observed->dirty == QRegion(QRect(QPoint(), original.size())),
            "failed paint requires complete retry");
    observed->failMove = true;
    require(!presenter.move(1, original.translated(2, 2)) &&
                presenter.committedGeometry() == original,
            "failed move must not commit geometry");
    presenter.reset();
    require(!presenter.hasFrame() && presenter.committedGeometry().isEmpty(),
            "surface destruction resets state");
}

#if defined(Q_OS_WIN)
void hostFailureTransactions() {
    class Faults final : public PinnedImagePresentationBackend {
      public:
        std::unique_ptr<PinnedImagePresentationBackend> native =
            createWindowsPinnedImagePresentationBackend();
        bool failAllocation = false;
        bool failPublication = false;
        bool failMove = false;
        QColor lastCorner;
        std::unique_ptr<PinnedImageSurface> allocate(const QSize& pixels) override {
            return failAllocation ? nullptr : native->allocate(pixels);
        }
        bool publish(WId window, const QRect& geometry, PinnedImageSurface& surface, quint8 opacity,
                     const QRegion& dirty) override {
            lastCorner = surface.image().pixelColor(geometry.width() - 1, geometry.height() - 1);
            return !failPublication && native->publish(window, geometry, surface, opacity, dirty);
        }
        bool move(WId window, const QRect& geometry) override {
            return !failMove && native->move(window, geometry);
        }
    };
    class Host final : public PinnedWindowHost {
      public:
        SnowCanvasView view;
        int paints = 0;
        QColor color = QColor(25, 50, 100, 128);
        Host() {
            setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
            setCanvasView(&view);
        }
        bool paintNativeFrame(QPainter& painter, const QRegion& dirty) override {
            ++paints;
            painter.fillRect(dirty.boundingRect(), color);
            return true;
        }
    };
    auto host = std::make_unique<Host>();
    auto faults = std::make_unique<Faults>();
    auto* observed = faults.get();
    PinnedWindowHostTestAccess::setBackend(*host, std::move(faults));
    const auto rectangle = [&] {
        RECT rect{};
        require(GetWindowRect(reinterpret_cast<HWND>(host->winId()), &rect), "read host rectangle");
        return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    };
    const QRect original(-17, -29, 1001, 251);
    host->winId();
    require(host->applyNativeGeometry(original), "place hidden host");
    observed->failPublication = true;
    require(!host->publishNativeFrame() && !host->isVisible(),
            "first failure must leave host hidden");
    observed->failPublication = false;
    require(host->publishNativeFrame(), "publish first complete host frame");
    host->color = QColor(200, 40, 60);
    host->update();
    require(host->publishNativeFrame() && observed->lastCorner == host->color,
            "full invalidation must repaint the final physical pixel at fractional DPI");
    const int painted = host->paints;
    require(host->applyNativeGeometry(original.translated(2, 3)) && host->paints == painted,
            "host position-only move must not rerender");
    const QRect committed = rectangle();
    observed->failAllocation = true;
    require(!host->applyNativeGeometry(QRect(0, 0, 997, 241)) && rectangle() == committed,
            "allocation failure must roll native geometry back");
    observed->failAllocation = false;
    observed->failPublication = true;
    require(!host->applyNativeGeometry(QRect(0, 0, 997, 241)) && rectangle() == committed,
            "publication failure must roll native geometry back");
    host->setWindowOpacity(0.5);
    require(host->windowOpacity() == 1.0, "failed opacity publication must roll opacity back");
    observed->failPublication = false;
    observed->failMove = true;
    require(!host->applyNativeGeometry(committed.translated(3, 4)) && rectangle() == committed,
            "failed position change must retain the last native rectangle");
    require(host->view.physicalSize() == committed.size(),
            "failed resize must restore input and rendering surface metrics");
    const QRect systemMoved = committed.translated(11, 13);
    require(SetWindowPos(reinterpret_cast<HWND>(host->winId()), nullptr, systemMoved.x(),
                         systemMoved.y(), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER),
            "simulate USER32 position-only move");
    observed->failPublication = true;
    require(!host->publishNativeFrame() && rectangle() == systemMoved,
            "repaint failure after a system move must retain the new position");
    observed->failPublication = false;
    require(host->publishNativeFrame(), "recover before input bridge checks");
    require(host->windowHandle()->focusObject() == &host->view,
            "native focus object must expose the shared canvas");
    host->view.setInteractionEnabled(true);
    require(host->view.setCanvasTool(SnowCanvasTool::Text), "select text on native host");
    const QPointF point(80, 80);
    QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(host->windowHandle(), &press);
    QCoreApplication::sendEvent(host->windowHandle(), &release);
    QInputMethodEvent preedit(QString::fromUtf8("\xe4\xb8\xad"), {});
    QCoreApplication::sendEvent(host->windowHandle(), &preedit);
    QInputMethodEvent commit;
    commit.setCommitString(QString::fromUtf8("\xe4\xb8\xad"));
    QCoreApplication::sendEvent(host->windowHandle(), &commit);
    QInputMethodQueryEvent query(Qt::ImEnabled | Qt::ImSurroundingText | Qt::ImCursorRectangle);
    QCoreApplication::sendEvent(host->windowHandle(), &query);
    require(query.value(Qt::ImEnabled).toBool() &&
                query.value(Qt::ImSurroundingText).toString() ==
                    QString::fromUtf8("\xe4\xb8\xad") &&
                QRectF(QPointF(), host->view.logicalExtent())
                    .contains(query.value(Qt::ImCursorRectangle).toRectF()),
            "native input bridge must retain composition and report a local caret rectangle");
    require(host->view.setCanvasTool(SnowCanvasTool::Select) && host->view.undo() &&
                host->view.redo(),
            "native text commit must participate in shared undo and redo");
    host->update();
    host.reset();
    QApplication::processEvents(); // queued updates must not outlive their QObject host
}

void nativeWindowGeometry() {
    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int iteration = 0; iteration < 16; ++iteration) {
        HWND window =
            CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1,
                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window != nullptr, "create native layered window");
        {
            PinnedImagePresenter presenter(createWindowsPinnedImagePresentationBackend());
            for (int width : {1000, 1001, 997, 1001}) {
                const QRect expected(-17, -23, width, 71);
                require(presenter.present(reinterpret_cast<WId>(window), expected, 1.5, 255, {},
                                          [](QPainter& painter, const QRegion& dirty) {
                                              painter.fillRect(dirty.boundingRect(),
                                                               QColor(20, 40, 60, 128));
                                              return true;
                                          }),
                        "publish directly to HWND");
                RECT actual{};
                require(GetWindowRect(window, &actual) != FALSE, "read native rectangle");
                require(QRect(actual.left, actual.top, actual.right - actual.left,
                              actual.bottom - actual.top) == expected,
                        "USER32 retains every requested physical pixel");
                QApplication::processEvents();
                require(presenter.move(reinterpret_cast<WId>(window), expected.translated(1, 1)),
                        "move native surface without painting");
            }
        }
        DestroyWindow(window);
    }
    require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before,
            "DIBs and device contexts must be released after repeated native lifecycles");
}
#endif
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    exactPixels();
    failedPublication();
#if defined(Q_OS_WIN)
    if (application.arguments().contains(QStringLiteral("--native"))) {
        nativeWindowGeometry();
        hostFailureTransactions();
    }
#endif
    return EXIT_SUCCESS;
}
