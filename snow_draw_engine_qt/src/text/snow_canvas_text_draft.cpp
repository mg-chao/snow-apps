#include "snow_canvas_text_draft.h"

#include <QInputMethodEvent>
#include <QTextBoundaryFinder>

#include <limits>

namespace {

int boundedTextPosition(qsizetype position) {
    constexpr qsizetype kMaximumPosition = std::numeric_limits<int>::max();
    return static_cast<int>(qBound(qsizetype{0}, position, kMaximumPosition));
}

int boundedTextLength(const QString& text) {
    return boundedTextPosition(text.size());
}

QString normalizedInsertedText(QString text) {
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

int previousGraphemeBoundary(const QString& text, int position) {
    const int bounded = qBound(0, position, boundedTextLength(text));
    if (bounded <= 0) {
        return 0;
    }

    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.setPosition(bounded);
    const qsizetype previous = finder.toPreviousBoundary();
    return previous >= 0 ? boundedTextPosition(previous) : bounded - 1;
}

int nextGraphemeBoundary(const QString& text, int position) {
    const int textLength = boundedTextLength(text);
    const int bounded = qBound(0, position, textLength);
    if (bounded >= textLength) {
        return textLength;
    }

    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    finder.setPosition(bounded);
    const qsizetype next = finder.toNextBoundary();
    return next >= 0 ? boundedTextPosition(next) : bounded + 1;
}

int preeditCursorFromAttributes(const QInputMethodEvent& event) {
    const QString& preedit = event.preeditString();
    int cursor = boundedTextLength(preedit);
    for (const QInputMethodEvent::Attribute& attribute : event.attributes()) {
        if (attribute.type == QInputMethodEvent::Cursor) {
            cursor = attribute.start;
            break;
        }
    }
    return qBound(0, cursor, boundedTextLength(preedit));
}

} // namespace

void SnowCanvasTextDraft::reset() {
    m_text.clear();
    m_cursorPosition = 0;
    m_anchorPosition = 0;
    m_undoStack.clear();
    m_redoStack.clear();
    clearPreedit();
}

void SnowCanvasTextDraft::releaseRetainedState() {
    reset();
    std::vector<Snapshot>().swap(m_undoStack);
    std::vector<Snapshot>().swap(m_redoStack);
}

std::size_t SnowCanvasTextDraft::retainedHistoryStorageBytes() const {
    return (m_undoStack.capacity() + m_redoStack.capacity()) * sizeof(Snapshot);
}

void SnowCanvasTextDraft::begin(const QString& text) {
    reset();
    m_text = text;
    m_cursorPosition = boundedTextLength(m_text);
    m_anchorPosition = m_cursorPosition;
}

const QString& SnowCanvasTextDraft::text() const {
    return m_text;
}

QString SnowCanvasTextDraft::displayText() const {
    if (m_preeditText.isEmpty()) {
        return m_text;
    }

    QString text = m_text;
    const int textLength = boundedTextLength(text);
    const int start = qBound(0, m_preeditStart, textLength);
    const int length = qBound(0, m_preeditReplacementLength, textLength - start);
    text.replace(start, length, m_preeditText);
    return text;
}

QString SnowCanvasTextDraft::selectedText() const {
    if (!hasSelection()) {
        return {};
    }
    return m_text.mid(selectionStart(), selectionEnd() - selectionStart());
}

QString SnowCanvasTextDraft::inputMethodCurrentSelection() const {
    return m_preeditText.isEmpty() ? selectedText() : QString();
}

int SnowCanvasTextDraft::cursorPosition() const {
    return m_cursorPosition;
}

int SnowCanvasTextDraft::anchorPosition() const {
    return m_anchorPosition;
}

int SnowCanvasTextDraft::displayCursorPosition() const {
    if (m_preeditText.isEmpty()) {
        return m_cursorPosition;
    }
    const int cursor = boundedTextPosition(static_cast<qsizetype>(m_preeditStart) +
                                           static_cast<qsizetype>(m_preeditCursor));
    return qBound(0, cursor, boundedTextLength(displayText()));
}

int SnowCanvasTextDraft::inputMethodCursorPosition() const {
    if (m_preeditText.isEmpty()) {
        return m_cursorPosition;
    }
    return qBound(0, m_preeditStart, boundedTextLength(displayText()));
}

int SnowCanvasTextDraft::inputMethodAnchorPosition() const {
    return m_preeditText.isEmpty() ? m_anchorPosition : inputMethodCursorPosition();
}

int SnowCanvasTextDraft::selectionStart() const {
    return selectedRange().start;
}

int SnowCanvasTextDraft::selectionEnd() const {
    const Range range = selectedRange();
    return range.start + range.length;
}

int SnowCanvasTextDraft::preeditStart() const {
    return m_preeditStart;
}

int SnowCanvasTextDraft::preeditLength() const {
    return boundedTextLength(m_preeditText);
}

bool SnowCanvasTextDraft::hasSelection() const {
    return m_cursorPosition != m_anchorPosition;
}

bool SnowCanvasTextDraft::hasPreedit() const {
    return !m_preeditText.isEmpty();
}

bool SnowCanvasTextDraft::setCursorPosition(int position, bool keepSelection) {
    const int nextCursor = qBound(0, position, boundedTextLength(m_text));
    const int nextAnchor = keepSelection ? m_anchorPosition : nextCursor;
    if (m_cursorPosition == nextCursor && m_anchorPosition == nextAnchor) {
        return false;
    }
    m_cursorPosition = nextCursor;
    m_anchorPosition = nextAnchor;
    return true;
}

bool SnowCanvasTextDraft::selectAll() {
    const bool clearedPreedit = clearPreedit();
    const int textLength = boundedTextLength(m_text);
    if (m_anchorPosition == 0 && m_cursorPosition == textLength) {
        return clearedPreedit;
    }
    m_anchorPosition = 0;
    m_cursorPosition = textLength;
    return true;
}

bool SnowCanvasTextDraft::replaceSelection(const QString& text) {
    const bool clearedPreedit = ensureNoPreeditForDirectEdit();
    const QString insertedText = normalizedInsertedText(text);
    if (!replaceRange(selectedRange(), insertedText)) {
        return clearedPreedit;
    }
    return true;
}

bool SnowCanvasTextDraft::deletePreviousCharacter() {
    const bool clearedPreedit = ensureNoPreeditForDirectEdit();
    if (hasSelection()) {
        return replaceSelection(QString()) || clearedPreedit;
    }
    if (m_cursorPosition <= 0) {
        return clearedPreedit;
    }
    const int previous = previousGraphemeBoundary(m_text, m_cursorPosition);
    return replaceRange(Range{previous, m_cursorPosition - previous}, QString()) || clearedPreedit;
}

bool SnowCanvasTextDraft::deleteNextCharacter() {
    const bool clearedPreedit = ensureNoPreeditForDirectEdit();
    if (hasSelection()) {
        return replaceSelection(QString()) || clearedPreedit;
    }
    if (m_cursorPosition >= boundedTextLength(m_text)) {
        return clearedPreedit;
    }
    const int next = nextGraphemeBoundary(m_text, m_cursorPosition);
    return replaceRange(Range{m_cursorPosition, next - m_cursorPosition}, QString()) ||
           clearedPreedit;
}

bool SnowCanvasTextDraft::clearPreedit() {
    if (m_preeditText.isEmpty() && m_preeditStart == 0 && m_preeditReplacementLength == 0 &&
        m_preeditCursor == 0) {
        return false;
    }
    m_preeditText.clear();
    m_preeditStart = 0;
    m_preeditReplacementLength = 0;
    m_preeditCursor = 0;
    return true;
}

bool SnowCanvasTextDraft::ensureNoPreeditForDirectEdit() {
    if (!m_preeditText.isEmpty()) {
        return clearPreedit();
    }
    return false;
}

bool SnowCanvasTextDraft::undoEdit() {
    ensureNoPreeditForDirectEdit();
    if (m_undoStack.empty()) {
        return false;
    }
    m_redoStack.push_back(snapshot());
    const Snapshot previous = m_undoStack.back();
    m_undoStack.pop_back();
    restoreSnapshot(previous);
    return true;
}

bool SnowCanvasTextDraft::redoEdit() {
    ensureNoPreeditForDirectEdit();
    if (m_redoStack.empty()) {
        return false;
    }
    m_undoStack.push_back(snapshot());
    const Snapshot next = m_redoStack.back();
    m_redoStack.pop_back();
    restoreSnapshot(next);
    return true;
}

bool SnowCanvasTextDraft::handleInputMethodEvent(const QInputMethodEvent& event) {
    bool changed = false;
    const bool hasCommit = !event.commitString().isEmpty() || event.replacementLength() > 0 ||
                           event.replacementStart() != 0;
    if (hasCommit) {
        const int textSize = boundedTextLength(m_text);
        Range replacement = selectedRange();
        if (!m_preeditText.isEmpty()) {
            const int preeditStart = qBound(0, m_preeditStart, textSize);
            replacement = Range{
                preeditStart,
                qBound(0, m_preeditReplacementLength, textSize - preeditStart),
            };
        }
        if (event.replacementLength() > 0 || event.replacementStart() != 0) {
            const int base = m_preeditText.isEmpty() ? m_cursorPosition : m_preeditStart;
            const int replacementStart =
                boundedTextPosition(static_cast<qsizetype>(base) + event.replacementStart());
            replacement.start = qBound(0, replacementStart, textSize);
            replacement.length = qBound(0, event.replacementLength(), textSize - replacement.start);
        }
        const QString committedText = normalizedInsertedText(event.commitString());
        changed = replaceRange(replacement, committedText) || changed;
        changed = clearPreedit() || changed;
    }

    const QString& preedit = event.preeditString();
    if (!preedit.isEmpty()) {
        if (m_preeditText.isEmpty()) {
            m_preeditStart = selectionStart();
            m_preeditReplacementLength = selectionEnd() - selectionStart();
            m_cursorPosition = m_preeditStart;
            m_anchorPosition = m_cursorPosition;
        }
        m_preeditText = preedit;
        m_preeditCursor = preeditCursorFromAttributes(event);
        changed = true;
    } else if (!m_preeditText.isEmpty()) {
        clearPreedit();
        changed = true;
    }

    return changed;
}

void SnowCanvasTextDraft::rememberUndoState() {
    m_undoStack.push_back(snapshot());
    m_redoStack.clear();
}

SnowCanvasTextDraft::Snapshot SnowCanvasTextDraft::snapshot() const {
    return Snapshot{
        m_text,
        m_cursorPosition,
        m_anchorPosition,
    };
}

void SnowCanvasTextDraft::restoreSnapshot(const Snapshot& snapshot) {
    m_text = snapshot.text;
    const int textLength = boundedTextLength(m_text);
    m_cursorPosition = qBound(0, snapshot.cursorPosition, textLength);
    m_anchorPosition = qBound(0, snapshot.anchorPosition, textLength);
    clearPreedit();
}

SnowCanvasTextDraft::Range SnowCanvasTextDraft::selectedRange() const {
    const int textLength = boundedTextLength(m_text);
    const int start = qBound(0, qMin(m_cursorPosition, m_anchorPosition), textLength);
    const int end = qBound(start, qMax(m_cursorPosition, m_anchorPosition), textLength);
    return Range{start, end - start};
}

bool SnowCanvasTextDraft::replaceRange(const Range& range, const QString& text) {
    const int textLength = boundedTextLength(m_text);
    const int start = qBound(0, range.start, textLength);
    const int length = qBound(0, range.length, textLength - start);
    if (length == 0 && text.isEmpty()) {
        return false;
    }

    rememberUndoState();
    m_text.replace(start, length, text);
    m_cursorPosition = boundedTextPosition(static_cast<qsizetype>(start) + text.size());
    m_anchorPosition = m_cursorPosition;
    return true;
}
