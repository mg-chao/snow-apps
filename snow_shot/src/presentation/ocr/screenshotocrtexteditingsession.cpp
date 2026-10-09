#include "snow_shot/presentation/screenshotocrtexteditingsession.h"

#include "snow_shot/presentation/screenshotocrtexttransform.h"

#include <algorithm>
#include <utility>

ScreenshotOcrTextEditingSession::ScreenshotOcrTextEditingSession(QString originalText,
                                                                 bool preserveSource)
    : m_originalText(std::move(originalText)), m_preserveSource(preserveSource) {
    m_document.setUndoRedoEnabled(false);
    if (m_preserveSource)
        m_preservedSource = m_originalText;
    m_document.setPlainText(m_originalText);
    if (m_preserveSource) {
        updatePreservedSourceMapping();
        m_sourceChangeConnection =
            QObject::connect(&m_document, &QTextDocument::contentsChange,
                             [this](int position, int removed, int added) {
                                 synchronizePreservedSource(position, removed, added);
                             });
    }
    m_history.push_back(m_originalText);
    m_documentChangeConnection = QObject::connect(&m_document, &QTextDocument::contentsChanged,
                                                  [this]() { recordCurrentText(); });
}

ScreenshotOcrTextEditingSession::~ScreenshotOcrTextEditingSession() {
    // The document outlives the history members during teardown. Its callbacks must
    // stop before member destruction, while the editor's document tracking stays intact.
    QObject::disconnect(m_sourceChangeConnection);
    QObject::disconnect(m_documentChangeConnection);
}

const QString& ScreenshotOcrTextEditingSession::originalText() const {
    return m_originalText;
}

QString ScreenshotOcrTextEditingSession::text() const {
    return m_preserveSource ? m_preservedSource : m_document.toPlainText();
}

QTextDocument* ScreenshotOcrTextEditingSession::document() {
    return &m_document;
}

bool ScreenshotOcrTextEditingSession::replaceText(const QString& text) {
    if (text == this->text()) {
        return false;
    }
    if (m_historyIndex + 1 < m_history.size()) {
        m_history.resize(m_historyIndex + 1);
    }
    m_history.push_back(text);
    ++m_historyIndex;
    applyText(text);
    return true;
}

bool ScreenshotOcrTextEditingSession::reset() {
    clearTransforms();
    return replaceText(m_originalText);
}

bool ScreenshotOcrTextEditingSession::applyInitialTransforms(const QString& formatting,
                                                             const QString& punctuation,
                                                             const QString& smartText) {
    m_formatting = formatting == QStringLiteral("keep") || formatting == QStringLiteral("remove") ||
                           formatting == QStringLiteral("smart")
                       ? formatting
                       : QString{};
    m_punctuation = punctuation == QStringLiteral("half") || punctuation == QStringLiteral("full")
                        ? punctuation
                        : QString{};
    if (m_formatting.isEmpty() && m_punctuation.isEmpty()) {
        m_transformBaseline.clear();
        return false;
    }
    m_transformBaseline = text();
    m_smartText = smartText;
    return replaceText(transformedText());
}

bool ScreenshotOcrTextEditingSession::setFormatting(const QString& value,
                                                    const QString& smartText) {
    QString normalized;
    if (value == QStringLiteral("keep") || value == QStringLiteral("remove") ||
        value == QStringLiteral("smart")) {
        normalized = value;
    }
    if (normalized == m_formatting) {
        return false;
    }
    if (m_formatting.isEmpty() && m_punctuation.isEmpty()) {
        m_transformBaseline = text();
    }
    m_formatting = normalized;
    if (normalized == QStringLiteral("smart")) {
        m_smartText = smartText;
    }
    const bool changed = replaceText(transformedText());
    if (m_formatting.isEmpty() && m_punctuation.isEmpty()) {
        m_transformBaseline.clear();
    }
    return changed;
}

bool ScreenshotOcrTextEditingSession::setPunctuation(const QString& value) {
    QString normalized;
    if (value == QStringLiteral("half") || value == QStringLiteral("full")) {
        normalized = value;
    }
    if (normalized == m_punctuation) {
        return false;
    }
    if (m_formatting.isEmpty() && m_punctuation.isEmpty()) {
        m_transformBaseline = text();
    }
    m_punctuation = normalized;
    const bool changed = replaceText(transformedText());
    if (m_formatting.isEmpty() && m_punctuation.isEmpty()) {
        m_transformBaseline.clear();
    }
    return changed;
}

void ScreenshotOcrTextEditingSession::clearTransforms() {
    m_transformBaseline.clear();
    m_smartText.clear();
    m_formatting.clear();
    m_punctuation.clear();
}

const QString& ScreenshotOcrTextEditingSession::formatting() const {
    return m_formatting;
}

const QString& ScreenshotOcrTextEditingSession::punctuation() const {
    return m_punctuation;
}

void ScreenshotOcrTextEditingSession::establishBaseline(const QString& text) {
    clearTransforms();
    m_originalText = text;
    establishHistory(text);
}

void ScreenshotOcrTextEditingSession::establishHistory(const QString& text) {
    clearTransforms();
    m_history = {text};
    m_historyIndex = 0;
    applyText(text);
}

void ScreenshotOcrTextEditingSession::replaceTextWithoutHistory(const QString& text) {
    applyText(text);
}

void ScreenshotOcrTextEditingSession::recordCurrentText() {
    if (m_applying) {
        return;
    }
    if (m_preserveSource)
        synchronizePreservedSource();
    const QString current = text();
    if (current == m_history.at(m_historyIndex)) {
        return;
    }
    clearTransforms();
    if (m_historyIndex + 1 < m_history.size()) {
        m_history.resize(m_historyIndex + 1);
    }
    m_history.push_back(current);
    ++m_historyIndex;
}

void ScreenshotOcrTextEditingSession::applyText(const QString& text) {
    m_applying = true;
    if (m_preserveSource)
        m_preservedSource = text;
    m_document.setPlainText(text);
    if (m_preserveSource)
        updatePreservedSourceMapping();
    m_applying = false;
}

void ScreenshotOcrTextEditingSession::updatePreservedSourceMapping() {
    m_documentRawText = m_document.toRawText();
    m_documentSourceOffsets.clear();
    m_documentSourceOffsets.reserve(m_documentRawText.size() + 1);
    m_documentSourceOffsets.push_back(0);
    for (qsizetype index = 0; index < m_preservedSource.size(); ++index) {
        // QTextDocument stores every paragraph as one U+2029, including a CRLF pair.
        if (m_preservedSource.at(index) == QLatin1Char('\r') &&
            index + 1 < m_preservedSource.size() &&
            m_preservedSource.at(index + 1) == QLatin1Char('\n')) {
            ++index;
        }
        m_documentSourceOffsets.push_back(index + 1);
    }
}

void ScreenshotOcrTextEditingSession::synchronizePreservedSource(int position, int charsRemoved,
                                                                 int charsAdded) {
    if (m_applying)
        return;
    const QString current = m_document.toRawText();
    if (current == m_documentRawText)
        return;
    const qsizetype start = std::clamp<qsizetype>(position, 0, m_documentRawText.size());
    const qsizetype end = std::min(start + charsRemoved, m_documentRawText.size());
    const qsizetype sourceStart = m_documentSourceOffsets.at(start);
    const qsizetype sourceEnd = m_documentSourceOffsets.at(end);
    QString inserted = current.mid(start, charsAdded);
    inserted.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    m_preservedSource.replace(sourceStart, sourceEnd - sourceStart, inserted);
    updatePreservedSourceMapping();
}

void ScreenshotOcrTextEditingSession::synchronizePreservedSource() {
    const QString current = m_document.toRawText();
    if (current == m_documentRawText)
        return;
    // A detached document may have no layout and emit only contentsChanged. Preserve
    // unchanged source spans using its raw text until the editor installs a layout.
    qsizetype prefix = 0;
    const qsizetype commonSize = std::min(current.size(), m_documentRawText.size());
    while (prefix < commonSize && current.at(prefix) == m_documentRawText.at(prefix))
        ++prefix;
    qsizetype suffix = 0;
    while (suffix < commonSize - prefix &&
           current.at(current.size() - suffix - 1) ==
               m_documentRawText.at(m_documentRawText.size() - suffix - 1)) {
        ++suffix;
    }
    synchronizePreservedSource(static_cast<int>(prefix),
                               static_cast<int>(m_documentRawText.size() - prefix - suffix),
                               static_cast<int>(current.size() - prefix - suffix));
}

QString ScreenshotOcrTextEditingSession::transformedText() const {
    return snow_shot::presentation::applyOcrTextTransforms(
        m_formatting == QStringLiteral("smart") ? m_smartText : m_transformBaseline, m_formatting,
        m_punctuation);
}

void ScreenshotOcrTextEditingSession::undo() {
    if (!canUndo()) {
        return;
    }
    --m_historyIndex;
    clearTransforms();
    applyText(m_history.at(m_historyIndex));
}

void ScreenshotOcrTextEditingSession::redo() {
    if (!canRedo()) {
        return;
    }
    ++m_historyIndex;
    clearTransforms();
    applyText(m_history.at(m_historyIndex));
}

bool ScreenshotOcrTextEditingSession::canUndo() const {
    return m_historyIndex > 0;
}

bool ScreenshotOcrTextEditingSession::canRedo() const {
    return m_historyIndex + 1 < m_history.size();
}
