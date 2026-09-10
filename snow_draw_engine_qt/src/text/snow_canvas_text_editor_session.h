#pragma once

#include <QPoint>
#include <QPointF>
#include <QRegion>
#include <QString>
#include <QTextCursor>
#include <QVariant>

#include "snow_canvas_display_cache.h"
#include "snow_canvas_text_draft.h"
#include "snow_draw_engine.h"

class QFont;
class QInputMethodEvent;
class QKeyEvent;
class QPainter;

class SnowCanvasTextEditorSession final {
  public:
    struct FinishedEdit {
        QString text;
        QPointF canvasCenter;
        SnowElementId elementId{};
        SnowElementId arrowId{};
        bool hasExistingElement = false;
        SnowTextLayoutSize measuredLayout{1.0, 1.0};
        SnowTextStyle style{};
        bool autoResize = false;
        bool styleChanged = false;

        bool shouldCommit(bool hasViewport) const;
    };

    enum class EventCommand {
        None,
        Commit,
        Cancel,
        DeleteElement,
    };

    struct KeyResult {
        EventCommand command = EventCommand::None;
        bool handled = false;
        bool changed = false;
    };

    bool begin(const SnowTextElementInfo& info, const SnowSceneDisplayItem* existingSceneItem,
               const QFont& baseFont, const SnowTextStyle* newTextStyle = nullptr,
               const QString* completeText = nullptr);
    FinishedEdit finish(const QFont& baseFont);
    void cancel();

    SnowElementId arrowId() const {
        return m_arrowId;
    }
    bool isActive() const;
    bool containsViewPosition(const SceneDisplayInfo& sceneInfo, const QPointF& viewPosition) const;

    const SnowSceneDisplayItem* previewItem() const;
    QString presentationText() const;
    bool activeDraftHasExistingElement() const;
    bool activeDraftAutoResize() const;
    SnowTextStyle currentTextStyle() const;
    bool applyActiveDraftPresentation(const SnowTextElementInfo& info, const SnowTextStyle& style);
    bool syncActiveDraftPresentation(SnowRuntime runtime, SnowViewport viewport,
                                     const QFont& baseFont, const SceneDisplayInfo& sceneInfo,
                                     QRegion* outUpdateRegion = nullptr);
    QRegion previewRegion(const SceneDisplayInfo& sceneInfo, int paddingPx = 2) const;
    QRegion editingRegion(const QFont& baseFont, const SceneDisplayInfo& sceneInfo) const;
    QRegion caretRegion(const QFont& baseFont, const SceneDisplayInfo& sceneInfo) const;
    void updatePreviewFromState(const QFont& baseFont);
    QRegion updatePreviewFromState(const QFont& baseFont, const SceneDisplayInfo& sceneInfo);
    QRegion applyTextStyle(const SnowTextStyle& style, const QFont& baseFont,
                           const SceneDisplayInfo& sceneInfo);
    void updateGeometry(const QFont& baseFont, const SceneDisplayInfo& sceneInfo);
    bool setCursorFromViewPosition(const QFont& baseFont, const SceneDisplayInfo& sceneInfo,
                                   const QPointF& viewPosition, bool keepSelection = false);
    KeyResult handleKeyPress(QKeyEvent* event, const QFont& baseFont,
                             const SceneDisplayInfo& sceneInfo);
    bool handleInputMethodEvent(QInputMethodEvent* event, const QFont& baseFont);
    QVariant inputMethodQuery(Qt::InputMethodQuery query, const QFont& baseFont,
                              const SceneDisplayInfo& sceneInfo) const;
    void renderEditorOverlay(QPainter& painter, const QFont& baseFont,
                             const SceneDisplayInfo& sceneInfo, bool caretVisible = true) const;

  private:
    void resetState();
    void updatePreviewLayout(const QFont& baseFont, bool forceLayout);
    void updatePreviewAnchor();
    bool moveCursor(QTextCursor::MoveOperation operation, QTextCursor::MoveMode mode,
                    const QFont& baseFont, const SceneDisplayInfo& sceneInfo);

    QPointF m_canvasAnchor;
    SnowElementId m_elementId{};
    SnowElementId m_arrowId{};
    double m_arrowWidth = 0.0;
    bool m_hasExistingElement = false;
    SnowCanvasSceneItem m_previewItem;
    bool m_hasPreview = false;
    bool m_previewAutoResize = false;
    bool m_styleChanged = false;
    SnowCanvasTextDraft m_draft;
};
