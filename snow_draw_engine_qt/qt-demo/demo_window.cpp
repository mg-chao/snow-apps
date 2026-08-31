#include "demo_window.h"
#include "demo_serial_number_controls.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <functional>

namespace {

constexpr int kPanelWidth = 272;
constexpr double kMinGridSize = 5.0;
constexpr double kMaxGridSize = 100.0;
constexpr double kMinTextFontSize = 6.0;
constexpr int kFontFamilyItemKindRole = Qt::UserRole + 1;

enum class FontFamilyItemKind {
    Mixed,
    Default,
    Family,
    Unavailable,
};

class SerialNumberLineEdit : public QLineEdit {
  public:
    using StepCallback = std::function<void(qint64)>;

    explicit SerialNumberLineEdit(QWidget* parent = nullptr) : QLineEdit(parent) {}

    void setStepCallback(StepCallback callback) {
        m_stepCallback = std::move(callback);
    }

  protected:
    void wheelEvent(QWheelEvent* event) override {
        const QPoint angleDelta = event->angleDelta();
        const QPoint pixelDelta = event->pixelDelta();
        const int primaryDelta = angleDelta.y() != 0 ? angleDelta.y() : pixelDelta.y();
        if (primaryDelta == 0 || !m_stepCallback) {
            QLineEdit::wheelEvent(event);
            return;
        }

        m_stepCallback(primaryDelta < 0 ? 1 : -1);
        event->accept();
    }

  private:
    StepCallback m_stepCallback;
};

QFrame* makeSidebarPanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setFrameShape(QFrame::NoFrame);
    panel->setAttribute(Qt::WA_StyledBackground, true);
    panel->setStyleSheet(
        "QFrame {"
        "  background: rgba(252, 252, 252, 232);"
        "  border: 1px solid rgba(20, 24, 28, 36);"
        "  border-radius: 12px;"
        "}"
        "QLabel { color: #17232e; }"
        "QPushButton, QToolButton, QDoubleSpinBox, QComboBox, QCheckBox, QLineEdit {"
        "  font-size: 12px;"
        "}"
        "QPushButton, QToolButton {"
        "  background: white;"
        "  border: 1px solid rgba(20, 24, 28, 32);"
        "  border-radius: 8px;"
        "  padding: 6px 10px;"
        "}"
        "QToolButton:checked {"
        "  background: #17232e;"
        "  color: white;"
        "  border-color: #17232e;"
        "}"
        "QDoubleSpinBox {"
        "  background: white;"
        "  border: 1px solid rgba(20, 24, 28, 32);"
        "  border-radius: 8px;"
        "  padding: 4px 8px;"
        "}"
        "QLineEdit {"
        "  background: white;"
        "  border: 1px solid rgba(20, 24, 28, 32);"
        "  border-radius: 8px;"
        "  padding: 4px 8px;"
        "}"
        "QComboBox {"
        "  background: white;"
        "  border: 1px solid rgba(20, 24, 28, 32);"
        "  border-radius: 8px;"
        "  padding: 4px 8px;"
        "}"
        "QComboBox::drop-down {"
        "  border: 0px;"
        "  width: 22px;"
        "}"
        "QCheckBox {"
        "  spacing: 6px;"
        "}");
    panel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    panel->setFixedWidth(kPanelWidth);
    return panel;
}

void configureCornerSpinBox(QDoubleSpinBox* spinBox) {
    spinBox->setRange(0.0, 999.0);
    spinBox->setDecimals(1);
    spinBox->setSingleStep(1.0);
}

QString arrowheadLabel(SnowCanvasArrowhead arrowhead) {
    switch (arrowhead) {
    case SnowCanvasArrowhead::None:
        return "None";
    case SnowCanvasArrowhead::Arrow:
        return "Arrow";
    case SnowCanvasArrowhead::Bar:
        return "Bar";
    case SnowCanvasArrowhead::Dot:
        return "Dot";
    case SnowCanvasArrowhead::Circle:
        return "Circle";
    case SnowCanvasArrowhead::CircleOutline:
        return "Circle Outline";
    case SnowCanvasArrowhead::Triangle:
        return "Triangle";
    case SnowCanvasArrowhead::TriangleOutline:
        return "Triangle Outline";
    case SnowCanvasArrowhead::Diamond:
        return "Diamond";
    case SnowCanvasArrowhead::DiamondOutline:
        return "Diamond Outline";
    case SnowCanvasArrowhead::CrowfootOne:
        return "Crowfoot One";
    case SnowCanvasArrowhead::CrowfootMany:
        return "Crowfoot Many";
    case SnowCanvasArrowhead::CrowfootOneOrMany:
        return "Crowfoot One Or Many";
    }
    return "Unknown";
}

QString arrowStrokeStyleLabel(SnowCanvasStrokeStyle strokeStyle) {
    switch (strokeStyle) {
    case SnowCanvasStrokeStyle::Solid:
        return "Solid";
    case SnowCanvasStrokeStyle::Dashed:
        return "Dashed";
    case SnowCanvasStrokeStyle::Dotted:
        return "Dotted";
    }
    return "Unknown";
}

QString arrowTypeLabel(SnowCanvasArrowType arrowType) {
    switch (arrowType) {
    case SnowCanvasArrowType::Straight:
        return "Straight";
    case SnowCanvasArrowType::Curve:
        return "Curve";
    case SnowCanvasArrowType::Elbow:
        return "Elbow";
    }
    return "Unknown";
}

QString fillStyleLabel(SnowCanvasFillStyle fillStyle) {
    switch (fillStyle) {
    case SnowCanvasFillStyle::Line:
        return "Line";
    case SnowCanvasFillStyle::CrossLine:
        return "Cross Line";
    case SnowCanvasFillStyle::Solid:
        return "Solid";
    }
    return "Unknown";
}

QString textHorizontalAlignLabel(SnowCanvasTextHorizontalAlign align) {
    switch (align) {
    case SnowCanvasTextHorizontalAlign::Left:
        return "Left";
    case SnowCanvasTextHorizontalAlign::Center:
        return "Center";
    case SnowCanvasTextHorizontalAlign::Right:
        return "Right";
    }
    return "Unknown";
}

void populateArrowheadComboBox(QComboBox* comboBox) {
    if (comboBox == nullptr) {
        return;
    }

    const SnowCanvasArrowhead arrowheads[] = {
        SnowCanvasArrowhead::None,
        SnowCanvasArrowhead::Arrow,
        SnowCanvasArrowhead::Bar,
        SnowCanvasArrowhead::Dot,
        SnowCanvasArrowhead::Circle,
        SnowCanvasArrowhead::CircleOutline,
        SnowCanvasArrowhead::Triangle,
        SnowCanvasArrowhead::TriangleOutline,
        SnowCanvasArrowhead::Diamond,
        SnowCanvasArrowhead::DiamondOutline,
        SnowCanvasArrowhead::CrowfootOne,
        SnowCanvasArrowhead::CrowfootMany,
        SnowCanvasArrowhead::CrowfootOneOrMany,
    };
    for (SnowCanvasArrowhead arrowhead : arrowheads) {
        comboBox->addItem(arrowheadLabel(arrowhead), static_cast<int>(arrowhead));
    }
}

void populateArrowStrokeStyleComboBox(QComboBox* comboBox) {
    if (comboBox == nullptr) {
        return;
    }

    const SnowCanvasStrokeStyle strokeStyles[] = {
        SnowCanvasStrokeStyle::Solid,
        SnowCanvasStrokeStyle::Dashed,
        SnowCanvasStrokeStyle::Dotted,
    };
    for (SnowCanvasStrokeStyle strokeStyle : strokeStyles) {
        comboBox->addItem(arrowStrokeStyleLabel(strokeStyle), static_cast<int>(strokeStyle));
    }
}

void populateArrowTypeComboBox(QComboBox* comboBox) {
    if (comboBox == nullptr) {
        return;
    }

    const SnowCanvasArrowType arrowTypes[] = {
        SnowCanvasArrowType::Straight,
        SnowCanvasArrowType::Curve,
        SnowCanvasArrowType::Elbow,
    };
    for (SnowCanvasArrowType arrowType : arrowTypes) {
        comboBox->addItem(arrowTypeLabel(arrowType), static_cast<int>(arrowType));
    }
}

void populateFillStyleComboBox(QComboBox* comboBox) {
    if (comboBox == nullptr) {
        return;
    }

    comboBox->addItem("Mixed", -1);
    const SnowCanvasFillStyle fillStyles[] = {
        SnowCanvasFillStyle::Solid,
        SnowCanvasFillStyle::Line,
        SnowCanvasFillStyle::CrossLine,
    };
    for (SnowCanvasFillStyle fillStyle : fillStyles) {
        comboBox->addItem(fillStyleLabel(fillStyle), static_cast<int>(fillStyle));
    }
}

void populateTextHorizontalAlignComboBox(QComboBox* comboBox) {
    if (comboBox == nullptr) {
        return;
    }

    comboBox->addItem("Mixed", -1);
    const SnowCanvasTextHorizontalAlign alignments[] = {
        SnowCanvasTextHorizontalAlign::Left,
        SnowCanvasTextHorizontalAlign::Center,
        SnowCanvasTextHorizontalAlign::Right,
    };
    for (SnowCanvasTextHorizontalAlign align : alignments) {
        comboBox->addItem(textHorizontalAlignLabel(align), static_cast<int>(align));
    }
}

void setComboBoxItemEnabled(QComboBox* comboBox, int index, bool enabled) {
    if (comboBox == nullptr) {
        return;
    }

    auto* model = qobject_cast<QStandardItemModel*>(comboBox->model());
    if (model == nullptr) {
        return;
    }

    if (QStandardItem* item = model->item(index)) {
        item->setEnabled(enabled);
    }
}

void addFontFamilyComboBoxItem(QComboBox* comboBox, const QString& label, const QString& fontFamily,
                               FontFamilyItemKind kind, bool enabled, bool previewWithFont) {
    if (comboBox == nullptr) {
        return;
    }

    comboBox->addItem(label, fontFamily);
    const int index = comboBox->count() - 1;
    comboBox->setItemData(index, static_cast<int>(kind), kFontFamilyItemKindRole);
    if (previewWithFont && !fontFamily.isEmpty()) {
        comboBox->setItemData(index, QFont(fontFamily), Qt::FontRole);
    }
    setComboBoxItemEnabled(comboBox, index, enabled);
}

void populateFontFamilyComboBox(QComboBox* comboBox) {
    if (comboBox == nullptr) {
        return;
    }

    comboBox->clear();
    comboBox->setMaxVisibleItems(18);
    comboBox->setMinimumContentsLength(14);
    comboBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    addFontFamilyComboBoxItem(comboBox, "Mixed", QString(), FontFamilyItemKind::Mixed, false,
                              false);
    addFontFamilyComboBoxItem(comboBox, "Default", QString(), FontFamilyItemKind::Default, true,
                              false);

    QStringList families = QFontDatabase::families();
    families.removeDuplicates();
    families.sort(Qt::CaseInsensitive);
    for (const QString& family : families) {
        const QString trimmed = family.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        addFontFamilyComboBoxItem(comboBox, trimmed, trimmed, FontFamilyItemKind::Family, true,
                                  true);
    }
}

FontFamilyItemKind fontFamilyComboBoxItemKind(const QComboBox* comboBox, int index) {
    if (comboBox == nullptr || index < 0 || index >= comboBox->count()) {
        return FontFamilyItemKind::Default;
    }
    return static_cast<FontFamilyItemKind>(
        comboBox->itemData(index, kFontFamilyItemKindRole).toInt());
}

int findFontFamilyComboBoxItem(QComboBox* comboBox, FontFamilyItemKind kind) {
    if (comboBox == nullptr) {
        return -1;
    }
    for (int index = 0; index < comboBox->count(); ++index) {
        if (fontFamilyComboBoxItemKind(comboBox, index) == kind) {
            return index;
        }
    }
    return -1;
}

int findFontFamilyComboBoxValue(QComboBox* comboBox, const QString& fontFamily) {
    if (comboBox == nullptr) {
        return -1;
    }
    const QString trimmed = fontFamily.trimmed();
    for (int index = 0; index < comboBox->count(); ++index) {
        const FontFamilyItemKind kind = fontFamilyComboBoxItemKind(comboBox, index);
        if (kind != FontFamilyItemKind::Family && kind != FontFamilyItemKind::Unavailable) {
            continue;
        }
        const QString itemFamily = comboBox->itemData(index).toString().trimmed();
        if (itemFamily == trimmed) {
            return index;
        }
    }
    for (int index = 0; index < comboBox->count(); ++index) {
        const FontFamilyItemKind kind = fontFamilyComboBoxItemKind(comboBox, index);
        if (kind != FontFamilyItemKind::Family && kind != FontFamilyItemKind::Unavailable) {
            continue;
        }
        const QString itemFamily = comboBox->itemData(index).toString().trimmed();
        if (QString::compare(itemFamily, trimmed, Qt::CaseInsensitive) == 0) {
            return index;
        }
    }
    return -1;
}

void removeUnavailableFontFamilyComboBoxItems(QComboBox* comboBox) {
    if (comboBox == nullptr) {
        return;
    }
    for (int index = comboBox->count() - 1; index >= 0; --index) {
        if (fontFamilyComboBoxItemKind(comboBox, index) == FontFamilyItemKind::Unavailable) {
            comboBox->removeItem(index);
        }
    }
}

void setFontFamilyComboBoxValue(QComboBox* comboBox, const QString& fontFamily) {
    if (comboBox == nullptr) {
        return;
    }

    removeUnavailableFontFamilyComboBoxItems(comboBox);
    const QString trimmed = fontFamily.trimmed();
    if (trimmed.isEmpty()) {
        const int index = findFontFamilyComboBoxItem(comboBox, FontFamilyItemKind::Default);
        comboBox->setCurrentIndex(index >= 0 ? index : 0);
        return;
    }

    int index = findFontFamilyComboBoxValue(comboBox, trimmed);
    if (index < 0) {
        addFontFamilyComboBoxItem(comboBox, trimmed + " (Unavailable)", trimmed,
                                  FontFamilyItemKind::Unavailable, false, false);
        index = comboBox->count() - 1;
    }
    comboBox->setCurrentIndex(index);
}

void setFontFamilyComboBoxMixed(QComboBox* comboBox, bool mixed, const QString& fontFamily) {
    if (comboBox == nullptr) {
        return;
    }
    if (mixed) {
        removeUnavailableFontFamilyComboBoxItems(comboBox);
        const int index = findFontFamilyComboBoxItem(comboBox, FontFamilyItemKind::Mixed);
        comboBox->setCurrentIndex(index >= 0 ? index : 0);
        return;
    }
    setFontFamilyComboBoxValue(comboBox, fontFamily);
}

QString fontFamilyFromComboBox(const QComboBox* comboBox) {
    if (comboBox == nullptr) {
        return QString();
    }
    const FontFamilyItemKind kind = fontFamilyComboBoxItemKind(comboBox, comboBox->currentIndex());
    if (kind != FontFamilyItemKind::Family && kind != FontFamilyItemKind::Unavailable) {
        return QString();
    }
    return comboBox->currentData().toString().trimmed();
}

SnowCanvasArrowhead arrowheadFromComboBox(const QComboBox* comboBox) {
    return static_cast<SnowCanvasArrowhead>(comboBox != nullptr ? comboBox->currentData().toInt()
                                                                : 0);
}

SnowCanvasStrokeStyle arrowStrokeStyleFromComboBox(const QComboBox* comboBox) {
    return static_cast<SnowCanvasStrokeStyle>(
        comboBox != nullptr ? comboBox->currentData().toInt() : 0);
}

SnowCanvasArrowType arrowTypeFromComboBox(const QComboBox* comboBox) {
    return static_cast<SnowCanvasArrowType>(comboBox != nullptr ? comboBox->currentData().toInt()
                                                                : 0);
}

SnowCanvasFillStyle fillStyleFromComboBox(const QComboBox* comboBox) {
    const int value = comboBox != nullptr ? comboBox->currentData().toInt() : 0;
    return static_cast<SnowCanvasFillStyle>(
        value >= 0 ? value : static_cast<int>(SnowCanvasFillStyle::Solid));
}

SnowCanvasTextHorizontalAlign textHorizontalAlignFromComboBox(const QComboBox* comboBox) {
    const int value = comboBox != nullptr ? comboBox->currentData().toInt() : 0;
    return static_cast<SnowCanvasTextHorizontalAlign>(
        value >= 0 ? value : static_cast<int>(SnowCanvasTextHorizontalAlign::Left));
}

void setComboBoxValue(QComboBox* comboBox, int value) {
    if (comboBox == nullptr) {
        return;
    }

    const int index = comboBox->findData(value);
    comboBox->setCurrentIndex(index >= 0 ? index : 0);
}

void setStyleRowVisible(QLabel* label, QWidget* control, bool visible) {
    if (label != nullptr) {
        label->setVisible(visible);
    }
    if (control != nullptr) {
        control->setVisible(visible);
    }
}

void setComboBoxMixed(QComboBox* comboBox, bool mixed, int value) {
    if (comboBox == nullptr) {
        return;
    }
    setComboBoxValue(comboBox, mixed ? -1 : value);
}

void setSpinBoxMixed(QDoubleSpinBox* spinBox, bool mixed, double value) {
    if (spinBox == nullptr) {
        return;
    }
    if (mixed) {
        spinBox->setSpecialValueText("Mixed");
        spinBox->setValue(spinBox->minimum());
        return;
    }
    spinBox->setSpecialValueText(QString());
    spinBox->setValue(value);
}

} // namespace

DemoWindow::DemoWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle("Snow Draw Engine Qt Canvas Demo");
    setObjectName("demoWindow");
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet("QWidget#demoWindow { background: #eef2f7; }");

    initializeUi();
    connectCanvasSignals();
    syncAllControls();
    refocusCanvas();
}

void DemoWindow::initializeUi() {
    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(16);

    auto* toolPanel = makeSidebarPanel(this);
    auto* toolLayout = new QVBoxLayout(toolPanel);
    toolLayout->setContentsMargins(14, 14, 14, 14);
    toolLayout->setSpacing(10);

    auto* toolTitle = new QLabel("Tools", toolPanel);
    toolTitle->setStyleSheet("font-weight: 600; color: #17232e;");
    toolLayout->addWidget(toolTitle);

    auto* historyRow = new QHBoxLayout();
    historyRow->setSpacing(8);
    m_undoToolButton = new QToolButton(toolPanel);
    m_undoToolButton->setText("Undo");
    m_undoToolButton->setToolTip("Undo the last committed edit (Ctrl+Z)");
    m_undoToolButton->setFocusPolicy(Qt::NoFocus);
    m_redoToolButton = new QToolButton(toolPanel);
    m_redoToolButton->setText("Redo");
    m_redoToolButton->setToolTip("Redo the last undone edit (Ctrl+Shift+Z)");
    m_redoToolButton->setFocusPolicy(Qt::NoFocus);
    historyRow->addWidget(m_undoToolButton);
    historyRow->addWidget(m_redoToolButton);
    toolLayout->addLayout(historyRow);

    auto* toolGrid = new QGridLayout();
    toolGrid->setHorizontalSpacing(8);
    toolGrid->setVerticalSpacing(8);
    m_selectToolButton = new QToolButton(toolPanel);
    m_selectToolButton->setText("Select (V)");
    m_selectToolButton->setCheckable(true);
    m_selectToolButton->setFocusPolicy(Qt::NoFocus);
    m_shapeToolButton = new QToolButton(toolPanel);
    m_shapeToolButton->setText("Shape (R)");
    m_shapeToolButton->setCheckable(true);
    m_shapeToolButton->setFocusPolicy(Qt::NoFocus);
    m_arrowToolButton = new QToolButton(toolPanel);
    m_arrowToolButton->setText("Arrow (A)");
    m_arrowToolButton->setCheckable(true);
    m_arrowToolButton->setFocusPolicy(Qt::NoFocus);
    m_textToolButton = new QToolButton(toolPanel);
    m_textToolButton->setText("Text (T)");
    m_textToolButton->setCheckable(true);
    m_textToolButton->setFocusPolicy(Qt::NoFocus);
    m_serialNumberToolButton = new QToolButton(toolPanel);
    m_serialNumberToolButton->setText("Serial (N)");
    m_serialNumberToolButton->setCheckable(true);
    m_serialNumberToolButton->setFocusPolicy(Qt::NoFocus);
    toolGrid->addWidget(m_selectToolButton, 0, 0);
    toolGrid->addWidget(m_shapeToolButton, 0, 1);
    toolGrid->addWidget(m_arrowToolButton, 1, 0);
    toolGrid->addWidget(m_textToolButton, 1, 1);
    toolGrid->addWidget(m_serialNumberToolButton, 2, 0, 1, 2);
    toolLayout->addLayout(toolGrid);

    auto* serialTitle = new QLabel("Serial Number", toolPanel);
    serialTitle->setStyleSheet("font-weight: 600; color: #17232e;");
    toolLayout->addWidget(serialTitle);

    auto* serialGrid = new QGridLayout();
    serialGrid->setHorizontalSpacing(8);
    serialGrid->setVerticalSpacing(8);
    m_createSerialTextToolButton = new QToolButton(toolPanel);
    m_createSerialTextToolButton->setText("Create Text");
    m_createSerialTextToolButton->setToolTip(
        "Create or edit text linked to the selected serial number");
    m_createSerialTextToolButton->setFocusPolicy(Qt::NoFocus);
    m_duplicateSelectionToolButton = new QToolButton(toolPanel);
    m_duplicateSelectionToolButton->setText("Duplicate");
    m_duplicateSelectionToolButton->setToolTip("Duplicate the selected elements");
    m_duplicateSelectionToolButton->setFocusPolicy(Qt::NoFocus);
    m_deleteSelectionToolButton = new QToolButton(toolPanel);
    m_deleteSelectionToolButton->setText("Delete");
    m_deleteSelectionToolButton->setToolTip("Delete the selected elements");
    m_deleteSelectionToolButton->setFocusPolicy(Qt::NoFocus);
    m_decrementSerialNumberToolButton = new QToolButton(toolPanel);
    m_decrementSerialNumberToolButton->setText("-");
    m_decrementSerialNumberToolButton->setToolTip("Decrease");
    m_decrementSerialNumberToolButton->setFocusPolicy(Qt::NoFocus);
    m_decrementSerialNumberToolButton->setAccessibleName("Decrease");
    m_incrementSerialNumberToolButton = new QToolButton(toolPanel);
    m_incrementSerialNumberToolButton->setText("+");
    m_incrementSerialNumberToolButton->setToolTip("Increase");
    m_incrementSerialNumberToolButton->setFocusPolicy(Qt::NoFocus);
    m_incrementSerialNumberToolButton->setAccessibleName("Increase");
    m_serialNumberLineEdit = new SerialNumberLineEdit(toolPanel);
    m_serialNumberLineEdit->setObjectName("serialNumberLineEdit");
    m_serialNumberLineEdit->setToolTip("Number");
    m_serialNumberLineEdit->setAccessibleName("Number");
    m_serialNumberLineEdit->setFixedHeight(32);
    m_serialNumberLineEdit->setMaximumWidth(140);
    m_serialNumberLineEdit->setAlignment(Qt::AlignCenter);
    m_serialNumberLineEdit->setValidator(
        new QRegularExpressionValidator(QRegularExpression("\\d*"), m_serialNumberLineEdit));
    static_cast<SerialNumberLineEdit*>(m_serialNumberLineEdit)
        ->setStepCallback([this](qint64 delta) { stepSerialNumber(delta); });
    auto* serialNumberLabel = new QLabel("Number", toolPanel);
    serialGrid->addWidget(m_createSerialTextToolButton, 0, 0, 1, 2);
    serialGrid->addWidget(m_duplicateSelectionToolButton, 1, 0);
    serialGrid->addWidget(m_deleteSelectionToolButton, 1, 1);
    serialGrid->addWidget(serialNumberLabel, 2, 0, 1, 2);
    auto* serialNumberControlRow = new QHBoxLayout();
    serialNumberControlRow->setSpacing(4);
    serialNumberControlRow->addWidget(m_decrementSerialNumberToolButton);
    serialNumberControlRow->addWidget(m_serialNumberLineEdit, 1);
    serialNumberControlRow->addWidget(m_incrementSerialNumberToolButton);
    serialGrid->addLayout(serialNumberControlRow, 3, 0, 1, 2);
    toolLayout->addLayout(serialGrid);

    auto* snapTitle = new QLabel("Snapping", toolPanel);
    snapTitle->setStyleSheet("font-weight: 600; color: #17232e;");
    toolLayout->addWidget(snapTitle);

    auto* snapRow = new QHBoxLayout();
    snapRow->setSpacing(8);
    m_objectSnapToolButton = new QToolButton(toolPanel);
    m_objectSnapToolButton->setText("Object");
    m_objectSnapToolButton->setCheckable(true);
    m_objectSnapToolButton->setFocusPolicy(Qt::NoFocus);
    m_gridSnapToolButton = new QToolButton(toolPanel);
    m_gridSnapToolButton->setText("Grid");
    m_gridSnapToolButton->setCheckable(true);
    m_gridSnapToolButton->setFocusPolicy(Qt::NoFocus);
    snapRow->addWidget(m_objectSnapToolButton);
    snapRow->addWidget(m_gridSnapToolButton);
    toolLayout->addLayout(snapRow);

    auto* gridSizeRow = new QHBoxLayout();
    gridSizeRow->setSpacing(8);
    auto* gridSizeLabel = new QLabel("Grid Size", toolPanel);
    m_gridSizeSpinBox = new QDoubleSpinBox(toolPanel);
    m_gridSizeSpinBox->setRange(kMinGridSize, kMaxGridSize);
    m_gridSizeSpinBox->setDecimals(1);
    m_gridSizeSpinBox->setSingleStep(1.0);
    m_gridSizeSpinBox->setSuffix(" px");
    gridSizeRow->addWidget(gridSizeLabel);
    gridSizeRow->addWidget(m_gridSizeSpinBox);
    toolLayout->addLayout(gridSizeRow);

    auto* debugTitle = new QLabel("Debug", toolPanel);
    debugTitle->setStyleSheet("font-weight: 600; color: #17232e;");
    toolLayout->addWidget(debugTitle);

    m_showDirtyRectsToolButton = new QToolButton(toolPanel);
    m_showDirtyRectsToolButton->setText("Dirty Rects (F4)");
    m_showDirtyRectsToolButton->setCheckable(true);
    m_showDirtyRectsToolButton->setFocusPolicy(Qt::NoFocus);
    m_showDirtyRectsToolButton->setToolTip("Overlay the latest scene and overlay dirty rectangles");
    toolLayout->addWidget(m_showDirtyRectsToolButton);
    toolLayout->addStretch(1);

    m_canvas = new SnowCanvasWidget(this);
    m_canvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_canvas->setMinimumWidth(480);

    auto* stylePanel = makeSidebarPanel(this);
    auto* styleLayout = new QVBoxLayout(stylePanel);
    styleLayout->setContentsMargins(14, 14, 14, 14);
    styleLayout->setSpacing(10);

    auto* styleTitle = new QLabel("Style", stylePanel);
    styleTitle->setStyleSheet("font-weight: 600; color: #17232e;");
    styleLayout->addWidget(styleTitle);

    m_styleSourceLabel = new QLabel(stylePanel);
    m_styleSourceLabel->setStyleSheet("color: rgba(23, 35, 46, 0.72);");
    styleLayout->addWidget(m_styleSourceLabel);

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);

    m_primaryLabel = new QLabel("Primary", stylePanel);
    m_fillLabel = new QLabel("Fill", stylePanel);
    m_fillStyleLabel = new QLabel("Fill Style", stylePanel);
    m_strokeLabel = new QLabel("Stroke", stylePanel);
    m_strokeWidthLabel = new QLabel("Stroke Width", stylePanel);
    m_fontSizeLabel = new QLabel("Font Size", stylePanel);
    m_fontFamilyLabel = new QLabel("Font Family", stylePanel);
    m_opacityLabel = new QLabel("Opacity", stylePanel);
    m_textAlignLabel = new QLabel("Text Align", stylePanel);
    m_startArrowheadLabel = new QLabel("Start Head", stylePanel);
    m_endArrowheadLabel = new QLabel("End Head", stylePanel);
    m_arrowStrokeStyleLabel = new QLabel("Line Style", stylePanel);
    m_arrowTypeLabel = new QLabel("Arrow Type", stylePanel);
    m_topLeftCornerRadiusLabel = new QLabel("Top Left Radius", stylePanel);
    m_topRightCornerRadiusLabel = new QLabel("Top Right Radius", stylePanel);
    m_bottomRightCornerRadiusLabel = new QLabel("Bottom Right Radius", stylePanel);
    m_bottomLeftCornerRadiusLabel = new QLabel("Bottom Left Radius", stylePanel);

    m_primaryColorButton = new QPushButton("Choose...", stylePanel);
    m_fillColorButton = new QPushButton("Choose...", stylePanel);
    m_strokeColorButton = new QPushButton("Choose...", stylePanel);
    m_primaryColorButton->setFocusPolicy(Qt::NoFocus);
    m_fillColorButton->setFocusPolicy(Qt::NoFocus);
    m_strokeColorButton->setFocusPolicy(Qt::NoFocus);
    m_startArrowheadComboBox = new QComboBox(stylePanel);
    m_endArrowheadComboBox = new QComboBox(stylePanel);
    m_arrowStrokeStyleComboBox = new QComboBox(stylePanel);
    m_arrowTypeComboBox = new QComboBox(stylePanel);
    m_fillStyleComboBox = new QComboBox(stylePanel);
    m_textHorizontalAlignComboBox = new QComboBox(stylePanel);
    populateArrowheadComboBox(m_startArrowheadComboBox);
    populateArrowheadComboBox(m_endArrowheadComboBox);
    populateArrowStrokeStyleComboBox(m_arrowStrokeStyleComboBox);
    populateArrowTypeComboBox(m_arrowTypeComboBox);
    populateFillStyleComboBox(m_fillStyleComboBox);
    populateTextHorizontalAlignComboBox(m_textHorizontalAlignComboBox);
    m_startArrowheadComboBox->setFocusPolicy(Qt::NoFocus);
    m_endArrowheadComboBox->setFocusPolicy(Qt::NoFocus);
    m_arrowStrokeStyleComboBox->setFocusPolicy(Qt::NoFocus);
    m_arrowTypeComboBox->setFocusPolicy(Qt::NoFocus);
    m_fillStyleComboBox->setFocusPolicy(Qt::NoFocus);
    m_textHorizontalAlignComboBox->setFocusPolicy(Qt::NoFocus);
    m_fontFamilyComboBox = new QComboBox(stylePanel);
    populateFontFamilyComboBox(m_fontFamilyComboBox);
    m_fontFamilyComboBox->setFocusPolicy(Qt::NoFocus);
    m_fontFamilyComboBox->setToolTip("Font Family");
    m_fontFamilyComboBox->setAccessibleName("Font Family");
    m_strokeWidthSpinBox = new QDoubleSpinBox(stylePanel);
    m_fontSizeSpinBox = new QDoubleSpinBox(stylePanel);
    m_opacitySpinBox = new QDoubleSpinBox(stylePanel);
    m_topLeftCornerRadiusSpinBox = new QDoubleSpinBox(stylePanel);
    m_topRightCornerRadiusSpinBox = new QDoubleSpinBox(stylePanel);
    m_bottomRightCornerRadiusSpinBox = new QDoubleSpinBox(stylePanel);
    m_bottomLeftCornerRadiusSpinBox = new QDoubleSpinBox(stylePanel);
    m_strokeWidthSpinBox->setRange(0.0, 999.0);
    m_strokeWidthSpinBox->setDecimals(1);
    m_strokeWidthSpinBox->setSingleStep(0.5);
    m_fontSizeSpinBox->setRange(kMinTextFontSize, 256.0);
    m_fontSizeSpinBox->setDecimals(1);
    m_fontSizeSpinBox->setSingleStep(1.0);
    m_opacitySpinBox->setRange(0.0, 1.0);
    m_opacitySpinBox->setDecimals(2);
    m_opacitySpinBox->setSingleStep(0.05);
    configureCornerSpinBox(m_topLeftCornerRadiusSpinBox);
    configureCornerSpinBox(m_topRightCornerRadiusSpinBox);
    configureCornerSpinBox(m_bottomRightCornerRadiusSpinBox);
    configureCornerSpinBox(m_bottomLeftCornerRadiusSpinBox);

    grid->addWidget(m_primaryLabel, 0, 0);
    grid->addWidget(m_primaryColorButton, 0, 1);
    grid->addWidget(m_fillLabel, 1, 0);
    grid->addWidget(m_fillColorButton, 1, 1);
    grid->addWidget(m_fillStyleLabel, 2, 0);
    grid->addWidget(m_fillStyleComboBox, 2, 1);
    grid->addWidget(m_strokeLabel, 3, 0);
    grid->addWidget(m_strokeColorButton, 3, 1);
    grid->addWidget(m_strokeWidthLabel, 4, 0);
    grid->addWidget(m_strokeWidthSpinBox, 4, 1);
    grid->addWidget(m_fontSizeLabel, 5, 0);
    grid->addWidget(m_fontSizeSpinBox, 5, 1);
    grid->addWidget(m_fontFamilyLabel, 6, 0);
    grid->addWidget(m_fontFamilyComboBox, 6, 1);
    grid->addWidget(m_opacityLabel, 7, 0);
    grid->addWidget(m_opacitySpinBox, 7, 1);
    grid->addWidget(m_textAlignLabel, 8, 0);
    grid->addWidget(m_textHorizontalAlignComboBox, 8, 1);
    grid->addWidget(m_startArrowheadLabel, 9, 0);
    grid->addWidget(m_startArrowheadComboBox, 9, 1);
    grid->addWidget(m_endArrowheadLabel, 10, 0);
    grid->addWidget(m_endArrowheadComboBox, 10, 1);
    grid->addWidget(m_arrowStrokeStyleLabel, 11, 0);
    grid->addWidget(m_arrowStrokeStyleComboBox, 11, 1);
    grid->addWidget(m_arrowTypeLabel, 12, 0);
    grid->addWidget(m_arrowTypeComboBox, 12, 1);
    grid->addWidget(m_topLeftCornerRadiusLabel, 13, 0);
    grid->addWidget(m_topLeftCornerRadiusSpinBox, 13, 1);
    grid->addWidget(m_topRightCornerRadiusLabel, 14, 0);
    grid->addWidget(m_topRightCornerRadiusSpinBox, 14, 1);
    grid->addWidget(m_bottomRightCornerRadiusLabel, 15, 0);
    grid->addWidget(m_bottomRightCornerRadiusSpinBox, 15, 1);
    grid->addWidget(m_bottomLeftCornerRadiusLabel, 16, 0);
    grid->addWidget(m_bottomLeftCornerRadiusSpinBox, 16, 1);
    styleLayout->addLayout(grid);
    styleLayout->addStretch(1);

    rootLayout->addWidget(toolPanel);
    rootLayout->addWidget(m_canvas, 1);
    rootLayout->addWidget(stylePanel);

    connect(m_undoToolButton, &QToolButton::clicked, this, [this]() {
        m_canvas->undo();
        refocusCanvas();
    });
    connect(m_redoToolButton, &QToolButton::clicked, this, [this]() {
        m_canvas->redo();
        refocusCanvas();
    });
    connect(m_selectToolButton, &QToolButton::clicked, this, [this]() {
        m_canvas->setCanvasTool(SnowCanvasTool::Select);
        refocusCanvas();
    });
    connect(m_shapeToolButton, &QToolButton::clicked, this, [this]() {
        m_canvas->setCanvasTool(SnowCanvasTool::Shape);
        refocusCanvas();
    });
    connect(m_arrowToolButton, &QToolButton::clicked, this, [this]() {
        m_canvas->setCanvasTool(SnowCanvasTool::Arrow);
        refocusCanvas();
    });
    connect(m_textToolButton, &QToolButton::clicked, this, [this]() {
        m_canvas->setCanvasTool(SnowCanvasTool::Text);
        refocusCanvas();
    });
    connect(m_serialNumberToolButton, &QToolButton::clicked, this, [this]() {
        m_canvas->setCanvasTool(SnowCanvasTool::SerialNumber);
        refocusCanvas();
    });
    connect(m_createSerialTextToolButton, &QToolButton::clicked, this, [this]() {
        m_canvas->createSerialNumberText();
        refocusCanvas();
    });
    connect(m_duplicateSelectionToolButton, &QToolButton::clicked, this, [this]() {
        m_canvas->duplicateSelected();
        refocusCanvas();
    });
    connect(m_deleteSelectionToolButton, &QToolButton::clicked, this, [this]() {
        m_canvas->deleteSelected();
        refocusCanvas();
    });
    connect(m_decrementSerialNumberToolButton, &QToolButton::clicked, this,
            [this]() { stepSerialNumber(-1); });
    connect(m_incrementSerialNumberToolButton, &QToolButton::clicked, this,
            [this]() { stepSerialNumber(1); });
    connect(m_serialNumberLineEdit, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (!text.isEmpty()) {
            applySerialNumberFromControls();
        }
    });
    connect(m_serialNumberLineEdit, &QLineEdit::editingFinished, this, [this]() {
        if (m_serialNumberLineEdit->text().isEmpty()) {
            syncStyleControls();
            return;
        }
        applySerialNumberFromControls();
    });
    connect(m_objectSnapToolButton, &QToolButton::clicked, this,
            &DemoWindow::applySnapConfigFromControls);
    connect(m_gridSnapToolButton, &QToolButton::clicked, this,
            &DemoWindow::applyGridConfigFromControls);
    connect(m_showDirtyRectsToolButton, &QToolButton::toggled, this, [this](bool checked) {
        if (m_syncingUi) {
            return;
        }
        m_canvas->setShowDirtyRects(checked);
        refocusCanvas();
    });
    connect(m_gridSizeSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double) { applyGridConfigFromControls(); });
    connect(m_primaryColorButton, &QPushButton::clicked, this, &DemoWindow::choosePrimaryColor);
    connect(m_fillColorButton, &QPushButton::clicked, this, &DemoWindow::chooseFillColor);
    connect(m_strokeColorButton, &QPushButton::clicked, this, &DemoWindow::chooseStrokeColor);
    connect(m_strokeWidthSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double) {
                clearMixedFlagForSender(m_strokeWidthSpinBox);
                applyCurrentStyleFromControls();
            });
    connect(m_fontSizeSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double) {
                clearMixedFlagForSender(m_fontSizeSpinBox);
                applyCurrentStyleFromControls();
            });
    connect(m_fontFamilyComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) {
                clearMixedFlagForSender(m_fontFamilyComboBox);
                applyCurrentStyleFromControls();
            });
    connect(m_opacitySpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double) {
                clearMixedFlagForSender(m_opacitySpinBox);
                applyCurrentStyleFromControls();
            });
    connect(m_fillStyleComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) {
                clearMixedFlagForSender(m_fillStyleComboBox);
                applyCurrentStyleFromControls();
            });
    connect(m_textHorizontalAlignComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) {
                clearMixedFlagForSender(m_textHorizontalAlignComboBox);
                applyCurrentStyleFromControls();
            });
    connect(m_startArrowheadComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { applyCurrentStyleFromControls(); });
    connect(m_endArrowheadComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { applyCurrentStyleFromControls(); });
    connect(m_arrowStrokeStyleComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { applyCurrentStyleFromControls(); });
    connect(m_arrowTypeComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { applyCurrentStyleFromControls(); });
    connect(m_topLeftCornerRadiusSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double) {
                clearMixedFlagForSender(m_topLeftCornerRadiusSpinBox);
                applyCurrentStyleFromControls();
            });
    connect(m_topRightCornerRadiusSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double) {
                clearMixedFlagForSender(m_topRightCornerRadiusSpinBox);
                applyCurrentStyleFromControls();
            });
    connect(m_bottomRightCornerRadiusSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double) {
                clearMixedFlagForSender(m_bottomRightCornerRadiusSpinBox);
                applyCurrentStyleFromControls();
            });
    connect(m_bottomLeftCornerRadiusSpinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double) {
                clearMixedFlagForSender(m_bottomLeftCornerRadiusSpinBox);
                applyCurrentStyleFromControls();
            });
}

void DemoWindow::connectCanvasSignals() {
    connect(m_canvas, &SnowCanvasWidget::activeToolChanged, this, &DemoWindow::syncToolControls);
    connect(m_canvas, &SnowCanvasWidget::historyStateChanged, this,
            &DemoWindow::syncHistoryControls);
    connect(m_canvas, &SnowCanvasWidget::snapConfigChanged, this, &DemoWindow::syncSnapControls);
    connect(m_canvas, &SnowCanvasWidget::gridConfigChanged, this, &DemoWindow::syncSnapControls);
    connect(m_canvas, &SnowCanvasWidget::styleToolbarStateChanged, this,
            &DemoWindow::syncStyleControls);
    connect(m_canvas, &SnowCanvasWidget::showDirtyRectsChanged, this,
            &DemoWindow::syncDebugControls);
}

void DemoWindow::syncAllControls() {
    syncToolControls();
    syncHistoryControls();
    syncSnapControls();
    syncStyleControls();
    syncDebugControls();
}

void DemoWindow::syncToolControls() {
    const QSignalBlocker selectBlocker(m_selectToolButton);
    const QSignalBlocker shapeBlocker(m_shapeToolButton);
    const QSignalBlocker arrowBlocker(m_arrowToolButton);
    const QSignalBlocker textBlocker(m_textToolButton);
    const QSignalBlocker serialNumberBlocker(m_serialNumberToolButton);
    const SnowCanvasTool activeTool = m_canvas->canvasTool();
    m_selectToolButton->setChecked(activeTool == SnowCanvasTool::Select);
    m_shapeToolButton->setChecked(activeTool == SnowCanvasTool::Shape);
    m_arrowToolButton->setChecked(activeTool == SnowCanvasTool::Arrow);
    m_textToolButton->setChecked(activeTool == SnowCanvasTool::Text);
    m_serialNumberToolButton->setChecked(activeTool == SnowCanvasTool::SerialNumber);
}

void DemoWindow::syncHistoryControls() {
    const SnowCanvasHistoryState state = m_canvas->canvasHistoryState();
    m_undoToolButton->setEnabled(state.canUndo);
    m_redoToolButton->setEnabled(state.canRedo);
}

void DemoWindow::syncSnapControls() {
    const SnowCanvasSnapConfig snapConfig = m_canvas->canvasSnapConfig();
    const SnowCanvasGridConfig gridConfig = m_canvas->canvasGridConfig();
    const bool objectEnabled = snapConfig.enabled;
    const bool gridEnabled = gridConfig.enabled;

    m_syncingUi = true;
    const QSignalBlocker objectSnapBlocker(m_objectSnapToolButton);
    const QSignalBlocker gridSnapBlocker(m_gridSnapToolButton);
    const QSignalBlocker gridSizeBlocker(m_gridSizeSpinBox);
    m_objectSnapToolButton->setChecked(objectEnabled);
    m_gridSnapToolButton->setChecked(gridEnabled);
    m_objectSnapToolButton->setEnabled(!gridEnabled || objectEnabled);
    m_gridSnapToolButton->setEnabled(!objectEnabled || gridEnabled);
    m_gridSizeSpinBox->setValue(gridConfig.size);
    m_syncingUi = false;
}

void DemoWindow::syncStyleControls() {
    const SnowCanvasStyleToolbarState state = m_canvas->canvasStyleToolbarState();
    m_primaryColor = state.shapeStyle.stroke;
    m_fillColor = state.shapeStyle.fill;
    m_strokeColor = state.shapeStyle.stroke;
    SnowCanvasFillStyle fillStyle = SnowCanvasFillStyle::Solid;
    double strokeWidth = state.shapeStyle.strokeWidth;
    double fontSize = 21.0;
    QString fontFamily;
    double opacity = 1.0;
    SnowCanvasTextHorizontalAlign textAlign = SnowCanvasTextHorizontalAlign::Left;
    SnowCanvasCornerRadii cornerRadii = state.shapeStyle.cornerRadii;

    switch (state.source) {
    case SnowCanvasStyleToolbarSource::SelectedRectangle:
        m_styleSourceLabel->setText("Selected Rectangle");
        break;
    case SnowCanvasStyleToolbarSource::SelectedArrow:
        m_styleSourceLabel->setText("Selected Arrow");
        m_primaryColor = state.shapeStyle.stroke;
        m_strokeColor = state.shapeStyle.stroke;
        strokeWidth = state.shapeStyle.strokeWidth;
        break;
    case SnowCanvasStyleToolbarSource::SelectedText:
        m_styleSourceLabel->setText("Selected Text");
        m_primaryColor = state.textStyle.color;
        m_fillColor = state.textStyle.fill;
        m_strokeColor = state.textStyle.stroke;
        fillStyle = state.textStyle.fillStyle;
        strokeWidth = state.textStyle.strokeWidth;
        fontSize = state.textStyle.fontSize;
        fontFamily = state.textStyle.fontFamily;
        opacity = state.textStyle.opacity;
        textAlign = state.textStyle.horizontalAlign;
        cornerRadii = state.textStyle.cornerRadii;
        break;
    case SnowCanvasStyleToolbarSource::SelectedSerialNumber:
        m_styleSourceLabel->setText("Selected Serial Number");
        m_primaryColor = state.serialNumberStyle.color;
        m_fillColor = state.serialNumberStyle.fill;
        m_strokeColor = state.serialNumberStyle.color;
        fillStyle = state.serialNumberStyle.fillStyle;
        strokeWidth = state.serialNumberStyle.strokeWidth;
        fontSize = state.serialNumberStyle.fontSize;
        fontFamily = state.serialNumberStyle.fontFamily;
        opacity = state.serialNumberStyle.opacity;
        break;
    case SnowCanvasStyleToolbarSource::DefaultArrow:
        m_styleSourceLabel->setText("New Arrow Defaults");
        m_primaryColor = state.shapeStyle.stroke;
        m_strokeColor = state.shapeStyle.stroke;
        strokeWidth = state.shapeStyle.strokeWidth;
        break;
    case SnowCanvasStyleToolbarSource::DefaultText:
        m_styleSourceLabel->setText("New Text Defaults");
        m_primaryColor = state.textStyle.color;
        m_fillColor = state.textStyle.fill;
        m_strokeColor = state.textStyle.stroke;
        fillStyle = state.textStyle.fillStyle;
        strokeWidth = state.textStyle.strokeWidth;
        fontSize = state.textStyle.fontSize;
        fontFamily = state.textStyle.fontFamily;
        opacity = state.textStyle.opacity;
        textAlign = state.textStyle.horizontalAlign;
        cornerRadii = state.textStyle.cornerRadii;
        break;
    case SnowCanvasStyleToolbarSource::DefaultSerialNumber:
        m_styleSourceLabel->setText("New Serial Number Defaults");
        m_primaryColor = state.serialNumberStyle.color;
        m_fillColor = state.serialNumberStyle.fill;
        m_strokeColor = state.serialNumberStyle.color;
        fillStyle = state.serialNumberStyle.fillStyle;
        strokeWidth = state.serialNumberStyle.strokeWidth;
        fontSize = state.serialNumberStyle.fontSize;
        fontFamily = state.serialNumberStyle.fontFamily;
        opacity = state.serialNumberStyle.opacity;
        break;
    case SnowCanvasStyleToolbarSource::DefaultRectangle:
    default:
        m_styleSourceLabel->setText("New Rectangle Defaults");
        break;
    }
    const bool textControlsVisible = state.source == SnowCanvasStyleToolbarSource::DefaultText ||
                                     state.source == SnowCanvasStyleToolbarSource::SelectedText;
    const bool serialControlsVisible =
        state.source == SnowCanvasStyleToolbarSource::DefaultSerialNumber ||
        state.source == SnowCanvasStyleToolbarSource::SelectedSerialNumber;
    const bool selectedTextControlsVisible =
        state.source == SnowCanvasStyleToolbarSource::SelectedText;
    const bool selectedSerialControlsVisible =
        state.source == SnowCanvasStyleToolbarSource::SelectedSerialNumber;

    m_primaryColorMixed =
        (selectedTextControlsVisible &&
         (state.textStyleMixed & SnowCanvasTextStyleMixedColor) != 0) ||
        (selectedSerialControlsVisible &&
         (state.serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedColor) != 0);
    m_fillColorMixed = (selectedTextControlsVisible &&
                        (state.textStyleMixed & SnowCanvasTextStyleMixedFill) != 0) ||
                       (selectedSerialControlsVisible &&
                        (state.serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedFill) != 0);
    m_strokeColorMixed =
        selectedTextControlsVisible && (state.textStyleMixed & SnowCanvasTextStyleMixedStroke) != 0;
    m_fillStyleMixed =
        (selectedTextControlsVisible &&
         (state.textStyleMixed & SnowCanvasTextStyleMixedFillStyle) != 0) ||
        (selectedSerialControlsVisible &&
         (state.serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedFillStyle) != 0);
    m_fontSizeMixed =
        (selectedTextControlsVisible &&
         (state.textStyleMixed & SnowCanvasTextStyleMixedFontSize) != 0) ||
        (selectedSerialControlsVisible &&
         (state.serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedFontSize) != 0);
    m_fontFamilyMixed =
        (selectedTextControlsVisible &&
         (state.textStyleMixed & SnowCanvasTextStyleMixedFontFamily) != 0) ||
        (selectedSerialControlsVisible &&
         (state.serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedFontFamily) != 0);
    m_opacityMixed =
        (selectedTextControlsVisible &&
         (state.textStyleMixed & SnowCanvasTextStyleMixedOpacity) != 0) ||
        (selectedSerialControlsVisible &&
         (state.serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedOpacity) != 0);
    m_textAlignMixed = selectedTextControlsVisible &&
                       (state.textStyleMixed & SnowCanvasTextStyleMixedHorizontalAlign) != 0;
    m_strokeWidthMixed = selectedTextControlsVisible &&
                         (state.textStyleMixed & SnowCanvasTextStyleMixedStrokeWidth) != 0;
    m_cornerRadiiMixed = selectedTextControlsVisible &&
                         (state.textStyleMixed & SnowCanvasTextStyleMixedCornerRadii) != 0;

    auto updateStyleColorButton = [this](QPushButton* button, const QColor& color, bool mixed) {
        if (button == nullptr) {
            return;
        }
        if (mixed) {
            button->setText("Mixed");
            button->setStyleSheet("QPushButton {"
                                  "  background: white;"
                                  "  color: rgba(23, 35, 46, 0.72);"
                                  "  border: 1px dashed rgba(20, 24, 28, 72);"
                                  "  border-radius: 8px;"
                                  "  padding: 6px 10px;"
                                  "}");
            return;
        }
        updateColorButtonAppearance(button, color);
    };
    updateStyleColorButton(m_primaryColorButton, m_primaryColor, m_primaryColorMixed);
    updateStyleColorButton(m_fillColorButton, m_fillColor, m_fillColorMixed);
    updateStyleColorButton(m_strokeColorButton, m_strokeColor, m_strokeColorMixed);

    m_syncingUi = true;
    const QSignalBlocker fontSizeBlocker(m_fontSizeSpinBox);
    const QSignalBlocker fontFamilyBlocker(m_fontFamilyComboBox);
    const QSignalBlocker opacityBlocker(m_opacitySpinBox);
    const QSignalBlocker fillStyleBlocker(m_fillStyleComboBox);
    const QSignalBlocker textAlignBlocker(m_textHorizontalAlignComboBox);
    const QSignalBlocker strokeWidthBlocker(m_strokeWidthSpinBox);
    const QSignalBlocker startArrowheadBlocker(m_startArrowheadComboBox);
    const QSignalBlocker endArrowheadBlocker(m_endArrowheadComboBox);
    const QSignalBlocker arrowStrokeStyleBlocker(m_arrowStrokeStyleComboBox);
    const QSignalBlocker arrowTypeBlocker(m_arrowTypeComboBox);
    const QSignalBlocker topLeftCornerRadiusBlocker(m_topLeftCornerRadiusSpinBox);
    const QSignalBlocker topRightCornerRadiusBlocker(m_topRightCornerRadiusSpinBox);
    const QSignalBlocker bottomRightCornerRadiusBlocker(m_bottomRightCornerRadiusSpinBox);
    const QSignalBlocker bottomLeftCornerRadiusBlocker(m_bottomLeftCornerRadiusSpinBox);
    const QSignalBlocker serialNumberBlocker(m_serialNumberLineEdit);
    setSpinBoxMixed(m_strokeWidthSpinBox, m_strokeWidthMixed, strokeWidth);
    setSpinBoxMixed(m_fontSizeSpinBox, m_fontSizeMixed, fontSize);
    setFontFamilyComboBoxMixed(m_fontFamilyComboBox, m_fontFamilyMixed, fontFamily);
    setSpinBoxMixed(m_opacitySpinBox, m_opacityMixed, opacity);
    setComboBoxMixed(m_fillStyleComboBox, m_fillStyleMixed, static_cast<int>(fillStyle));
    setComboBoxMixed(m_textHorizontalAlignComboBox, m_textAlignMixed, static_cast<int>(textAlign));
    const SnowCanvasShapeStyle& arrowStyle = state.shapeStyle;
    setComboBoxValue(m_startArrowheadComboBox, static_cast<int>(arrowStyle.startArrowhead));
    setComboBoxValue(m_endArrowheadComboBox, static_cast<int>(arrowStyle.endArrowhead));
    setComboBoxValue(m_arrowStrokeStyleComboBox, static_cast<int>(arrowStyle.strokeStyle));
    setComboBoxValue(m_arrowTypeComboBox, static_cast<int>(arrowStyle.arrowType));
    setSpinBoxMixed(m_topLeftCornerRadiusSpinBox, m_cornerRadiiMixed, cornerRadii.topLeft);
    setSpinBoxMixed(m_topRightCornerRadiusSpinBox, m_cornerRadiiMixed, cornerRadii.topRight);
    setSpinBoxMixed(m_bottomRightCornerRadiusSpinBox, m_cornerRadiiMixed, cornerRadii.bottomRight);
    setSpinBoxMixed(m_bottomLeftCornerRadiusSpinBox, m_cornerRadiiMixed, cornerRadii.bottomLeft);

    const bool serialNumberMixed =
        (state.serialNumberStyleMixed & SnowCanvasSerialNumberStyleMixedNumber) != 0;
    m_serialNumberLineEdit->setEnabled(serialControlsVisible);
    m_serialNumberLineEdit->setPlaceholderText(serialNumberMixed ? "Mixed" : QString());
    if (serialNumberMixed) {
        m_serialNumberLineEdit->clear();
    } else {
        m_serialNumberLineEdit->setText(
            QString::number(qMax<qint64>(0, state.serialNumberStyle.number)));
    }
    m_decrementSerialNumberToolButton->setEnabled(
        serialControlsVisible &&
        demo_serial_number_controls::serialNumberControlsCanDecrease(state));
    m_incrementSerialNumberToolButton->setEnabled(serialControlsVisible);

    const bool serialStyleControlsVisible = serialControlsVisible;
    const bool arrowControlsVisible = state.source == SnowCanvasStyleToolbarSource::DefaultArrow ||
                                      state.source == SnowCanvasStyleToolbarSource::SelectedArrow;
    const bool fillControlsVisible = !arrowControlsVisible;
    const bool fillDetailControlsVisible =
        fillControlsVisible && (m_fillColorMixed || m_fillColor.alpha() > 0);
    const bool textStrokeColorVisible =
        textControlsVisible && (m_strokeWidthMixed || strokeWidth > 0.0);
    const bool fontControlsVisible = textControlsVisible || serialStyleControlsVisible;
    const bool cornerControlsVisible = (textControlsVisible || fillControlsVisible) &&
                                       fillDetailControlsVisible && !serialStyleControlsVisible;

    setStyleRowVisible(m_primaryLabel, m_primaryColorButton, true);
    setStyleRowVisible(m_fillLabel, m_fillColorButton, fillControlsVisible);
    setStyleRowVisible(m_fillStyleLabel, m_fillStyleComboBox, fillDetailControlsVisible);
    setStyleRowVisible(m_strokeLabel, m_strokeColorButton, textStrokeColorVisible);
    setStyleRowVisible(m_strokeWidthLabel, m_strokeWidthSpinBox, !serialStyleControlsVisible);
    setStyleRowVisible(m_fontSizeLabel, m_fontSizeSpinBox, fontControlsVisible);
    setStyleRowVisible(m_fontFamilyLabel, m_fontFamilyComboBox, fontControlsVisible);
    setStyleRowVisible(m_opacityLabel, m_opacitySpinBox, fontControlsVisible);
    setStyleRowVisible(m_textAlignLabel, m_textHorizontalAlignComboBox, textControlsVisible);
    setStyleRowVisible(m_startArrowheadLabel, m_startArrowheadComboBox, arrowControlsVisible);
    setStyleRowVisible(m_endArrowheadLabel, m_endArrowheadComboBox, arrowControlsVisible);
    setStyleRowVisible(m_arrowStrokeStyleLabel, m_arrowStrokeStyleComboBox, arrowControlsVisible);
    setStyleRowVisible(m_arrowTypeLabel, m_arrowTypeComboBox, arrowControlsVisible);
    setStyleRowVisible(m_topLeftCornerRadiusLabel, m_topLeftCornerRadiusSpinBox,
                       cornerControlsVisible);
    setStyleRowVisible(m_topRightCornerRadiusLabel, m_topRightCornerRadiusSpinBox,
                       cornerControlsVisible);
    setStyleRowVisible(m_bottomRightCornerRadiusLabel, m_bottomRightCornerRadiusSpinBox,
                       cornerControlsVisible);
    setStyleRowVisible(m_bottomLeftCornerRadiusLabel, m_bottomLeftCornerRadiusSpinBox,
                       cornerControlsVisible);
    m_syncingUi = false;
}

void DemoWindow::syncDebugControls() {
    m_syncingUi = true;
    const QSignalBlocker showDirtyRectsBlocker(m_showDirtyRectsToolButton);
    m_showDirtyRectsToolButton->setChecked(m_canvas->showDirtyRects());
    m_syncingUi = false;
}

void DemoWindow::applyRectangleStyleFromControls() {
    applyCurrentStyleFromControls();
}

void DemoWindow::applyCurrentStyleFromControls() {
    if (m_syncingUi) {
        return;
    }

    const SnowCanvasStyleToolbarState state = m_canvas->canvasStyleToolbarState();
    const SnowCanvasCornerRadii cornerRadii{
        m_topLeftCornerRadiusSpinBox->value(),
        m_topRightCornerRadiusSpinBox->value(),
        m_bottomRightCornerRadiusSpinBox->value(),
        m_bottomLeftCornerRadiusSpinBox->value(),
    };

    switch (state.source) {
    case SnowCanvasStyleToolbarSource::SelectedText:
    case SnowCanvasStyleToolbarSource::DefaultText: {
        SnowCanvasTextStyle style = state.textStyle;
        if (!m_primaryColorMixed) {
            style.color = m_primaryColor;
        }
        if (!m_fillColorMixed) {
            style.fill = m_fillColor;
        }
        if (!m_fillStyleMixed) {
            style.fillStyle = fillStyleFromComboBox(m_fillStyleComboBox);
        }
        if (!m_strokeColorMixed) {
            style.stroke = m_strokeColor;
        }
        if (!m_strokeWidthMixed) {
            style.strokeWidth = m_strokeWidthSpinBox->value();
        }
        if (!m_fontSizeMixed) {
            style.fontSize = m_fontSizeSpinBox->value();
        }
        if (!m_fontFamilyMixed) {
            style.fontFamily = fontFamilyFromComboBox(m_fontFamilyComboBox);
        }
        if (!m_opacityMixed) {
            style.opacity = m_opacitySpinBox->value();
        }
        if (!m_textAlignMixed) {
            style.horizontalAlign = textHorizontalAlignFromComboBox(m_textHorizontalAlignComboBox);
        }
        if (!m_cornerRadiiMixed) {
            style.cornerRadii = cornerRadii;
        }
        m_canvas->setCanvasTextStyle(style);
        break;
    }
    case SnowCanvasStyleToolbarSource::SelectedSerialNumber:
    case SnowCanvasStyleToolbarSource::DefaultSerialNumber: {
        SnowCanvasSerialNumberStyle style = state.serialNumberStyle;
        if (!m_primaryColorMixed) {
            style.color = m_primaryColor;
        }
        if (!m_fillColorMixed) {
            style.fill = m_fillColor;
        }
        if (!m_fillStyleMixed) {
            style.fillStyle = fillStyleFromComboBox(m_fillStyleComboBox);
        }
        if (!m_fontSizeMixed) {
            style.fontSize = m_fontSizeSpinBox->value();
        }
        if (!m_fontFamilyMixed) {
            style.fontFamily = fontFamilyFromComboBox(m_fontFamilyComboBox);
        }
        if (!m_opacityMixed) {
            style.opacity = m_opacitySpinBox->value();
        }
        m_canvas->setCanvasSerialNumberStyle(style);
        break;
    }
    case SnowCanvasStyleToolbarSource::SelectedArrow:
    case SnowCanvasStyleToolbarSource::DefaultArrow: {
        SnowCanvasShapeStyle style = state.shapeStyle;
        style.stroke = m_primaryColor;
        style.strokeWidth = m_strokeWidthSpinBox->value();
        style.startArrowhead = arrowheadFromComboBox(m_startArrowheadComboBox);
        style.endArrowhead = arrowheadFromComboBox(m_endArrowheadComboBox);
        style.strokeStyle = arrowStrokeStyleFromComboBox(m_arrowStrokeStyleComboBox);
        style.arrowType = arrowTypeFromComboBox(m_arrowTypeComboBox);
        m_canvas->setCanvasShapeStylePatch(
            style,
            SnowCanvasShapeStylePropertyStrokeColor | SnowCanvasShapeStylePropertyStrokeWidth |
                SnowCanvasShapeStylePropertyStartArrowhead |
                SnowCanvasShapeStylePropertyEndArrowhead | SnowCanvasShapeStylePropertyStrokeStyle |
                SnowCanvasShapeStylePropertyArrowType,
            SnowCanvasShapeKind::Arrow);
        break;
    }
    case SnowCanvasStyleToolbarSource::SelectedRectangle:
    case SnowCanvasStyleToolbarSource::DefaultRectangle:
    default: {
        SnowCanvasShapeStyle style = state.shapeStyle;
        style.fill = m_fillColor;
        style.stroke = m_primaryColor;
        style.strokeWidth = m_strokeWidthSpinBox->value();
        style.cornerRadii = cornerRadii;
        m_canvas->setCanvasShapeStylePatch(
            style,
            SnowCanvasShapeStylePropertyFillColor | SnowCanvasShapeStylePropertyStrokeColor |
                SnowCanvasShapeStylePropertyStrokeWidth | SnowCanvasShapeStylePropertyCornerRadius,
            SnowCanvasShapeKind::Rectangle);
        break;
    }
    }
    refocusCanvas();
}

void DemoWindow::applySerialNumberFromControls() {
    if (m_syncingUi || m_serialNumberLineEdit == nullptr) {
        return;
    }

    bool ok = false;
    const qint64 number = m_serialNumberLineEdit->text().toLongLong(&ok);
    if (!ok || number < 0) {
        return;
    }

    SnowCanvasSerialNumberStyle style = m_canvas->canvasStyleToolbarState().serialNumberStyle;
    style.number = number;
    m_canvas->setCanvasSerialNumberStyle(style);
}

void DemoWindow::stepSerialNumber(qint64 delta) {
    if (m_syncingUi || delta == 0) {
        return;
    }

    const SnowCanvasStyleToolbarState state = m_canvas->canvasStyleToolbarState();
    const bool serialControlsVisible =
        demo_serial_number_controls::isSerialNumberSource(state.source);
    if (!serialControlsVisible) {
        return;
    }

    if (state.source == SnowCanvasStyleToolbarSource::SelectedSerialNumber) {
        if (delta < 0 && !demo_serial_number_controls::selectedSerialNumberCanDecrease(state)) {
            return;
        }
        m_canvas->adjustSelectedSerialNumbers(delta);
        syncStyleControls();
        return;
    }

    SnowCanvasSerialNumberStyle style = state.serialNumberStyle;
    style.number = demo_serial_number_controls::defaultSerialNumberAfterStep(style.number, delta);
    m_canvas->setCanvasSerialNumberStyle(style);
    syncStyleControls();
}

void DemoWindow::applySnapConfigFromControls() {
    if (m_syncingUi) {
        return;
    }

    SnowCanvasSnapConfig config = m_canvas->canvasSnapConfig();
    config.enabled = m_objectSnapToolButton->isChecked();
    m_canvas->setCanvasSnapConfig(config);
    refocusCanvas();
}

void DemoWindow::applyGridConfigFromControls() {
    if (m_syncingUi) {
        return;
    }

    SnowCanvasGridConfig config = m_canvas->canvasGridConfig();
    config.enabled = m_gridSnapToolButton->isChecked();
    config.size = m_gridSizeSpinBox->value();
    m_canvas->setCanvasGridConfig(config);
    refocusCanvas();
}

void DemoWindow::chooseFillColor() {
    const QColor chosen = QColorDialog::getColor(m_fillColor, this, "Choose Fill Color",
                                                 QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) {
        return;
    }

    m_fillColor = chosen;
    m_fillColorMixed = false;
    updateColorButtonAppearance(m_fillColorButton, m_fillColor);
    applyRectangleStyleFromControls();
}

void DemoWindow::choosePrimaryColor() {
    const QColor chosen = QColorDialog::getColor(m_primaryColor, this, "Choose Primary Color",
                                                 QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) {
        return;
    }

    m_primaryColor = chosen;
    m_primaryColorMixed = false;
    updateColorButtonAppearance(m_primaryColorButton, m_primaryColor);
    applyCurrentStyleFromControls();
}

void DemoWindow::chooseStrokeColor() {
    const QColor chosen = QColorDialog::getColor(m_strokeColor, this, "Choose Stroke Color",
                                                 QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) {
        return;
    }

    m_strokeColor = chosen;
    m_strokeColorMixed = false;
    const SnowCanvasStyleToolbarState state = m_canvas->canvasStyleToolbarState();
    if (state.source != SnowCanvasStyleToolbarSource::SelectedText &&
        state.source != SnowCanvasStyleToolbarSource::DefaultText) {
        m_primaryColor = chosen;
        m_primaryColorMixed = false;
        updateColorButtonAppearance(m_primaryColorButton, m_primaryColor);
    }
    updateColorButtonAppearance(m_strokeColorButton, m_strokeColor);
    applyCurrentStyleFromControls();
}

void DemoWindow::refocusCanvas() {
    if (m_canvas != nullptr) {
        m_canvas->setFocus(Qt::OtherFocusReason);
    }
}

void DemoWindow::clearMixedFlagForSender(QObject* sender) {
    if (m_syncingUi || sender == nullptr) {
        return;
    }
    if (sender == m_fillStyleComboBox) {
        m_fillStyleMixed = false;
    } else if (sender == m_textHorizontalAlignComboBox) {
        m_textAlignMixed = false;
    } else if (sender == m_fontSizeSpinBox) {
        m_fontSizeMixed = false;
    } else if (sender == m_fontFamilyComboBox) {
        m_fontFamilyMixed = false;
    } else if (sender == m_opacitySpinBox) {
        m_opacityMixed = false;
    } else if (sender == m_strokeWidthSpinBox) {
        m_strokeWidthMixed = false;
    } else if (sender == m_topLeftCornerRadiusSpinBox || sender == m_topRightCornerRadiusSpinBox ||
               sender == m_bottomRightCornerRadiusSpinBox ||
               sender == m_bottomLeftCornerRadiusSpinBox) {
        m_cornerRadiiMixed = false;
    }
}

void DemoWindow::updateColorButtonAppearance(QPushButton* button, const QColor& color) {
    if (button == nullptr) {
        return;
    }

    const QColor textColor = color.lightnessF() < 0.55 ? QColor(Qt::white) : QColor(23, 35, 46);
    button->setText(color.name(QColor::HexArgb).toUpper());
    button->setStyleSheet(QString("QPushButton {"
                                  "  background: %1;"
                                  "  color: %2;"
                                  "  border: 1px solid rgba(20, 24, 28, 32);"
                                  "  border-radius: 8px;"
                                  "  padding: 6px 10px;"
                                  "}")
                              .arg(color.name(QColor::HexArgb))
                              .arg(textColor.name()));
}
