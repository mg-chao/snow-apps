#include "snow_shot/presentation/screenshotquicktextannotationtoolbar.h"

#include "../tools/screenshottoolpalettebuttons.h"
#include "../tools/screenshottoolpalettestylecomponents.h"
#include "../tools/screenshottoolpalettestylepresets.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"

#include "antd_icons.h"
#include "widgets/color_picker.h"
#include "widgets/detail/overlay_popup_surface.h"
#include "widgets/popover.h"

#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QPointer>
#include <QScreen>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {
using Tool = ScreenshotTextAnnotationTool;
using Style = ScreenshotTextAnnotationStyle;
using Toolbar = ScreenshotQuickTextAnnotationToolbar;
using namespace snow_shot::presentation;
using adqt::widgets::AdButton;
using adqt::widgets::AdPopover;
using adqt::widgets::detail::OverlayPopupSurface;

QPointer<Toolbar> visibleToolbar;
const QString kPreferencesKey = QStringLiteral("pin_to_screen/quick_text_annotation_styles");
constexpr ScreenshotToolPaletteButtonMetrics kToolMetrics{screenshot_action_toolbar::ControlSize,
                                                          screenshot_action_toolbar::IconSize, 1.0};
constexpr ScreenshotToolPaletteButtonMetrics kStyleMetrics{28, 18, 1.0};

int indexOf(Tool tool) {
    return static_cast<int>(tool);
}

QString toolKey(Tool tool) {
    switch (tool) {
    case Tool::Highlight:
        return QStringLiteral("highlight");
    case Tool::WavyUnderline:
        return QStringLiteral("wavy_underline");
    case Tool::Underline:
        return QStringLiteral("underline");
    case Tool::Strikethrough:
        return QStringLiteral("strikethrough");
    }
    return {};
}

Style defaultStyle(Tool tool) {
    const auto defaults = screenshotCanvasStyleDefaults();
    return {tool == Tool::Highlight ? defaults.rectangleHighlight.fill : defaults.rectangle.stroke,
            defaults.rectangle.strokeWidth, defaults.rectangle.strokeStyle,
            defaults.rectangle.strokeRenderingStyle};
}

Style readStyle(Tool tool, const QJsonObject& object) {
    Style style = defaultStyle(tool);
    const QColor color(object.value(QStringLiteral("color")).toString());
    if (color.isValid())
        style.color = color;
    const double width = object.value(QStringLiteral("width")).toDouble(style.width);
    if (std::isfinite(width) && width > 0.0 && width <= 72.0)
        style.width = width;
    const int pattern = object.value(QStringLiteral("stroke_style")).toInt(-1);
    if (pattern >= static_cast<int>(SnowCanvasStrokeStyle::Solid) &&
        pattern <= static_cast<int>(SnowCanvasStrokeStyle::Dotted))
        style.strokeStyle = static_cast<SnowCanvasStrokeStyle>(pattern);
    const int rendering = object.value(QStringLiteral("rendering_style")).toInt(-1);
    if (rendering >= static_cast<int>(SnowCanvasStrokeRenderingStyle::Architect) &&
        rendering <= static_cast<int>(SnowCanvasStrokeRenderingStyle::Cartoonist))
        style.renderingStyle = static_cast<SnowCanvasStrokeRenderingStyle>(rendering);
    return style;
}

void configurePopover(AdPopover* popover) {
    popover->setTriggers({});
    popover->setVisibilityPolicy(AdPopover::VisibilityPolicy::Manual);
    popover->setPopupLayerMode(AdPopover::PopupLayerMode::QtTool);
    popover->setPopupLifetime(AdPopover::PopupLifetime::Retained);
    // Selection is the sole visibility authority. Ignore hover, outside press and Esc requests.
}

bool inWidgetFamily(const QWidget* widget, const QWidget* ancestor) {
    for (const QWidget* current = widget; current; current = current->parentWidget()) {
        if (current == ancestor)
            return true;
    }
    return false;
}

} // namespace

struct ScreenshotQuickTextAnnotationToolbar::Private final {
    Toolbar* q;
    QPointer<QWidget> owner;
    QWidget* anchor;
    AdPopover* main;
    AdPopover* editor;
    QWidget* editorContent = nullptr;
    AdButton* copyButton = nullptr;
    std::array<AdButton*, 4> buttons{};
    std::array<Style, 4> preferences;
    States states;
    std::unique_ptr<ScreenshotToolPaletteStrokeEditor> strokeEditor;
    std::unique_ptr<ScreenshotToolPaletteColorEditor> highlightEditor;
    std::optional<Tool> editorTool;
    std::optional<Tool> lastTool;
    std::optional<Tool> synchronizedTool;
    std::optional<Style> synchronizedStyle;
    quint32 synchronizedMixedProperties = 0;
    quint64 revision = 0;
    bool selected = false;
    bool superseded = false;
    bool suppressEditor = false;
    bool changingEditor = false;
    bool positioning = false;

    Private(Toolbar* toolbar, QWidget* window)
        : q(toolbar), owner(window), anchor(new QWidget(window)), main(new AdPopover(toolbar)),
          editor(new AdPopover(toolbar)) {
        anchor->setObjectName(QStringLiteral("quickTextAnnotationSelectionAnchor"));
        anchor->setAttribute(Qt::WA_TransparentForMouseEvents);
        anchor->setAttribute(Qt::WA_NoSystemBackground);
        anchor->setFocusPolicy(Qt::NoFocus);
        configurePopover(main);
        configurePopover(editor);
        main->setSourceWidget(anchor);
        main->setPlacement(AdPopover::Placement::Top);
        main->setAutoAdjustOverflow(false);
        main->setArrowVisible(true);
        editor->setPlacement(AdPopover::Placement::TopLeft);
        editor->setContentMargins(QMargins(8, 8, 8, 8));
        editor->setArrowVisible(false);
        editor->setAutoAdjustOverflow(false);
        auto* content = new QWidget;
        content->setObjectName(QStringLiteral("quickTextAnnotationToolbar"));
        content->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        auto* layout = new QHBoxLayout(content);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        copyButton = createScreenshotToolPaletteToolButton(
            content, nullptr, adqt::icons::antd::outlined::Copy(), kToolMetrics);
        copyButton->setObjectName(QStringLiteral("quickTextAnnotation_copy"));
        copyButton->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(copyButton);
        QObject::connect(copyButton, &AdButton::clicked, q, &Toolbar::copyClicked);
        for (int i = 0; i < 4; ++i) {
            const Tool tool = static_cast<Tool>(i);
            const auto icon =
                tool == Tool::Highlight       ? icons::custom::outlined::ToolHighlight()
                : tool == Tool::WavyUnderline ? icons::custom::outlined::ToolWavyUnderline()
                : tool == Tool::Strikethrough ? icons::custom::outlined::ToolStrikethrough()
                                              : icons::custom::outlined::ToolUnderline();
            auto* button =
                createScreenshotToolPaletteToolButton(content, nullptr, icon, kToolMetrics);
            buttons[static_cast<size_t>(i)] = button;
            button->setObjectName(QStringLiteral("quickTextAnnotation_%1").arg(toolKey(tool)));
            button->setFocusPolicy(Qt::NoFocus);
            layout->addWidget(button);
            QObject::connect(button, &AdButton::clicked, q, [this, tool]() {
                closeStylePopups();
                const bool removing = states[static_cast<size_t>(indexOf(tool))].active;
                lastTool = tool;
                suppressEditor = removing;
                editorTool = removing ? std::nullopt : std::optional<Tool>(tool);
                synchronizeEditor();
                emit q->toolClicked(tool);
            });
        }
        main->setContentWidget(content);
        retranslate();
        loadPreferences();
    }

    ~Private() {
        strokeEditor.reset();
        highlightEditor.reset();
        delete anchor;
    }

    void loadPreferences() {
        QJsonObject values;
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (storage.isInitialized())
            values = storage.configuration().value(kPreferencesKey).toObject();
        for (int i = 0; i < 4; ++i) {
            const Tool tool = static_cast<Tool>(i);
            preferences[static_cast<size_t>(i)] =
                readStyle(tool, values.value(toolKey(tool)).toObject());
        }
    }

    void persistPreference(Tool tool, const Style& style) {
        preferences[static_cast<size_t>(indexOf(tool))] = style;
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (!storage.isInitialized())
            return;
        // Merge against storage so another pin's preferences cannot be overwritten.
        auto values = storage.configuration().value(kPreferencesKey).toObject();
        values.insert(
            toolKey(tool),
            QJsonObject{
                {QStringLiteral("color"), style.color.name(QColor::HexArgb)},
                {QStringLiteral("width"), style.width},
                {QStringLiteral("stroke_style"), static_cast<int>(style.strokeStyle)},
                {QStringLiteral("rendering_style"), static_cast<int>(style.renderingStyle)}});
        storage.configuration().setValue(kPreferencesKey, values);
    }

    void retranslate() {
        const QString copyText = ScreenshotQuickTextAnnotationToolbar::tr("Copy text");
        copyButton->setToolTip(copyText);
        copyButton->setAccessibleName(copyText);
        const QString names[] = {ScreenshotQuickTextAnnotationToolbar::tr("Highlight"),
                                 ScreenshotQuickTextAnnotationToolbar::tr("Wavy underline"),
                                 ScreenshotQuickTextAnnotationToolbar::tr("Underline"),
                                 ScreenshotQuickTextAnnotationToolbar::tr("Strikethrough")};
        for (size_t i = 0; i < buttons.size(); ++i) {
            buttons[i]->setToolTip(names[i]);
            buttons[i]->setAccessibleName(names[i]);
        }
        if (editorContent) {
            retranslateScreenshotToolPalette(editorContent);
            for (auto* picker : editorContent->findChildren<adqt::widgets::AdColorPicker*>())
                retranslateScreenshotToolPalette(picker->popupContent());
        }
    }

    Style currentEditorStyle(Tool tool) const {
        const auto& state = states[static_cast<size_t>(indexOf(tool))];
        return state.style.value_or(preferences[static_cast<size_t>(indexOf(tool))]);
    }

    void editStyle(quint32 properties, const std::function<void(Style&)>& change) {
        if (!selected || !editorTool || changingEditor)
            return;
        const Tool tool = *editorTool;
        Style style = currentEditorStyle(tool);
        change(style);
        auto& state = states[static_cast<size_t>(indexOf(tool))];
        state.style = style;
        const quint32 mixedProperties =
            state.mixed ? (state.mixedProperties != 0 ? state.mixedProperties
                                                      : ScreenshotTextAnnotationStylePropertyAll)
                        : 0;
        state.mixedProperties = mixedProperties & ~properties;
        state.mixed = state.mixedProperties != 0;
        Style remembered = preferences[static_cast<size_t>(indexOf(tool))];
        if (properties & ScreenshotTextAnnotationStylePropertyColor)
            remembered.color = style.color;
        if (properties & ScreenshotTextAnnotationStylePropertyWidth)
            remembered.width = style.width;
        if (properties & ScreenshotTextAnnotationStylePropertyStrokeStyle)
            remembered.strokeStyle = style.strokeStyle;
        if (properties & ScreenshotTextAnnotationStylePropertyRenderingStyle)
            remembered.renderingStyle = style.renderingStyle;
        persistPreference(tool, remembered);
        emit q->styleEdited(tool, style, properties);
        synchronizeEditor();
    }

    void chooseEditor() {
        editorTool.reset();
        if (suppressEditor)
            return;
        const auto applicable = [this](Tool tool) {
            const auto& state = states[static_cast<size_t>(indexOf(tool))];
            return state.active || state.style.has_value();
        };
        if (lastTool && applicable(*lastTool))
            editorTool = lastTool;
        if (!editorTool) {
            for (Tool tool : {Tool::WavyUnderline, Tool::Underline, Tool::Strikethrough}) {
                if (applicable(tool)) {
                    editorTool = tool;
                    break;
                }
            }
        }
        if (!editorTool && applicable(Tool::Highlight))
            editorTool = Tool::Highlight;
    }

    void closeStylePopups() {
        if (!editorContent)
            return;
        changingEditor = true;
        for (auto* picker : editorContent->findChildren<adqt::widgets::AdColorPicker*>())
            picker->setPopupVisible(false);
        changingEditor = false;
    }

    void synchronizeEditor() {
        if (!selected || !editorTool || suppressEditor) {
            if (editor->isVisible()) {
                closeStylePopups();
                editor->hide();
            }
            position();
            return;
        }
        const Tool tool = *editorTool;
        const bool highlight = tool == Tool::Highlight;
        const Style style = currentEditorStyle(tool);
        const auto& state = states[static_cast<size_t>(indexOf(tool))];
        const quint32 mixedProperties =
            state.mixed ? (state.mixedProperties != 0 ? state.mixedProperties
                                                      : ScreenshotTextAnnotationStylePropertyAll)
                        : 0;
        const bool colorMixed = (mixedProperties & ScreenshotTextAnnotationStylePropertyColor) != 0;
        if (editorContent == nullptr) {
            editorContent = new QWidget;
            editorContent->setObjectName(QStringLiteral("quickTextAnnotationStyleToolbar"));
            editorContent->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            auto* layout = new QHBoxLayout(editorContent);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(4);
            ScreenshotToolPaletteEditorServices services;
            ScreenshotToolPaletteStrokeEditorConfig strokeConfig;
            strokeConfig.accessibleName = QStringLiteral("Stroke color");
            strokeConfig.popupObjectName = QStringLiteral("quickTextAnnotationStrokeOptions");
            strokeConfig.styleRowObjectName = QStringLiteral("quickTextAnnotationStrokeStyles");
            strokeConfig.colorValues = style_presets::strokeColors();
            strokeConfig.presetSource = ScreenshotToolPaletteColorPresetSource::Stroke;
            strokeConfig.colorTooltip = [](const QColor& color) {
                return ScreenshotToolPaletteTranslationText("Stroke color %1").arg(color.name());
            };
            strokeConfig.styleTooltip = [](SnowCanvasStrokeStyle pattern) {
                return ScreenshotToolPaletteTranslationText(
                    pattern == SnowCanvasStrokeStyle::Dashed   ? "Dashed stroke"
                    : pattern == SnowCanvasStrokeStyle::Dotted ? "Dotted stroke"
                                                               : "Solid stroke");
            };
            strokeConfig.renderingStylesEnabled = true;
            strokeConfig.setRenderingStyle = [this](SnowCanvasStrokeRenderingStyle rendering) {
                editStyle(ScreenshotTextAnnotationStylePropertyRenderingStyle,
                          [rendering](Style& value) { value.renderingStyle = rendering; });
            };
            strokeEditor = std::make_unique<ScreenshotToolPaletteStrokeEditor>();
            strokeEditor->build(
                layout, editorContent, q, strokeConfig, style.color, style.strokeStyle,
                [this](const QColor& color) {
                    editStyle(ScreenshotTextAnnotationStylePropertyColor,
                              [&color](Style& value) { value.color = color; });
                },
                [this](SnowCanvasStrokeStyle pattern) {
                    editStyle(ScreenshotTextAnnotationStylePropertyStrokeStyle,
                              [pattern](Style& value) { value.strokeStyle = pattern; });
                },
                services, kStyleMetrics);
            ScreenshotToolPaletteColorEditorConfig colorConfig;
            colorConfig.accessibleName = QStringLiteral("Stroke color");
            colorConfig.presetValues = style_presets::highlightColors();
            colorConfig.presetSource = ScreenshotToolPaletteColorPresetSource::Highlight;
            colorConfig.presetTooltip = strokeConfig.colorTooltip;
            highlightEditor = std::make_unique<ScreenshotToolPaletteColorEditor>();
            highlightEditor->build(
                layout, editorContent, q, colorConfig, style.color,
                [this](const QColor& color) {
                    editStyle(ScreenshotTextAnnotationStylePropertyColor,
                              [&color](Style& value) { value.color = color; });
                },
                {}, services, kStyleMetrics);
            editor->setContentWidget(editorContent);
        }
        const bool layoutChanged =
            !synchronizedTool || highlight != (*synchronizedTool == Tool::Highlight);
        if (layoutChanged) {
            strokeEditor->rootWidget()->setVisible(!highlight);
            highlightEditor->rootWidget()->setVisible(highlight);
        }
        if (synchronizedTool != editorTool || synchronizedStyle != style ||
            synchronizedMixedProperties != mixedProperties) {
            if (highlight)
                highlightEditor->update(style.color, colorMixed);
            else {
                strokeEditor->update(
                    style.color, style.strokeStyle, colorMixed,
                    (mixedProperties & ScreenshotTextAnnotationStylePropertyStrokeStyle) != 0);
                strokeEditor->updateRenderingStyle(
                    style.renderingStyle,
                    (mixedProperties & ScreenshotTextAnnotationStylePropertyRenderingStyle) != 0);
            }
            synchronizedTool = tool;
            synchronizedStyle = style;
            synchronizedMixedProperties = mixedProperties;
        }
        if (layoutChanged) {
            editorContent->layout()->activate();
            editorContent->adjustSize();
        }
        editor->setSourceWidget(main->surfaceWidget());
        auto* mainSurface = qobject_cast<OverlayPopupSurface*>(main->surfaceWidget());
        editor->setAnchorWidget(mainSurface ? mainSurface->bodyWidget() : main->surfaceWidget());
        if (layoutChanged || !editor->surfaceWidget())
            editor->preparePopup();
        if (auto* surface = editor->surfaceWidget())
            surface->installEventFilter(q);
        position();
    }

    void position() {
        if (positioning || !selected || !owner)
            return;
        auto* mainSurface = qobject_cast<OverlayPopupSurface*>(main->surfaceWidget());
        if (!mainSurface)
            return;
        positioning = true;
        const QRect selectionBounds(anchor->mapToGlobal(QPoint()), anchor->size());
        const QSize mainSize = mainSurface->visualSizeHint();
        const bool showEditor = editorTool.has_value() && !suppressEditor;
        auto* editorSurface =
            showEditor ? qobject_cast<OverlayPopupSurface*>(editor->surfaceWidget()) : nullptr;
        const QSize editorSize = editorSurface ? editorSurface->visualSizeHint() : QSize();
        constexpr int gap = screenshot_action_toolbar::RowSpacing;
        const int above = editorSurface ? editorSize.height() + gap : 0;
        int x = selectionBounds.left() + (selectionBounds.width() - mainSize.width()) / 2;
        int top = selectionBounds.top() - main->popupOffset() - mainSize.height() - above;
        QScreen* screen = QGuiApplication::screenAt(selectionBounds.center());
        if (!screen)
            screen = owner->screen();
        if (screen) {
            const QRect available = screen->availableGeometry().adjusted(4, 4, -4, -4);
            const int pairWidth = std::max(mainSize.width(), editorSize.width());
            const int pairHeight = mainSize.height() + above;
            x = std::clamp(x, available.left(),
                           std::max(available.left(), available.right() - pairWidth + 1));
            top = std::clamp(top, available.top(),
                             std::max(available.top(), available.bottom() - pairHeight + 1));
        }
        // The popup controller owns native geometry. A placement-only anchor also survives its
        // deferred relayouts; directly moving a surface would compete with those same relayouts.
        main->setAnchorGlobalRect(QRect(
            QPoint(x + mainSize.width() / 2, top + above + mainSize.height() + main->popupOffset()),
            QSize(1, 1)));
        if (editorSurface)
            editor->setAnchorGlobalRect(
                QRect(QPoint(x, top + editorSize.height() + editor->popupOffset()), QSize(1, 1)));
        if (!main->isVisible())
            main->show();
        if (editorSurface && !editor->isVisible())
            editor->show();
        positioning = false;
    }
};

ScreenshotQuickTextAnnotationToolbar::ScreenshotQuickTextAnnotationToolbar(QWidget* owner)
    : QObject(owner), d(std::make_unique<Private>(this, owner)) {
    owner->installEventFilter(this);
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (storage.isInitialized()) {
        connect(&storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged,
                this, [this](const QString& key, const QJsonValue&) {
                    if (key == kPreferencesKey)
                        d->loadPreferences();
                });
    }
}

ScreenshotQuickTextAnnotationToolbar::~ScreenshotQuickTextAnnotationToolbar() {
    // Releasing the anchor sends ChildRemoved to the owner while Private is being
    // destroyed. Stop observing before member and QObject child teardown begins.
    if (d->owner)
        d->owner->removeEventFilter(this);
    for (auto* popup : {d->main, d->editor}) {
        if (auto* surface = popup->surfaceWidget())
            surface->removeEventFilter(this);
    }
    clearSelection();
}

void ScreenshotQuickTextAnnotationToolbar::updateSelection(const QRect& globalBounds,
                                                           const States& states, quint64 revision) {
    if (globalBounds.isEmpty() || !d->owner || !d->owner->isVisible()) {
        clearSelection();
        return;
    }
    if (d->superseded && revision == d->revision)
        return;
    d->superseded = false;
    if (visibleToolbar && visibleToolbar != this) {
        const auto previous = visibleToolbar;
        visibleToolbar->clearSelection();
        if (previous) {
            previous->d->superseded = true;
            emit previous->superseded();
        }
    }
    visibleToolbar = this;
    const bool changed = !d->selected || revision != d->revision;
    if (changed)
        d->closeStylePopups();
    d->selected = true;
    d->revision = revision;
    for (size_t i = 0; i < d->buttons.size(); ++i) {
        if (d->states[i].active != states[i].active)
            setScreenshotToolPaletteButtonActive(d->buttons[i], states[i].active);
    }
    d->states = states;
    if (changed) {
        d->suppressEditor = false;
        d->chooseEditor();
    }
    d->anchor->setGeometry(
        QRect(d->owner->mapFromGlobal(globalBounds.topLeft()), globalBounds.size()));
    d->anchor->show();
    if (!d->main->surfaceWidget())
        d->main->preparePopup();
    if (auto* surface = d->main->surfaceWidget())
        surface->installEventFilter(this);
    d->synchronizeEditor();
}

void ScreenshotQuickTextAnnotationToolbar::updateSelectionGeometry(const QRect& globalBounds) {
    updateSelection(globalBounds, d->states, d->revision);
}

void ScreenshotQuickTextAnnotationToolbar::clearSelection() {
    d->selected = false;
    d->superseded = false;
    d->suppressEditor = false;
    d->editorTool.reset();
    d->synchronizeEditor();
    d->main->hide();
    d->anchor->hide();
    if (visibleToolbar == this)
        visibleToolbar.clear();
}

bool ScreenshotQuickTextAnnotationToolbar::isVisible() const {
    return d->main->isVisible();
}

bool ScreenshotQuickTextAnnotationToolbar::ownsWindow(const QWidget* widget) const {
    if (!widget)
        return false;
    if (inWidgetFamily(widget, d->main->surfaceWidget()) ||
        inWidgetFamily(widget, d->editor->surfaceWidget()))
        return true;
    if (d->editorContent) {
        // Color-picker QtTool surfaces can be separate native ownership roots.
        for (auto* popup : d->editorContent->findChildren<AdPopover*>()) {
            if (inWidgetFamily(widget, popup->surfaceWidget()))
                return true;
        }
        for (auto* picker : d->editorContent->findChildren<adqt::widgets::AdColorPicker*>()) {
            if (QWidget* content = picker->popupContent();
                content &&
                (inWidgetFamily(widget, content) || inWidgetFamily(widget, content->window())))
                return true;
        }
    }
    return false;
}

ScreenshotQuickTextAnnotationToolbar::Style
ScreenshotQuickTextAnnotationToolbar::styleForTool(Tool tool) const {
    return d->preferences[static_cast<size_t>(indexOf(tool))];
}

bool ScreenshotQuickTextAnnotationToolbar::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == d->main->surfaceWidget() || watched == d->editor->surfaceWidget()) &&
        (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest))
        d->position();
    if (watched == d->owner) {
        if (event->type() == QEvent::Hide)
            clearSelection();
        else if (event->type() == QEvent::LanguageChange) {
            d->retranslate();
            d->position();
        } else if (event->type() == QEvent::PaletteChange)
            d->retranslate();
        else if (event->type() == QEvent::Move || event->type() == QEvent::Resize)
            d->position();
    }
    return QObject::eventFilter(watched, event);
}
