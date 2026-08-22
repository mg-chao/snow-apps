#include "snow_shot/presentation/screenshotocrtexteditingsession.h"

#include "snow_shot/presentation/screenshotocrtexttransform.h"

#include <utility>

ScreenshotOcrTextEditingSession::ScreenshotOcrTextEditingSession(QString originalText)
    : m_originalText(std::move(originalText)) {
    m_document.setUndoRedoEnabled(false);
    m_document.setPlainText(m_originalText);
    m_history.push_back(m_originalText);
    QObject::connect(&m_document, &QTextDocument::contentsChanged, [this]() {
        recordCurrentText();
    });
}

const QString& ScreenshotOcrTextEditingSession::originalText() const {
    return m_originalText;
}

QString ScreenshotOcrTextEditingSession::text() const {
    return m_document.toPlainText();
}

QTextDocument* ScreenshotOcrTextEditingSession::document() {
    return &m_document;
}

const QTextDocument* ScreenshotOcrTextEditingSession::document() const {
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

bool ScreenshotOcrTextEditingSession::setFormatting(const QString& value) {
    QString normalized;
    if (value == QStringLiteral("keep") || value == QStringLiteral("remove")) {
        normalized = value;
    }
    if (normalized == m_formatting) {
        return false;
    }
    if (m_formatting.isEmpty() && m_punctuation.isEmpty()) {
        m_transformBaseline = text();
    }
    m_formatting = normalized;
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
    m_document.setPlainText(text);
    m_applying = false;
}

QString ScreenshotOcrTextEditingSession::transformedText() const {
    QString transformed = m_transformBaseline;
    if (m_formatting == QStringLiteral("remove")) {
        transformed = snow_shot::presentation::removeOcrLineBreaks(transformed);
    }
    if (m_punctuation == QStringLiteral("half")) {
        transformed = snow_shot::presentation::convertOcrPunctuation(transformed, false);
    } else if (m_punctuation == QStringLiteral("full")) {
        transformed = snow_shot::presentation::convertOcrPunctuation(transformed, true);
    }
    return transformed;
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
