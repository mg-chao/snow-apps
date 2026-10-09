#include "snow_shot/presentation/screenshotlatexrenderer.h"
#include "snow_preview.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QThread>
#include <QTimer>

#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

using Error = ScreenshotLatexRenderer::Error;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void until(const std::function<bool()>& condition) {
    QElapsedTimer deadline;
    deadline.start();
    while (!condition() && deadline.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(condition(), "formula rendering did not finish");
}

struct Result final {
    QImage image;
    Error error = Error::None;
};

Result render(ScreenshotLatexRenderer& renderer, const QString& source, qreal dpr = 1,
              QSize viewport = QSize(480, 360), QColor foreground = Qt::black) {
    Result result;
    bool completed = false;
    quint64 expectedToken = 0;
    const auto connection =
        QObject::connect(&renderer, &ScreenshotLatexRenderer::rendered, &renderer,
                         [&](quint64 token, const QImage& image, Error error) {
                             require(token == expectedToken, "renderer returned a stale request");
                             result = {image, error};
                             completed = true;
                         });
    expectedToken = renderer.request(source, viewport, dpr, foreground);
    until([&]() { return completed; });
    QObject::disconnect(connection);
    return result;
}

bool hasVisiblePixels(const QImage& image) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 0) {
                return true;
            }
        }
    }
    return false;
}

void mathematicalAndUnicodeRendering() {
    ScreenshotLatexRenderer renderer;
    for (const auto& source : {QStringLiteral("\\frac{a+b}{\\sqrt{x^2+1}}"),
                               QStringLiteral("\\begin{pmatrix}1&2\\\\3&4\\end{pmatrix}"),
                               QStringLiteral("α + \\beta + \\text{Ελληνικά русский 中文}"),
                               QStringLiteral("$$\\sum_{n=0}^{\\infty} \\frac{x^n}{n!}$$")}) {
        const auto result = render(renderer, source);
        if (result.error != Error::None || result.image.isNull() ||
            !hasVisiblePixels(result.image)) {
            std::cerr << "formula=" << source.toStdString()
                      << " error=" << static_cast<int>(result.error)
                      << " width=" << result.image.width() << " height=" << result.image.height()
                      << " dpr=" << result.image.devicePixelRatio() << '\n';
        }
        require(result.error == Error::None && !result.image.isNull() &&
                    hasVisiblePixels(result.image),
                "a supported formula produced no visible preview");
    }
    const auto empty = render(renderer, QString());
    require(empty.error == Error::None && empty.image.isNull(), "empty draft should clear preview");
    const auto normal = render(renderer, QStringLiteral("x^2"));
    const auto highDpi = render(renderer, QStringLiteral("x^2"), 2);
    require(highDpi.image.devicePixelRatio() == 2 &&
                highDpi.image.size() == normal.image.size() * 2,
            "preview pixels should scale with the requested DPR");
    for (const auto viewport : {QSize(1, 1), QSize(20, 12)}) {
        const auto small = render(renderer, QStringLiteral("x^2"), 1, viewport);
        require(small.error == Error::None && !small.image.isNull() &&
                    hasVisiblePixels(small.image) && small.image.width() < normal.image.width() &&
                    small.image.height() < normal.image.height(),
                "small viewports should reduce transparent padding without losing formula pixels");
    }
}

void formulaStateIsIsolated() {
    ScreenshotLatexRenderer renderer;
    const auto plain = render(renderer, QStringLiteral("x"));
    for (const auto& source :
         {QStringLiteral("\\newcommand{\\snowtest}{x^2}\\snowtest"),
          QStringLiteral("\\newcommand{\\snowtest}{x}\\renewcommand{\\snowtest}{y}\\snowtest"),
          QStringLiteral("\\newenvironment{snowenv}[1]{#1}{}"
                         "\\begin{snowenv}{x}y\\end{snowenv}"),
          QStringLiteral("\\newenvironment{snowenv}[1]{#1}{}"
                         "\\renewenvironment{snowenv}[1]{#1^2}{}"
                         "\\begin{snowenv}{x}y\\end{snowenv}"),
          QStringLiteral("\\newcolumntype{z}{r}\\begin{array}{zz}1&2\\\\3&4\\end{array}"),
          QStringLiteral("\\definecolor{snowtest}{rgb}{1,0,0}\\textcolor{snowtest}{x}"),
          QStringLiteral("\\magnification{200}x")}) {
        const auto first = render(renderer, source);
        renderer.clearCache();
        const auto second = render(renderer, source);
        if (first.error != Error::None || second.error != Error::None ||
            first.image != second.image) {
            std::cerr << "isolation formula=" << source.toStdString()
                      << " first_error=" << static_cast<int>(first.error)
                      << " second_error=" << static_cast<int>(second.error)
                      << " first_size=" << first.image.width() << 'x' << first.image.height()
                      << " second_size=" << second.image.width() << 'x' << second.image.height()
                      << " pixels_equal=" << (first.image == second.image) << '\n';
        }
        require(first.error == Error::None && second.error == Error::None &&
                    first.image == second.image,
                "repeated rendering must not retain custom command or setting changes");
        renderer.clearCache();
        require(render(renderer, QStringLiteral("x")).image == plain.image,
                "formula settings leaked into another request");
    }
    const auto invalid =
        render(renderer, QStringLiteral("\\newcommand{\\snowtest}{x}\\snowUndefined"));
    require(invalid.error == Error::InvalidFormula && invalid.image.isNull(),
            "strict parser should report an unsupported formula");
    renderer.clearCache();
    require(render(renderer, QStringLiteral("\\newcommand{\\snowtest}{x}\\snowtest")).error ==
                Error::None,
            "failed parsing left a custom command behind");
    ScreenshotLatexRenderer independent;
    require(render(independent, QStringLiteral("\\newcommand{\\snowtest}{y}\\snowtest")).error ==
                Error::None,
            "independent previews must not share custom definitions");
}

void rasterCacheAndResources() {
    ScreenshotLatexRenderer renderer;
    const auto first = render(renderer, QStringLiteral("x^2"));
    const auto repeated = render(renderer, QStringLiteral("x^2"));
    require(first.image.cacheKey() == repeated.image.cacheKey(),
            "identical requests should reuse cached pixels");
    const auto resized = render(renderer, QStringLiteral("x^2"), 1, QSize(320, 240));
    require(resized.image.cacheKey() != first.image.cacheKey(),
            "viewport changes need their own raster cache key");
    require(render(renderer, QStringLiteral("x^2")).image.cacheKey() == first.image.cacheKey(),
            "returning to the previous viewport should reuse its raster");
    require(render(renderer, QStringLiteral("\\snowUndefined")).error == Error::InvalidFormula,
            "an invalid current draft should report its parse error");
    require(render(renderer, QStringLiteral("x^2")).image.cacheKey() == first.image.cacheKey(),
            "an invalid draft should retain the most recent valid raster");
    require(render(renderer, QStringLiteral("x^2"), 1, QSize(320, 240)).image.cacheKey() !=
                resized.image.cacheKey(),
            "an invalid draft should release the older displaced valid raster");
    renderer.clearCache();
    const auto fresh = render(renderer, QStringLiteral("x^2"));
    require(fresh.image.cacheKey() != first.image.cacheKey() && fresh.image == first.image,
            "target invalidation must release cached storage and preserve formula rendering");
    require(render(renderer, QStringLiteral("x^2"), 1, QSize(480, 360), Qt::red).image !=
                first.image,
            "foreground/theme changes must invalidate cached pixels");
    tinyxml2::XMLDocument missing;
    require(tex::snowLoadXml(missing, ":/snow-shot/microtex/missing.xml") ==
                tinyxml2::XML_ERROR_FILE_NOT_FOUND,
            "missing packaged XML must fail explicitly");
    tinyxml2::XMLDocument available;
    require(tex::snowLoadXml(available, ":/snow-shot/microtex/greek/fcmrpg.xml") ==
                tinyxml2::XML_SUCCESS,
            "Greek font metrics must load from the packaged Qt resource");
}

void boundedWorkAndLatestRequestWins() {
    ScreenshotLatexRenderer renderer;
    int heartbeats = 0;
    QTimer timer;
    timer.setInterval(0);
    QObject::connect(&timer, &QTimer::timeout, &timer, [&]() { ++heartbeats; });
    timer.start();
    const auto recursive =
        render(renderer, QStringLiteral("\\newcommand{\\snowloop}{\\snowloop}\\snowloop"));
    require(recursive.error == Error::LimitExceeded && heartbeats > 0,
            "recursive macros must stop while the GUI event loop remains responsive");
    const auto expanded = render(renderer, QString(32769, QLatin1Char('x')));
    require(expanded.error == Error::LimitExceeded,
            "oversized source must remain editable without rendering");

    int completions = 0;
    quint64 lastToken = 0;
    const auto connection =
        QObject::connect(&renderer, &ScreenshotLatexRenderer::rendered, &renderer,
                         [&](quint64 token, const QImage& image, Error error) {
                             require(token == lastToken && error == Error::None && !image.isNull(),
                                     "only the latest request may update the preview");
                             ++completions;
                         });
    for (int i = 0; i < 100; ++i) {
        lastToken = renderer.request(QString::number(i), QSize(480, 360), 1, Qt::black);
    }
    until([&]() { return completions == 1; });
    QObject::disconnect(connection);

    auto transient = std::make_unique<ScreenshotLatexRenderer>();
    static_cast<void>(
        transient->request(QStringLiteral("\\newcommand{\\snowloop}{\\snowloop}\\snowloop"),
                           QSize(480, 360), 1, Qt::black));
    transient.reset();
    require(render(renderer, QStringLiteral("x")).error == Error::None,
            "destroying another preview must cancel its work safely");
    const auto huge = render(renderer, QStringLiteral("\\rule{1000000000px}{1000000000px}"));
    require(huge.error == Error::LimitExceeded,
            "oversized rendered dimensions must not allocate an image");
    for (const auto& source :
         {QStringLiteral("\\begin{array}{*{1000000000}{r}}x\\end{array}"),
          QStringLiteral("\\begin{array}{*{1000000000}{}}x\\end{array}"),
          QStringLiteral("\\newcommand{\\snowargs}[1000000000]{x}\\snowargs")}) {
        require(render(renderer, source).error == Error::LimitExceeded,
                "numeric allocation requests must be checked before expanding layout storage");
    }
    const auto ragged = QStringLiteral("\\begin{matrix}") + QStringLiteral("x&").repeated(1000) +
                        QStringLiteral("x\\\\") + QStringLiteral("x\\\\").repeated(1000) +
                        QStringLiteral("x\\end{matrix}");
    require(render(renderer, ragged).error == Error::LimitExceeded,
            "ragged matrices must be bounded before padding to a rectangle");
    require(render(renderer, QStringLiteral("\\frac{x}")).error == Error::InvalidFormula,
            "missing arguments should produce a parse error without accessing past the source");
}

void numericSpansAreValidated() {
    ScreenshotLatexRenderer renderer;
    for (const auto& source :
         {QStringLiteral("\\multicolumn{1}{c}{x}"),
          QStringLiteral("\\begin{array}{c}\\multicolumn{-1}{c}{x}\\end{array}"),
          QStringLiteral("\\begin{array}{c}\\multicolumn{0}{c}{x}\\end{array}"),
          QStringLiteral("\\begin{array}{c}\\hdotsfor{-1}\\end{array}"),
          QStringLiteral("\\begin{array}{c}\\hdotsfor{0}\\end{array}"),
          QStringLiteral("\\begin{array}{c}\\multirow{0}{*}{x}\\end{array}"),
          QStringLiteral("\\newenvironment{snowenv}[-1]{x}{y}")}) {
        const auto result = render(renderer, source);
        if (result.error != Error::InvalidFormula)
            std::cerr << "invalid span formula=" << source.toStdString()
                      << " error=" << static_cast<int>(result.error) << '\n';
        require(result.error == Error::InvalidFormula,
                "invalid spans and environment counts must fail before indexing matrix storage");
        renderer.clearCache();
        require(render(renderer, QStringLiteral("x")).error == Error::None,
                "invalid numeric counts must preserve the next rendering request");
    }
    for (const auto& source :
         {QStringLiteral("\\begin{array}{c}\\multicolumn{-2147483648}{c}{x}\\end{array}"),
          QStringLiteral("\\begin{array}{c}\\multicolumn{2147483647}{c}{x}\\end{array}"),
          QStringLiteral("\\begin{array}{c}\\hdotsfor{2147483647}\\end{array}"),
          QStringLiteral("\\begin{array}{c}\\multirow{-2147483648}{*}{x}\\end{array}"),
          QStringLiteral("\\begin{array}{c}\\multirow{2147483647}{*}{x}\\end{array}"),
          QStringLiteral("\\newenvironment{snowenv}[2147483647]{x}{y}"),
          QStringLiteral("\\newenvironment{snowenv}{x}{y}"
                         "\\renewenvironment{snowenv}[2147483647]{x}{y}")}) {
        const auto result = render(renderer, source);
        if (result.error != Error::LimitExceeded && result.error != Error::InvalidFormula)
            std::cerr << "extreme span formula=" << source.toStdString()
                      << " error=" << static_cast<int>(result.error) << '\n';
        require(result.error == Error::LimitExceeded || result.error == Error::InvalidFormula,
                "extreme counts must be rejected before signed arithmetic or allocation");
        renderer.clearCache();
        require(render(renderer, QStringLiteral("x")).error == Error::None,
                "an extreme count must not damage renderer state");
    }
    for (const auto& source :
         {QStringLiteral("\\begin{array}{c}\\multirow{2}{*}{\\frac{\\frac{x}{y}}{z}}\\end{array}"),
          QStringLiteral("\\begin{array}{c}x\\\\\\multirow{-2}{*}{\\frac{x}{y}}\\end{array}")}) {
        const auto result = render(renderer, source);
        if (result.error != Error::None || !hasVisiblePixels(result.image))
            std::cerr << "row span formula=" << source.toStdString()
                      << " error=" << static_cast<int>(result.error) << '\n';
        require(result.error == Error::None && hasVisiblePixels(result.image),
                "positive and negative row spans must remain safe at incomplete matrix edges");
    }
}

void checkedLongDivisionIsSafe() {
    ScreenshotLatexRenderer renderer;
    const auto minimum = QString::number(static_cast<qlonglong>(std::numeric_limits<long>::min()));
    const auto maximum = QString::number(static_cast<qlonglong>(std::numeric_limits<long>::max()));
    for (const auto& source : {QStringLiteral("\\longdiv{%1}{-1}").arg(minimum),
                               QStringLiteral("\\longdiv{%1}{1}").arg(minimum),
                               QStringLiteral("\\longdiv{%1}{-1}").arg(maximum)}) {
        require(render(renderer, source).error == Error::LimitExceeded,
                "long division must reject arithmetic overflow before evaluating or casting it");
        renderer.clearCache();
        require(render(renderer, QStringLiteral("x")).error == Error::None,
                "long division overflow must preserve the next rendering request");
    }
    for (const auto& source :
         {QStringLiteral("\\longdiv{12}{3}"), QStringLiteral("\\longdiv{-12}{3}"),
          QStringLiteral("\\longdiv{12}{-3}"), QStringLiteral("\\longdiv{-12}{-3}"),
          QStringLiteral("\\longdiv{%1}{1}").arg(maximum),
          QStringLiteral("\\longdiv{%1}{-2}").arg(minimum)}) {
        const auto result = render(renderer, source);
        if (result.error != Error::None)
            std::cerr << "long division formula=" << source.toStdString()
                      << " error=" << static_cast<int>(result.error) << '\n';
        require(result.error == Error::None && hasVisiblePixels(result.image),
                "representable ordinary and signed long division inputs must remain supported");
    }
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    try {
        mathematicalAndUnicodeRendering();
        formulaStateIsIsolated();
        rasterCacheAndResources();
        boundedWorkAndLatestRequestWins();
        numericSpansAreValidated();
        checkedLongDivisionIsSafe();
        std::cout << "LaTeX renderer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
