#pragma once

#include <QColor>
#include <QWidget>

#include "snow_draw_engine_qt/snow_canvas_widget.h"

class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QObject;
class QPushButton;
class QComboBox;
class QToolButton;

class DemoWindow : public QWidget {
  public:
    explicit DemoWindow(QWidget* parent = nullptr);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void initializeUi();
    void connectCanvasSignals();
    void syncAllControls();
    void syncToolControls();
    void syncHistoryControls();
    void syncSnapControls();
    void syncStyleControls();
    void syncDebugControls();
    void applyRectangleStyleFromControls();
    void applyCurrentStyleFromControls();
    void applySerialNumberFromControls();
    void stepSerialNumber(qint64 delta);
    void applySnapConfigFromControls();
    void applyGridConfigFromControls();
    void choosePrimaryColor();
    void chooseFillColor();
    void chooseStrokeColor();
    void refocusCanvas();
    void clearMixedFlagForSender(QObject* sender);
    static void updateColorButtonAppearance(QPushButton* button, const QColor& color);

    SnowCanvasWidget* m_canvas = nullptr;
    QLabel* m_styleSourceLabel = nullptr;
    QToolButton* m_selectToolButton = nullptr;
    QToolButton* m_shapeToolButton = nullptr;
    QToolButton* m_arrowToolButton = nullptr;
    QToolButton* m_textToolButton = nullptr;
    QToolButton* m_serialNumberToolButton = nullptr;
    QToolButton* m_undoToolButton = nullptr;
    QToolButton* m_redoToolButton = nullptr;
    QToolButton* m_createSerialTextToolButton = nullptr;
    QToolButton* m_editArrowTextToolButton = nullptr;
    QToolButton* m_duplicateSelectionToolButton = nullptr;
    QToolButton* m_deleteSelectionToolButton = nullptr;
    QToolButton* m_decrementSerialNumberToolButton = nullptr;
    QToolButton* m_incrementSerialNumberToolButton = nullptr;
    QLineEdit* m_serialNumberLineEdit = nullptr;
    QToolButton* m_objectSnapToolButton = nullptr;
    QToolButton* m_gridSnapToolButton = nullptr;
    QToolButton* m_showDirtyRectsToolButton = nullptr;
    QLabel* m_primaryLabel = nullptr;
    QLabel* m_fillLabel = nullptr;
    QLabel* m_fillStyleLabel = nullptr;
    QLabel* m_strokeLabel = nullptr;
    QLabel* m_strokeWidthLabel = nullptr;
    QLabel* m_fontSizeLabel = nullptr;
    QLabel* m_fontFamilyLabel = nullptr;
    QLabel* m_opacityLabel = nullptr;
    QLabel* m_textAlignLabel = nullptr;
    QLabel* m_startArrowheadLabel = nullptr;
    QLabel* m_endArrowheadLabel = nullptr;
    QLabel* m_arrowStrokeStyleLabel = nullptr;
    QLabel* m_arrowTypeLabel = nullptr;
    QLabel* m_topLeftCornerRadiusLabel = nullptr;
    QLabel* m_topRightCornerRadiusLabel = nullptr;
    QLabel* m_bottomRightCornerRadiusLabel = nullptr;
    QLabel* m_bottomLeftCornerRadiusLabel = nullptr;
    QPushButton* m_primaryColorButton = nullptr;
    QPushButton* m_fillColorButton = nullptr;
    QPushButton* m_strokeColorButton = nullptr;
    QComboBox* m_fontFamilyComboBox = nullptr;
    QComboBox* m_startArrowheadComboBox = nullptr;
    QComboBox* m_endArrowheadComboBox = nullptr;
    QComboBox* m_arrowStrokeStyleComboBox = nullptr;
    QComboBox* m_arrowTypeComboBox = nullptr;
    QComboBox* m_fillStyleComboBox = nullptr;
    QComboBox* m_textHorizontalAlignComboBox = nullptr;
    QDoubleSpinBox* m_gridSizeSpinBox = nullptr;
    QDoubleSpinBox* m_fontSizeSpinBox = nullptr;
    QDoubleSpinBox* m_opacitySpinBox = nullptr;
    QDoubleSpinBox* m_strokeWidthSpinBox = nullptr;
    QDoubleSpinBox* m_topLeftCornerRadiusSpinBox = nullptr;
    QDoubleSpinBox* m_topRightCornerRadiusSpinBox = nullptr;
    QDoubleSpinBox* m_bottomRightCornerRadiusSpinBox = nullptr;
    QDoubleSpinBox* m_bottomLeftCornerRadiusSpinBox = nullptr;
    QColor m_primaryColor;
    QColor m_fillColor;
    QColor m_strokeColor;
    bool m_primaryColorMixed = false;
    bool m_fillColorMixed = false;
    bool m_strokeColorMixed = false;
    bool m_fillStyleMixed = false;
    bool m_fontSizeMixed = false;
    bool m_fontFamilyMixed = false;
    bool m_opacityMixed = false;
    bool m_textAlignMixed = false;
    bool m_strokeWidthMixed = false;
    bool m_cornerRadiiMixed = false;
    bool m_syncingUi = false;
};
