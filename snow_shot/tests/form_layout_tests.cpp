#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/form.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_number.h"
#include "widgets/modal.h"
#include "widgets/select.h"
#include "theme/theme_manager.h"

#include "snow_shot/presentation/screenshotselectionresizemodalcontent.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QLayout>
#include <QListView>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QSize>
#include <QToolButton>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
using adqt::widgets::AdForm;
using adqt::widgets::AdFormItem;

QtMessageHandler previousMessageHandler = nullptr;
bool modalGeometryWarningEmitted = false;

void captureModalGeometryWarning(QtMsgType type, const QMessageLogContext& context,
                                 const QString& message) {
    if (type == QtWarningMsg && message.contains(QStringLiteral("QWindowsWindow::setGeometry")) &&
        message.contains(QStringLiteral("ad-modal-overlay"))) {
        modalGeometryWarningEmitted = true;
    }
    if (previousMessageHandler != nullptr) {
        previousMessageHandler(type, context, message);
    }
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
}

bool hasFocusWithin(QWidget* widget) {
    QWidget* focusWidget = QApplication::focusWidget();
    return widget != nullptr && focusWidget != nullptr &&
           (focusWidget == widget || widget->isAncestorOf(focusWidget));
}

QWidget* fixedControl(int width = 154) {
    auto* control = new QWidget();
    control->setFixedSize(width, 32);
    return control;
}

void layoutForm(AdForm& form, int width) {
    form.setFixedWidth(width);
    form.show();
    flushEvents();

    QLayout* layout = form.layout();
    require(layout != nullptr, "form should own a root layout");
    const int height =
        layout->hasHeightForWidth() ? layout->heightForWidth(width) : layout->sizeHint().height();
    form.resize(width, height);
    layout->setGeometry(form.rect());
    flushEvents();
}

QWidget* itemPart(AdFormItem* item, const char* objectName) {
    require(item != nullptr, "form item should exist");
    QWidget* part =
        item->findChild<QWidget*>(QString::fromLatin1(objectName), Qt::FindDirectChildrenOnly);
    require(part != nullptr, "form item should expose the requested layout part");
    return part;
}

void sendMouseEvent(QWidget* target, QEvent::Type type, const QPoint& localPosition,
                    Qt::MouseButton button, Qt::MouseButtons buttons) {
    require(target != nullptr, "mouse event target should exist");
    QMouseEvent event(type, QPointF(localPosition), QPointF(target->mapToGlobal(localPosition)),
                      button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &event);
}

bool selectHasOption(const adqt::widgets::AdSelect* select, const QString& value,
                     const QString& label = QString()) {
    if (select == nullptr) {
        return false;
    }
    const auto options = select->options();
    return std::any_of(options.cbegin(), options.cend(), [&](const auto& option) {
        return option.value.toString() == value && (label.isEmpty() || option.label == label);
    });
}

class PopupResizeRecorder final : public QObject {
  public:
    QVector<QSize> sizes;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched != nullptr && event != nullptr && event->type() == QEvent::Resize) {
            const QSize size = static_cast<QResizeEvent*>(event)->size();
            if (sizes.isEmpty() || sizes.constLast() != size) {
                sizes.push_back(size);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

void inlineFormPreservesAntItemLayoutPrecedence() {
    AdForm form;
    form.setFormLayout(AdForm::FormLayout::Inline);

    AdFormItem* quick = form.addField(QStringLiteral("Quick set"), fixedControl(240));
    quick->setItemLayout(AdFormItem::ItemLayout::Vertical);
    quick->setFixedWidth(452);

    AdFormItem* x = form.addField(QStringLiteral("Position X"), fixedControl());
    x->setItemLayout(AdFormItem::ItemLayout::Vertical);
    x->setFixedWidth(218);

    AdFormItem* y = form.addField(QStringLiteral("Position Y"), fixedControl());
    y->setItemLayout(AdFormItem::ItemLayout::Vertical);
    y->setFixedWidth(218);

    layoutForm(form, 452);

    require(quick->geometry() == QRect(0, 0, 452, 86),
            "vertical quick-set item should use 30px label, 32px control, and 24px margin");
    require(x->geometry() == QRect(0, 86, 218, 86),
            "first vertical column should begin directly below the full-width item");
    require(y->geometry() == QRect(218, 86, 218, 86),
            "vertical item override should not inherit the inline item's 16px end margin");

    QWidget* label = itemPart(quick, "ad-form-item-label-host");
    QWidget* control = itemPart(quick, "ad-form-item-control");
    require(label->geometry() == QRect(0, 0, 452, 30),
            "vertical label row should match Ant Design's 22px line height plus 8px padding");
    require(control->geometry() == QRect(0, 30, 452, 32),
            "vertical control should immediately follow the 30px label row");
}

void inlineItemEndMarginParticipatesInWrapping() {
    const auto populate = [](AdForm& form) {
        form.setFormLayout(AdForm::FormLayout::Inline);
        AdFormItem* first = form.addField(QStringLiteral("A"), fixedControl(80));
        first->setFixedWidth(180);
        AdFormItem* second = form.addField(QStringLiteral("B"), fixedControl(80));
        second->setFixedWidth(180);
        return qMakePair(first, second);
    };

    AdForm narrow;
    const auto narrowItems = populate(narrow);
    layoutForm(narrow, 376);
    require(narrowItems.first->geometry() == QRect(0, 0, 180, 32),
            "first inline item should start at the logical row origin");
    require(narrowItems.second->geometry() == QRect(0, 32, 180, 32),
            "each inline item's 16px end margin should participate in flex wrapping");

    AdForm exact;
    const auto exactItems = populate(exact);
    layoutForm(exact, 392);
    require(exactItems.second->geometry() == QRect(196, 0, 180, 32),
            "inline item should start after the preceding 180px width and 16px end margin");

    AdForm rightToLeft;
    rightToLeft.setLayoutDirection(Qt::RightToLeft);
    const auto rightToLeftItems = populate(rightToLeft);
    layoutForm(rightToLeft, 392);
    require(rightToLeftItems.first->geometry() == QRect(212, 0, 180, 32) &&
                rightToLeftItems.second->geometry() == QRect(16, 0, 180, 32),
            "inline end margins should follow the form's logical layout direction");
}

void verticalColumnsExpandInputNumberControls() {
    AdForm form;
    form.setFormLayout(AdForm::FormLayout::Inline);

    auto* x = new adqt::widgets::AdInputNumber();
    x->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    AdFormItem* xItem = form.addField(QStringLiteral("Position X"), x);
    xItem->setItemLayout(AdFormItem::ItemLayout::Vertical);
    xItem->setFixedWidth(218);

    auto* y = new adqt::widgets::AdInputNumber();
    y->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    AdFormItem* yItem = form.addField(QStringLiteral("Position Y"), y);
    yItem->setItemLayout(AdFormItem::ItemLayout::Vertical);
    yItem->setFixedWidth(218);

    layoutForm(form, 452);

    QWidget* xControl = itemPart(xItem, "ad-form-item-control");
    QWidget* yControl = itemPart(yItem, "ad-form-item-control");
    require(x->width() == xControl->width(),
            "expanding input number should fill the first vertical form column");
    require(y->width() == yControl->width(),
            "expanding input number should fill the second vertical form column");
}

void selectionResizeModalUsesTwoColumnGutter() {
    ScreenshotSelectionParams params;
    params.selection = QRect(0, 0, 100, 100);
    ScreenshotSelectionResizeModalContent content(params, QRect(0, 0, 1920, 1080), false, params,
                                                  {});

    AdForm* form = content.findChild<AdForm*>();
    require(form != nullptr, "selection resize modal should create its normal form");
    layoutForm(*form, 452);

    AdFormItem* x = form->itemForName(QStringLiteral("x"));
    AdFormItem* y = form->itemForName(QStringLiteral("y"));
    require(x != nullptr && y != nullptr,
            "selection resize modal should create both position fields");
    require(y->geometry().x() - x->geometry().right() - 1 == 16,
            "selection resize modal should preserve the 16px two-column gutter");
}

void selectionResizeModalAlignsColorPickerTriggerToControlStart() {
    ScreenshotSelectionParams params;
    params.selection = QRect(0, 0, 100, 100);
    ScreenshotSelectionResizeModalContent content(params, QRect(0, 0, 1920, 1080), false, params,
                                                  {});

    AdForm* form = content.findChild<AdForm*>();
    require(form != nullptr, "selection resize modal should create its normal form");
    layoutForm(*form, 452);

    AdFormItem* colorItem = form->itemForName(QStringLiteral("shadowColor"));
    require(colorItem != nullptr, "selection resize modal should create its color field");
    auto* picker = colorItem->findChild<adqt::widgets::AdColorPicker*>();
    auto* trigger =
        picker == nullptr
            ? nullptr
            : picker->findChild<QWidget*>(QStringLiteral("ad-color-picker-trigger-frame"));
    require(picker != nullptr && trigger != nullptr,
            "shadow color picker should expose its trigger");
    require(trigger->mapTo(picker, QPoint()).x() == 0,
            "shadow color picker trigger should not have leading outer spacing");
}

void selectionResizeModalUsesSingleAspectRatioLockControl() {
    ScreenshotSelectionParams params;
    params.selection = QRect(0, 0, 200, 100);
    ScreenshotSelectionResizeModalContent content(params, QRect(0, 0, 1920, 1080), false, params,
                                                  {});

    AdForm* form = content.findChild<AdForm*>();
    require(form != nullptr, "selection resize modal should create its normal form");
    layoutForm(*form, 452);

    require(form->itemForName(QStringLiteral("lockAspectRatio")) == nullptr &&
                form->itemForName(QStringLiteral("lockDragAspectRatio")) == nullptr,
            "selection resize modal should not expose separate aspect-lock fields");

    auto* lockButton = content.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("selectionAspectRatioLockButton"));
    require(lockButton != nullptr, "selection resize modal should provide an aspect-lock button");
    require(lockButton->isCheckable(), "aspect-lock button should be checkable");
    require(!lockButton->interactionBackgroundVisible(),
            "aspect-lock button should not add an interaction background color");
    require(lockButton->iconSize() == QSize(20, 20),
            "aspect-lock button should use the enlarged icon size");
    require(lockButton->accentRole() == adqt::widgets::AdButton::AccentRole::Neutral,
            "inactive aspect-lock button should use the neutral Ant Design color");

    AdFormItem* widthItem = form->itemForName(QStringLiteral("width"));
    AdFormItem* heightItem = form->itemForName(QStringLiteral("height"));
    require(widthItem != nullptr && heightItem != nullptr,
            "selection resize modal should create both size fields");
    require(widthItem->geometry().width() == 210 && heightItem->geometry().width() == 210,
            "aspect-lock button should sit between the width and height fields");

    auto* width = widthItem->findChild<adqt::widgets::AdInputNumber*>();
    auto* height = heightItem->findChild<adqt::widgets::AdInputNumber*>();
    require(width != nullptr && height != nullptr, "size fields should expose their number inputs");

    lockButton->click();
    require(lockButton->isChecked(), "aspect-lock button should activate when clicked");
    require(lockButton->accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
            "active aspect-lock button should use the Ant Design primary color");
    const QColor primaryActive =
        adqt::theme::ThemeManager::instance().resolveTheme(lockButton).colorPrimaryActive;
    require(lockButton->iconRef().colors().primarySlot() == primaryActive,
            "active aspect-lock icon should use the Ant Design primary active color");

    width->setValue(300);
    require(height->hasValue() && height->value() == 150.0,
            "active aspect lock should preserve the width-height ratio");

    ScreenshotSelectionParams committed;
    require(content.commit(&committed, nullptr, nullptr) ==
                ScreenshotSelectionResizeModalContent::CommitResult::ApplySelection,
            "selection resize modal should commit the active aspect lock");
    require(committed.lockAspectRatio && committed.lockDragAspectRatio,
            "single aspect-lock button should keep both lock behaviors in sync");
}

void selectionResizeModalUsesSettledFirstFrameGeometry() {
    QWidget owner;
    owner.resize(900, 700);
    owner.show();
    owner.activateWindow();
    flushEvents();

    ScreenshotSelectionParams params;
    params.selection = QRect(40, 30, 640, 360);
    auto* content = new ScreenshotSelectionResizeModalContent(params, QRect(0, 0, 1920, 1080),
                                                              false, params, {});

    adqt::widgets::AdModal modal(&owner);
    modal.setOwnerWindow(&owner);
    modal.setMode(adqt::widgets::AdModal::Mode::Window);
    modal.setWindowTitle(QStringLiteral("Resize selection"));
    modal.setCentered(true);
    modal.setPreferredWidth(500);
    modal.setMaskVisible(false);
    modal.setStandardButtons(adqt::widgets::AdModal::StandardButton::Ok |
                             adqt::widgets::AdModal::StandardButton::Cancel);
    modal.setContentWidget(content);
    modal.setInitialFocusWidget(content->initialFocusWidget());
    modalGeometryWarningEmitted = false;
    previousMessageHandler = qInstallMessageHandler(captureModalGeometryWarning);
    modal.open();

    QWidget* overlay = owner.findChild<QWidget*>(QStringLiteral("ad-modal-overlay"));
    require(overlay != nullptr, "selection resize modal should create its window surface");
    QWidget* panel = overlay->findChild<QWidget*>(QStringLiteral("ad-modal-panel"));
    require(panel != nullptr, "selection resize modal should create its content panel");
    flushEvents();
    qInstallMessageHandler(previousMessageHandler);
    previousMessageHandler = nullptr;

    require(!modalGeometryWarningEmitted,
            "selection resize modal should not require native geometry correction");
    require(overlay->size() == panel->sizeHint() && panel->size() == panel->sizeHint(),
            "selection resize modal window should keep the settled content size");
    require(hasFocusWithin(content->initialFocusWidget()),
            "selection resize modal should initially focus the quick-set control");
    modal.close();
    flushEvents();
}

void selectionPresetSelectUsesPrimaryAddActionAndClearsAfterEdits() {
    ScreenshotSelectionParams params;
    params.selection = QRect(40, 30, 640, 360);
    ScreenshotSelectionPreset preset;
    preset.name = QStringLiteral("Landscape");
    preset.params = params;
    preset.params.selection = QRect(80, 60, 800, 450);

    ScreenshotSelectionResizeModalContent content(params, QRect(0, 0, 1920, 1080), false, params,
                                                  {preset});
    auto* select =
        content.findChild<adqt::widgets::AdSelect*>(QStringLiteral("selectionPresetSelect"));
    require(select != nullptr, "resize form should expose its preset select");
    require(!select->currentValue().isValid(), "preset select should be empty by default");
    require(selectHasOption(select, QStringLiteral("preset:0"), preset.name),
            "preset select should place saved presets in its options");
    require(!selectHasOption(select, QStringLiteral("addPreset")),
            "preset select should not retain the old add/manage option");

    auto* addButton =
        content.findChild<adqt::widgets::AdButton*>(QStringLiteral("selectionPresetAddButton"));
    require(addButton != nullptr, "preset popup should expose an Add action");
    require(addButton->text() == QStringLiteral("Add"),
            "preset popup action should use the concise Add label");
    require(addButton->accentRole() == adqt::widgets::AdButton::AccentRole::Primary,
            "preset popup Add action should use the primary color");
    require(adqt::icons::describeIcon(addButton->iconRef()).key.name == QStringLiteral("plus"),
            "preset popup Add action should include the plus icon");

    content.show();
    select->showPopup();
    flushEvents();
    QListView* view = select->view();
    QModelIndex presetIndex;
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        const QModelIndex candidate = view->model()->index(row, 0);
        if (candidate.data(Qt::UserRole).toString() == QStringLiteral("preset:0")) {
            presetIndex = candidate;
            break;
        }
    }
    require(presetIndex.isValid(), "preset option should have a visible popup row");
    const bool invoked = QMetaObject::invokeMethod(view, "clicked", Qt::DirectConnection,
                                                   Q_ARG(QModelIndex, presetIndex));
    require(invoked, "test should activate the preset option through the popup view");
    flushEvents();
    require(select->currentValue().toString() == QStringLiteral("preset:0"),
            "applied preset should remain visible until a form value changes");

    auto* form = content.findChild<AdForm*>(QStringLiteral("selectionResizeForm"));
    auto* width = form == nullptr ? nullptr
                                  : form->itemForName(QStringLiteral("width"))
                                        ->findChild<adqt::widgets::AdInputNumber*>();
    require(width != nullptr && width->value() == 800.0,
            "selecting a preset should apply its values to the form");
    require(width->isEnabled() && !width->readOnly(),
            "selecting a preset should keep form fields editable");
    form->setFieldValue(QStringLiteral("width"), 801);
    require(!select->currentValue().isValid(),
            "changing a form value should clear the preset select");
}

void selectedPresetPopupOpensAtItsSettledSize() {
    ScreenshotSelectionParams params;
    params.selection = QRect(40, 30, 640, 360);
    ScreenshotSelectionPreset preset;
    preset.name = QStringLiteral("Landscape");
    preset.params = params;

    ScreenshotSelectionResizeModalContent content(params, QRect(0, 0, 1920, 1080), false, params,
                                                  {preset});
    content.show();
    flushEvents();

    auto* select =
        content.findChild<adqt::widgets::AdSelect*>(QStringLiteral("selectionPresetSelect"));
    require(select != nullptr, "resize form should expose its preset select");

    select->showPopup();
    flushEvents();
    QWidget* popup = content.window()->findChild<QWidget*>(QStringLiteral("adselect-popup"));
    require(popup != nullptr && popup->isVisible(),
            "preset select should expose its popup surface");
    const QSize settledSize = popup->size();

    select->hidePopup();
    select->setCurrentValue(QStringLiteral("preset:0"));
    flushEvents();

    PopupResizeRecorder recorder;
    popup->installEventFilter(&recorder);
    select->showPopup();
    flushEvents();
    popup->removeEventFilter(&recorder);

    require(popup->size() == settledSize,
            "selected preset popup should retain its settled dimensions");
    require(recorder.sizes.size() <= 1,
            "selected preset popup should not resize through multiple visible layouts");
}

void selectionPresetCreateModalRequiresNameAndUsesCurrentDimensions() {
    ScreenshotSelectionParams params;
    params.selection = QRect(40, 30, 640, 360);
    ScreenshotSelectionResizeModalContent content(params, QRect(0, 0, 1920, 1080), false, params,
                                                  {});
    content.show();
    flushEvents();

    int updateCount = 0;
    QVector<ScreenshotSelectionPreset> updatedPresets;
    QObject::connect(
        &content, &ScreenshotSelectionResizeModalContent::presetsUpdated, &content,
        [&updateCount, &updatedPresets](const QVector<ScreenshotSelectionPreset>& presets) {
            ++updateCount;
            updatedPresets = presets;
        });

    auto* addButton =
        content.findChild<adqt::widgets::AdButton*>(QStringLiteral("selectionPresetAddButton"));
    require(addButton != nullptr, "preset popup should create its Add button");
    addButton->click();
    flushEvents();

    auto* modal =
        content.findChild<adqt::widgets::AdModal*>(QStringLiteral("selectionPresetCreateModal"));
    require(modal != nullptr && modal->isOpen(), "Add should open the preset creation modal");
    require(modal->mode() == adqt::widgets::AdModal::Mode::Window,
            "preset creation modal should use Window mode");
    auto* form = qobject_cast<AdForm*>(modal->contentWidget());
    auto* nameInput = modal->contentWidget()->findChild<adqt::widgets::AdLineEdit*>(
        QStringLiteral("selectionPresetNameInput"));
    AdFormItem* nameItem =
        form == nullptr ? nullptr : form->itemForName(QStringLiteral("presetName"));
    require(nameInput != nullptr && nameInput->text() == QStringLiteral("640 x 360"),
            "preset name should default to the current width and height");
    require(nameItem != nullptr && nameItem->required(),
            "preset name should be a required form field");

    auto* clearButton = nameInput->findChild<QToolButton*>(QStringLiteral("ad-input-clear"));
    require(clearButton != nullptr && clearButton->isVisible(),
            "preset name input should expose a visible clear button");
    clearButton->click();
    flushEvents();
    require(nameInput->text().isEmpty(), "preset name clear button should empty the input");
    modal->acceptButton()->click();
    flushEvents();
    require(modal->isOpen(), "empty preset name should keep the modal open");
    require(nameItem->validateStatus() == AdFormItem::ValidateStatus::Error,
            "empty preset name should show a form validation error");

    nameInput->setText(QStringLiteral("Presentation"));
    modal->acceptButton()->click();
    flushEvents();
    require(updateCount == 1 && updatedPresets.size() == 1,
            "confirming the creation modal should create one preset");
    require(updatedPresets.constFirst().name == QStringLiteral("Presentation"),
            "created preset should use the required form value");
    auto* select =
        content.findChild<adqt::widgets::AdSelect*>(QStringLiteral("selectionPresetSelect"));
    require(selectHasOption(select, QStringLiteral("preset:0"), QStringLiteral("Presentation")),
            "created preset should immediately appear in the select");
    require(!select->currentValue().isValid(),
            "creating a preset should leave the select display empty");
}

void selectionPresetDeleteActionUsesDangerConfirmation() {
    ScreenshotSelectionParams params;
    params.selection = QRect(40, 30, 640, 360);
    ScreenshotSelectionPreset preset;
    preset.name = QStringLiteral("Landscape");
    preset.params = params;
    ScreenshotSelectionResizeModalContent content(params, QRect(0, 0, 1920, 1080), false, params,
                                                  {preset});
    content.show();
    flushEvents();

    int updateCount = 0;
    QVector<ScreenshotSelectionPreset> updatedPresets;
    QObject::connect(
        &content, &ScreenshotSelectionResizeModalContent::presetsUpdated, &content,
        [&updateCount, &updatedPresets](const QVector<ScreenshotSelectionPreset>& presets) {
            ++updateCount;
            updatedPresets = presets;
        });

    auto* select =
        content.findChild<adqt::widgets::AdSelect*>(QStringLiteral("selectionPresetSelect"));
    require(select != nullptr, "resize form should expose its preset select");
    select->showPopup();
    flushEvents();
    QListView* view = select->view();
    QModelIndex presetIndex;
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        const QModelIndex candidate = view->model()->index(row, 0);
        if (candidate.data(Qt::UserRole).toString() == QStringLiteral("preset:0")) {
            presetIndex = candidate;
            break;
        }
    }
    require(presetIndex.isValid(), "preset option should have a visible popup row");
    const QRect rowRect = view->visualRect(presetIndex);
    const QPoint deletePoint(rowRect.right() - 15, rowRect.center().y());
    sendMouseEvent(view->viewport(), QEvent::MouseMove, deletePoint, Qt::NoButton, Qt::NoButton);
    sendMouseEvent(view->viewport(), QEvent::MouseButtonPress, deletePoint, Qt::LeftButton,
                   Qt::LeftButton);
    sendMouseEvent(view->viewport(), QEvent::MouseButtonRelease, deletePoint, Qt::LeftButton,
                   Qt::NoButton);
    flushEvents();

    auto* modal =
        content.findChild<adqt::widgets::AdModal*>(QStringLiteral("selectionPresetDeleteModal"));
    require(modal != nullptr && modal->isOpen(),
            "preset delete action should open a confirmation modal");
    require(modal->mode() == adqt::widgets::AdModal::Mode::Window,
            "preset delete confirmation should use Window mode");
    require(modal->acceptAccentRole() == adqt::widgets::AdButton::AccentRole::Danger,
            "preset delete confirmation should use a danger accept action");
    require(modal->text().contains(preset.name),
            "preset delete confirmation should identify the preset");
    require(!select->currentValue().isValid(),
            "clicking the delete icon should not apply the preset");

    modal->acceptButton()->click();
    flushEvents();
    require(updateCount == 1 && updatedPresets.isEmpty(),
            "confirming deletion should remove the preset");
    require(!selectHasOption(select, QStringLiteral("preset:0")),
            "deleted preset should immediately disappear from the select");
}

void horizontalLabelsUseIntrinsicWidthWithoutExtraGridGap() {
    AdForm form;
    AdFormItem* shortItem = form.addField(QStringLiteral("A"), fixedControl(200));
    AdFormItem* longItem = form.addField(QStringLiteral("Long label"), fixedControl(200));
    layoutForm(form, 600);

    QWidget* shortLabel = itemPart(shortItem, "ad-form-item-label-host");
    QWidget* longLabel = itemPart(longItem, "ad-form-item-label-host");
    QWidget* shortControl = itemPart(shortItem, "ad-form-item-control");
    QWidget* longControl = itemPart(longItem, "ad-form-item-control");

    require(shortLabel->width() < longLabel->width(),
            "horizontal labels without labelCol should keep their intrinsic widths");
    require(shortControl->x() == shortLabel->geometry().right() + 1,
            "short label and control should not receive an extra grid gap");
    require(longControl->x() == longLabel->geometry().right() + 1,
            "long label and control should not receive an extra grid gap");

    form.setLabelColumnWidth(96);
    flushEvents();
    require(shortLabel->width() == 96 && longLabel->width() == 96,
            "an explicit Qt label column width should continue to align horizontal labels");
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCursor::setPos(0, 0);
    flushEvents();
    inlineFormPreservesAntItemLayoutPrecedence();
    inlineItemEndMarginParticipatesInWrapping();
    verticalColumnsExpandInputNumberControls();
    selectionResizeModalUsesTwoColumnGutter();
    selectionResizeModalAlignsColorPickerTriggerToControlStart();
    selectionResizeModalUsesSingleAspectRatioLockControl();
    selectionResizeModalUsesSettledFirstFrameGeometry();
    selectionPresetSelectUsesPrimaryAddActionAndClearsAfterEdits();
    selectedPresetPopupOpensAtItsSettledSize();
    selectionPresetCreateModalRequiresNameAndUsesCurrentDimensions();
    selectionPresetDeleteActionUsesDangerConfirmation();
    horizontalLabelsUseIntrinsicWidthWithoutExtraGridGap();
    return 0;
}
