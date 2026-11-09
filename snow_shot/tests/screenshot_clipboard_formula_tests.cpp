// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotclipboardformularenderer.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QTemporaryDir>
#include <QFile>
#include <QTextDocument>
#include <QThread>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void until(const std::function<bool()>& ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), "clipboard formula request did not finish");
}

ScreenshotClipboardContent textContent(const QString& source) {
    ScreenshotClipboardContent content;
    content.kind = ScreenshotClipboardContentKind::FormattedText;
    content.image = QImage(32, 16, QImage::Format_ARGB32_Premultiplied);
    content.image.fill(Qt::magenta);
    content.formattedDocument = std::make_shared<QTextDocument>();
    content.formattedDocument->setPlainText(source);
    content.originalContent.text = source;
    content.originalContent.textStyle.emplace();
    content.originalContent.textStyle->fontFamily = QStringLiteral("Sans Serif");
    content.plainText = source;
    content.sourceIdentity.key = QStringLiteral("clipboard:test-formula");
    return content;
}

ScreenshotClipboardContent convert(ScreenshotClipboardFormulaRenderer& renderer,
                                   ScreenshotClipboardFormulaRenderer::PreparedContent content,
                                   qreal dpr = 1) {
    std::optional<ScreenshotClipboardContent> result;
    bool completed = false;
    renderer.convert(std::move(content), dpr,
                     [&](std::optional<ScreenshotClipboardContent> rendered) {
                         result = std::move(rendered);
                         completed = true;
                     });
    until([&] { return completed; });
    require(result.has_value(), "clipboard conversion must produce valid content");
    require(!renderer.active(), "completed formula conversion must clear pending state");
    return std::move(*result);
}

void recognition() {
    using Renderer = ScreenshotClipboardFormulaRenderer;
    const QString formula = QStringLiteral("\\frac{x^2}{\\sqrt{y}} + \\alpha");
    for (const auto& source :
         {formula, QStringLiteral(" $%1$ \n").arg(formula),
          QStringLiteral("$$\n%1\n$$").arg(formula), QStringLiteral("\\(%1\\)").arg(formula),
          QStringLiteral("\\[%1\\]").arg(formula)}) {
        require(Renderer::formulaSource(source) == formula,
                "whole formulas must preserve bare source and strip exactly one delimiter pair");
    }
    for (const auto& source :
         {QStringLiteral("\\begin{aligned}x&=1\\\\y&=2\\end{aligned}"),
          QStringLiteral("x=\\sum_{i=0}^n i"), QStringLiteral("\\int_0^1 x dx"),
          QStringLiteral("\\Gamma + \\beta"), QStringLiteral("\\frac{abc}{xyz}"),
          QStringLiteral("\\frac{1}{2}\\text{ metres per second}"),
          QStringLiteral("$\\text{Price: \\$5}$")}) {
        require(Renderer::formulaSource(source).has_value(),
                "math environments, symbols, text arguments and escaped dollars are supported");
    }
    for (const auto& source :
         {QString(), QStringLiteral("   "), QStringLiteral("$$ $$"), QStringLiteral("$"),
          QStringLiteral("$x"), QStringLiteral("$x\\$"), QStringLiteral("x^2+y^2=1"),
          QStringLiteral("Ordinary clipboard text"), QStringLiteral("$x$ and $y$"),
          QStringLiteral("See $\\frac{x}{y}$ for details."),
          QStringLiteral("The formula is \\frac{x}{y}"), QStringLiteral("C:\\alpha\\beta"),
          QStringLiteral("\\\\server\\alpha"), QStringLiteral("https://example.com/\\alpha"),
          QStringLiteral("```latex\n\\frac{x}{y}\n```"), QStringLiteral(".\\alpha\\beta"),
          QStringLiteral("../\\alpha"), QStringLiteral("/tmp/\\alpha"),
          QStringLiteral("\\begin{document}text\\end{document}")}) {
        require(!Renderer::formulaSource(source),
                "empty, ambiguous, mixed prose, paths and non-math documents must remain text");
    }
    require(!Renderer::formulaSource(QString(1000000, QLatin1Char('\\'))),
            "oversized clipboard text must bypass formula detection before scanning");
    const QString maximumBody(32768, QLatin1Char('x'));
    require(Renderer::formulaSource(QStringLiteral("$$") + maximumBody + QStringLiteral("$$")) ==
                maximumBody,
            "the complete renderer source budget fits inside display delimiters");
    require(!Renderer::formulaSource(QStringLiteral("$") + maximumBody + QStringLiteral("x$")),
            "oversized bodies are rejected before submitting a render request");
    for (const auto& source :
         {QString(32773, QLatin1Char(' ')),
          QString(32770, QLatin1Char(' ')) + QStringLiteral("$x$"),
          QStringLiteral("$x$") + QString(32770, QChar(0x2003)),
          QLatin1Char(' ') + QString(1000000, QLatin1Char('x')) + QLatin1Char(' ')}) {
        require(!Renderer::formulaSource(source),
                "the raw input limit includes padding and applies before trimming");
    }
}

ScreenshotClipboardContentSnapshot textSnapshot(const QString& source) {
    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.text = source;
    snapshot.textStyle.emplace();
    snapshot.textStyle->fontFamily = QStringLiteral("Sans Serif");
    return snapshot;
}

void deferredTextPreparation() {
    using Renderer = ScreenshotClipboardFormulaRenderer;
    ScreenshotClipboardFormulaRenderer renderer;
    auto snapshot = textSnapshot(QStringLiteral("$\\frac{x}{y}$"));
    snapshot.devicePixelRatio = 2;
    for (const bool html : {false, true}) {
        if (html)
            snapshot.html = QStringLiteral("<p style='color:red'>$\\frac{x}{y}$</p>");
        int admissions = 0;
        auto prepared = Renderer::prepare(snapshot, {}, [&](qint64) { return ++admissions == 1; });
        require(
            prepared && std::holds_alternative<Renderer::DeferredFormula>(*prepared) &&
                admissions == 1,
            "formula candidates require only snapshot admission, with no text document or raster");
        const auto content = convert(renderer, std::move(*prepared), 2);
        require(content.isValid() && !content.isFormattedText() &&
                    content.image.devicePixelRatio() == 2 && content.originalContent.isEmpty(),
                "deferred formulas produce standalone images at the requested display scale");
    }
    snapshot.text.clear();
    auto htmlOnly = Renderer::prepare(snapshot);
    require(htmlOnly && std::holds_alternative<Renderer::DeferredFormula>(*htmlOnly) &&
                !convert(renderer, std::move(*htmlOnly), 2).isFormattedText(),
            "HTML-only formulas extract plain text without rasterizing their fallback");

    for (const bool html : {false, true}) {
        auto invalid = textSnapshot(QStringLiteral("$\\snowUndefined$"));
        invalid.devicePixelRatio = 2;
        invalid.textStyle->textColor = Qt::blue;
        invalid.textStyle->backgroundColor = Qt::yellow;
        if (html)
            invalid.html = QStringLiteral("<p><b>$\\snowUndefined$</b></p>");
        const auto expected = ScreenshotClipboardContentReader::decode(invalid);
        auto prepared = Renderer::prepare(invalid);
        require(expected && prepared &&
                    std::holds_alternative<Renderer::DeferredFormula>(*prepared),
                "invalid formula candidates defer their original formatted fallback");
        const auto fallback = convert(renderer, std::move(*prepared), 2);
        require(
            fallback.isFormattedText() && fallback.image == expected->image &&
                fallback.originalContent.text == invalid.text &&
                fallback.originalContent.html == invalid.html &&
                fallback.originalContent.textStyle == invalid.textStyle &&
                fallback.formattedTextDevicePixelRatio == 2 &&
                fallback.formattedDocument->thread() == qApp->thread(),
            "lazy decoding preserves text, HTML, style, pixels, DPI and GUI document ownership");
    }
    auto ordinary = textSnapshot(QStringLiteral("Ordinary clipboard text"));
    auto plain = Renderer::prepare(ordinary);
    require(plain && std::holds_alternative<ScreenshotClipboardContent>(*plain) &&
                convert(renderer, std::move(*plain)).isFormattedText(),
            "ordinary text keeps the existing decoder path");
    auto markdown = textSnapshot(QStringLiteral("$x^2$"));
    markdown.textFormat = snow_shot::storage::PinnedTextFormat::Markdown;
    auto marked = Renderer::prepare(markdown);
    require(marked && std::holds_alternative<ScreenshotClipboardContent>(*marked) &&
                convert(renderer, std::move(*marked)).isFormattedText(),
            "explicit Markdown content does not enter deferred formula rendering");
    auto image = textSnapshot(QStringLiteral("$x^2$"));
    image.detachedImage = QImage(12, 9, QImage::Format_ARGB32_Premultiplied);
    image.detachedImage.fill(Qt::green);
    auto inlineImage = Renderer::prepare(image);
    require(inlineImage && std::holds_alternative<ScreenshotClipboardContent>(*inlineImage) &&
                convert(renderer, std::move(*inlineImage)).image == image.detachedImage,
            "valid inline images retain precedence over accompanying formulas");
    QTemporaryDir directory;
    require(directory.isValid(), "file precedence fixture needs isolated storage");
    const auto path = directory.filePath(QStringLiteral("image.png"));
    require(image.detachedImage.save(path), "file precedence fixture must save");
    const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({path});
    require(files.size() == 1, "file precedence fixture must be readable");
    image.localImage = files.first();
    image.detachedImage = {};
    auto file = Renderer::prepare(image);
    require(file && std::holds_alternative<ScreenshotClipboardContent>(*file) &&
                !convert(renderer, std::move(*file)).isFormattedText(),
            "valid file images retain precedence over accompanying formulas");
    image.localImage.reset();
    image.encodedImages.append({QByteArrayLiteral("invalid PNG"), QStringLiteral("image/png")});
    auto invalidImage = Renderer::prepare(image);
    require(invalidImage && std::holds_alternative<Renderer::DeferredFormula>(*invalidImage) &&
                std::get<Renderer::DeferredFormula>(*invalidImage).text.encodedImages.isEmpty(),
            "failed image payloads reach formula detection and are not retained for text fallback");
    require(!Renderer::prepare(snapshot, [] { return true; }),
            "cancelled preparation must not return a deferred request");
}

void imagesAndFallbacks() {
    ScreenshotClipboardFormulaRenderer renderer;
    QImage reference;
    for (const auto& source :
         {QStringLiteral("\\frac{x}{y}"), QStringLiteral("$\\frac{x}{y}$"),
          QStringLiteral("$$\\frac{x}{y}$$"), QStringLiteral("\\(\\frac{x}{y}\\)"),
          QStringLiteral("\\[\\frac{x}{y}\\]")}) {
        const auto content = convert(renderer, textContent(source), 2);
        require(content.isValid() && !content.isFormattedText() &&
                    content.originalContent.isEmpty() && content.plainText.isEmpty() &&
                    content.sourceIdentity.key == QStringLiteral("clipboard:test-formula"),
                "formula pins must be standalone image content with their source identity");
        require(content.image.devicePixelRatio() == 2,
                "formula pins must render at the destination display scale");
        if (!reference.isNull())
            require(reference == content.image,
                    "delimiter spelling must not change formula pixels");
        reference = content.image;
    }
    bool darkPixel = false;
    for (int y = 0; y < reference.height(); ++y) {
        for (int x = 0; x < reference.width(); ++x) {
            const auto pixel = reference.pixelColor(x, y);
            require(pixel.alpha() == 255, "formula image backgrounds must be completely opaque");
            darkPixel = darkPixel || pixel.lightness() < 128;
        }
    }
    require(darkPixel && reference.pixelColor(0, 0) == QColor(Qt::white),
            "formula pins must contain visible black mathematics on a white background");
    const auto singleScale = convert(renderer, textContent(QStringLiteral("$\\frac{x}{y}$")));
    require(reference.size() == singleScale.image.size() * 2,
            "display scaling must preserve logical formula dimensions");
    auto narrow = textContent(QStringLiteral("$\\sqrt{x}+1+2+3+4+5+6+7+8+9+10+11+12$"));
    narrow.originalContent.textStyle->maximumWidth = 180;
    const auto wrapped = convert(renderer, narrow);
    narrow.originalContent.textStyle->maximumWidth = 1024;
    const auto unwrapped = convert(renderer, narrow);
    require(!wrapped.isFormattedText() && !unwrapped.isFormattedText() &&
                wrapped.image.width() < unwrapped.image.width() &&
                wrapped.image.height() > unwrapped.image.height(),
            "formula wrapping must use the configured text pin maximum width");
    const auto multiline = convert(
        renderer, textContent(QStringLiteral("\\begin{aligned}a&=b+c\\\\d&=e\\end{aligned}")));
    require(!multiline.isFormattedText(), "multiline environments must render as images");
    for (const auto& source :
         {QStringLiteral("$\\snowUndefined$"),
          QStringLiteral("$$") + QString(33000, QLatin1Char('x')) + QStringLiteral("$$"),
          QStringLiteral("Ordinary clipboard text")}) {
        const auto original = textContent(source);
        const auto fallback = convert(renderer, original);
        require(fallback.isFormattedText() && fallback.image == original.image &&
                    fallback.formattedDocument == original.formattedDocument &&
                    fallback.originalContent.text == source,
                "parse errors, renderer limits and ordinary text must retain the decoded content");
    }
    auto image = textContent(QStringLiteral("$x^2$"));
    image.kind = ScreenshotClipboardContentKind::Image;
    const auto unchanged = convert(renderer, image);
    require(unchanged.image == image.image &&
                unchanged.originalContent.text == image.originalContent.text,
            "image payloads must take precedence over accompanying formula text");
    auto markdown = textContent(QStringLiteral("$x^2$"));
    markdown.originalContent.textFormat = snow_shot::storage::PinnedTextFormat::Markdown;
    require(convert(renderer, markdown).isFormattedText(),
            "explicit Markdown content must retain its current text behavior");
    require(convert(renderer, textContent(QStringLiteral("$x$")), 9).isFormattedText(),
            "unsupported display scales must fall back through the renderer's existing limits");
    auto html = textContent(QStringLiteral("$x^2$"));
    html.originalContent.html = QStringLiteral("<p>$x^2$</p>");
    html.originalContent.text.clear();
    require(!convert(renderer, html).isFormattedText(),
            "HTML-only formula copies may use the decoded plain text");
}

void cancellationAndLifetime() {
    ScreenshotClipboardFormulaRenderer renderer;
    int obsolete = 0;
    renderer.convert(textContent(QStringLiteral("$x^2$")), 1,
                     [&](std::optional<ScreenshotClipboardContent>) { ++obsolete; });
    require(renderer.active(), "formula conversion must stay pending until rendering completes");
    renderer.cancel();
    require(!renderer.active(), "cancellation must clear formula pending state");
    require(!convert(renderer, textContent(QStringLiteral("$y^2$"))).isFormattedText() &&
                obsolete == 0,
            "cancelled callbacks must remain discarded after the worker processes later requests");
    renderer.convert(textContent(QStringLiteral("$x^2$")), 1,
                     [&](std::optional<ScreenshotClipboardContent>) { ++obsolete; });
    const auto latest = convert(renderer, textContent(QStringLiteral("$z^2$")));
    require(!latest.isFormattedText() && obsolete == 0,
            "superseded formula requests must not publish stale results");
    {
        ScreenshotClipboardFormulaRenderer closing;
        closing.convert(textContent(QStringLiteral("$x^2$")), 1,
                        [&](std::optional<ScreenshotClipboardContent>) { ++obsolete; });
    }
    static_cast<void>(convert(renderer, textContent(QStringLiteral("$w^2$"))));
    require(obsolete == 0, "destroying the recipient must discard queued formula callbacks");
}

void deferredFallbackCancellation() {
    using Renderer = ScreenshotClipboardFormulaRenderer;
    Renderer renderer;
    auto* worker = renderer.findChild<ScreenshotLatexRenderer*>();
    require(worker != nullptr, "formula conversion must expose its renderer child");
    int obsolete = 0;
    bool fallbackReached = false;
    const auto connection = QObject::connect(
        worker, &ScreenshotLatexRenderer::rendered, &renderer,
        [&](quint64, const QImage&, ScreenshotLatexRenderer::Error error) {
            if (error == ScreenshotLatexRenderer::Error::None)
                return;
            require(renderer.active(), "fallback decoding must retain the busy state");
            fallbackReached = true;
            renderer.cancel();
        });
    auto prepared = Renderer::prepare(textSnapshot(QStringLiteral("$\\snowUndefined$")));
    require(prepared.has_value(), "invalid formula must prepare a deferred fallback");
    renderer.convert(std::move(*prepared), 1,
                     [&](std::optional<ScreenshotClipboardContent>) { ++obsolete; });
    until([&] { return fallbackReached; });
    QObject::disconnect(connection);
    require(!renderer.active(), "cancelling fallback decoding clears the busy state");
    auto latest = Renderer::prepare(textSnapshot(QStringLiteral("$\\sqrt{x}$")));
    require(latest && !convert(renderer, std::move(*latest)).isFormattedText(),
            "a newer formula succeeds after cancelling a text fallback");
    until([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; });
    QCoreApplication::processEvents();
    require(obsolete == 0, "cancelled text fallback must not publish a stale result");

    // A formula can succeed even when the unused text fallback would exceed its
    // layout limits. Conversely, a failed formula must report a failed fallback.
    auto oversized = textSnapshot(QStringLiteral("$x") + QString(32000, QLatin1Char('\n')) +
                                  QStringLiteral("$"));
    require(!ScreenshotClipboardContentReader::decode(oversized),
            "oversized text fixture must exceed the decoder's layout budget");
    auto smallFormula = Renderer::prepare(oversized);
    require(smallFormula && !convert(renderer, std::move(*smallFormula)).isFormattedText(),
            "valid formulas do not depend on materializing an oversized text fallback");
    oversized.text.replace(1, 1, QStringLiteral("\\snowUndefined"));
    auto invalid = Renderer::prepare(oversized);
    require(invalid.has_value(), "invalid oversized text must still prepare a formula candidate");
    bool completed = false;
    renderer.convert(std::move(*invalid), 1, [&](std::optional<ScreenshotClipboardContent> result) {
        require(!result, "failed lazy text decoding must report no content");
        completed = true;
    });
    until([&] { return completed; });
    require(!renderer.active(), "failed fallback must clear the busy state");

    struct BeforeFallbackDelivery final : QObject {
        std::function<void()> action;
        bool fired = false;
        bool eventFilter(QObject*, QEvent* event) override {
            if (!fired && event->type() == QEvent::MetaCall) {
                fired = true;
                action();
            }
            return false;
        }
    } observer;
    auto closing = std::make_unique<Renderer>();
    observer.action = [&] {
        require(closing->active(), "closing during fallback must see a busy conversion");
        closing.reset();
    };
    auto& coordinator = ScreenshotExportCoordinator::shared();
    coordinator.installEventFilter(&observer);
    auto closingInput = Renderer::prepare(textSnapshot(QStringLiteral("$\\snowUndefined$")));
    require(closingInput.has_value(), "closing fixture must prepare a deferred fallback");
    closing->convert(std::move(*closingInput), 1,
                     [&](std::optional<ScreenshotClipboardContent>) { ++obsolete; });
    until([&] { return observer.fired; });
    coordinator.removeEventFilter(&observer);
    require(!closing && obsolete == 0,
            "destroying the recipient must discard a queued fallback completion");
}

void unavailableFallbackQueue() {
    using Renderer = ScreenshotClipboardFormulaRenderer;
    ScreenshotExportCoordinator::shared().shutdown();
    Renderer renderer;
    auto prepared = Renderer::prepare(textSnapshot(QStringLiteral("$\\snowUndefined$")));
    require(prepared.has_value(), "queue rejection fixture must prepare a deferred formula");
    bool completed = false;
    renderer.convert(std::move(*prepared), 1,
                     [&](std::optional<ScreenshotClipboardContent> result) {
                         require(!result, "an unavailable fallback queue reports failure");
                         completed = true;
                     });
    until([&] { return completed; });
    require(!renderer.active(), "queue rejection must clear conversion busy state");
}

} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    try {
        const int font = QFontDatabase::addApplicationFont(
            QStringLiteral(":/recording-test-fonts/SnowRecordingTestSans-Regular.ttf"));
        require(font >= 0, "formula text fallbacks need an offscreen fixture font");
        QGuiApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font).first()));
        recognition();
        deferredTextPreparation();
        imagesAndFallbacks();
        cancellationAndLifetime();
        deferredFallbackCancellation();
        unavailableFallbackQueue();
        std::cout << "Clipboard formula tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
