#include "snow_shot/presentation/screenshotquicktextannotationtoolbar.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"

#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/detail/overlay_popup_surface.h"
#include "widgets/popover.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSet>
#include <QWidget>

#include <cstdlib>
#include <iostream>

namespace {
using Toolbar = ScreenshotQuickTextAnnotationToolbar;
using Tool = ScreenshotTextAnnotationTool;
using Style = ScreenshotTextAnnotationStyle;
using adqt::widgets::AdButton;
using adqt::widgets::AdPopover;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

AdPopover* popover(Toolbar& toolbar, const char* contentName) {
    for (auto* candidate : toolbar.findChildren<AdPopover*>()) {
        if (candidate->contentWidget() &&
            candidate->contentWidget()->objectName() == QString::fromLatin1(contentName))
            return candidate;
    }
    return nullptr;
}

QRect bodyBounds(AdPopover* popover) {
    auto* surface =
        qobject_cast<adqt::widgets::detail::OverlayPopupSurface*>(popover->surfaceWidget());
    auto* body = surface ? surface->bodyWidget() : popover->surfaceWidget();
    return QRect(body->mapToGlobal(QPoint()), body->size());
}

AdButton* toolButton(Toolbar& toolbar, const char* key) {
    auto* main = popover(toolbar, "quickTextAnnotationToolbar");
    require(main != nullptr, "quick annotation main popover is materialized");
    auto* button = main->contentWidget()->findChild<AdButton*>(
        QStringLiteral("quickTextAnnotation_%1").arg(QString::fromLatin1(key)));
    require(button != nullptr, "quick annotation tool button exists");
    return button;
}

void savePreview(AdPopover* main, AdPopover* editor, const QString& name) {
    const QString directory = qEnvironmentVariable("SNOW_TEST_QUICK_ANNOTATION_PREVIEW_DIR");
    if (directory.isEmpty())
        return;
    require(QDir().mkpath(directory), "create quick toolbar visual QA directory");
    const QRect bounds =
        main->surfaceWidget()->geometry().united(editor->surfaceWidget()->geometry());
    QImage preview(bounds.size() + QSize(24, 24), QImage::Format_ARGB32_Premultiplied);
    preview.fill(QColor(230, 233, 239));
    QPainter painter(&preview);
    for (AdPopover* popover : {main, editor})
        painter.drawPixmap(popover->surfaceWidget()->pos() - bounds.topLeft() + QPoint(12, 12),
                           popover->surfaceWidget()->grab());
    painter.end();
    require(preview.save(QDir(directory).filePath(name)), "save quick toolbar visual QA image");
}

class PopupGeometryEvents final : public QObject {
  public:
    int moves = 0;
    int resizes = 0;
    int layouts = 0;

    void observe(AdPopover* popup) {
        require(popup && popup->surfaceWidget(), "geometry probe requires a prepared popup");
        auto* surface = popup->surfaceWidget();
        surfaces.insert(surface);
        surface->installEventFilter(this);
    }
    void reset() {
        moves = resizes = layouts = 0;
    }
    bool still() const {
        return moves == 0 && resizes == 0 && layouts == 0;
    }
    void requireStill(const char* message) const {
        if (!still())
            std::cerr << "popup geometry events: move=" << moves << " resize=" << resizes
                      << " layout=" << layouts << '\n';
        require(still(), message);
    }
    void requireNoMovesOrResizes(const char* message) const {
        if (moves != 0 || resizes != 0)
            std::cerr << "popup geometry events: move=" << moves << " resize=" << resizes
                      << " layout=" << layouts << '\n';
        require(moves == 0 && resizes == 0, message);
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (surfaces.contains(watched)) {
            if (event->type() == QEvent::Move)
                ++moves;
            else if (event->type() == QEvent::Resize)
                ++resizes;
            else if (event->type() == QEvent::LayoutRequest)
                ++layouts;
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    QSet<QObject*> surfaces;
};

void processPopupEvents() {
    // A geometry feedback loop must not be able to keep this test inside one event drain.
    for (int pass = 0; pass < 12; ++pass)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

void layoutSettlesAndColorEditsKeepAllPopupsStill() {
    QWidget owner;
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    owner.setGeometry(available.adjusted(20, 20, -20, -20));
    owner.show();
    processPopupEvents();
    Toolbar toolbar(&owner);
    Toolbar::States states;
    states[static_cast<size_t>(Tool::Underline)] = {true, Style{}, false};
    const QRect selection(available.center() + QPoint(-80, 40), QSize(160, 30));
    const quint64 revision = 90;
    QObject::connect(&toolbar, &Toolbar::styleEdited, &owner,
                     [&](Tool tool, const Style& style, quint32 properties) {
                         auto& state = states[static_cast<size_t>(tool)];
                         state.style = style;
                         state.mixedProperties &= ~properties;
                         state.mixed = state.mixedProperties != 0;
                         toolbar.updateSelection(selection, states, revision);
                     });
    toolbar.updateSelection(selection, states, revision);
    auto* main = popover(toolbar, "quickTextAnnotationToolbar");
    auto* editor = popover(toolbar, "quickTextAnnotationStyleToolbar");
    require(main && editor && main->isVisible() && editor->isVisible(),
            "geometry regression fixture shows its main and style popovers");
    PopupGeometryEvents events;
    events.observe(main);
    events.observe(editor);
    processPopupEvents();
    events.reset();
    processPopupEvents();
    events.requireStill("idle quick annotation popovers must settle without geometry feedback");
    const QRect mainBounds = bodyBounds(main);
    const QRect editorBounds = bodyBounds(editor);
    for (int update = 0; update < 8; ++update) {
        toolbar.updateSelection(selection, states, revision);
        processPopupEvents();
    }
    events.requireStill("synchronizing an unchanged selection must not move or relayout popovers");
    require(bodyBounds(main) == mainBounds && bodyBounds(editor) == editorBounds,
            "unchanged selection updates preserve both toolbar rectangles");
    adqt::widgets::AdColorPicker* picker = nullptr;
    for (auto* candidate : editor->contentWidget()->findChildren<adqt::widgets::AdColorPicker*>()) {
        if (candidate->isVisibleTo(editor->contentWidget())) {
            picker = candidate;
            break;
        }
    }
    require(picker != nullptr, "geometry fixture exposes the active stroke picker");
    picker->setPopupVisible(true);
    processPopupEvents();
    AdPopover* colorPopup = nullptr;
    for (auto* candidate : picker->findChildren<AdPopover*>()) {
        if (candidate->isVisible()) {
            colorPopup = candidate;
            break;
        }
    }
    require(colorPopup != nullptr, "stroke picker opens its nested color popup");
    events.observe(colorPopup);
    processPopupEvents();
    const QRect colorBounds = bodyBounds(colorPopup);
    events.reset();
    for (int update = 0; update < 16; ++update) {
        picker->setValue(adqt::widgets::AdColorValue::solid(QColor(15 + update, 80, 195, 97)));
        processPopupEvents();
        require(picker->popupVisible() && colorPopup->isVisible(),
                "live stroke color changes must keep the color editor open");
        require(bodyBounds(main) == mainBounds && bodyBounds(editor) == editorBounds &&
                    bodyBounds(colorPopup) == colorBounds,
                "live color edits must not move the main, style or color popover");
    }
    events.requireNoMovesOrResizes("live color edits must not move or resize any popup surface");
    events.reset();
    processPopupEvents();
    events.requireStill("popup layout must settle after live stroke color editing");
    picker->setPopupVisible(false);
}

void mainPopoverStaysAboveSelectionAndCenteredWhenEditorOpens() {
    QWidget owner;
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    owner.setGeometry(available.adjusted(20, 20, -20, -20));
    owner.show();
    processPopupEvents();
    Toolbar toolbar(&owner);
    Toolbar::States states;
    const QRect selection(available.center() + QPoint(-80, 40), QSize(160, 30));
    const quint64 revision = 91;
    toolbar.updateSelection(selection, states, revision);
    processPopupEvents();
    auto* main = popover(toolbar, "quickTextAnnotationToolbar");
    require(main && main->isVisible(), "a completed text selection shows its quick popover");
    const QRect before = bodyBounds(main);
    require(before.bottom() < selection.top() &&
                std::abs(before.left() + before.right() - selection.left() - selection.right()) <=
                    1,
            "the main quick popover is centered above the selected text bounding box");
    QObject::connect(&toolbar, &Toolbar::toolClicked, &owner, [&](Tool tool) {
        states[static_cast<size_t>(tool)] = {true, toolbar.styleForTool(tool), false};
        toolbar.updateSelection(selection, states, revision);
    });
    toolButton(toolbar, "underline")->click();
    processPopupEvents();
    auto* editor = popover(toolbar, "quickTextAnnotationStyleToolbar");
    require(editor && editor->isVisible() && bodyBounds(main) == before,
            "opening the style editor must preserve the main popover's selection anchor");
    require(bodyBounds(editor).left() == before.left() &&
                bodyBounds(editor).bottom() < before.top(),
            "the style editor aligns above the main popover without shifting it");
}

void selectionControlsVisibilityAndEdits() {
    QWidget owner;
    owner.setGeometry(150, 250, 700, 400);
    owner.show();
    QApplication::processEvents();
    Toolbar toolbar(&owner);
    Toolbar::States states;
    QRect selection(owner.mapToGlobal(QPoint(80, 160)), QSize(120, 30));
    const quint64 revision = 11;
    int toolClicks = 0;
    int edits = 0;
    quint32 lastProperties = 0;
    QObject::connect(&toolbar, &Toolbar::toolClicked, &owner, [&](Tool tool) {
        ++toolClicks;
        auto& state = states[static_cast<size_t>(tool)];
        state.active = !state.active;
        state.mixed = false;
        state.mixedProperties = 0;
        state.style =
            state.active ? std::optional<Style>(toolbar.styleForTool(tool)) : std::nullopt;
        toolbar.updateSelection(selection, states, revision);
    });
    QObject::connect(&toolbar, &Toolbar::styleEdited, &owner,
                     [&](Tool tool, const Style& style, quint32 properties) {
                         ++edits;
                         lastProperties = properties;
                         auto& state = states[static_cast<size_t>(tool)];
                         state.style = style;
                         const quint32 mixed =
                             state.mixed ? (state.mixedProperties != 0
                                                ? state.mixedProperties
                                                : ScreenshotTextAnnotationStylePropertyAll)
                                         : 0;
                         state.mixedProperties = mixed & ~properties;
                         state.mixed = state.mixedProperties != 0;
                         toolbar.updateSelection(selection, states, revision);
                     });
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(80, 160),
                      QPointF(owner.mapToGlobal(QPoint(80, 160))), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&owner, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(80, 160),
                        QPointF(owner.mapToGlobal(QPoint(80, 160))), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&owner, &release);
    toolbar.updateSelection(selection, states, revision);
    require(toolbar.isVisible() && QWidget::mouseGrabber() == nullptr,
            "a completed nonempty selection shows a popover without grabbing the mouse");
    require(toolClicks == 0 && edits == 0, "selection changes alone must never apply annotations");
    auto* main = popover(toolbar, "quickTextAnnotationToolbar");
    require(main && main->isVisible() && !popover(toolbar, "quickTextAnnotationStyleToolbar"),
            "unstyled selections show four tools without a style editor");
    const auto previousBounds = bodyBounds(main);
    selection.translate(60, 0);
    toolbar.updateSelection(selection, states, revision);
    require(bodyBounds(main).left() != previousBounds.left(),
            "popover follows updated selection geometry");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&owner, &escape);
    QMouseEvent outside(QEvent::MouseButtonPress, QPointF(500, 350),
                        QPointF(owner.mapToGlobal(QPoint(500, 350))), Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&owner, &outside);
    require(toolbar.isVisible(), "outside clicks and Escape do not dismiss a live selection");
    toolButton(toolbar, "underline")->click();
    auto* editor = popover(toolbar, "quickTextAnnotationStyleToolbar");
    require(toolClicks == 1 && toolbar.isVisible() && editor && editor->isVisible(),
            "activating a tool retains the main popover and reveals the style editor");
    require(bodyBounds(editor).left() == bodyBounds(main).left() &&
                bodyBounds(editor).bottom() < bodyBounds(main).top(),
            "style editor is outside above the main popover with aligned left edges");
    savePreview(main, editor, QStringLiteral("quick-text-annotation-underline.png"));
    auto* visiblePicker = [&]() -> adqt::widgets::AdColorPicker* {
        for (auto* picker : editor->contentWidget()->findChildren<adqt::widgets::AdColorPicker*>())
            if (!picker->isHidden() && picker->isVisibleTo(editor->contentWidget()))
                return picker;
        return nullptr;
    }();
    require(visiblePicker && visiblePicker->alphaChannelEnabled(),
            "line styles reuse the alpha-enabled stroke color editor");
    visiblePicker->setPopupVisible(true);
    QApplication::processEvents();
    bool nestedOwned = false;
    for (auto* nested : visiblePicker->findChildren<AdPopover*>()) {
        if (nested->surfaceWidget() && nested->isVisible())
            nestedOwned = toolbar.ownsWindow(nested->surfaceWidget());
    }
    require(nestedOwned, "nested stroke editor surfaces belong to the selection focus family");
    visiblePicker->setPopupVisible(false);
    const QColor updated(15, 80, 195, 97);
    visiblePicker->setValue(adqt::widgets::AdColorValue::solid(updated));
    require(edits == 1 && lastProperties == ScreenshotTextAnnotationStylePropertyColor &&
                toolbar.styleForTool(Tool::Underline).color == updated &&
                toolbar.styleForTool(Tool::Highlight).color != updated,
            "color and opacity edits persist independently for each tool");
    const auto clickStrokeOption = [&](const QString& tooltip) {
        QWidget* popup = visiblePicker->popupContent();
        require(popup != nullptr, "stroke options are materialized on demand");
        for (auto* button : popup->findChildren<AdButton*>()) {
            if (button->toolTip() == tooltip) {
                button->click();
                return;
            }
        }
        require(false, "stroke option exists in the reused editor");
    };
    clickStrokeOption(QStringLiteral("Dashed stroke"));
    require(lastProperties == ScreenshotTextAnnotationStylePropertyStrokeStyle &&
                toolbar.styleForTool(Tool::Underline).strokeStyle == SnowCanvasStrokeStyle::Dashed,
            "stroke patterns persist independently and emit a pattern-only edit");
    clickStrokeOption(QStringLiteral("Artist"));
    require(lastProperties == ScreenshotTextAnnotationStylePropertyRenderingStyle &&
                toolbar.styleForTool(Tool::Underline).renderingStyle ==
                    SnowCanvasStrokeRenderingStyle::Artist,
            "hand-drawn rendering persists and emits only its rendering property");
    states[static_cast<size_t>(Tool::Underline)].mixed = true;
    toolbar.updateSelection(selection, states, revision);
    const int beforeMixedCommit = edits;
    visiblePicker->commitValue(adqt::widgets::AdColorValue::solid(updated));
    require(edits == beforeMixedCommit + 1 &&
                lastProperties == ScreenshotTextAnnotationStylePropertyColor,
            "choosing the first displayed mixed color still applies it uniformly");
    const snow_shot::storage::ScreenshotColorPresetSettings presetSettings;
    const auto previousColors = presetSettings.strokeColors();
    auto configuredColors = previousColors;
    configuredColors[0] = QColor(31, 113, 169);
    require(presetSettings.setStrokeColors(configuredColors),
            "Settings accepts shared stroke color presets");
    bool refreshedPreset = false;
    for (auto* button : editor->contentWidget()->findChildren<AdButton*>()) {
        if (button->toolTip() == QStringLiteral("Stroke color #1f71a9"))
            refreshedPreset = true;
    }
    require(refreshedPreset, "open quick style editors update shared Settings color presets live");
    require(presetSettings.setStrokeColors(previousColors), "restore shared stroke color presets");
    auto& mixedLine = states[static_cast<size_t>(Tool::Underline)];
    mixedLine.style->color = previousColors.first();
    mixedLine.mixed = true;
    mixedLine.mixedProperties = ScreenshotTextAnnotationStylePropertyStrokeStyle |
                                ScreenshotTextAnnotationStylePropertyRenderingStyle;
    toolbar.updateSelection(selection, states, revision);
    bool uniformColorSelected = false;
    const QString uniformColorTooltip =
        QStringLiteral("Stroke color %1").arg(previousColors.first().name());
    for (auto* button : editor->contentWidget()->findChildren<AdButton*>()) {
        if (button->isVisibleTo(editor->contentWidget()) &&
            button->toolTip() == uniformColorTooltip &&
            button->buttonStyle() == AdButton::ButtonStyle::Tonal)
            uniformColorSelected = true;
    }
    require(uniformColorSelected,
            "mixed line patterns keep the uniformly selected color preset active");
    toolButton(toolbar, "underline")->click();
    require(toolClicks == 2 && toolbar.isVisible() && !editor->isVisible(),
            "removing an active tool suppresses its sub-toolbar");
    toolbar.updateSelection(selection, states, revision);
    require(!editor->isVisible(), "style synchronization must not reopen a removed tool editor");
    states[static_cast<size_t>(Tool::Highlight)] = {true, toolbar.styleForTool(Tool::Highlight),
                                                    false};
    toolbar.updateSelection(selection, states, revision + 1);
    require(editor->isVisible(), "selecting previously styled text reveals an applicable editor");
    int visiblePickers = 0;
    for (auto* picker : editor->contentWidget()->findChildren<adqt::widgets::AdColorPicker*>())
        if (picker->isVisibleTo(editor->contentWidget()))
            ++visiblePickers;
    require(visiblePickers == 1,
            "highlight exposes one color editor without line-pattern controls");
    savePreview(main, editor, QStringLiteral("quick-text-annotation-highlight.png"));
    toolbar.clearSelection();
    require(!toolbar.isVisible() && !editor->isVisible(), "clearing selection hides both toolbars");
    Toolbar restored(&owner);
    require(restored.styleForTool(Tool::Underline).color == updated &&
                restored.styleForTool(Tool::Underline).strokeStyle ==
                    SnowCanvasStrokeStyle::Dashed &&
                restored.styleForTool(Tool::Underline).renderingStyle ==
                    SnowCanvasStrokeRenderingStyle::Artist,
            "new toolbar instances restore per-tool alpha colors, patterns and hand-drawn styles");
}

void oneGlobalToolbarAndPreferenceValidation() {
    auto& config = snow_shot::storage::ApplicationStorage::instance().configuration();
    require(config.setValue(
                QStringLiteral("pin_to_screen/quick_text_annotation_styles"),
                QJsonObject{{QStringLiteral("underline"),
                             QJsonObject{{QStringLiteral("width"), -5},
                                         {QStringLiteral("color"), QStringLiteral("invalid")},
                                         {QStringLiteral("stroke_style"), 555},
                                         {QStringLiteral("rendering_style"), -3}}}}),
            "invalid preference fields can be loaded for validation");
    QWidget first;
    QWidget second;
    first.setGeometry(50, 150, 500, 350);
    second.setGeometry(600, 150, 500, 350);
    first.show();
    second.show();
    QApplication::processEvents();
    Toolbar firstToolbar(&first);
    Toolbar secondToolbar(&second);
    require(firstToolbar.styleForTool(Tool::Underline) == Style{},
            "invalid persisted fields recover product stroke defaults");
    firstToolbar.updateSelection(QRect(first.mapToGlobal(QPoint(60, 100)), QSize(100, 20)), {}, 1);
    require(firstToolbar.isVisible(), "first pin owns the initial quick popover");
    secondToolbar.updateSelection(QRect(second.mapToGlobal(QPoint(60, 100)), QSize(100, 20)), {},
                                  1);
    require(!firstToolbar.isVisible() && secondToolbar.isVisible(),
            "only one quick annotation popover can be visible across pins");
    firstToolbar.updateSelection(QRect(first.mapToGlobal(QPoint(65, 100)), QSize(100, 20)), {}, 1);
    require(!firstToolbar.isVisible() && secondToolbar.isVisible(),
            "geometry updates to an older selection cannot steal the active quick popover");
    const auto* main = popover(secondToolbar, "quickTextAnnotationToolbar");
    require(main && secondToolbar.ownsWindow(main->surfaceWidget()) &&
                !firstToolbar.ownsWindow(main->surfaceWidget()),
            "popup ownership distinguishes the current pin's surface family");
    second.hide();
    require(!secondToolbar.isVisible(), "hiding a pin retires its quick annotation surface");
}

void toolbarPairClampsAtScreenEdges() {
    QWidget owner;
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    owner.setGeometry(available);
    owner.show();
    QApplication::processEvents();
    Toolbar toolbar(&owner);
    Toolbar::States states;
    states[static_cast<size_t>(Tool::Underline)] = {true, Style{}, false};
    quint64 revision = 1;
    for (QPoint point :
         {available.topLeft() + QPoint(2, 2), available.topRight() + QPoint(-32, 2),
          available.bottomLeft() + QPoint(2, -22), available.bottomRight() + QPoint(-32, -22)}) {
        toolbar.updateSelection(QRect(point, QSize(30, 20)), states, revision++);
        auto* main = popover(toolbar, "quickTextAnnotationToolbar");
        auto* editor = popover(toolbar, "quickTextAnnotationStyleToolbar");
        require(main && editor && main->isVisible() && editor->isVisible(),
                "screen-edge selections retain both annotation toolbars");
        const QRect mainBody = bodyBounds(main);
        const QRect editorBody = bodyBounds(editor);
        require(available.contains(mainBody) && available.contains(editorBody) &&
                    mainBody.left() == editorBody.left() && editorBody.bottom() < mainBody.top(),
                "the entire toolbar pair fits the work area while retaining alignment and order");
    }
}
} // namespace

void runScreenshotQuickTextAnnotationToolbarTests() {
    layoutSettlesAndColorEditsKeepAllPopupsStill();
    mainPopoverStaysAboveSelectionAndCenteredWhenEditorOpens();
    selectionControlsVisibilityAndEdits();
    oneGlobalToolbarAndPreferenceValidation();
    toolbarPairClampsAtScreenEdges();
}
