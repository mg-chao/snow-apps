#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTABLEEDITOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTABLEEDITOR_H

#include "snow_shot/presentation/screenshottabledocument.h"

#include <QPoint>
#include <QTableView>
#include <QUndoStack>

#include <functional>
#include <memory>
#include <optional>

class QContextMenuEvent;
class QMouseEvent;
class QWheelEvent;
class ScreenshotTableModel;
class ScreenshotTableDelegate;

struct ScreenshotTableCommandState {
    bool canUndo = false;
    bool canRedo = false;
    bool canMerge = false;
    bool canSplit = false;
    bool canReset = false;
    bool hasSelection = false;

    [[nodiscard]] bool operator==(const ScreenshotTableCommandState& other) const {
        return canUndo == other.canUndo && canRedo == other.canRedo && canMerge == other.canMerge &&
               canSplit == other.canSplit && canReset == other.canReset &&
               hasSelection == other.hasSelection;
    }
    [[nodiscard]] bool operator!=(const ScreenshotTableCommandState& other) const {
        return !(*this == other);
    }
};

class ScreenshotTableEditingSession final {
  public:
    explicit ScreenshotTableEditingSession(ScreenshotTableDocument recognizedDocument);

    ScreenshotTableDocument baseline;
    ScreenshotTableDocument document;
    QUndoStack undoStack;
    ScreenshotTableRange selection;
    QPoint currentCell;
    int horizontalScrollValue = 0;
    int verticalScrollValue = 0;
    std::function<void()> documentChanged;

    void replaceDocument(const ScreenshotTableDocument& replacement);
};

class ScreenshotTableEditor final : public QTableView {
    Q_OBJECT

  public:
    explicit ScreenshotTableEditor(QWidget* parent = nullptr);
    ~ScreenshotTableEditor() override;

    void setSession(std::shared_ptr<ScreenshotTableEditingSession> session);
    void clearSession();
    [[nodiscard]] std::shared_ptr<ScreenshotTableEditingSession> session() const;
    [[nodiscard]] ScreenshotTableCommandState commandState() const;
    [[nodiscard]] ScreenshotTableRange selectedRange() const;
    [[nodiscard]] bool isEditingCell() const;
    bool commitActiveEdit();
    bool cancelActiveEdit();
    [[nodiscard]] bool copySelectionToClipboard();

  public slots:
    void mergeSelection();
    void splitSelection();
    void resetDocument();
    void undoEdit();
    void redoEdit();
    void copySelection();
    void pasteSelection();
    void clearSelectionContents();

  signals:
    void commandStateChanged(const ScreenshotTableCommandState& state);
    void documentChanged();
    void operationRejected(const QString& message);
    void copyCompleted();

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void updateGeometries() override;

  private:
    friend class ScreenshotTableModel;
    friend class ScreenshotTableDelegate;

    struct PendingCellEdit {
        std::shared_ptr<ScreenshotTableEditingSession> session;
        ScreenshotTableDocument replacement;
        QString label;
        quint64 id = 0;
    };

    bool commitCellText(const QModelIndex& index, const QString& text);
    bool flushPendingCellEdit(quint64 expectedId = 0);
    void continueEditingAfter(QWidget* closingEditor, int row, int column, int rowDelta,
                              int columnDelta);
    void continueEditingFrom(int row, int column, int rowDelta, int columnDelta);
    void pushDocumentChange(const ScreenshotTableDocument& replacement, const QString& label);
    void handleDocumentChanged();
    void refreshCommandState();
    void normalizeSelection();
    void restoreSessionViewState();
    void saveSessionViewState();
    void synchronizeSpans();
    void updateTableMetrics();
    void applyTheme();
    [[nodiscard]] QModelIndex anchorIndex(const QModelIndex& index) const;
    [[nodiscard]] bool pasteDocument(const ScreenshotTableDocument& source);
    void selectRange(const ScreenshotTableRange& range);

    ScreenshotTableModel* m_model = nullptr;
    ScreenshotTableDelegate* m_delegate = nullptr;
    std::shared_ptr<ScreenshotTableEditingSession> m_session;
    ScreenshotTableCommandState m_commandState;
    QPoint m_pressPosition;
    QModelIndex m_pressedIndex;
    bool m_pointerDragged = false;
    bool m_normalizingSelection = false;
    int m_tablePadding = 0;
    std::optional<PendingCellEdit> m_pendingCellEdit;
    quint64 m_nextCellEditId = 0;
};

Q_DECLARE_METATYPE(ScreenshotTableCommandState)

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTABLEEDITOR_H
