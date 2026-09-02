#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTE_H

#include "icon_core.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotscrollingtypes.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QColor>
#include <QMargins>
#include <QHash>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <memory>
#include <initializer_list>
#include <optional>

class QFrame;
class QEvent;
class QHideEvent;
class QBoxLayout;
class QLabel;
class QPaintEvent;
class QSpacerItem;
class QWheelEvent;

namespace adqt::widgets {
class AdButton;
class AdColorPicker;
class AdPopover;
class AdRadioButtonGroup;
class AdSelect;
class AdSlider;
} // namespace adqt::widgets

class ScreenshotToolPaletteStyleControls;
class ScreenshotToolbarMainPanel;
class IconNumericValuePreviewButton;

class ScreenshotToolPalette final : public QWidget {
    Q_OBJECT

  public:
    enum class Tool {
        Move,
        Select,
        Shape,
        Arrow,
        Line,
        FreeDraw,
        RectangleHighlight,
        Highlight = RectangleHighlight,
        PenHighlight,
        Eraser,
        RectangleFilter,
        Filter = RectangleFilter,
        Watermark,
        Text,
        SerialNumber,
        Ocr,
        TextTranslation,
        Table,
        Qr,
        ScrollingScreenshot,
        PenFilter,
        Spotlight,
    };

    enum class RecordingState {
        Idle,
        Recording,
        Paused,
    };

    enum class ActionFamily {
        Selection,
        TextRecognition,
        TableRecognition,
        ScrollingRecognition,
    };

    enum class MaterializationState {
        Uninitialized,
        Constructing,
        Ready,
    };

    enum Action {
        NoActions = 0,
        PinAction = 1 << 0,
        CancelAction = 1 << 1,
        CopyAction = 1 << 2,
        ConfirmAction = 1 << 3,
    };
    Q_DECLARE_FLAGS(Actions, Action)

    struct Options {
        bool showDragHandle = false;
        bool showHistoryActions = false;
        bool showMoveTool = false;
        bool showSelectTool = true;
        bool showShapeTool = true;
        bool showArrowTool = true;
        bool showLineTool = false;
        bool showFreeDrawTool = false;
        bool showHighlightTool = false;
        bool showRectangleHighlightTool = false;
        bool showPenHighlightTool = false;
        bool showSpotlightTool = false;
        bool showEraserTool = false;
        bool showFilterTool = false;
        bool showWatermarkTool = false;
        bool showTextTool = false;
        bool showSerialNumberTool = false;
        bool showOcrTool = false;
        bool showTextTranslationTool = false;
        bool showTableTool = false;
        bool showQrTool = false;
        bool showScrollingScreenshotTool = false;
        bool showSaveButton = false;
        bool saveButtonWithResultActions = false;
        bool copyButtonWithNeutralIcon = false;
        bool showScreenRecordButton = false;
        bool showRecordingControls = false;
        bool showTrailingDragHandle = false;
        bool enableStyleToolbar = true;
        bool separatorAfterSelect = false;
        bool separatorBeforeShape = false;
        bool separatorBeforeConfirm = false;
        bool showDrawingModeShortcutOnConfirm = false;
        Actions actions = NoActions;
        std::optional<snow_shot::storage::ScreenshotToolbarLayout> toolbarLayout;
        SnowCanvasStyleDefaults styleDefaults =
            snow_shot::presentation::screenshotCanvasStyleDefaults();
    };

    explicit ScreenshotToolPalette(const Options& options, QWidget* parent = nullptr);
    ~ScreenshotToolPalette() override;

    QWidget* mainPanel() const;
    QWidget* actionPanel() const;
    QWidget* stylePanel() const;
    QWidget* dragHandle() const;
    QWidget* trailingDragHandle() const;
    QSize contentSizeHint() const;
    QRect occupiedContentRect() const;
    QRect visualContentRect() const;
    QRect fullContentRect() const;
    QRect bottomPlacementContentRect() const;
    QRect topPlacementContentRect() const;
    QRect topRightMainToolbarContentRect() const;
    QRect mainToolbarContentRect() const;
    ScreenshotToolbarPlacementSnapshot placementSnapshot() const;
    QPoint contentOffset() const;
    void prepareForDisplay();
    void resetStyleState();
    void setCreationStyleDefaults(const SnowCanvasStyleDefaults& defaults);
    [[nodiscard]] SnowCanvasStyleDefaults creationStyleDefaults() const;
    bool setShadowMargins(const QMargins& margins);
    bool setPhysicalScale(qreal scale);
    qreal physicalScale() const;
    void setToolbarLayout(const snow_shot::storage::ScreenshotToolbarLayout& layout);
    bool stepStrokeWidth(int direction);
    bool stepSelectionOpacity(int direction);
    bool stepSpotlightOpacity(int direction);
    bool stepFilterIntensity(int direction);
    bool stepPenFilterStrokeWidth(int direction);
    bool stepWatermarkFontSize(int direction);
    void setStyleToolbarAboveMain(bool above);
    void setStyleToolbarVisible(bool visible);
    bool styleToolbarVisible() const;
    bool actionToolbarVisible() const;
    void setActiveTool(Tool tool);
    void refreshConfirmShortcutHint();
    [[nodiscard]] bool activateDrawingShortcut(const QString& toolId);
    void clearActiveTool();
    void setHistoryState(const SnowCanvasHistoryState& state);
    void setScrollingScreenshotMode(bool enabled);
    [[nodiscard]] bool scrollingScreenshotMode() const;
    void setScrollingRecognitionMode(ScreenshotScrollingRecognitionMode mode);
    [[nodiscard]] ScreenshotScrollingRecognitionMode scrollingRecognitionMode() const;
    SnowCanvasShapeStyle rectangleStyle() const;
    void setRectangleStyle(const SnowCanvasShapeStyle& style);
    void setStyleToolbarState(const SnowCanvasStyleToolbarState& state);
    void setWatermarkConfig(const SnowCanvasWatermarkConfig& config);
    void setSpotlightConfig(const SnowCanvasSpotlightConfig& config);
    void setSelectionOpacity(qreal opacity, bool mixed = false);
    void installWheelFilters(QObject* receiver, QWidget* scope = nullptr);
    bool handleToolbarWheel(QWheelEvent* event);
    void setRecordingState(RecordingState state);
    void setRecordingDuration(qint64 durationMilliseconds);
    void setRecordingMicrophoneEnabled(bool enabled);
    void setRecordingSystemAudioEnabled(bool enabled);
    void setRecordingBusy(bool busy);
    void setOcrEnabled(bool enabled);
    void setOcrBusy(bool busy);
    void setTableEnabled(bool enabled);
    void setTableBusy(bool busy);
    void setQrEnabled(bool enabled);
    void setQrBusy(bool busy);
    void setTableEditingState(bool available, bool canUndo, bool canRedo, bool canMerge,
                              bool canSplit, bool canReset);
    void setTextEditingState(bool available, bool editing, bool canUndo = false,
                             bool canRedo = false);
    void setTextTranslationState(bool available, bool translating, bool streaming,
                                 bool canUndo = false, bool canRedo = false,
                                 bool canReset = false);
    void setTextTransformSelections(const QString& formatting, const QString& punctuation);
    [[nodiscard]] bool ensureActionFamily(ActionFamily family);
    [[nodiscard]] bool ensureStyleFamily(Tool tool);

#if defined(SNOW_SHOT_TEST_HOOKS)
    [[nodiscard]] std::optional<Tool> activeToolForTests() const;
    [[nodiscard]] quint64 styleStateNoopCountForTests() const;
    [[nodiscard]] quint64 propertyGroupRefreshCountForTests() const;
    [[nodiscard]] quint64 layoutCommitCountForTests() const;
    [[nodiscard]] SnowCanvasStyleDefaults styleStateForTests() const;
    [[nodiscard]] MaterializationState actionFamilyStateForTests(ActionFamily family) const;
    [[nodiscard]] MaterializationState styleFamilyStateForTests(Tool tool) const;
#endif

  signals:
    void undoRequested();
    void redoRequested();
    void moveRequested();
    void selectRequested();
    void shapeRequested();
    void arrowRequested();
    void lineRequested();
    void freeDrawRequested();
    void highlightRequested();
    void penHighlightRequested();
    void spotlightRequested();
    void eraserRequested();
    void filterRequested();
    void rectangleFilterRequested();
    void penFilterRequested();
    void watermarkRequested();
    void textRequested();
    void serialNumberRequested();
    void ocrRequested();
    void textTranslationRequested();
    void tableRequested();
    void qrRequested();
    void tableMergeRequested();
    void tableSplitRequested();
    void tableResetRequested();
    void textEditRequested();
    void textTranslateRequested();
    void textResetRequested();
    void textSettingsRequested();
    void textFormattingRequested(const QString& value);
    void textPunctuationRequested(const QString& value);
    void scrollingScreenshotRequested();
    void saveRequested();
    void scrollingRecognitionModeChanged(ScreenshotScrollingRecognitionMode mode);
    void screenRecordRequested();
    void serialNumberDecrementRequested();
    void serialNumberIncrementRequested();
    void serialNumberCreateTextRequested();
    void pinRequested();
    void cancelRequested();
    void copyRequested();
    void confirmRequested();
    void shapeStyleChanged(const SnowCanvasShapeStyle& style, quint32 properties,
                           SnowCanvasShapeKind kind);
    void filterStyleChanged(const SnowCanvasFilterStyle& style, quint32 properties);
    void watermarkConfigChanged(const SnowCanvasWatermarkConfig& config);
    void watermarkPreviewChanged(const SnowCanvasWatermarkConfig& config);
    void spotlightConfigChanged(const SnowCanvasSpotlightConfig& config);
    void spotlightPreviewChanged(const SnowCanvasSpotlightConfig& config);
    void textStyleChanged(const SnowCanvasTextStyle& style);
    void textStylePopupInteractionBegan();
    void textStylePopupInteractionEnded();
    void serialNumberStyleChanged(const SnowCanvasSerialNumberStyle& style);
    void canvasColorSamplingRequested(adqt::widgets::AdColorPicker* picker);
    void sendSelectionToBackRequested();
    void sendSelectionBackwardRequested();
    void bringSelectionForwardRequested();
    void bringSelectionToFrontRequested();
    void selectionOpacityChanged(qreal opacity);
    void duplicateSelectionRequested();
    void deleteSelectionRequested();
    void visibleContentChanged();
    void recordingStartRequested();
    void recordingStopRequested();
    void recordingPauseRequested();
    void recordingResumeRequested();
    void recordingMicrophoneToggled(bool enabled);
    void recordingSystemAudioToggled(bool enabled);
    void recordingOpenFolderRequested();
    void recordingCloseRequested();
    void recordingCopyAnimatedImageRequested();
    void recordingCopyVideoRequested();
    void materializedScope(QWidget* scope);

  private:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

    adqt::widgets::AdButton* addToolButton(const char* tooltip,
                                           const adqt::icons::IconRef& iconRef);
    adqt::widgets::AdButton* addActionButton(const char* tooltip,
                                             const adqt::icons::IconRef& iconRef,
                                             bool danger = false, bool primary = false);
    void createMainToolbar(const Options& options);
    void createSecondaryToolbarShell();
    void createSelectionActionFamily();
    void createTextRecognitionActionFamily();
    void createTableRecognitionActionFamily();
    void createScrollingRecognitionActionFamily();
    void createStyleFamily(Tool tool);
    void registerStyleFamily(QWidget* controls, std::initializer_list<Tool> tools);
    void replayMaterializedState(Tool tool);
    void clearSecondaryResourceBindings();
    bool evictSecondaryToolbarContents();
    bool addMainToolButtons(const Options& options, QBoxLayout* layout);
    bool addMainHistoryButtons(const Options& options, QBoxLayout* layout);
    bool addMainSecondaryButtons(const Options& options, QBoxLayout* layout);
    void addMainActionButtons(const Options& options, QBoxLayout* layout);
    void applyMainToolbarLayout(bool notify);
    adqt::widgets::AdButton* drawingToolButton(const QString& itemId) const;
    adqt::widgets::AdButton* drawingToolEntryButton(Tool tool) const;
    void clearDrawingToolGroups();
    void activateToolFromToolbar(Tool tool, bool toggleVisibleButton = true);
    void activateDrawingTool(Tool tool);
    [[nodiscard]] Tool drawingShortcutEntryTool(const QString& itemId, Tool fallback) const;
    void selectDrawingToolGroupEntry(Tool tool);
    void refreshDrawingToolGroup(int groupIndex);
    void addRecordingControls(QBoxLayout* layout);
    void activateTableQrTool(Tool tool, bool toggleVisibleButton = true);
    void setTableQrEntryTool(Tool tool);
    void refreshTableQrTrigger();
    void selectDynamicEntryTool(Tool tool);
    void updateTableQrBusy();
    void updateTableQrEnabled();
    void updateToolbarGeometry();
    void updateStyleToolbarGeometryOnly();
    void commitLayout();
    // Geometry queries are synchronous boundaries. A dirty profile is committed
    // once here; movement reads the immutable result without touching layouts.
    void ensureLayoutApplied() const;
    void markLayoutDirty(bool rowOrderChanged = false);
    void updateToolbarRowGeometry(bool styleToolbarVisible);
    void setActiveToolButton(adqt::widgets::AdButton* activeButton);
    bool setStyleControlsActive(Tool tool);
    QWidget* styleControlsForTool(Tool tool) const;
    bool applyActiveToolSecondaryToolbarVisibility();
    [[nodiscard]] bool activeToolUsesStyleToolbar() const;
    bool setSecondaryToolbarVisibility(bool actionToolbarVisible, bool styleToolbarVisible);
    void updateSelectionActionAvailability(bool hasSelection);
    void updateHistoryActionAvailability();
    void updateTextRecognitionBusy();
    void updateScrollingRecognitionButtons();
    void updateSelectionOpacityIcon();
    void refreshThemeDependentIcons();
    void synchronizeFilterModeGroups(Tool tool);
    void updatePenFilterStrokeWidthControls();
    void updateRecordingControls();
    void updateRecordingControlMetrics();
    QSize styleToolbarSizeHint();
    QSize maximumSecondaryToolbarSizeHint() const;
    QSize contentSizeForVisibleRows() const;
    QSize fullContentSize() const;
    QRect panelContentRect(const QWidget* panel) const;
    ScreenshotToolbarPlacementSnapshot buildPlacementSnapshot() const;
    int scaledMetric(int value) const;
    qreal scaledMetric(qreal value) const;
    QMargins scaledMargins(int left, int top, int right, int bottom) const;
    QMargins scaledPanelMargins(int horizontalMargin, int verticalMargin,
                                int baseContentHeight) const;
    void addMainToolbarSpacing(int baseSpacing);
    void addMainToolbarSeparator();
    QSpacerItem* addStyleToolbarSpacing(QBoxLayout* layout, int baseSpacing);
    QSpacerItem* insertStyleToolbarSpacing(QBoxLayout* layout, int index, int baseSpacing);
    void setStyleToolbarSpacingVisible(QSpacerItem* spacer, bool visible);
    QFrame* createStyleToolbarSeparator(QWidget* parent);
    void applyScaledToolbarMetrics();
    void applyStyleMetricsForScope(QWidget* scope);
    void initializeStyleLayoutProfiles();
    void applyCumulativeStyleLayoutMetrics(QWidget* scope);
    void updatePanelMetrics(QFrame* panel);
    void updatePanelStyle(QFrame* panel);
    void retranslateUi();

    struct SpacingItem {
        QSpacerItem* item = nullptr;
        QWidget* owner = nullptr;
        int baseSpacing = 0;
        bool visible = true;
    };

    struct StyleLayoutSegment {
        QWidget* widget = nullptr;
        QSpacerItem* spacer = nullptr;
        int referenceWidth = 0;
    };

    struct StyleLayoutProfile {
        QBoxLayout* layout = nullptr;
        QWidget* owner = nullptr;
        QVector<StyleLayoutSegment> segments;
        QVector<QSpacerItem*> automaticGaps;
        int referenceAutomaticSpacing = 0;
        int referenceWidthAdjustment = 0;
    };

    struct FilterEditor {
        QWidget* controls = nullptr;
        adqt::widgets::AdSelect* typeSelect = nullptr;
        QLabel* intensityIcon = nullptr;
        adqt::widgets::AdSlider* intensitySlider = nullptr;
        Tool tool = Tool::RectangleFilter;
    };

    void updateFilterIntensityIcon(FilterEditor& editor);

    struct FilterEditorConfig {
        Tool tool = Tool::RectangleFilter;
        QString controlsObjectName;
        QString typeSelectObjectName;
        QString intensityIconObjectName;
        QString intensitySliderObjectName;
        bool includeStrokeWidth = false;
    };

    FilterEditor createFilterEditor(const FilterEditorConfig& config);
    void refreshFilterEditorMetrics(FilterEditor& editor);
    void refreshFilterEditorState(FilterEditor& editor, bool refreshWidth);
    SnowCanvasFilterStyle& filterStyleForEditor(const FilterEditor& editor);

    struct StyleModeOption {
        QString tooltip;
        adqt::icons::IconRef icon;
        Tool tool = Tool::Shape;
    };

    QWidget* createStyleModeSelector(QWidget* parent, const QString& objectName,
                                     const QVector<StyleModeOption>& options, Tool initialTool,
                                     QVector<adqt::widgets::AdRadioButtonGroup*>& groups);

    struct StyleEditorBinding {
        QWidget* controls = nullptr;
        QVector<Tool> tools;
    };

    struct DrawingToolGroup {
        QStringList itemIds;
        QVector<Tool> tools;
        Tool entryTool = Tool::Shape;
        adqt::widgets::AdButton* trigger = nullptr;
        adqt::widgets::AdPopover* popover = nullptr;
        QVector<adqt::widgets::AdButton*> optionButtons;
        QVector<int> optionValues;
        QStringList popoverItemIds;
        bool ownsTrigger = false;
        bool popoverConstructing = false;
    };

    void ensureDrawingToolGroupPopover(adqt::widgets::AdButton* trigger);
    void ensureTableQrPopover();

    ScreenshotToolbarMainPanel* m_mainPanel = nullptr;
    QWidget* m_selectActionPanel = nullptr;
    QWidget* m_rectangleStylePanel = nullptr;
    QBoxLayout* m_rootLayout = nullptr;
    QBoxLayout* m_rectangleStyleLayout = nullptr;
    QBoxLayout* m_selectActionLayout = nullptr;
    QVector<QBoxLayout*> m_styleControlLayouts;
    QWidget* m_rectangleStyleControlsWidget = nullptr;
    QWidget* m_arrowStyleControlsWidget = nullptr;
    QWidget* m_highlightStyleControlsWidget = nullptr;
    QWidget* m_penHighlightStyleControlsWidget = nullptr;
    QWidget* m_spotlightStyleControlsWidget = nullptr;
    QWidget* m_textStyleControlsWidget = nullptr;
    QWidget* m_serialNumberStyleControlsWidget = nullptr;
    QWidget* m_filterStyleControlsWidget = nullptr;
    QWidget* m_penFilterStyleControlsWidget = nullptr;
    QWidget* m_watermarkStyleControlsWidget = nullptr;
    QWidget* m_activeStyleControlsWidget = nullptr;
    std::optional<Tool> m_activeStyleTool;
    QVector<StyleEditorBinding> m_styleEditorBindings;
    QFrame* m_shapeStyleGroupSeparator = nullptr;
    QSpacerItem* m_shapeStyleGroupSeparatorLeadingSpacing = nullptr;
    QSpacerItem* m_shapeStyleGroupSeparatorTrailingSpacing = nullptr;
    adqt::widgets::AdButton* m_moveButton = nullptr;
    adqt::widgets::AdButton* m_undoButton = nullptr;
    adqt::widgets::AdButton* m_redoButton = nullptr;
    adqt::widgets::AdButton* m_selectButton = nullptr;
    adqt::widgets::AdButton* m_shapeButton = nullptr;
    adqt::widgets::AdButton* m_arrowButton = nullptr;
    adqt::widgets::AdButton* m_lineButton = nullptr;
    adqt::widgets::AdButton* m_freeDrawButton = nullptr;
    adqt::widgets::AdButton* m_highlighterButton = nullptr;
    adqt::widgets::AdButton* m_spotlightButton = nullptr;
    QVector<adqt::widgets::AdRadioButtonGroup*> m_highlightModeGroups;
    QVector<adqt::widgets::AdRadioButtonGroup*> m_filterModeGroups;
    adqt::widgets::AdButton* m_eraserButton = nullptr;
    adqt::widgets::AdButton* m_filterButton = nullptr;
    adqt::widgets::AdButton* m_watermarkButton = nullptr;
    adqt::widgets::AdButton* m_textButton = nullptr;
    adqt::widgets::AdButton* m_serialNumberButton = nullptr;
    adqt::widgets::AdButton* m_ocrButton = nullptr;
    adqt::widgets::AdButton* m_textTranslationButton = nullptr;
    adqt::widgets::AdButton* m_tableButton = nullptr;
    adqt::widgets::AdButton* m_tableOptionButton = nullptr;
    adqt::widgets::AdButton* m_qrButton = nullptr;
    adqt::widgets::AdPopover* m_tableQrPopover = nullptr;
    QVector<adqt::widgets::AdButton*> m_tableQrOptionButtons;
    QVector<int> m_tableQrOptionValues;
    Tool m_tableQrEntryTool = Tool::Table;
    adqt::widgets::AdButton* m_textEditButton = nullptr;
    adqt::widgets::AdButton* m_textTranslateButton = nullptr;
    adqt::widgets::AdButton* m_textResetButton = nullptr;
    adqt::widgets::AdButton* m_textSettingsButton = nullptr;
    adqt::widgets::AdButton* m_tableMergeButton = nullptr;
    adqt::widgets::AdButton* m_tableSplitButton = nullptr;
    adqt::widgets::AdButton* m_tableResetButton = nullptr;
    adqt::widgets::AdSelect* m_textFormattingSelect = nullptr;
    adqt::widgets::AdSelect* m_textPunctuationSelect = nullptr;
    adqt::widgets::AdButton* m_scrollingScreenshotButton = nullptr;
    adqt::widgets::AdButton* m_saveButton = nullptr;
    QWidget* m_scrollingRecognitionControls = nullptr;
    adqt::widgets::AdButton* m_scrollingVerticalButton = nullptr;
    adqt::widgets::AdButton* m_scrollingHorizontalButton = nullptr;
    adqt::widgets::AdButton* m_screenRecordButton = nullptr;
    adqt::widgets::AdButton* m_recordStartButton = nullptr;
    adqt::widgets::AdButton* m_recordStopButton = nullptr;
    adqt::widgets::AdButton* m_recordPauseButton = nullptr;
    adqt::widgets::AdButton* m_recordResumeButton = nullptr;
    adqt::widgets::AdButton* m_recordMicrophoneButton = nullptr;
    adqt::widgets::AdButton* m_recordSystemAudioButton = nullptr;
    adqt::widgets::AdButton* m_recordOpenFolderButton = nullptr;
    adqt::widgets::AdButton* m_recordCloseButton = nullptr;
    adqt::widgets::AdButton* m_recordCopyAnimatedImageButton = nullptr;
    adqt::widgets::AdButton* m_recordCopyVideoButton = nullptr;
    QLabel* m_recordDurationLabel = nullptr;
    adqt::widgets::AdButton* m_pinButton = nullptr;
    adqt::widgets::AdButton* m_cancelButton = nullptr;
    adqt::widgets::AdButton* m_copyButton = nullptr;
    adqt::widgets::AdButton* m_confirmButton = nullptr;
    QLabel* m_selectionOpacityIcon = nullptr;
    adqt::widgets::AdSlider* m_selectionOpacitySlider = nullptr;
    QVector<QWidget*> m_selectionActionControls;
    QVector<QSpacerItem*> m_selectionActionSpacers;
    QVector<QSpacerItem*> m_textActionSpacers;
    QVector<QSpacerItem*> m_tableActionSpacers;
    std::optional<Tool> m_activeTool;
    adqt::widgets::AdButton* m_activeToolButton = nullptr;
    QVector<QFrame*> m_styleSeparatorFrames;
    QVector<QFrame*> m_panelFrames;
    QVector<SpacingItem> m_styleSpacingItems;
    QVector<StyleLayoutProfile> m_styleLayoutProfiles;
    std::unique_ptr<ScreenshotToolPaletteStyleControls> m_styleControls;
    QMargins m_baseShadowMargins;
    QMargins m_shadowMargins;
    QHash<QWidget*, quint64> m_styleMetricRevisions;
    quint64 m_metricProfileRevision = 1;
    qreal m_physicalScale = 1.0;
    qreal m_selectionOpacity = 1.0;
    bool m_selectionOpacityMixed = false;
    bool m_selectionOpacityInitialized = false;
    bool m_styleToolbarAboveMain = false;
    bool m_styleToolbarTargetVisible = false;
    bool m_actionToolbarTargetVisible = false;
    bool m_hasSelectedElements = false;
    bool m_selectionOpacityAvailable = false;
    bool m_selectionActionAvailabilityInitialized = false;
    bool m_scrollingScreenshotMode = false;
    ScreenshotScrollingRecognitionMode m_scrollingRecognitionMode =
        ScreenshotScrollingRecognitionMode::Vertical;
    RecordingState m_recordingState = RecordingState::Idle;
    bool m_recordingMicrophoneEnabled = false;
    bool m_recordingSystemAudioEnabled = true;
    bool m_recordingBusy = false;
    bool m_ocrEnabled = true;
    bool m_ocrBusy = false;
    bool m_tableEnabled = true;
    bool m_qrEnabled = true;
    bool m_tableBusy = false;
    bool m_qrBusy = false;
    bool m_tableEditingAvailable = false;
    bool m_tableCanUndo = false;
    bool m_tableCanRedo = false;
    bool m_textEditingAvailable = false;
    bool m_textEditing = false;
    bool m_textTranslating = false;
    bool m_textTranslationStreaming = false;
    bool m_textCanUndo = false;
    bool m_textCanRedo = false;
    bool m_textCanReset = false;
    bool m_tableCanMerge = false;
    bool m_tableCanSplit = false;
    bool m_tableCanReset = false;
    QString m_textFormattingSelection;
    QString m_textPunctuationSelection;
    qint64 m_recordingDurationMilliseconds = 0;
    bool m_replayingMaterializedState = false;
    bool m_releasingSecondaryResources = false;
    QHash<int, MaterializationState> m_actionFamilyStates;
    QHash<int, MaterializationState> m_styleFamilyStates;
    SnowCanvasHistoryState m_canvasHistoryState;
    const SnowCanvasStyleDefaults m_styleDefaults;
    const Options m_options;
    std::optional<snow_shot::storage::ScreenshotToolbarLayout> m_toolbarLayout;
    QVector<DrawingToolGroup> m_drawingToolGroups;
    FilterEditor m_filterEditor;
    FilterEditor m_penFilterEditor;
    QLabel* m_spotlightOpacityIcon = nullptr;
    adqt::widgets::AdSlider* m_spotlightOpacitySlider = nullptr;

    struct LayoutResult {
        QSize paletteSize;
        QSize contentSize;
        QPoint contentOffset;
        QRect occupiedContentRect;
        QRect fullContentRect;
        QRect mainToolbarContentRect;
    };

    mutable LayoutResult m_layoutResult;
    mutable bool m_layoutDirty = true;
    mutable bool m_rowOrderDirty = true;

#if defined(SNOW_SHOT_TEST_HOOKS)
    quint64 m_layoutCommitCount = 0;
    quint64 m_rootRowReorderCount = 0;
    quint64 m_styleStateNoopCount = 0;
    quint64 m_propertyGroupRefreshCount = 0;
#endif
};

Q_DECLARE_OPERATORS_FOR_FLAGS(ScreenshotToolPalette::Actions)

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLPALETTE_H
