#pragma once

#include <QString>

#include <vector>

class QInputMethodEvent;

class SnowCanvasTextDraft final {
  public:
    void reset();
    void releaseRetainedState();
    std::size_t retainedHistoryStorageBytes() const;
    void begin(const QString& text);

    const QString& text() const;
    QString displayText() const;
    QString selectedText() const;
    QString inputMethodCurrentSelection() const;

    int cursorPosition() const;
    int anchorPosition() const;
    int displayCursorPosition() const;
    int inputMethodCursorPosition() const;
    int inputMethodAnchorPosition() const;
    int selectionStart() const;
    int selectionEnd() const;
    int preeditStart() const;
    int preeditLength() const;
    bool hasSelection() const;
    bool hasPreedit() const;

    bool setCursorPosition(int position, bool keepSelection = false);
    bool selectAll();
    bool replaceSelection(const QString& text);
    bool deletePreviousCharacter();
    bool deleteNextCharacter();
    bool clearPreedit();
    bool ensureNoPreeditForDirectEdit();
    bool undoEdit();
    bool redoEdit();
    bool handleInputMethodEvent(const QInputMethodEvent& event);

  private:
    struct Snapshot {
        QString text;
        int cursorPosition = 0;
        int anchorPosition = 0;
    };

    struct Range {
        int start = 0;
        int length = 0;
    };

    void rememberUndoState();
    Snapshot snapshot() const;
    void restoreSnapshot(const Snapshot& snapshot);
    Range selectedRange() const;
    bool replaceRange(const Range& range, const QString& text);

    QString m_text;
    int m_cursorPosition = 0;
    int m_anchorPosition = 0;
    QString m_preeditText;
    int m_preeditStart = 0;
    int m_preeditReplacementLength = 0;
    int m_preeditCursor = 0;
    std::vector<Snapshot> m_undoStack;
    std::vector<Snapshot> m_redoStack;
};
