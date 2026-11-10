// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotclipboardformularenderer.h"

#include <QSet>
#include <QStringView>

#include <utility>

namespace {

bool escaped(QStringView text, qsizetype position) {
    qsizetype slashes = 0;
    while (position > 0 && text[--position] == QLatin1Char('\\'))
        ++slashes;
    return slashes % 2 != 0;
}

bool containsMathDelimiter(QStringView text) {
    for (qsizetype index = 0; index < text.size(); ++index) {
        if (text[index] == QLatin1Char('$'))
            return true;
        if (text[index] == QLatin1Char('\\') && index + 1 < text.size()) {
            if (QStringView(u"()[]").contains(text[index + 1]))
                return true;
            ++index; // Consume escaped punctuation, including dollars and backslashes.
        }
    }
    return false;
}

// Text/font arguments and environment names are not bare prose. Skip balanced groups,
// including escaped braces, while leaving mathematical arguments to the normal scan.
qsizetype groupEnd(QStringView text, qsizetype position) {
    while (position < text.size() && text[position].isSpace())
        ++position;
    if (position == text.size() || text[position] != QLatin1Char('{'))
        return position;
    int depth = 0;
    for (; position < text.size(); ++position) {
        if (text[position] == QLatin1Char('\\') && position + 1 < text.size()) {
            ++position;
            continue;
        }
        if (text[position] == QLatin1Char('{'))
            ++depth;
        else if (text[position] == QLatin1Char('}') && --depth == 0)
            return position + 1;
    }
    return position;
}

bool bareFormula(QStringView text) {
    if (text.startsWith(u"```") || text.startsWith(u"\\\\") || text.startsWith(u".\\") ||
        text.startsWith(u"..\\") || text.startsWith(u"./") || text.startsWith(u"../") ||
        text.startsWith(u"/") || text.contains(u"://") ||
        (text.size() >= 3 && text[0].isLetter() && text[1] == QLatin1Char(':') &&
         (text[2] == QLatin1Char('\\') || text[2] == QLatin1Char('/'))))
        return false;
    static const QSet<QString> mathCommands = {
        QStringLiteral("frac"),       QStringLiteral("dfrac"),    QStringLiteral("tfrac"),
        QStringLiteral("sqrt"),       QStringLiteral("sum"),      QStringLiteral("prod"),
        QStringLiteral("int"),        QStringLiteral("iint"),     QStringLiteral("iiint"),
        QStringLiteral("oint"),       QStringLiteral("lim"),      QStringLiteral("sin"),
        QStringLiteral("cos"),        QStringLiteral("tan"),      QStringLiteral("log"),
        QStringLiteral("ln"),         QStringLiteral("infty"),    QStringLiteral("partial"),
        QStringLiteral("nabla"),      QStringLiteral("alpha"),    QStringLiteral("beta"),
        QStringLiteral("gamma"),      QStringLiteral("delta"),    QStringLiteral("epsilon"),
        QStringLiteral("varepsilon"), QStringLiteral("zeta"),     QStringLiteral("eta"),
        QStringLiteral("theta"),      QStringLiteral("vartheta"), QStringLiteral("iota"),
        QStringLiteral("kappa"),      QStringLiteral("lambda"),   QStringLiteral("mu"),
        QStringLiteral("nu"),         QStringLiteral("xi"),       QStringLiteral("pi"),
        QStringLiteral("varpi"),      QStringLiteral("rho"),      QStringLiteral("varrho"),
        QStringLiteral("sigma"),      QStringLiteral("varsigma"), QStringLiteral("tau"),
        QStringLiteral("upsilon"),    QStringLiteral("phi"),      QStringLiteral("varphi"),
        QStringLiteral("chi"),        QStringLiteral("psi"),      QStringLiteral("omega"),
        QStringLiteral("Gamma"),      QStringLiteral("Delta"),    QStringLiteral("Theta"),
        QStringLiteral("Lambda"),     QStringLiteral("Xi"),       QStringLiteral("Pi"),
        QStringLiteral("Sigma"),      QStringLiteral("Upsilon"),  QStringLiteral("Phi"),
        QStringLiteral("Psi"),        QStringLiteral("Omega"),    QStringLiteral("mathbb"),
        QStringLiteral("mathcal"),    QStringLiteral("mathfrak")};
    static const QSet<QString> textCommands = {
        QStringLiteral("text"),     QStringLiteral("textrm"), QStringLiteral("textnormal"),
        QStringLiteral("textbf"),   QStringLiteral("textit"), QStringLiteral("operatorname"),
        QStringLiteral("mathrm"),   QStringLiteral("mathbf"), QStringLiteral("mathsf"),
        QStringLiteral("mathtt"),   QStringLiteral("mathbb"), QStringLiteral("mathcal"),
        QStringLiteral("mathfrak"), QStringLiteral("color"),  QStringLiteral("textcolor")};
    static const QSet<QString> environments = {
        QStringLiteral("matrix"),      QStringLiteral("pmatrix"),   QStringLiteral("bmatrix"),
        QStringLiteral("Bmatrix"),     QStringLiteral("vmatrix"),   QStringLiteral("Vmatrix"),
        QStringLiteral("smallmatrix"), QStringLiteral("cases"),     QStringLiteral("array"),
        QStringLiteral("aligned"),     QStringLiteral("alignedat"), QStringLiteral("gathered"),
        QStringLiteral("split"),       QStringLiteral("equation"),  QStringLiteral("equation*"),
        QStringLiteral("align"),       QStringLiteral("align*"),    QStringLiteral("gather"),
        QStringLiteral("gather*")};
    static const QSet<QString> operators = {
        QStringLiteral("sin"), QStringLiteral("cos"), QStringLiteral("tan"), QStringLiteral("log"),
        QStringLiteral("lim"), QStringLiteral("max"), QStringLiteral("min"), QStringLiteral("det")};
    bool mathematical = false;
    int depth = 0;
    for (qsizetype index = 0; index < text.size();) {
        if (text[index] == QLatin1Char('\\')) {
            const auto start = ++index;
            while (index < text.size() && text[index].isLetter())
                ++index;
            if (index == start) {
                if (index < text.size())
                    ++index; // Escaped punctuation, spacing, or a line break.
                continue;
            }
            const QString command = text.mid(start, index - start).toString();
            mathematical = mathematical || mathCommands.contains(command);
            if (command == QLatin1String("begin") || command == QLatin1String("end")) {
                const auto end = groupEnd(text, index);
                const auto group = text.mid(index, end - index).trimmed();
                if (group.size() < 2 ||
                    !environments.contains(group.mid(1, group.size() - 2).toString()))
                    return false;
                mathematical = true;
                index = end;
            } else if (textCommands.contains(command)) {
                index = groupEnd(text, index);
            }
        } else if (text[index].isLetter()) {
            const auto start = index++;
            while (index < text.size() && text[index].isLetter())
                ++index;
            // Common adjacent variables/differentials are valid, whereas words such as
            // "formula" in "The formula is \\frac{a}{b}" indicate surrounding prose.
            if (depth == 0 && index - start > 2 &&
                !operators.contains(text.mid(start, index - start).toString()))
                return false;
        } else {
            if (text[index] == QLatin1Char('{'))
                ++depth;
            else if (text[index] == QLatin1Char('}') && depth > 0)
                --depth;
            ++index;
        }
    }
    return mathematical;
}

} // namespace

std::optional<QString> ScreenshotClipboardFormulaRenderer::formulaSource(const QString& text) {
    // Bound the raw input before even trimming: oversized padded input must not
    // scan or copy an arbitrarily large clipboard string on the calling thread.
    // A display delimiter pair can add four code units to the 32768-unit formula.
    if (text.size() > 32772)
        return std::nullopt;
    const QStringView source = QStringView(text).trimmed();
    for (const auto& delimiters : {std::pair{QStringView(u"$$"), QStringView(u"$$")},
                                   std::pair{QStringView(u"$"), QStringView(u"$")},
                                   std::pair{QStringView(u"\\("), QStringView(u"\\)")},
                                   std::pair{QStringView(u"\\["), QStringView(u"\\]")}}) {
        const auto [opening, closing] = delimiters;
        if (!source.startsWith(opening))
            continue;
        if (source.size() < opening.size() + closing.size() || !source.endsWith(closing) ||
            escaped(source, source.size() - closing.size()))
            return std::nullopt;
        const auto body =
            source.mid(opening.size(), source.size() - opening.size() - closing.size()).trimmed();
        return body.isEmpty() || body.size() > 32768 || containsMathDelimiter(body)
                   ? std::nullopt
                   : std::optional(body.toString());
    }
    if (source.isEmpty() || source.size() > 32768 || containsMathDelimiter(source) ||
        !bareFormula(source))
        return std::nullopt;
    return source.toString();
}

std::optional<ScreenshotClipboardFormulaRenderer::PreparedContent>
ScreenshotClipboardFormulaRenderer::prepare(
    ScreenshotClipboardContentSnapshot snapshot,
    ScreenshotClipboardContentReader::CancellationCheck cancelled,
    ScreenshotClipboardContentReader::AllocationCheck allocate) {
    std::optional<DeferredFormula> deferred;
    auto decoded = ScreenshotClipboardContentReader::decode(
        std::move(snapshot), cancelled, std::move(allocate),
        [&deferred](const ScreenshotClipboardContentSnapshot& text, const QString& plainText) {
            if (text.textFormat != snow_shot::storage::PinnedTextFormat::PlainText ||
                (text.textStyle && text.textStyle->colorSwatch))
                return false;
            auto source = formulaSource(plainText);
            if (!source)
                return false;
            // Do not retain failed image payloads or retry file imports on formula failure.
            ScreenshotClipboardContentSnapshot fallback;
            fallback.text = text.text;
            fallback.html = text.html;
            fallback.textStyle = text.textStyle;
            fallback.textFormat = text.textFormat;
            fallback.baseColor = text.baseColor;
            fallback.devicePixelRatio = text.devicePixelRatio;
            deferred = DeferredFormula{std::move(*source), std::move(fallback)};
            return true;
        });
    if (cancelled && cancelled())
        return std::nullopt;
    if (deferred)
        return PreparedContent(std::move(*deferred));
    if (decoded && decoded->isValid())
        return PreparedContent(std::move(*decoded));
    return std::nullopt;
}

ScreenshotClipboardFormulaRenderer::ScreenshotClipboardFormulaRenderer(QObject* parent)
    : QObject(parent), m_renderer(this) {
    connect(&m_renderer, &ScreenshotLatexRenderer::rendered, this,
            [this](quint64 token, const QImage& image, ScreenshotLatexRenderer::Error error) {
                if (token != m_token || !m_fallback)
                    return;
                auto fallback = std::move(*m_fallback);
                m_fallback.reset();
                m_token = 0;
                m_renderer.clearCache();
                if (error == ScreenshotLatexRenderer::Error::None && !image.isNull()) {
                    ScreenshotClipboardContent rendered;
                    if (auto* content = std::get_if<ScreenshotClipboardContent>(&fallback))
                        rendered.sourceIdentity = std::move(content->sourceIdentity);
                    rendered.image = image;
                    finish(std::move(rendered));
                    return;
                }
                if (auto* content = std::get_if<ScreenshotClipboardContent>(&fallback)) {
                    finish(std::move(*content));
                    return;
                }
                const quint64 generation = m_generation;
                auto content = std::make_shared<std::optional<ScreenshotClipboardContent>>();
                m_fallbackJob = ScreenshotExportCoordinator::shared().submit(
                    this, ScreenshotExportCoordinator::Priority::Foreground,
                    [snapshot = std::get<ScreenshotClipboardContentSnapshot>(std::move(fallback)),
                     content](const ScreenshotExportCancellation& cancellation) mutable {
                        *content = ScreenshotClipboardContentReader::decode(
                            std::move(snapshot),
                            [&cancellation] { return cancellation.isCancellationRequested(); });
                        return ScreenshotExportTaskResult{};
                    },
                    [this, generation, content](const ScreenshotExportTaskResult& result) {
                        if (generation == m_generation)
                            finish(result.succeeded() ? std::move(*content) : std::nullopt);
                    });
                if (!m_fallbackJob.isValid())
                    finish(std::nullopt);
            });
}

ScreenshotClipboardFormulaRenderer::~ScreenshotClipboardFormulaRenderer() {
    cancel();
}

void ScreenshotClipboardFormulaRenderer::convert(PreparedContent prepared, qreal devicePixelRatio,
                                                 Completion completion) {
    cancel();
    QString source;
    int width = 1024;
    if (auto* deferred = std::get_if<DeferredFormula>(&prepared)) {
        source = std::move(deferred->source);
        if (deferred->text.textStyle)
            width = deferred->text.textStyle->maximumWidth;
        m_fallback = std::move(deferred->text);
    } else {
        // File imports can already contain decoded text. Preserve their conversion
        // behavior and reuse the existing fallback without decoding it a second time.
        auto content = std::get<ScreenshotClipboardContent>(std::move(prepared));
        const auto detected = content.isFormattedText() &&
                                      content.originalContent.textFormat ==
                                          snow_shot::storage::PinnedTextFormat::PlainText &&
                                      !(content.originalContent.textStyle &&
                                        content.originalContent.textStyle->colorSwatch)
                                  ? formulaSource(content.originalContent.text.isEmpty()
                                                      ? content.plainText
                                                      : content.originalContent.text)
                                  : std::nullopt;
        if (!detected) {
            completion(std::move(content));
            return;
        }
        source = *detected;
        if (content.originalContent.textStyle)
            width = content.originalContent.textStyle->maximumWidth;
        m_fallback = std::move(content);
    }
    m_completion = std::move(completion);
    m_token =
        m_renderer.request(source, QSize(width, 8192), devicePixelRatio, Qt::black, Qt::white);
}

void ScreenshotClipboardFormulaRenderer::finish(std::optional<ScreenshotClipboardContent> content) {
    m_fallbackJob = {};
    auto completion = std::exchange(m_completion, {});
    if (completion)
        completion(std::move(content));
}

void ScreenshotClipboardFormulaRenderer::cancel() {
    ++m_generation;
    m_fallbackJob.cancel();
    m_fallbackJob = {};
    m_renderer.clearCache();
    m_token = 0;
    m_fallback.reset();
    m_completion = {};
}
