#include "snow_shot/presentation/screenshotimageconversionview.h"

#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/scroll_area.h"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextTable>
#include <QTextBlock>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {
class InertDocument final : public QTextDocument {
  public:
    explicit InertDocument(QObject* parent) : QTextDocument(parent) {}

  protected:
    QVariant loadResource(int, const QUrl&) override {
        return {};
    }
};
} // namespace

ScreenshotImageConversionView::ScreenshotImageConversionView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("screenshotImageConversionView"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_browser = new QTextBrowser(this);
    m_browser->setObjectName(QStringLiteral("screenshotImageConversionPreview"));
    m_browser->setDocument(new InertDocument(m_browser));
    m_browser->setFrameStyle(QFrame::NoFrame);
    m_browser->setOpenLinks(false);
    m_browser->setOpenExternalLinks(false);
    m_browser->setVerticalScrollBar(new adqt::widgets::AdScrollBar(Qt::Vertical, m_browser));
    m_browser->setHorizontalScrollBar(new adqt::widgets::AdScrollBar(Qt::Horizontal, m_browser));
    m_browser->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_browser->setLineWrapMode(QTextEdit::WidgetWidth);
    layout->addWidget(m_browser, 1);
    setFocusProxy(m_browser);
    connect(m_browser, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.isValid() &&
            (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http"))) {
            emit linkActivated(url);
        }
    });
    m_status = new QWidget(this);
    auto* statusLayout = new QHBoxLayout(m_status);
    statusLayout->setContentsMargins(12, 6, 12, 6);
    m_statusText = new QLabel(m_status);
    m_statusText->setObjectName(QStringLiteral("screenshotImageConversionStatus"));
    m_statusText->setWordWrap(true);
    m_statusText->setTextFormat(Qt::PlainText);
    m_retry = new adqt::widgets::AdButton(m_status);
    m_retry->setObjectName(QStringLiteral("screenshotImageConversionRetry"));
    m_retry->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    m_retry->setSizeClass(adqt::widgets::AdButton::SizeClass::Small);
    connect(m_retry, &adqt::widgets::AdButton::clicked, this,
            &ScreenshotImageConversionView::retryRequested);
    statusLayout->addWidget(m_statusText, 1);
    statusLayout->addWidget(m_retry);
    layout->addWidget(m_status);
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            [this]() { render(); });
    retranslate();
    render();
}

void ScreenshotImageConversionView::setContent(SnowShotImageConversionFormat format,
                                               const QString& source, bool busy,
                                               const QString& error) {
    const bool changed = m_format != format || m_source != source;
    m_format = format;
    m_source = source;
    m_busy = busy;
    m_error = error;
    if (changed) {
        render();
    }
    retranslate();
}

void ScreenshotImageConversionView::retranslate() {
    m_browser->setAccessibleName(m_format == SnowShotImageConversionFormat::Markdown
                                     ? tr("Markdown preview")
                                     : tr("HTML preview"));
    m_retry->setText(tr("Retry"));
    m_status->setVisible(!m_busy && !m_error.isEmpty());
    m_retry->setVisible(!m_busy && !m_error.isEmpty());
    m_statusText->setText(m_source.isEmpty() ? m_error : tr("Incomplete result: %1").arg(m_error));
}

void ScreenshotImageConversionView::render() {
    if (m_browser == nullptr) {
        return;
    }
    const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(m_browser);
    QPalette colors = m_browser->palette();
    colors.setColor(QPalette::Base, theme.colorBgContainer);
    colors.setColor(QPalette::Text, theme.colorText);
    colors.setColor(QPalette::Link, theme.colorLink);
    m_browser->setPalette(colors);
    QTextDocument* document = m_browser->document();
    QFont font = theme.appFont;
    font.setPixelSize(qRound(theme.fontSize));
    document->setDefaultFont(font);
    document->setDocumentMargin(12);
    const QString mono = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    // Theme fills carry alpha. Rich-text CSS needs the composited opaque color,
    // otherwise a translucent white dark-theme fill turns into solid white.
    const QColor fill = theme.colorFillTertiary;
    const QColor base = theme.colorBgContainer;
    const auto blended = [fill](int foreground, int background) {
        return qRound(fill.alphaF() * foreground + (1.0 - fill.alphaF()) * background);
    };
    const QColor codeBackground(blended(fill.red(), base.red()),
                                blended(fill.green(), base.green()),
                                blended(fill.blue(), base.blue()));
    document->setDefaultStyleSheet(
        QStringLiteral(
            "body { color: %1; } p { margin-top: 6px; margin-bottom: 6px; } "
            "h1,h2,h3 { margin-top: 12px; margin-bottom: 8px; } "
            "a { color: %2; } pre,code { font-family: '%3'; color: %1; background-color: %4; } "
            "blockquote { margin-left: 16px; color: %5; } "
            "table { border-collapse: collapse; } td,th { border: 1px solid %6; padding: 6px; }")
            .arg(theme.colorText.name(), theme.colorLink.name(), mono, codeBackground.name(),
                 theme.colorTextSecondary.name(), theme.colorBorder.name()));
    const QTextCursor previous = m_browser->textCursor();
    const int previousAnchor = previous.anchor();
    const int previousPosition = previous.position();
    const int scroll = m_browser->verticalScrollBar()->value();
    const int horizontal = m_browser->horizontalScrollBar()->value();
    const bool follow =
        scroll >= m_browser->verticalScrollBar()->maximum() - 2 && !previous.hasSelection();
    if (m_format == SnowShotImageConversionFormat::Markdown) {
        document->setMarkdown(m_source, QTextDocument::MarkdownDialectGitHub);
    } else {
        document->setHtml(m_source);
    }
    // Markdown import does not apply the HTML stylesheet to code blocks or links.
    QVector<QTextCursor> links;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const auto text = fragment.fragment();
            if (text.isValid() && text.charFormat().isAnchor()) {
                QTextCursor link(document);
                link.setPosition(text.position());
                link.setPosition(text.position() + text.length(), QTextCursor::KeepAnchor);
                links.push_back(link);
            }
        }
        auto blockFormat = block.blockFormat();
        if (blockFormat.hasProperty(QTextFormat::BlockCodeFence) ||
            blockFormat.hasProperty(QTextFormat::BlockCodeLanguage) ||
            blockFormat.nonBreakableLines()) {
            QTextCursor code(block);
            blockFormat.setBackground(codeBackground);
            blockFormat.setLeftMargin(8);
            blockFormat.setRightMargin(8);
            code.setBlockFormat(blockFormat);
            code.select(QTextCursor::BlockUnderCursor);
            QTextCharFormat characters;
            characters.setFontFamilies({mono});
            characters.setForeground(theme.colorText);
            characters.setBackground(codeBackground);
            code.mergeCharFormat(characters);
        }
    }
    QTextCharFormat linkFormat;
    linkFormat.setForeground(theme.colorLink);
    for (auto& link : links) {
        link.mergeCharFormat(linkFormat);
    }
    const int end = std::max(0, document->characterCount() - 1);
    QTextCursor restored(document);
    restored.setPosition(std::min(previousAnchor, end));
    restored.setPosition(std::min(previousPosition, end), QTextCursor::KeepAnchor);
    m_browser->setTextCursor(restored);
    m_browser->verticalScrollBar()->setValue(follow ? m_browser->verticalScrollBar()->maximum()
                                                    : scroll);
    m_browser->horizontalScrollBar()->setValue(horizontal);
}

bool ScreenshotImageConversionView::copyToClipboard() const {
    if (m_source.isEmpty()) {
        return false;
    }
    const QTextCursor cursor = m_browser->textCursor();
    QApplication::clipboard()->setText(
        cursor.hasSelection() ? QTextDocumentFragment(cursor).toPlainText() : m_source);
    return true;
}

void ScreenshotImageConversionView::selectAll() {
    m_browser->selectAll();
}

void ScreenshotImageConversionView::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
}
