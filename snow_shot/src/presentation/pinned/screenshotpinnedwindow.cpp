#include "snow_shot/presentation/screenshotpinnedwindow.h"

#include "screenshotpinnednativegeometrycontroller.h"
#include "screenshotpinnedresizegeometry.h"
#include "screenshotpinnedwindownative.h"
#include "screenshotpintoperfinstrumentation.h"
#include "snow_shot/platform/physicalcursor.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotcanvasrenderer.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotimagefileservice.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "snow_shot/presentation/screenshotmessageservice.h"
#include "snow_shot/presentation/screenshotpinnedcopyservice.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshottableeditor.h"
#include "snow_shot/presentation/screenshotpinnededitcontroller.h"
#include "snow_shot/presentation/screenshotfloatingtoolpalettewindow.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/presentation/screenshottoolpalettehost.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include "antd_icons.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/context_menu.h"
#include "widgets/message.h"

#include <QActionGroup>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QCursor>
#include <QEnterEvent>
#include <QFrame>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMimeData>
#include <QMoveEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QRegion>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QTextDocument>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#include <windowsx.h>
#endif

namespace {
namespace native = screenshot_pinned_window_native;
namespace resize_geometry = screenshot_pinned_resize_geometry;
namespace outlined_icons = adqt::icons::antd::outlined;
namespace custom_outlined_icons = snow_shot::presentation::icons::custom::outlined;

#if defined(Q_OS_WIN) || defined(_WIN32)
template <typename T> T* pointerFromLParam(LPARAM value) {
    return reinterpret_cast<T*>(value); // NOLINT(performance-no-int-to-ptr)
}
#endif

constexpr int kControlsInset = 16;
constexpr int kControlButtonSize = 32;
constexpr int kControlIconSize = 16;
constexpr int kControlButtonSpacing = 8;
constexpr int kControlsMinimumNativeDimension = 383;
constexpr int kThumbnailSize = 83;
constexpr int kThumbnailAnimationDurationMs = 150;
constexpr int kResizeHitWidth = 6;
constexpr int kScaleReadoutInset = 8;
constexpr int kScaleReadoutDurationMs = 1000;
constexpr int kMinimumScalePercent = 10;
constexpr int kMaximumScalePercent = 500;
constexpr int kWheelScaleStep = 10;
constexpr int kMinimumOpacityPercent = 25;
constexpr int kMaximumOpacityPercent = 100;
constexpr int kWheelOpacityStep = 5;
const QColor kDefaultPinnedBorderColor(219, 219, 219, 255);
constexpr auto kTranslationSourceProperty = "screenshotPinnedTranslationSource";
constexpr auto kShortcutDisplayProperty = "screenshotPinnedShortcutDisplay";
constexpr auto kRecognitionMessageKey = "screenshot-pinned-recognition-status";
constexpr auto kModelDownloadMessageKey = "screenshot-pinned-model-download-status";
constexpr auto kOcrTooLargeDescription = "Image size is too large.";
[[maybe_unused]] constexpr const char* kPinnedTranslations[] = {
    QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Enable drawing mode"),
    QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Close"),
    QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Save as File"),
    QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "Image size is too large."),
    QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "The pinned image could not be prepared"),
    QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "The pinned image copy could not be started"),
    QT_TRANSLATE_NOOP("ScreenshotPinnedWindow", "The pinned image save could not be started"),
};

QString translatePinnedText(const char* source) {
    return QCoreApplication::translate("ScreenshotPinnedWindow", source);
}

void showPinnedRecognitionMessage(QWidget* owner, const QString& message, bool error) {
    if (owner == nullptr || message.isEmpty()) {
        return;
    }
    adqt::widgets::AdMessage::Request request;
    request.key = QStringLiteral("screenshot-pinned-recognition-status");
    request.content = message;
    if (error) {
        adqt::widgets::AdMessageService::error(std::move(request), owner);
    } else {
        adqt::widgets::AdMessageService::warning(std::move(request), owner);
    }
}

void setActionTranslationSource(QAction* action, const char* source) {
    if (action != nullptr && source != nullptr && source[0] != '\0') {
        action->setProperty(kTranslationSourceProperty, QString::fromUtf8(source));
    }
}

void setWidgetTranslationSource(QWidget* widget, const char* source) {
    if (widget != nullptr && source != nullptr && source[0] != '\0') {
        widget->setProperty(kTranslationSourceProperty, QString::fromUtf8(source));
    }
}

void updatePinnedBorderGeometry(QFrame& border, const QRect& geometry) {
    constexpr int borderWidth = 2;
    border.setGeometry(geometry);

    QRegion borderRegion(border.rect());
    const QRect innerRect =
        border.rect().adjusted(borderWidth, borderWidth, -borderWidth, -borderWidth);
    if (innerRect.isValid() && !innerRect.isEmpty()) {
        borderRegion = borderRegion.subtracted(QRegion(innerRect));
    }
    border.setMask(borderRegion);
}

#if defined(Q_OS_WIN) || defined(_WIN32)
Qt::Edges resizeEdgesForNativeHitTest(LRESULT hitTest) {
    switch (hitTest) {
    case HTLEFT:
        return Qt::LeftEdge;
    case HTRIGHT:
        return Qt::RightEdge;
    case HTTOP:
        return Qt::TopEdge;
    case HTBOTTOM:
        return Qt::BottomEdge;
    case HTTOPLEFT:
        return Qt::TopEdge | Qt::LeftEdge;
    case HTTOPRIGHT:
        return Qt::TopEdge | Qt::RightEdge;
    case HTBOTTOMLEFT:
        return Qt::BottomEdge | Qt::LeftEdge;
    case HTBOTTOMRIGHT:
        return Qt::BottomEdge | Qt::RightEdge;
    default:
        return {};
    }
}

bool dragHandleForSizingEdge(WPARAM sizingEdge, resize_geometry::DragHandle* handle) {
    if (handle == nullptr) {
        return false;
    }
    switch (sizingEdge) {
    case WMSZ_TOPLEFT:
        *handle = resize_geometry::DragHandle::TopLeft;
        return true;
    case WMSZ_TOPRIGHT:
        *handle = resize_geometry::DragHandle::TopRight;
        return true;
    case WMSZ_BOTTOMRIGHT:
        *handle = resize_geometry::DragHandle::BottomRight;
        return true;
    case WMSZ_BOTTOMLEFT:
        *handle = resize_geometry::DragHandle::BottomLeft;
        return true;
    case WMSZ_TOP:
        *handle = resize_geometry::DragHandle::Top;
        return true;
    case WMSZ_RIGHT:
        *handle = resize_geometry::DragHandle::Right;
        return true;
    case WMSZ_BOTTOM:
        *handle = resize_geometry::DragHandle::Bottom;
        return true;
    case WMSZ_LEFT:
        *handle = resize_geometry::DragHandle::Left;
        return true;
    default:
        return false;
    }
}

bool dragHandleForHitTest(LRESULT hitTest, resize_geometry::DragHandle* handle) {
    if (handle == nullptr) {
        return false;
    }
    switch (hitTest) {
    case HTTOPLEFT:
        *handle = resize_geometry::DragHandle::TopLeft;
        return true;
    case HTTOP:
        *handle = resize_geometry::DragHandle::Top;
        return true;
    case HTTOPRIGHT:
        *handle = resize_geometry::DragHandle::TopRight;
        return true;
    case HTRIGHT:
        *handle = resize_geometry::DragHandle::Right;
        return true;
    case HTBOTTOMRIGHT:
        *handle = resize_geometry::DragHandle::BottomRight;
        return true;
    case HTBOTTOM:
        *handle = resize_geometry::DragHandle::Bottom;
        return true;
    case HTBOTTOMLEFT:
        *handle = resize_geometry::DragHandle::BottomLeft;
        return true;
    case HTLEFT:
        *handle = resize_geometry::DragHandle::Left;
        return true;
    default:
        return false;
    }
}

QRect qRectFromNativeRect(const RECT& rect) {
    return QRect(rect.left, rect.top, std::max(1, static_cast<int>(rect.right - rect.left)),
                 std::max(1, static_cast<int>(rect.bottom - rect.top)));
}

void writeNativeRect(const QRect& source, RECT* target) {
    if (target == nullptr) {
        return;
    }
    target->left = source.left();
    target->top = source.top();
    target->right = source.left() + source.width();
    target->bottom = source.top() + source.height();
}
#endif

QSize physicalSizeAtScale(const QSize& baseline, int percent) {
    return resize_geometry::scaledSize(baseline, percent / 100.0);
}

QList<QPointer<ScreenshotPinnedWindow>>& livePinnedWindows() {
    static QList<QPointer<ScreenshotPinnedWindow>> windows;
    return windows;
}

QColor& configuredPinnedBorderColor() {
    static QColor color = kDefaultPinnedBorderColor;
    return color;
}

bool& configuredTrayEnabled() {
    static bool enabled = true;
    return enabled;
}

void setActionDisplayText(QAction* action, const QString& text) {
    if (action == nullptr) {
        return;
    }
    const QString shortcut = action->property(kShortcutDisplayProperty).toString();
    action->setText(shortcut.isEmpty() ? text : text + QLatin1Char('\t') + shortcut);
}

QString shortcutDisplayText(const QStringList& shortcuts) {
    QStringList display;
    display.reserve(shortcuts.size());
    for (const QString& shortcut : shortcuts) {
        QKeySequence sequence =
            QKeySequence::fromString(shortcut.trimmed(), QKeySequence::PortableText);
        if (sequence.isEmpty()) {
            sequence = QKeySequence::fromString(shortcut.trimmed(), QKeySequence::NativeText);
        }
        const QString text =
            sequence.isEmpty() ? shortcut.trimmed() : sequence.toString(QKeySequence::NativeText);
        if (!text.isEmpty() && !display.contains(text)) {
            display.push_back(text);
        }
    }
    return display.join(QStringLiteral(" / "));
}

void setActionShortcutDisplay(QAction* action, const QString& shortcut) {
    if (action == nullptr) {
        return;
    }
    const QString label = action->text().section(QLatin1Char('\t'), 0, 0);
    action->setProperty(kShortcutDisplayProperty, shortcut);
    setActionDisplayText(action, label);
}

bool focusAcceptsTextInput(QWidget* focusWidget) {
    return qobject_cast<QLineEdit*>(focusWidget) != nullptr ||
           qobject_cast<QTextEdit*>(focusWidget) != nullptr ||
           qobject_cast<QPlainTextEdit*>(focusWidget) != nullptr;
}

bool trayMenuShowsMainInterface() {
    if (!snow_shot::storage::ApplicationStorage::instance().isInitialized()) {
        return true;
    }
    return snow_shot::storage::TraySettings().menuOptions().contains(
        QStringLiteral("tray.show-main-window"));
}

QTransform normalizedImageTransform(const QTransform& transform, const QSize& sourceSize) {
    const QRectF bounds = transform.mapRect(QRectF(QPointF(), QSizeF(sourceSize)));
    QTransform normalized;
    normalized.setMatrix(transform.m11(), transform.m12(), transform.m13(), transform.m21(),
                         transform.m22(), transform.m23(), transform.dx() - bounds.left(),
                         transform.dy() - bounds.top(), transform.m33());
    return normalized;
}

class PinnedControlButton final : public adqt::widgets::AdButton {
  public:
    enum class Intent : std::uint8_t { Edit, Close };

    explicit PinnedControlButton(Intent intent, QWidget* parent = nullptr)
        : adqt::widgets::AdButton(parent), m_intent(intent) {}

  protected:
    void paintEvent(QPaintEvent* event) override {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        QColor background = theme.colorBgMask.isValid() ? theme.colorBgMask : QColor(0, 0, 0, 115);
        if (isDown()) {
            background =
                m_intent == Intent::Close ? theme.colorErrorActive : theme.colorPrimaryActive;
        } else if (m_hovered) {
            background = m_intent == Intent::Close ? theme.colorError : theme.colorPrimary;
        }

        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawEllipse(rect());
        }

        adqt::widgets::AdButton::paintEvent(event);
    }

    void enterEvent(QEnterEvent* event) override {
        m_hovered = true;
        adqt::widgets::AdButton::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        m_hovered = false;
        adqt::widgets::AdButton::leaveEvent(event);
    }

  private:
    Intent m_intent;
    bool m_hovered = false;
};

void configurePinnedControlButton(adqt::widgets::AdButton* button) {
    if (button == nullptr) {
        return;
    }
    button->setFocusPolicy(Qt::NoFocus);
    button->setShape(adqt::widgets::AdButton::Shape::Circle);
    button->setSizeClass(adqt::widgets::AdButton::SizeClass::Medium);
    button->setFixedSize(kControlButtonSize, kControlButtonSize);
    button->setIconSize(QSize(kControlIconSize, kControlIconSize));
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    button->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    button->setAccentRole(adqt::widgets::AdButton::AccentRole::Neutral);
    button->setInteractionBackgroundVisible(false);
}

adqt::widgets::AdButton* createControlButton(QWidget* parent, const char* tooltip,
                                             const adqt::icons::IconRef& iconRef,
                                             PinnedControlButton::Intent intent) {
    auto* button = new PinnedControlButton(intent, parent);
    configurePinnedControlButton(button);
    setWidgetTranslationSource(button, tooltip);
    const QString translated = translatePinnedText(tooltip);
    button->setToolTip(translated);
    button->setAccessibleName(translated);
    button->setIconRef(iconRef.withColors(adqt::icons::IconColors::primary(QColor(Qt::white))));
    return button;
}

} // namespace

ScreenshotPinnedWindow::ScreenshotPinnedWindow(RuntimeMode mode, QWidget* parent)
    : QWidget(parent),
      m_runtime(SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()}),
      m_shortcutManager(std::make_unique<snow_shot::presentation::WindowShortcutManager>()),
      m_physicalCursor(std::make_unique<snow_shot::platform::PhysicalCursor>()),
      m_copyService(std::make_unique<ScreenshotPinnedCopyService>()),
      m_nativeGeometryController(std::make_unique<ScreenshotPinnedNativeGeometryController>()) {
    livePinnedWindows().push_back(QPointer<ScreenshotPinnedWindow>(this));
    setWindowFlags(native::windowFlags());
    setAttribute(Qt::WA_DeleteOnClose, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    // Pinned tool windows are often inactive while their controls are hovered.
    setAttribute(Qt::WA_AlwaysShowToolTips, true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    auto& themeManager = adqt::theme::ThemeManager::instance();
    connect(&themeManager, &adqt::theme::ThemeManager::themeChanged, this, [this]() { update(); });

    auto& applicationStorage = snow_shot::storage::ApplicationStorage::instance();
    if (applicationStorage.isInitialized()) {
        const auto applyDrawingPreferences = [this]() {
            const auto tools = snow_shot::presentation::screenshotQuickSelectionDisabledTools(
                snow_shot::storage::DrawingSettings().quickSelectionDisabledTools());
            if (!m_runtime.setQuickSelectionDisabledTools(tools)) {
                qWarning("Failed to apply pinned drawing quick-selection preferences");
            }
        };
        applyDrawingPreferences();
        connect(&applicationStorage.configuration(),
                &snow_shot::storage::ConfigurationStore::valueChanged, this,
                [applyDrawingPreferences](const QString& key, const QJsonValue&) {
                    if (key == QStringLiteral("drawing/quick_selection_disabled_tools")) {
                        applyDrawingPreferences();
                    }
                });
    }

    Q_UNUSED(mode);

    createUi();
    m_shortcutManager->addScopeWindow(this);
    registerWindowShortcuts();
    reloadPinnedWindowShortcuts();
    if (applicationStorage.isInitialized()) {
        connect(&applicationStorage.configuration(),
                &snow_shot::storage::ConfigurationStore::valueChanged, this,
                [this](const QString& key, const QJsonValue&) {
                    if (key.startsWith(QStringLiteral("pin_to_screen_shortcuts/"))) {
                        reloadPinnedWindowShortcuts();
                    }
                });
    }
}

void ScreenshotPinnedWindow::registerWindowShortcuts() {
    using ShortcutManager = snow_shot::presentation::WindowShortcutManager;

    const auto localCommandsAllowed = [this](const ShortcutManager::ActivationContext& context) {
        return !m_closing && !focusAcceptsTextInput(context.focusWidget) &&
               (m_canvas == nullptr || !m_canvas->hasActiveTextEditing());
    };

    ShortcutManager::Binding copyCurrent;
    copyCurrent.id = QStringLiteral("pinned.copy_current");
    copyCurrent.priority = ShortcutManager::StandardPriority::WindowCommand;
    copyCurrent.canActivate = localCommandsAllowed;
    copyCurrent.activate = [this](const auto&) {
        copyCurrentViewport();
        return true;
    };
    m_pinnedShortcutBindings.insert(QStringLiteral("copy_to_clipboard"),
                                    m_shortcutManager->addBinding(this, std::move(copyCurrent)));

    ShortcutManager::Binding copyOriginal;
    copyOriginal.id = QStringLiteral("pinned.copy_original");
    copyOriginal.priority = ShortcutManager::StandardPriority::WindowCommand;
    copyOriginal.canActivate = localCommandsAllowed;
    copyOriginal.activate = [this](const auto&) {
        copyOriginalContent();
        return true;
    };
    m_pinnedShortcutBindings.insert(QStringLiteral("copy_original_content"),
                                    m_shortcutManager->addBinding(this, std::move(copyOriginal)));

    ShortcutManager::Binding save;
    save.id = QStringLiteral("pinned.save_as_file");
    save.priority = ShortcutManager::StandardPriority::WindowCommand;
    save.canActivate = localCommandsAllowed;
    save.activate = [this](const auto&) {
        saveAsFile();
        return true;
    };
    m_pinnedShortcutBindings.insert(QStringLiteral("save_as_file"),
                                    m_shortcutManager->addBinding(this, std::move(save)));

    ShortcutManager::Binding recognition;
    recognition.id = QStringLiteral("pinned.show_recognition");
    recognition.priority = ShortcutManager::StandardPriority::WindowCommand;
    recognition.canActivate = [this, localCommandsAllowed](const auto& context) {
        return localCommandsAllowed(context) && m_ocrAction != nullptr && m_ocrAction->isEnabled();
    };
    recognition.activate = [this](const auto&) {
        m_ocrAction->setChecked(true);
        return true;
    };
    m_pinnedShortcutBindings.insert(QStringLiteral("show_text_recognition_results"),
                                    m_shortcutManager->addBinding(this, std::move(recognition)));

    ShortcutManager::Binding drawing;
    drawing.id = QStringLiteral("pinned.drawing_mode");
    drawing.priority = ShortcutManager::StandardPriority::WindowCommand;
    drawing.canActivate = [this, localCommandsAllowed](const auto& context) {
        return localCommandsAllowed(context) && m_drawingAction != nullptr &&
               m_drawingAction->isEnabled();
    };
    drawing.activate = [this](const auto&) {
        m_drawingAction->trigger();
        return true;
    };
    m_pinnedShortcutBindings.insert(QStringLiteral("drawing_mode"),
                                    m_shortcutManager->addBinding(this, std::move(drawing)));

    ShortcutManager::Binding thumbnail;
    thumbnail.id = QStringLiteral("pinned.thumbnail_mode");
    thumbnail.priority = ShortcutManager::StandardPriority::WindowCommand;
    thumbnail.canActivate = localCommandsAllowed;
    thumbnail.activate = [this](const auto&) {
        setThumbnailMode(!m_thumbnailMode);
        return true;
    };
    m_pinnedShortcutBindings.insert(QStringLiteral("thumbnail_mode"),
                                    m_shortcutManager->addBinding(this, std::move(thumbnail)));

    ShortcutManager::Binding closeWindow;
    closeWindow.id = QStringLiteral("pinned.close");
    closeWindow.priority = ShortcutManager::StandardPriority::WindowCommand + 1;
    closeWindow.canActivate = localCommandsAllowed;
    closeWindow.activate = [this](const auto&) {
        close();
        return true;
    };
    m_pinnedShortcutBindings.insert(QStringLiteral("close_window"),
                                    m_shortcutManager->addBinding(this, std::move(closeWindow)));

    const struct {
        const char* id;
        snow_shot::platform::PhysicalCursorDirection direction;
    } cursorMovements[] = {
        {"move_cursor_up", snow_shot::platform::PhysicalCursorDirection::Up},
        {"move_cursor_down", snow_shot::platform::PhysicalCursorDirection::Down},
        {"move_cursor_left", snow_shot::platform::PhysicalCursorDirection::Left},
        {"move_cursor_right", snow_shot::platform::PhysicalCursorDirection::Right},
    };
    for (const auto& movement : cursorMovements) {
        const QString actionId = QString::fromLatin1(movement.id);
        ShortcutManager::Binding binding;
        binding.id = QStringLiteral("pinned.") + actionId;
        binding.priority = ShortcutManager::StandardPriority::ScreenshotShortcut;
        binding.autoRepeat = true;
        binding.canActivate = [this, localCommandsAllowed](const auto& context) {
            const bool canvasColorSampling =
                m_editController != nullptr && m_editController->canvasColorSamplingActive();
            return m_physicalCursor != nullptr && m_physicalCursor->isSupported() &&
                   cursorMovementEnabled() &&
                   (m_windowDragActive || canvasColorSampling || localCommandsAllowed(context));
        };
        binding.canActivateOutsideScope = [this](const auto&) {
            return m_editController != nullptr && m_editController->canvasColorSamplingActive();
        };
        binding.activate = [this, direction = movement.direction](const auto&) {
            return moveCursorOnePixel(direction);
        };
        m_pinnedShortcutBindings.insert(actionId,
                                        m_shortcutManager->addBinding(this, std::move(binding)));
    }

    ShortcutManager::Binding selectAll;
    selectAll.id = QStringLiteral("pinned.ocr.select_all");
    selectAll.keyCombinations = {
        QKeyCombination(Qt::ControlModifier, Qt::Key_A),
        QKeyCombination(Qt::MetaModifier, Qt::Key_A),
    };
    selectAll.priority = ShortcutManager::StandardPriority::WindowCommand;
    selectAll.canActivate = [this, localCommandsAllowed](const auto& context) {
        return m_ocrMode && m_displayOcrPresentation != nullptr &&
               localCommandsAllowed(context);
    };
    selectAll.activate = [this](const auto&) {
        m_displayOcrPresentation->selectAll();
        m_screenshotRenderer->updateOcrSelection();
        return true;
    };
    static_cast<void>(m_shortcutManager->addBinding(this, std::move(selectAll)));

    ShortcutManager::Binding copy;
    copy.id = QStringLiteral("pinned.ocr.copy");
    copy.keyCombinations = {
        QKeyCombination(Qt::ControlModifier, Qt::Key_C),
        QKeyCombination(Qt::MetaModifier, Qt::Key_C),
    };
    copy.priority = ShortcutManager::StandardPriority::WindowCommand;
    copy.canActivate = [this, localCommandsAllowed](const auto& context) {
        return m_ocrMode && m_displayOcrPresentation != nullptr &&
               localCommandsAllowed(context);
    };
    copy.activate = [this](const auto&) {
        copyEditToolbarContent();
        return true;
    };
    static_cast<void>(m_shortcutManager->addBinding(this, std::move(copy)));
}

void ScreenshotPinnedWindow::reloadPinnedWindowShortcuts() {
    using ShortcutManager = snow_shot::presentation::WindowShortcutManager;
    const snow_shot::storage::PinToScreenShortcutSettings settings;
    const struct {
        const char* id;
        const char* actionObjectName;
    } actions[] = {
        {"copy_to_clipboard", "screenshotPinnedCopyAction"},
        {"copy_original_content", "screenshotPinnedCopyOriginalAction"},
        {"save_as_file", "screenshotPinnedSaveAsFileAction"},
        {"show_text_recognition_results", "screenshotPinnedOcrAction"},
        {"drawing_mode", "screenshotPinnedDrawingAction"},
        {"thumbnail_mode", "screenshotPinnedThumbnailAction"},
        {"close_window", "screenshotPinnedCloseAction"},
        {"move_cursor_up", nullptr},
        {"move_cursor_down", nullptr},
        {"move_cursor_left", nullptr},
        {"move_cursor_right", nullptr},
    };
    for (const auto& action : actions) {
        const QString actionId = QString::fromLatin1(action.id);
        const QStringList shortcuts = settings.shortcuts(actionId);
        const auto binding = m_pinnedShortcutBindings.constFind(actionId);
        if (binding != m_pinnedShortcutBindings.cend()) {
            static_cast<void>(m_shortcutManager->setKeyCombinations(
                binding.value(), ShortcutManager::keyCombinationsFromPortableText(shortcuts)));
        }
        if (action.actionObjectName != nullptr) {
            setActionShortcutDisplay(
                findChild<QAction*>(QString::fromLatin1(action.actionObjectName)),
                shortcutDisplayText(shortcuts));
        }
    }
}

bool ScreenshotPinnedWindow::prewarm(QScreen* screen) {
    if (m_presented || isVisible() || m_closing || m_canvas == nullptr) {
        return false;
    }
    if (screen != nullptr) {
        setScreen(screen);
    }
    ensurePolished();
    if (layout() != nullptr) {
        layout()->activate();
    }
    const WId nativeWindowId = winId();
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (!native::applySystemResizeStyle(nativeWindowId)) {
        return false;
    }
#else
    Q_UNUSED(nativeWindowId);
#endif
    return true;
}

ScreenshotPinnedWindow::~ScreenshotPinnedWindow() {
    invalidatePendingCopy();
    m_materializationJob.cancel();
    m_materializationJob = {};
    m_fileSaveJob.cancel();
    m_fileSaveJob = {};
    m_materializationCallbacks.clear();
    if (m_synchronizedResizeWindowId != 0) {
        native::removeSynchronizedResize(m_synchronizedResizeWindowId);
        m_synchronizedResizeWindowId = 0;
    }
    finishWindowMove();
    clearWindowDragCursor();
    livePinnedWindows().removeAll(QPointer<ScreenshotPinnedWindow>(this));
    stopRecognition();
    if (m_recognitionSession != nullptr) {
        m_recognitionSession->invalidate();
        m_recognitionSession.reset();
    }
    if (m_recognitionContent != nullptr) {
        delete m_recognitionContent;
        m_recognitionContent = nullptr;
    }
    if (m_geometryAnimation != nullptr) {
        m_geometryAnimation->stop();
    }
    m_geometryAnimating = false;
    delete m_editController;
    m_editController = nullptr;
    destroyCanvas();
}

bool ScreenshotPinnedWindow::event(QEvent* event) {
    const bool pointerPresenceChanged =
        event != nullptr && (event->type() == QEvent::Enter || event->type() == QEvent::Leave);
    if (pointerPresenceChanged) {
        m_pointerInside = event->type() == QEvent::Enter;
    } else if (event != nullptr && event->type() == QEvent::Hide) {
        m_pointerInside = false;
    }
    const bool scaleMayHaveChanged =
        event != nullptr && event->type() == QEvent::DevicePixelRatioChange;
    const bool nativeGeometryMayHaveSettled =
        event != nullptr &&
        (event->type() == QEvent::UpdateRequest || event->type() == QEvent::LayoutRequest ||
         event->type() == QEvent::Move || event->type() == QEvent::Resize ||
         event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate ||
         event->type() == QEvent::WindowStateChange || scaleMayHaveChanged);
    const bool handled = QWidget::event(event);
    if (pointerPresenceChanged) {
        updateControlsGeometry();
    }
    if (scaleMayHaveChanged) {
        m_preserveScaleForSettledGeometry = false;
        scheduleNativeScaleAdoption();
    }
    if (nativeGeometryMayHaveSettled) {
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (m_nativeGeometryController != nullptr &&
            m_nativeGeometryController->hasInteractiveTransaction() && !m_windowDragActive &&
            (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
            static_cast<void>(finishNativeGeometryInteraction());
        }
#endif
        static_cast<void>(reconcilePassiveNativeGeometry());
    }
    return handled;
}

bool ScreenshotPinnedWindow::nativeEvent(const QByteArray& eventType, void* message,
                                         qintptr* result) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const bool isWindowsMessage = eventType == QByteArrayLiteral("windows_generic_MSG") ||
                                  eventType == QByteArrayLiteral("windows_dispatcher_MSG");
    if (isWindowsMessage && message != nullptr) {
        auto* nativeMessage = static_cast<MSG*>(message);
        const HWND pinnedHwnd = nativeMessage->hwnd;
        if (pinnedHwnd == nullptr) {
            return QWidget::nativeEvent(eventType, message, result);
        }

        // The pinned image surface is exposed as HTCAPTION so a press can
        // start physical window capture. That changes the normal client hover
        // path into non-client mouse messages, which means Qt may not send the
        // top-level Enter event that drives the control visibility state.
        // Track both paths here and keep the state based on the actual HWND
        // bounds so moving between the image and its client-side controls
        // does not briefly hide the panel.
        const auto updateNativePointerPresence = [&](UINT message) {
            const bool pointerMove = message == WM_MOUSEMOVE || message == WM_NCMOUSEMOVE;
            const bool pointerLeave = message == WM_MOUSELEAVE || message == WM_NCMOUSELEAVE;
            if (!pointerMove && !pointerLeave) {
                return;
            }

            POINT pointer{};
            bool havePointerPosition = false;
            if (message == WM_NCMOUSEMOVE) {
                pointer.x = GET_X_LPARAM(nativeMessage->lParam);
                pointer.y = GET_Y_LPARAM(nativeMessage->lParam);
                havePointerPosition = true;
            } else if (message == WM_MOUSEMOVE) {
                pointer.x = GET_X_LPARAM(nativeMessage->lParam);
                pointer.y = GET_Y_LPARAM(nativeMessage->lParam);
                havePointerPosition = ClientToScreen(pinnedHwnd, &pointer) != FALSE;
            } else {
                havePointerPosition = GetCursorPos(&pointer) != FALSE;
            }

            const QRect nativeGeometry =
                native::currentClientGeometry(reinterpret_cast<WId>(pinnedHwnd));
            const bool inside = havePointerPosition && nativeGeometry.isValid() &&
                                nativeGeometry.contains(QPoint(pointer.x, pointer.y));
            if (inside != m_pointerInside) {
                m_pointerInside = inside;
                updateControlsGeometry();
            }

            if (pointerMove) {
                TRACKMOUSEEVENT tracking{};
                tracking.cbSize = sizeof(tracking);
                tracking.dwFlags = TME_LEAVE;
                if (message == WM_NCMOUSEMOVE) {
                    tracking.dwFlags |= TME_NONCLIENT;
                }
                tracking.hwndTrack = pinnedHwnd;
                TrackMouseEvent(&tracking);
            }
        };
        updateNativePointerPresence(nativeMessage->message);

        if (m_windowDragActive &&
            (nativeMessage->message == WM_MOUSEMOVE || nativeMessage->message == WM_NCMOUSEMOVE)) {
            POINT cursor{};
            if (GetCursorPos(&cursor) != FALSE) {
                static_cast<void>(updateWindowMove(QPoint(cursor.x, cursor.y)));
            }
            if (result != nullptr) {
                *result = 0;
            }
            return true;
        }

        if (m_windowDragActive && (nativeMessage->message == WM_CANCELMODE ||
                                   (nativeMessage->message == WM_CAPTURECHANGED &&
                                    reinterpret_cast<HWND>(nativeMessage->lParam) != pinnedHwnd))) {
            static_cast<void>(finishNativeGeometryInteraction());
            finishWindowMove();
        }

        if (nativeMessage->message == WM_WINDOWPOSCHANGING &&
            m_nativeGeometryController != nullptr) {
            auto* position = pointerFromLParam<WINDOWPOS>(nativeMessage->lParam);
            if (position != nullptr) {
                const bool moveRequested = (position->flags & SWP_NOMOVE) == 0;
                const bool sizeRequested = (position->flags & SWP_NOSIZE) == 0;
                QRect proposal = m_nativeGeometryController->targetGeometry();
                if (!proposal.isValid() || proposal.isEmpty()) {
                    proposal = native::currentClientGeometry(reinterpret_cast<WId>(pinnedHwnd));
                }
                if (proposal.isValid() && !proposal.isEmpty()) {
                    if (moveRequested) {
                        proposal.moveTopLeft(QPoint(position->x, position->y));
                    }
                    if (sizeRequested) {
                        proposal.setSize(
                            QSize(std::max(1, position->cx), std::max(1, position->cy)));
                    }
                    const QRect constrained = m_nativeGeometryController->constrainWindowPos(
                        proposal, moveRequested, sizeRequested);
                    if (moveRequested) {
                        position->x = constrained.x();
                        position->y = constrained.y();
                    }
                    if (sizeRequested) {
                        position->cx = constrained.width();
                        position->cy = constrained.height();
                    }
                }
                // A translucent QWidget is published with UpdateLayeredWindow.
                // Letting USER preserve/copy old client bits while Qt replaces
                // that alpha surface makes live shrinking alternate between
                // the stale surface and a cleared backing-store frame.
                if (!m_presented || (sizeRequested && m_systemSizingActive)) {
                    position->flags |= SWP_NOCOPYBITS;
                }
            }
        }

        if (nativeMessage->message == WM_DPICHANGED && m_nativeGeometryController != nullptr) {
            auto* suggestedRect = pointerFromLParam<RECT>(nativeMessage->lParam);
            POINT cursor{};
            static_cast<void>(GetCursorPos(&cursor));
            if (suggestedRect != nullptr && !m_presented) {
                writeNativeRect(m_nativeGeometryController->targetGeometry(), suggestedRect);
            } else if (suggestedRect != nullptr &&
                       m_nativeGeometryController->adoptDpiTarget(
                           qRectFromNativeRect(*suggestedRect), QPoint(cursor.x, cursor.y))) {
                writeNativeRect(m_nativeGeometryController->targetGeometry(), suggestedRect);
            }
        }

        if (nativeMessage->message == WM_GETMINMAXINFO && nativeTrackSizeConstraintsEnabled()) {
            const QSize baseline = orientedInitialPhysicalSize();
            auto* limits = pointerFromLParam<MINMAXINFO>(nativeMessage->lParam);
            if (limits != nullptr && baseline.isValid() && !baseline.isEmpty()) {
                const QSize minimumSize = physicalSizeAtScale(baseline, kMinimumScalePercent);
                const QSize maximumSize =
                    physicalSizeAtScale(baseline, kMaximumScalePercent).expandedTo(minimumSize);
                limits->ptMinTrackSize.x = minimumSize.width();
                limits->ptMinTrackSize.y = minimumSize.height();
                limits->ptMaxTrackSize.x = maximumSize.width();
                limits->ptMaxTrackSize.y = maximumSize.height();
                if (result != nullptr) {
                    *result = 0;
                }
                return true;
            }
        }

        if (nativeMessage->message == WM_SETCURSOR && m_windowDragCursorSet &&
            LOWORD(nativeMessage->lParam) == HTCAPTION) {
            const Qt::CursorShape dragCursorShape =
                m_windowDragActive ? Qt::ClosedHandCursor : Qt::OpenHandCursor;
            // HTCAPTION bypasses Qt's client cursor path. Reapply the native
            // handle because Windows may still be holding a resize cursor.
            if (!native::applyCursor(dragCursorShape)) {
                if (QWindow* handle = windowHandle()) {
                    handle->unsetCursor();
                    handle->setCursor(QCursor(dragCursorShape));
                }
            }
            if (result != nullptr) {
                *result = TRUE;
            }
            return true;
        }

        if (nativeMessage->message == WM_NCHITTEST) {
            int hitTest = HTCLIENT;
            const bool editMode = m_editController != nullptr && m_editController->editMode();
            const QRect nativeGeometry =
                native::currentClientGeometry(reinterpret_cast<WId>(pinnedHwnd));
            const QPoint screenPosition(GET_X_LPARAM(nativeMessage->lParam),
                                        GET_Y_LPARAM(nativeMessage->lParam));
            if (interactiveResizingEnabled() && nativeGeometry.isValid() &&
                !nativeGeometry.isEmpty()) {
                const int nativeHitWidth = std::max(
                    1, qRound(kResizeHitWidth * static_cast<double>(nativeGeometry.width()) /
                              std::max(1, width())));
                const int nativeHitHeight = std::max(
                    1, qRound(kResizeHitWidth * static_cast<double>(nativeGeometry.height()) /
                              std::max(1, height())));
                const bool inside =
                    screenPosition.x() >= nativeGeometry.left() &&
                    screenPosition.x() < nativeGeometry.left() + nativeGeometry.width() &&
                    screenPosition.y() >= nativeGeometry.top() &&
                    screenPosition.y() < nativeGeometry.top() + nativeGeometry.height();
                if (inside) {
                    const bool left = screenPosition.x() < nativeGeometry.left() + nativeHitWidth;
                    const bool right = screenPosition.x() >= nativeGeometry.left() +
                                                                 nativeGeometry.width() -
                                                                 nativeHitWidth;
                    const bool top = screenPosition.y() < nativeGeometry.top() + nativeHitHeight;
                    const bool bottom = screenPosition.y() >= nativeGeometry.top() +
                                                                  nativeGeometry.height() -
                                                                  nativeHitHeight;

                    if (left && top) {
                        hitTest = HTTOPLEFT;
                    } else if (right && top) {
                        hitTest = HTTOPRIGHT;
                    } else if (right && bottom) {
                        hitTest = HTBOTTOMRIGHT;
                    } else if (left && bottom) {
                        hitTest = HTBOTTOMLEFT;
                    } else if (top) {
                        hitTest = HTTOP;
                    } else if (right) {
                        hitTest = HTRIGHT;
                    } else if (bottom) {
                        hitTest = HTBOTTOM;
                    } else if (left) {
                        hitTest = HTLEFT;
                    }
                }

                if (hitTest == HTCLIENT) {
                    const QPoint clientPosition(
                        qRound((screenPosition.x() - nativeGeometry.left()) *
                               static_cast<double>(std::max(1, width())) / nativeGeometry.width()),
                        qRound((screenPosition.y() - nativeGeometry.top()) *
                               static_cast<double>(std::max(1, height())) /
                               nativeGeometry.height()));
                    if (!isControlsPanelPosition(clientPosition) && !editMode && !m_ocrMode &&
                        !m_geometryAnimating) {
                        hitTest = HTCAPTION;
                    }
                }
            } else if (!editMode && !m_ocrMode && !m_geometryAnimating) {
                if (nativeGeometry.isValid() && !nativeGeometry.isEmpty()) {
                    const QPoint clientPosition(
                        qRound((screenPosition.x() - nativeGeometry.left()) *
                               static_cast<double>(std::max(1, width())) / nativeGeometry.width()),
                        qRound((screenPosition.y() - nativeGeometry.top()) *
                               static_cast<double>(std::max(1, height())) /
                               nativeGeometry.height()));
                    if (!isControlsPanelPosition(clientPosition)) {
                        hitTest = HTCAPTION;
                    }
                }
            }
            if (hitTest == HTCAPTION) {
                setWindowDragCursor(m_windowDragActive ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
            } else if (!m_windowDragActive) {
                clearWindowDragCursor();
            }
            if (result != nullptr) {
                *result = hitTest;
            }
            return true;
        }

        if (nativeMessage->message == WM_NCRBUTTONDOWN && nativeMessage->wParam == HTCAPTION) {
            // The image surface is a synthetic native caption. Suppress the
            // default half of the non-client context interaction.
            if (result != nullptr) {
                *result = 0;
            }
            return true;
        }

        if (nativeMessage->message == WM_NCRBUTTONUP) {
            // Ordinary pinned content is exposed as HTCAPTION so Windows can
            // provide native dragging. That turns right-clicks into
            // non-client messages, bypassing Qt's QContextMenuEvent path. The
            // message point is in native pixels, while QMenu expects Qt global
            // coordinates.
            const QPoint nativePosition(GET_X_LPARAM(nativeMessage->lParam),
                                        GET_Y_LPARAM(nativeMessage->lParam));
            showContextMenu(globalPositionForNativePosition(nativePosition));
            if (result != nullptr) {
                *result = 0;
            }
            return true;
        }

        if (nativeMessage->message == WM_NCLBUTTONUP || nativeMessage->message == WM_LBUTTONUP) {
            static_cast<void>(finishNativeGeometryInteraction());
            if (nativeMessage->wParam == HTCAPTION || m_windowDragActive) {
                finishWindowMove();
            }
        }

        if (nativeMessage->message == WM_NCLBUTTONDOWN) {
            QWindow* handle = windowHandle();
            const Qt::Edges edges =
                resizeEdgesForNativeHitTest(static_cast<LRESULT>(nativeMessage->wParam));
            bool started = false;
            if (handle != nullptr && edges != Qt::Edges() && interactiveResizingEnabled()) {
                resize_geometry::DragHandle dragHandle = resize_geometry::DragHandle::BottomRight;
                if (dragHandleForHitTest(static_cast<LRESULT>(nativeMessage->wParam),
                                         &dragHandle) &&
                    m_nativeGeometryController != nullptr &&
                    m_nativeGeometryController->beginResize(dragHandle)) {
                    started = handle->startSystemResize(edges);
                }
            } else if (handle != nullptr && nativeMessage->wParam == HTCAPTION &&
                       !m_geometryAnimating &&
                       (m_editController == nullptr || !m_editController->editMode()) &&
                       !m_ocrMode) {
                started = startWindowMove();
            }
            if (!started) {
                if (m_nativeGeometryController != nullptr) {
                    m_nativeGeometryController->cancelPendingInteraction();
                }
            }
            if (started) {
                if (result != nullptr) {
                    *result = 0;
                }
                return true;
            }
        }

        if (nativeMessage->message == WM_ENTERSIZEMOVE) {
            if (m_nativeGeometryController != nullptr) {
                const auto phase = m_nativeGeometryController->phase();
                if (phase == ScreenshotPinnedNativeGeometryController::Phase::ResizePending ||
                    phase == ScreenshotPinnedNativeGeometryController::Phase::Resizing) {
                    m_preserveScaleForSettledGeometry = false;
                    m_systemSizingActive = true;
                }
            }
        }

        if (nativeMessage->message == WM_MOVING && m_nativeGeometryController != nullptr) {
            auto* proposedNativeRect = pointerFromLParam<RECT>(nativeMessage->lParam);
            POINT cursor{};
            if (proposedNativeRect != nullptr && GetCursorPos(&cursor) != FALSE) {
                const QRect target = m_nativeGeometryController->updateMove(
                    qRectFromNativeRect(*proposedNativeRect), QPoint(cursor.x, cursor.y));
                if (target.isValid() && !target.isEmpty()) {
                    writeNativeRect(target, proposedNativeRect);
                    if (result != nullptr) {
                        *result = TRUE;
                    }
                    return true;
                }
            }
        }

        if (nativeMessage->message == WM_SIZING && interactiveResizingEnabled()) {
            m_preserveScaleForSettledGeometry = false;
            resize_geometry::DragHandle handle = resize_geometry::DragHandle::BottomRight;
            auto* proposedNativeRect = pointerFromLParam<RECT>(nativeMessage->lParam);
            const QSize baseline = orientedInitialPhysicalSize();
            if (proposedNativeRect != nullptr &&
                dragHandleForSizingEdge(nativeMessage->wParam, &handle) &&
                m_nativeGeometryController != nullptr) {
                const std::optional<QRect> modified = m_nativeGeometryController->updateResize(
                    qRectFromNativeRect(*proposedNativeRect), handle, baseline,
                    kMinimumScalePercent / 100.0, kMaximumScalePercent / 100.0);
                if (!modified.has_value()) {
                    return QWidget::nativeEvent(eventType, message, result);
                }
                writeNativeRect(*modified, proposedNativeRect);
                m_systemSizingActive = true;
                setEffectiveScale(100.0 * modified->width() / std::max(1, baseline.width()), true);
                if (result != nullptr) {
                    *result = TRUE;
                }
                return true;
            }
        }

        if (nativeMessage->message == WM_EXITSIZEMOVE) {
            static_cast<void>(finishNativeGeometryInteraction());
            m_systemSizingActive = false;
            if (m_windowDragActive) {
                finishWindowMove();
            }
        }

        if (nativeMessage->message == WM_WINDOWPOSCHANGED &&
            m_nativeGeometryController != nullptr && m_presented) {
            const QRect actual = native::currentClientGeometry(winId());
            const QRect target = m_nativeGeometryController->targetGeometry();
            if (m_nativeGeometryController->phase() ==
                    ScreenshotPinnedNativeGeometryController::Phase::DpiChanging &&
                actual != target && !native::applyClientGeometry(winId(), target)) {
                static_cast<void>(restoreCommittedNativeGeometry());
            }
            if (m_nativeGeometryController->phase() ==
                    ScreenshotPinnedNativeGeometryController::Phase::DpiChanging &&
                native::currentClientGeometry(winId()) == target) {
                const auto change = m_nativeGeometryController->commitTarget();
                if (change.sizeChanged || change.dpiChanged) {
                    m_preserveScaleForSettledGeometry = false;
                    scheduleNativeScaleAdoption();
                }
            } else if (m_nativeGeometryController->phase() ==
                           ScreenshotPinnedNativeGeometryController::Phase::Stable &&
                       native::currentClientGeometry(winId()) !=
                           m_nativeGeometryController->committedGeometry()) {
                static_cast<void>(restoreCommittedNativeGeometry());
            }
        }
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

void ScreenshotPinnedWindow::changeEvent(QEvent* event) {
    if (event != nullptr && event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(event);
}

void ScreenshotPinnedWindow::retranslateUi() {
    const auto updateWidget = [](QWidget* widget) {
        if (widget == nullptr) {
            return;
        }
        const QString source = widget->property(kTranslationSourceProperty).toString();
        if (source.isEmpty()) {
            return;
        }
        const QByteArray sourceUtf8 = source.toUtf8();
        const QString translated = translatePinnedText(sourceUtf8.constData());
        widget->setToolTip(translated);
        widget->setAccessibleName(translated);
    };

    updateWidget(m_editButton);
    updateWidget(m_closeButton);

    if (m_contextMenu != nullptr) {
        for (QAction* action : m_contextMenu->findChildren<QAction*>()) {
            if (action == nullptr) {
                continue;
            }
            const QString source = action->property(kTranslationSourceProperty).toString();
            if (!source.isEmpty()) {
                const QByteArray sourceUtf8 = source.toUtf8();
                setActionDisplayText(action, translatePinnedText(sourceUtf8.constData()));
            }
        }
        if (m_opacityActions != nullptr) {
            for (QAction* action : m_opacityActions->actions()) {
                if (action != nullptr) {
                    action->setText(translatePinnedText("%1%").arg(action->data().toInt()));
                }
            }
        }
        if (m_scaleActions != nullptr) {
            for (QAction* action : m_scaleActions->actions()) {
                if (action != nullptr) {
                    action->setText(translatePinnedText("%1%").arg(action->data().toInt()));
                }
            }
        }
        refreshContextMenu();
    }
}

bool ScreenshotPinnedWindow::present(const Config& config, std::function<void(bool, QImage)> completion) {
    SNOW_SHOT_PIN_PERF_SCOPE("window.present");
    SNOW_SHOT_PIN_PERF_MILESTONE("window.present_enter");
    const QRectF contentCanvasRect =
        config.contentCanvasRect.isValid() && !config.contentCanvasRect.isEmpty()
            ? config.contentCanvasRect.normalized()
            : config.canvasSourceRect.normalized();
    const QRectF surfaceCanvasRect =
        config.surfaceCanvasRect.isValid() && !config.surfaceCanvasRect.isEmpty()
            ? config.surfaceCanvasRect.normalized()
            : contentCanvasRect;
    ScreenshotImageSource imageSource = config.imageSource;
    if (!imageSource.isValid() && !config.backgroundImage.isNull()) {
        imageSource = ScreenshotImageSource::fromImage(config.backgroundImage, contentCanvasRect);
    }
    if (m_presented || isVisible() || config.screen == nullptr ||
        !config.nativeGeometry.isValid() || config.nativeGeometry.isEmpty() ||
        !contentCanvasRect.isValid() || contentCanvasRect.isEmpty() ||
        !surfaceCanvasRect.isValid() || surfaceCanvasRect.isEmpty() ||
        !surfaceCanvasRect.contains(contentCanvasRect) ||
        (!imageSource.isValid() && !config.imageLoader) ||
        (config.formattedTextDocument != nullptr &&
         (!std::isfinite(config.formattedTextDevicePixelRatio) ||
          config.formattedTextDevicePixelRatio <= 0.0)) ||
        m_canvas == nullptr) {
        return false;
    }
    invalidatePendingCopy();
    applyRuntimeBorderColor();
    updateShowMainInterfaceAction();

#if defined(Q_OS_WIN) || defined(_WIN32)
    const qreal screenScale = std::max<qreal>(1.0, config.screen->devicePixelRatio());
    const QSize logicalSize(std::max(1, qRound(config.nativeGeometry.width() / screenScale)),
                            std::max(1, qRound(config.nativeGeometry.height() / screenScale)));
    if (!logicalSize.isValid() || logicalSize.isEmpty()) {
        return false;
    }
#else
    const QRect logicalGeometry =
        ScreenshotGeometryMapper::logicalRectForPhysicalRect(config.nativeGeometry, config.screen);
    if (!logicalGeometry.isValid() || logicalGeometry.isEmpty()) {
        return false;
    }
#endif

    // Qt may recreate an existing native window when its screen changes. Set
    // the screen before winId() so every native operation uses the final HWND.
    setScreen(config.screen);
    m_canvasSourceRect = contentCanvasRect;
    m_backgroundCanvasRect = m_canvasSourceRect;
    m_resultSurfaceCanvasRect = surfaceCanvasRect;
    m_resultStyle = ScreenshotResultCompositor::normalizedStyle(config.resultStyle);
    m_imageSource = std::move(imageSource);
    m_presentationCompletion = std::move(completion);
    m_imageLoader = config.imageLoader;
    // A materialized source may only be a geometry placeholder when a loader
    // is present. Do not expose it to OCR or editing as the final image.
    m_originalImage = !m_imageLoader && m_imageSource.isMaterialized()
                          ? m_imageSource.materializedImage
                          : QImage();
    if (!m_originalImage.isNull()) {
        m_originalImage.setDevicePixelRatio(1.0);
    }
    m_transformedImage = ScreenshotResultCompositor::normalizeImage(m_originalImage);
    m_originalPixelSize = !m_originalImage.isNull()
                              ? m_originalImage.size()
                              : QSize(std::max(1, qRound(m_canvasSourceRect.width())),
                                      std::max(1, qRound(m_canvasSourceRect.height())));
    m_ocrSupported = screenshotOcrImageWithinPixelLimit(m_originalPixelSize);
    m_ocrReady = false;
    m_ocrMode = false;
    m_formattedTextDocument = config.formattedTextDocument;
    m_formattedPlainText = config.formattedPlainText;
    m_formattedTextDevicePixelRatio =
        config.formattedTextDocument != nullptr ? config.formattedTextDevicePixelRatio : 1.0;
    m_originalClipboardContent = config.originalClipboardContent;
    if (m_formattedTextDocument != nullptr && m_formattedPlainText.isEmpty()) {
        m_formattedPlainText = m_formattedTextDocument->toPlainText();
    }
    m_formattedTextAvailable = m_formattedTextDocument != nullptr;
    m_automaticTextRecognition = !m_formattedTextAvailable && config.automaticTextRecognition;
    m_editingEnabled = config.enableEditing;
    m_mouseWheelZoomMode = config.mouseWheelZoomMode;
    m_imageTransform.reset();
    m_recognition = config.recognition;
    m_qrRecognition = config.qrRecognition;
    m_tableRecognition = config.tableRecognition;
    m_recognitionResults = config.recognitionResults;
    if (m_recognitionResults.text.has_value()) {
        m_recognitionResults.text->filteredImage = {};
    }
    m_initialPhysicalSize =
        config.fullResolutionScaleBasis.isValid() && !config.fullResolutionScaleBasis.isEmpty()
            ? config.fullResolutionScaleBasis
            : config.nativeGeometry.size();
    m_scalePercent =
        config.initialScalePercent > 0.0
            ? config.initialScalePercent
            : 100.0 * config.nativeGeometry.width() / std::max(1, m_initialPhysicalSize.width());
    SNOW_SHOT_PIN_PERF_MILESTONE("window.state_initialized");
    if (m_nativeGeometryController == nullptr ||
        !m_nativeGeometryController->initialize(config.nativeGeometry)) {
        finishPresentation(false);
        return false;
    }
    if (config.enableEditing) {
        if (m_editButton != nullptr) {
            m_editButton->show();
        }
    } else if (m_editButton != nullptr) {
        m_editButton->hide();
        m_canvas->setInteractionEnabled(false);
    }

    m_screenshotRenderer->setImageSource(m_imageSource);
    m_screenshotRenderer->setImageViewportPhysicalSize({});
    m_screenshotRenderer->setPinnedResultSurface(m_backgroundCanvasRect, m_resultSurfaceCanvasRect,
                                                 m_resultStyle);
    const bool deferContent = static_cast<bool>(m_imageLoader) || !m_imageSource.isMaterialized();
    m_canvas->setCanvasContentVisible(!deferContent);
    // The pinned renderer paints its image in renderBeforeCanvas, even when the
    // canvas scene content is suppressed. Keep the deferred shell transparent
    // by withholding the source until materialization completes.
    if (deferContent) {
        m_screenshotRenderer->setImageSource({});
    }
    SNOW_SHOT_PIN_PERF_MILESTONE("window.canvas_configured");
#if defined(Q_OS_WIN) || defined(_WIN32)
    // Only local dimensions belong to QWidget. The screen position is a
    // physical-pixel property of the HWND and is applied after creation.
    resize(logicalSize);
#else
    setGeometry(logicalGeometry);
#endif
    ensurePolished();
    if (layout() != nullptr) {
        layout()->activate();
    }

    const WId nativeWindowId = winId();
    SNOW_SHOT_PIN_PERF_MILESTONE("window.hwnd_created");
    SNOW_SHOT_PIN_PERF_COUNTER("window.hwnd", static_cast<qint64>(nativeWindowId));
    if (QWindow* handle = windowHandle()) {
        handle->setMinimumSize(QSize(1, 1));
        connect(handle, &QWindow::screenChanged, this, [this]() { scheduleNativeScaleAdoption(); });
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (!native::applySystemResizeStyle(nativeWindowId)) {
        finishPresentation(false);
        return false;
    }
    if (!native::applyClientGeometry(nativeWindowId, config.nativeGeometry,
                                     native::GeometryUpdate::DiscardClientPixels)) {
        finishPresentation(false);
        return false;
    }
    SNOW_SHOT_PIN_PERF_MILESTONE("window.native_geometry_applied");
#else
    Q_UNUSED(nativeWindowId);
#endif
    updateCanvasViewport();
    updateRecognitionContentGeometry();
    updateControlsGeometry();
    SNOW_SHOT_PIN_PERF_MILESTONE("window.geometry_updated");
    SNOW_SHOT_PIN_PERF_MILESTONE("window.edit_controller_deferred");

    if (m_recognitionSession != nullptr) {
        m_recognitionSession->invalidate();
        m_recognitionSession.reset();
    }
    if (m_recognitionContent != nullptr) {
        delete m_recognitionContent;
        m_recognitionContent = nullptr;
    }
    m_recognitionSession = std::make_unique<ScreenshotRecognitionSessionController>(
        m_recognition, m_qrRecognition, m_tableRecognition,
        ScreenshotRecognitionSessionActions{
            [this]() -> ScreenshotRecognitionWindow* {
                if (m_recognitionContent == nullptr) {
                    auto* content = new ScreenshotRecognitionWindow(
                        ScreenshotRecognitionWindowActions{
                            [this]() {
                                if (m_recognitionSession != nullptr) {
                                    m_recognitionSession->deactivate();
                                }
                            },
                            [this](const QString& text) {
                                if (m_recognitionSession != nullptr) {
                                    m_recognitionSession->setTextDraft(text);
                                }
                            },
                            [this](const ScreenshotTableCommandState& state) {
                                if (m_recognitionSession != nullptr) {
                                    m_recognitionSession->handleTableCommandState(state);
                                }
                            },
                            [this](const QString& message) {
                                showPinnedRecognitionMessage(this, message, false);
                            },
                            [this](const QUrl& url) {
                                if (m_recognitionSession != nullptr &&
                                    m_recognitionSession->qrModeActive()) {
                                    if (url.isValid()) {
                                        QDesktopServices::openUrl(url);
                                    }
                                }
                            },
                            [this]() {
                                if (m_recognitionSession != nullptr) {
                                    m_recognitionSession->undoTextEdit();
                                }
                            },
                            [this]() {
                                if (m_recognitionSession != nullptr) {
                                    m_recognitionSession->redoTextEdit();
                                }
                            },
                        },
                        this, ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild,
                        m_shortcutManager.get());
                    content->setObjectName(QStringLiteral("screenshotPinnedRecognitionContent"));
                    connect(content, &ScreenshotRecognitionWindow::embeddedContextMenuRequested,
                            this, &ScreenshotPinnedWindow::showContextMenu);
                    QScreen* contentScreen = windowHandle() != nullptr
                                                 ? windowHandle()->screen()
                                                 : QGuiApplication::primaryScreen();
                    if (contentScreen == nullptr || m_canvas == nullptr ||
                        !content->present(ScreenshotRecognitionWindow::Config{
                            contentScreen, this, m_canvas->geometry(), m_canvasSourceRect,
                            ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild,
                            m_formattedTextDevicePixelRatio})) {
                        delete content;
                        return nullptr;
                    }
                    content->hide();
                    m_recognitionContent = content;
                    updateRecognitionContentGeometry();
                }
                return m_recognitionContent;
            },
            [this]() {
                if (m_recognitionContent != nullptr) {
                    delete m_recognitionContent;
                    m_recognitionContent = nullptr;
                }
            },
            [this](std::shared_ptr<ScreenshotOcrPresentation> presentation) {
                m_ocrReady = presentation != nullptr;
                m_originalOcrPresentation = std::move(presentation);
                updateOcrPresentation();
            },
            [this](std::shared_ptr<ScreenshotOcrPresentation> presentation) {
                Q_UNUSED(presentation);
            },
            [this](std::shared_ptr<QTextDocument> document) {
                if (m_recognitionContent != nullptr) {
                    m_recognitionContent->showFormattedText(std::move(document));
                }
            },
            [this]() {
                if (m_screenshotRenderer != nullptr) {
                    m_screenshotRenderer->clearOcrPresentation();
                }
                m_originalOcrPresentation.reset();
                m_displayOcrPresentation.reset();
            },
            [this](bool active) {
                m_ocrMode = active;
                if (m_canvas != nullptr) {
                    m_canvas->setCanvasContentVisible(!active);
                    m_canvas->setInteractionEnabled(!active && m_editController != nullptr &&
                                                    m_editController->editMode());
                    if (active) {
                        m_canvas->setFocus(Qt::OtherFocusReason);
                    } else {
                        m_canvas->clearCursorForLayer(SnowCanvasCursorLayer::Host);
                    }
                }
                if (m_recognitionContent != nullptr) {
                    m_recognitionContent->setVisible(active);
                    if (active) {
                        m_recognitionContent->raise();
                        updateRecognitionContentGeometry();
                        m_recognitionContent->setFocus(Qt::OtherFocusReason);
                    }
                }
                if (m_borderFrame != nullptr) {
                    m_borderFrame->raise();
                }
                updateControlsGeometry();
            },
            [this](int mode) {
                if (ScreenshotPinnedEditController* controller = m_editController) {
                    if (ScreenshotToolPaletteHost* host = controller->toolbarHost()) {
                        if (mode ==
                            static_cast<int>(ScreenshotRecognitionSessionController::Mode::Text)) {
                            host->setActiveTool(m_translateAfterRecognition
                                                    ? ScreenshotToolPalette::Tool::TextTranslation
                                                    : ScreenshotToolPalette::Tool::Ocr);
                        } else if (mode ==
                                   static_cast<int>(
                                       ScreenshotRecognitionSessionController::Mode::Table)) {
                            host->setActiveTool(ScreenshotToolPalette::Tool::Table);
                        } else if (mode == static_cast<int>(
                                               ScreenshotRecognitionSessionController::Mode::Qr)) {
                            host->setActiveTool(ScreenshotToolPalette::Tool::Qr);
                        } else if (controller->editMode()) {
                            controller->restoreDrawingToolState();
                        } else {
                            host->clearActiveTool();
                        }
                    }
                }
            },
            [this](bool available, bool editing, bool canUndo, bool canRedo) {
                if (m_editController != nullptr && m_editController->toolbarWindow() != nullptr) {
                    if (ScreenshotToolPalette* toolbar =
                            m_editController->toolbarWindow()->palette()) {
                        toolbar->setTextEditingState(available, editing, canUndo, canRedo);
                    }
                }
            },
            [this](bool available, bool translating, bool streaming, bool canUndo, bool canRedo,
                   bool canReset) {
                if (m_editController != nullptr && m_editController->toolbarWindow() != nullptr) {
                    if (ScreenshotToolPalette* toolbar =
                            m_editController->toolbarWindow()->palette()) {
                        toolbar->setTextTranslationState(available, translating, streaming, canUndo,
                                                         canRedo, canReset);
                    }
                }
            },
            [this](bool available, bool canUndo, bool canRedo, bool canMerge, bool canSplit,
                   bool canReset) {
                if (m_editController != nullptr && m_editController->toolbarWindow() != nullptr) {
                    if (ScreenshotToolPalette* toolbar =
                            m_editController->toolbarWindow()->palette()) {
                        toolbar->setTableEditingState(available, canUndo, canRedo, canMerge,
                                                      canSplit, canReset);
                    }
                }
            },
            [this](bool textBusy, bool tableBusy, bool qrBusy) {
                if (m_editController != nullptr && m_editController->toolbarWindow() != nullptr) {
                    if (ScreenshotToolPalette* toolbar =
                            m_editController->toolbarWindow()->palette()) {
                        toolbar->setOcrBusy(textBusy);
                        toolbar->setTableBusy(tableBusy);
                        toolbar->setQrBusy(qrBusy);
                    }
                }
                refreshContextMenu();
            },
            [this](const QString& message) {
                ScreenshotMessageService::loadingFor(
                    this, QString::fromLatin1(kRecognitionMessageKey), message);
            },
            [this]() {
                ScreenshotMessageService::destroyFor(
                    this, QString::fromLatin1(kRecognitionMessageKey));
            },
            [this](const QString& message, bool error) {
                if (error) {
                    showPinnedRecognitionMessage(this, message, true);
                } else {
                    showPinnedRecognitionMessage(this, message, false);
                }
            },
            [this](const QUrl& url) {
                if (url.isValid()) {
                    QDesktopServices::openUrl(url);
                }
            },
            [this]() -> QWidget* { return this; },
            [this](const QString& formatting, const QString& punctuation) {
                if (m_editController != nullptr && m_editController->toolbarWindow() != nullptr) {
                    if (ScreenshotToolPalette* toolbar =
                            m_editController->toolbarWindow()->palette()) {
                        toolbar->setTextTransformSelections(formatting, punctuation);
                    }
                }
            },
            [this](const QString& message) {
                ScreenshotMessageService::loadingFor(
                    this, QString::fromLatin1(kModelDownloadMessageKey), message);
            },
            [this](const QString& message) {
                ScreenshotMessageService::loadingFor(
                    this, QString::fromLatin1(kRecognitionMessageKey), message);
            },
            [this]() {
                ScreenshotMessageService::destroyFor(
                    this, QString::fromLatin1(kModelDownloadMessageKey));
            },
            [this]() {
                const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
                return theme.colorBgContainer.isValid() ? theme.colorBgContainer
                                                         : QColor(Qt::white);
            },
            [this](ScreenshotOcrRequest& request) {
                if (!m_transformedImage.isNull() && !m_backgroundCanvasRect.isEmpty()) {
                    request.image = m_transformedImage;
                    request.canvasRect = m_backgroundCanvasRect;
                }
                if (m_displayOcrPresentation != nullptr) {
                    request.presentation = m_displayOcrPresentation;
                }
            },
            []() { return false; },
            [this](std::shared_ptr<ScreenshotOcrPresentation> presentation, QImage filteredImage,
                   QRectF filteredImageCanvasRect) {
                Q_UNUSED(presentation);
                if (m_screenshotRenderer != nullptr && !filteredImage.isNull()) {
                    const QRectF canvasRect =
                        filteredImageCanvasRect.isValid() && !filteredImageCanvasRect.isEmpty()
                            ? filteredImageCanvasRect.normalized()
                            : m_backgroundCanvasRect;
                    m_screenshotRenderer->setOcrFilteredImage(std::move(filteredImage),
                                                              canvasRect);
                }
            },
        },
        this);
    connect(m_recognitionSession.get(), &ScreenshotRecognitionSessionController::textResultChanged,
            this, [this](bool available) {
                m_ocrReady = available;
                refreshContextMenu();
                if (m_editController != nullptr && m_editController->toolbarWindow() != nullptr) {
                    if (ScreenshotToolPalette* toolbar =
                            m_editController->toolbarWindow()->palette()) {
                        toolbar->setOcrEnabled(m_formattedTextAvailable ||
                                               (m_ocrSupported && m_recognition != nullptr));
                    }
                }
                if (available && m_translateAfterRecognition && m_recognitionSession != nullptr &&
                    m_recognitionSession->active() &&
                    m_recognitionSession->mode() ==
                        ScreenshotRecognitionSessionController::Mode::Text) {
                    m_translateAfterRecognition = false;
                    m_recognitionSession->beginTextTranslation();
                }
            });
    if (!m_originalImage.isNull()) {
        m_recognitionSession->setTarget(ScreenshotRecognitionTarget{
            !m_recognitionResults.isEmpty()
                ? m_recognitionResults.key
                : QStringLiteral("pinned:%1").arg(reinterpret_cast<quintptr>(this)),
            m_originalImage,
            m_canvasSourceRect, m_formattedTextDocument, m_formattedPlainText});
        m_recognitionSession->seedRecognitionResults(m_recognitionResults);
        m_ocrReady = m_recognitionSession->hasTextResult();
    }
    SNOW_SHOT_PIN_PERF_MILESTONE("window.recognition_session_ready");
    SNOW_SHOT_PIN_PERF_MILESTONE("window.pinned_toolbar_deferred");
    refreshContextMenu();
    SNOW_SHOT_PIN_PERF_MILESTONE("window.context_menu_ready");
    if (m_automaticTextRecognition && m_recognitionSession != nullptr &&
        !m_recognitionSession->hasTextResult()) {
        QTimer::singleShot(0, this, [this]() {
            if (m_recognitionSession != nullptr && m_recognition != nullptr &&
                m_automaticTextRecognition && m_ocrSupported && !m_closing) {
                requestMaterializedImage([this](bool succeeded) {
                    if (succeeded && !m_closing && m_automaticTextRecognition &&
                        m_recognitionSession != nullptr && m_recognition != nullptr) {
                        m_recognitionSession->prefetchText();
                    }
                });
            }
        });
    }

    SNOW_SHOT_PIN_PERF_MILESTONE("window.before_show");
    show();
    SNOW_SHOT_PIN_PERF_MILESTONE("window.show_returned");
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (!deferContent) {
        m_canvas->repaint();
        SNOW_SHOT_PIN_PERF_MILESTONE("window.canvas_repainted");
        if (!native::synchronizeClientPaint(nativeWindowId)) {
            hide();
            finishPresentation(false);
            return false;
        }
        SNOW_SHOT_PIN_PERF_MILESTONE("window.native_paint_synchronized");
    }
#endif
    if (currentNativeGeometry() != config.nativeGeometry) {
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (!native::applyClientGeometry(nativeWindowId, config.nativeGeometry,
                                          native::GeometryUpdate::DiscardClientPixels)) {
            hide();
            finishPresentation(false);
            return false;
        }
        if (!deferContent && !native::synchronizeClientPaint(nativeWindowId)) {
            hide();
            finishPresentation(false);
            return false;
        }
#else
        setGeometry(logicalRectForNativeRect(config.nativeGeometry));
#endif
    }
    if (!native::installSynchronizedResize(nativeWindowId, &m_systemSizingActive)) {
        hide();
        finishPresentation(false);
        return false;
    }
    m_synchronizedResizeWindowId = nativeWindowId;
    m_presented = true;
    raise();
    if (deferContent) {
        requestMaterializedImage([this](bool succeeded) {
            if (!succeeded || m_closing) {
                finishPresentation(false);
                if (!m_closing) close();
                return;
            }
            if (m_canvas != nullptr) {
                m_canvas->setCanvasContentVisible(!m_ocrMode);
                m_canvas->update();
            }
            finishPresentation(true, m_originalImage);
        });
    } else if (m_presentationCompletion) {
        QTimer::singleShot(0, this, [this]() { finishPresentation(true, m_originalImage); });
    }
    return true;
}

QRect ScreenshotPinnedWindow::currentNativeGeometry() const {
#if defined(Q_OS_WIN) || defined(_WIN32)
    const QRect nativeGeometry = native::currentClientGeometry(winId());
    if (nativeGeometry.isValid() && !nativeGeometry.isEmpty()) {
        return nativeGeometry;
    }
#else
    const QRect widgetGeometry = frameGeometry();
    if (widgetGeometry.isValid() && !widgetGeometry.isEmpty()) {
        QScreen* geometryScreen = screen();
        if (geometryScreen == nullptr) {
            geometryScreen = QGuiApplication::screenAt(widgetGeometry.center());
        }
        return nativeRectForLogicalRect(widgetGeometry, geometryScreen);
    }
#endif
    if (m_nativeGeometryController != nullptr) {
        const QRect committed = m_nativeGeometryController->committedGeometry();
        if (committed.isValid() && !committed.isEmpty()) {
            return committed;
        }
    }
    return frameGeometry();
}

bool ScreenshotPinnedWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event == nullptr || m_closing) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::Wheel &&
        (handleOpacityWheel(watched, static_cast<QWheelEvent*>(event)) ||
         handleScaleWheel(watched, static_cast<QWheelEvent*>(event)))) {
        return true;
    }
    const bool watchedControls =
        watched == m_controlsPanel || watched == m_editButton || watched == m_closeButton;
    if (watchedControls && event->type() == QEvent::Enter) {
        if (!m_windowDragActive) {
            clearWindowDragCursor();
        }
        return QWidget::eventFilter(watched, event);
    }
    if (watched != m_canvas) {
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::Resize) {
        // The canvas is laid out independently of the top-level native window.
        // Keep its camera tied to the actual paint surface as that layout settles.
        updateCanvasViewport();
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::ContextMenu) {
        auto* contextEvent = static_cast<QContextMenuEvent*>(event);
        showContextMenu(contextEvent->globalPos());
        contextEvent->accept();
        return true;
    }
    const bool editMode = m_editController != nullptr && m_editController->editMode();
    if (!editMode && !m_ocrMode) {
        if (event->type() == QEvent::MouseMove) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            updateWindowDragCursor(
                windowPositionForEvent(watched, mouseEvent->position()).toPoint());
        } else if (event->type() == QEvent::Leave) {
            if (!m_windowDragActive) {
                clearWindowDragCursor();
            }
        } else if (event->type() == QEvent::MouseButtonPress) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton && startWindowMove()) {
                mouseEvent->accept();
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton && m_windowDragActive) {
                static_cast<void>(finishNativeGeometryInteraction());
                finishWindowMove();
                mouseEvent->accept();
                return true;
            }
        }
    }
    if (!m_ocrMode || m_displayOcrPresentation == nullptr) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            m_displayOcrPresentation->beginTextSelection(
                canvasPositionForViewPosition(mouseEvent->position()));
            m_screenshotRenderer->updateOcrSelection();
            mouseEvent->accept();
            return true;
        }
    } else if (event->type() == QEvent::MouseMove) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        const QPointF canvasPosition = canvasPositionForViewPosition(mouseEvent->position());
        if (m_displayOcrPresentation->textSelectionActive()) {
            m_displayOcrPresentation->updateTextSelection(
                m_displayOcrPresentation->textPositionAt(canvasPosition, true));
            m_screenshotRenderer->updateOcrSelection();
        }
        m_canvas->setCursorForLayer(
            SnowCanvasCursorLayer::Host,
            QCursor(m_displayOcrPresentation->lineAt(canvasPosition) >= 0 ? Qt::IBeamCursor
                                                                         : Qt::ArrowCursor));
        mouseEvent->accept();
        return true;
    } else if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            const QPointF canvasPosition = canvasPositionForViewPosition(mouseEvent->position());
            m_displayOcrPresentation->updateTextSelection(
                m_displayOcrPresentation->textPositionAt(canvasPosition, true));
            m_displayOcrPresentation->finishTextSelection();
            m_screenshotRenderer->updateOcrSelection();
            mouseEvent->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ScreenshotPinnedWindow::contextMenuEvent(QContextMenuEvent* event) {
    if (event == nullptr) {
        return;
    }
    showContextMenu(event->globalPos());
    event->accept();
}

void ScreenshotPinnedWindow::closeEvent(QCloseEvent* event) {
    m_closing = true;
    invalidatePendingCopy();
    finishPresentation(false);
    m_materializationJob.cancel();
    m_materializationJob = {};
    m_materializationLoading = false;
    m_imageLoader = {};
    m_materializationCallbacks.clear();
    m_fileSaveJob.cancel();
    m_fileSaveJob = {};
    if (m_synchronizedResizeWindowId != 0) {
        native::removeSynchronizedResize(m_synchronizedResizeWindowId);
        m_synchronizedResizeWindowId = 0;
    }
    finishWindowMove();
    clearWindowDragCursor();
    m_systemSizingActive = false;
    stopRecognition();
    // Recognition callbacks still target the renderer and canvas. Finish their
    // teardown before those backing objects are destroyed below.
    if (m_recognitionSession != nullptr) {
        m_recognitionSession.reset();
    }
    if (m_recognitionContent != nullptr) {
        delete m_recognitionContent;
        m_recognitionContent = nullptr;
    }
    if (m_geometryAnimation != nullptr) {
        m_geometryAnimation->stop();
    }
    m_geometryAnimating = false;
    if (m_nativeGeometryController != nullptr) {
        m_nativeGeometryController->beginClosing();
    }
    delete m_editController;
    m_editController = nullptr;
    destroyCanvas();
    m_runtime.destroyAsync();
    QWidget::closeEvent(event);
}

void ScreenshotPinnedWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_closing) {
        return;
    }
    invalidatePendingCopy();
    if (m_borderFrame != nullptr) {
        updatePinnedBorderGeometry(*m_borderFrame, rect());
        m_borderFrame->raise();
    }
    updateCanvasViewport();
    updateRecognitionContentGeometry();
    updateControlsGeometry();
    if (m_editController != nullptr) {
        m_editController->updatePlacement();
    }
}

void ScreenshotPinnedWindow::moveEvent(QMoveEvent* event) {
    if (m_closing) {
        QWidget::moveEvent(event);
        return;
    }

    const QPoint logicalDelta = event != nullptr ? event->pos() - event->oldPos() : QPoint();

    QWidget::moveEvent(event);
    bool passiveMismatch = false;
    if (m_nativeGeometryController != nullptr) {
        const auto phase = m_nativeGeometryController->phase();
        const bool passive =
            phase == ScreenshotPinnedNativeGeometryController::Phase::Stable ||
            ((phase == ScreenshotPinnedNativeGeometryController::Phase::MovePending ||
              phase == ScreenshotPinnedNativeGeometryController::Phase::ResizePending) &&
             !m_nativeGeometryController->hasAcceptedInteractiveGeometry());
        passiveMismatch =
            passive && currentNativeGeometry() != m_nativeGeometryController->targetGeometry();
    }
    if (m_editController != nullptr && !m_passiveGeometryReconciliationActive && !passiveMismatch) {
        m_editController->updateAfterPinnedWindowMove(logicalDelta);
    }
}

void ScreenshotPinnedWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (layout() != nullptr) {
        layout()->activate();
    }
    updateCanvasViewport();
    updateControlsGeometry();
    if (m_editController != nullptr) {
        m_editController->updatePlacement();
        m_editController->raiseToolbar();
    }
    updateWindowDragCursor(mapFromGlobal(QCursor::pos()));
}

void ScreenshotPinnedWindow::mousePressEvent(QMouseEvent* event) {
    if (m_closing) {
        QWidget::mousePressEvent(event);
        return;
    }

    const bool editMode = m_editController != nullptr && m_editController->editMode();
    if (event != nullptr && event->button() == Qt::LeftButton && !editMode && !m_ocrMode &&
        !isControlsPanelPosition(event->position().toPoint()) && startWindowMove()) {
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ScreenshotPinnedWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event != nullptr && event->button() == Qt::LeftButton) {
        static_cast<void>(finishNativeGeometryInteraction());
    }
    if (event != nullptr && event->button() == Qt::LeftButton && m_windowDragActive) {
        finishWindowMove();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ScreenshotPinnedWindow::wheelEvent(QWheelEvent* event) {
    if (!handleOpacityWheel(this, event) && !handleScaleWheel(this, event)) {
        QWidget::wheelEvent(event);
    }
}

void ScreenshotPinnedWindow::paintEvent(QPaintEvent* event) {
    QColor background = adqt::theme::ThemeManager::instance().resolveTheme(this).colorBgContainer;
    if (!background.isValid()) {
        background = palette().color(QPalette::Window);
    }
    background.setAlpha(255);

    QPainter painter(this);
    painter.fillRect(event != nullptr ? event->rect() : rect(), background);
}

void ScreenshotPinnedWindow::createUi() {
    setMinimumSize(1, 1);
    m_scaleLabelTimer = new QTimer(this);
    m_scaleLabelTimer->setSingleShot(true);
    m_scaleLabelTimer->setInterval(kScaleReadoutDurationMs);
    connect(m_scaleLabelTimer, &QTimer::timeout, this, [this]() {
        if (m_scaleLabel != nullptr) {
            m_scaleLabel->hide();
        }
    });
    m_nativeScaleSettleTimer = new QTimer(this);
    m_nativeScaleSettleTimer->setSingleShot(true);
    m_nativeScaleSettleTimer->setInterval(0);
    connect(m_nativeScaleSettleTimer, &QTimer::timeout, this,
            &ScreenshotPinnedWindow::adoptSettledNativeScale);
    auto* layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_canvas = new SnowCanvasWidget(m_runtime, this);
    m_canvas->setAttribute(Qt::WA_NativeWindow, false);
    m_canvas->setAttribute(Qt::WA_OpaquePaintEvent, false);
    m_canvas->setMinimumSize(1, 1);
    m_canvas->setWheelZoomEnabled(false);
    m_screenshotRenderer = std::make_unique<ScreenshotCanvasRenderer>(*m_canvas);
    m_canvas->setCustomRenderer(m_screenshotRenderer.get());
    // Child widgets participate in the translucent top-level backing store;
    // WA_TranslucentBackground is a top-level contract on Windows and makes
    // an alien child disappear from layered-window screen captures.
    m_canvas->setAttribute(Qt::WA_TranslucentBackground, false);
    m_canvas->setAttribute(Qt::WA_NoSystemBackground, true);
    m_canvas->setAutoFillBackground(false);
    m_canvas->setClearBackgroundEnabled(true);
    m_canvas->setMouseTracking(true);
    m_canvas->setFocusPolicy(Qt::StrongFocus);
    m_canvas->installEventFilter(this);
    connect(m_canvas, &SnowCanvasWidget::historyStateChanged, this,
            &ScreenshotPinnedWindow::invalidatePendingCopy);
    connect(m_canvas, &SnowCanvasWidget::watermarkPreviewApplied, this,
            &ScreenshotPinnedWindow::invalidatePendingCopy);
    connect(m_canvas, &SnowCanvasWidget::spotlightPreviewApplied, this,
            &ScreenshotPinnedWindow::invalidatePendingCopy);
    layout->addWidget(m_canvas);

    m_borderFrame = new QFrame(this);
    m_borderFrame->setObjectName(QStringLiteral("screenshotPinnedBorder"));
    m_borderFrame->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    applyRuntimeBorderColor();
    updatePinnedBorderGeometry(*m_borderFrame, rect());
    m_borderFrame->raise();

    m_scaleLabel = new QLabel(this);
    m_scaleLabel->setAttribute(Qt::WA_NativeWindow, false);
    m_scaleLabel->setObjectName(QStringLiteral("screenshotPinnedScaleLabel"));
    m_scaleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_scaleLabel->setAlignment(Qt::AlignCenter);
    m_scaleLabel->setStyleSheet(
        QStringLiteral("QLabel#screenshotPinnedScaleLabel { "
                       "color: white; background-color: rgba(0, 0, 0, 150); "
                       "padding: 3px 6px; border-radius: 4px; }"));
    m_scaleLabel->hide();

    m_controlsPanel = new QFrame(this);
    m_controlsPanel->setAttribute(Qt::WA_NativeWindow, false);
    m_controlsPanel->setObjectName(QStringLiteral("screenshotPinnedControlsPanel"));
    m_controlsPanel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* controlsLayout = new QHBoxLayout(m_controlsPanel);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(kControlButtonSpacing);
    m_editButton = createControlButton(m_controlsPanel, "Enable drawing mode",
                                       outlined_icons::Edit(), PinnedControlButton::Intent::Edit);
    m_editButton->setAttribute(Qt::WA_NativeWindow, false);
    m_editButton->setObjectName(QStringLiteral("screenshotPinnedEditButton"));
    m_closeButton = createControlButton(m_controlsPanel, "Close", outlined_icons::Close(),
                                        PinnedControlButton::Intent::Close);
    m_closeButton->setAttribute(Qt::WA_NativeWindow, false);
    m_closeButton->setObjectName(QStringLiteral("screenshotPinnedCloseButton"));
    m_controlsPanel->installEventFilter(this);
    m_editButton->installEventFilter(this);
    m_closeButton->installEventFilter(this);
    controlsLayout->addWidget(m_editButton);
    controlsLayout->addWidget(m_closeButton);
    m_controlsPanel->adjustSize();
    m_controlsPanel->raise();

    connect(m_editButton, &adqt::widgets::AdButton::clicked, this, [this]() { setEditMode(true); });
    connect(m_closeButton, &adqt::widgets::AdButton::clicked, this, [this]() {
        if (m_closing) {
            return;
        }
        m_closing = true;
        stopRecognition();
        QTimer::singleShot(0, this, [this]() { close(); });
    });
    createContextMenu();
}

void ScreenshotPinnedWindow::createContextMenu() {
    m_contextMenu = new adqt::widgets::AdContextMenu(this);
    m_contextMenu->setObjectName(QStringLiteral("screenshotPinnedContextMenu"));
    m_contextMenu->setFixedWidth(300);

    QAction* copyAction = m_contextMenu->addItem(tr("Copy to Clipboard"), outlined_icons::Copy());
    setActionTranslationSource(copyAction, "Copy to Clipboard");
    copyAction->setObjectName(QStringLiteral("screenshotPinnedCopyAction"));
    connect(copyAction, &QAction::triggered, this, &ScreenshotPinnedWindow::copyCurrentViewport);

    QAction* copyOriginalAction =
        m_contextMenu->addItem(tr("Copy Original Content"), outlined_icons::FileImage());
    setActionTranslationSource(copyOriginalAction, "Copy Original Content");
    copyOriginalAction->setObjectName(QStringLiteral("screenshotPinnedCopyOriginalAction"));
    connect(copyOriginalAction, &QAction::triggered, this,
            &ScreenshotPinnedWindow::copyOriginalContent);

    QAction* saveAction = m_contextMenu->addItem(tr("Save as File"), custom_outlined_icons::Save());
    setActionTranslationSource(saveAction, "Save as File");
    saveAction->setObjectName(QStringLiteral("screenshotPinnedSaveAsFileAction"));
    connect(saveAction, &QAction::triggered, this, &ScreenshotPinnedWindow::saveAsFile);

    m_ocrAction =
        m_contextMenu->addItem(tr("Recognizing text"), custom_outlined_icons::ToolRecognizeText());
    setActionTranslationSource(m_ocrAction, "Recognizing text");
    m_ocrAction->setObjectName(QStringLiteral("screenshotPinnedOcrAction"));
    m_ocrAction->setCheckable(true);
    m_ocrAction->setEnabled(false);
    connect(m_ocrAction, &QAction::toggled, this, [this](bool enabled) {
        if (enabled) {
            setEditMode(true);
            activateRecognitionMode(
                static_cast<int>(ScreenshotRecognitionSessionController::Mode::Text));
        } else {
            deactivateRecognition();
        }
    });

    m_contextMenu->addSeparator();

    m_drawingAction = m_contextMenu->addItem(tr("Drawing Mode"), outlined_icons::Edit());
    setActionTranslationSource(m_drawingAction, "Drawing Mode");
    m_drawingAction->setObjectName(QStringLiteral("screenshotPinnedDrawingAction"));
    m_drawingAction->setCheckable(true);
    connect(m_drawingAction, &QAction::toggled, this, [this](bool enabled) {
        if (enabled) {
            deactivateRecognition();
        }
        setEditMode(enabled);
    });

    auto* processMenu = m_contextMenu->addSubMenu(tr("Process Image"), outlined_icons::Picture());
    setActionTranslationSource(processMenu->menuAction(), "Process Image");
    processMenu->setObjectName(QStringLiteral("screenshotPinnedProcessImageMenu"));
    QAction* rotateClockwise =
        processMenu->addItem(tr("Rotate Clockwise"), outlined_icons::RotateRight());
    setActionTranslationSource(rotateClockwise, "Rotate Clockwise");
    connect(rotateClockwise, &QAction::triggered, this, [this]() {
        QTransform operation;
        operation.rotate(90.0);
        applyImageOperation(operation, 1);
    });
    QAction* rotateCounterClockwise =
        processMenu->addItem(tr("Rotate Counterclockwise"), outlined_icons::RotateLeft());
    setActionTranslationSource(rotateCounterClockwise, "Rotate Counterclockwise");
    connect(rotateCounterClockwise, &QAction::triggered, this, [this]() {
        QTransform operation;
        operation.rotate(-90.0);
        applyImageOperation(operation, -1);
    });
    QAction* flipHorizontal = processMenu->addItem(tr("Flip Horizontally"), outlined_icons::Swap());
    setActionTranslationSource(flipHorizontal, "Flip Horizontally");
    connect(flipHorizontal, &QAction::triggered, this, [this]() {
        QTransform operation;
        operation.scale(-1.0, 1.0);
        applyImageOperation(operation);
    });
    QAction* flipVertical =
        processMenu->addItem(tr("Flip Vertically"), custom_outlined_icons::FlipVertical());
    setActionTranslationSource(flipVertical, "Flip Vertically");
    connect(flipVertical, &QAction::triggered, this, [this]() {
        QTransform operation;
        operation.scale(1.0, -1.0);
        applyImageOperation(operation);
    });
    processMenu->addSeparator();
    QAction* resetTransform = processMenu->addItem(tr("Reset Transform"), outlined_icons::Reload());
    setActionTranslationSource(resetTransform, "Reset Transform");
    connect(resetTransform, &QAction::triggered, this,
            &ScreenshotPinnedWindow::resetImageTransform);

    auto* opacityMenu = m_contextMenu->addSubMenu(tr("Opacity"), outlined_icons::BgColors());
    setActionTranslationSource(opacityMenu->menuAction(), "Opacity");
    opacityMenu->setObjectName(QStringLiteral("screenshotPinnedOpacityMenu"));
    m_opacityActions = new QActionGroup(opacityMenu);
    m_opacityActions->setExclusive(true);
    for (int percent : {25, 50, 75, 100}) {
        QAction* action = opacityMenu->addItem(tr("%1%").arg(percent));
        action->setCheckable(true);
        action->setData(percent);
        m_opacityActions->addAction(action);
    }
    connect(m_opacityActions, &QActionGroup::triggered, this, [this](QAction* action) {
        if (action != nullptr) {
            setOpacityPercent(action->data().toInt());
        }
    });
    opacityMenu->addSeparator();
    m_opacityReadoutAction = opacityMenu->addItem(tr("Current: %1%").arg(m_opacityPercent));
    m_opacityReadoutAction->setObjectName(QStringLiteral("screenshotPinnedOpacityReadoutAction"));
    m_opacityReadoutAction->setEnabled(false);

    auto* scaleMenu = m_contextMenu->addSubMenu(tr("Scale"), outlined_icons::Percentage());
    setActionTranslationSource(scaleMenu->menuAction(), "Scale");
    scaleMenu->setObjectName(QStringLiteral("screenshotPinnedScaleMenu"));
    m_scaleMenuAction = scaleMenu->menuAction();
    m_scaleActions = new QActionGroup(scaleMenu);
    m_scaleActions->setExclusive(true);
    for (int percent : {25, 50, 75, 100}) {
        QAction* action = scaleMenu->addItem(tr("%1%").arg(percent));
        action->setCheckable(true);
        action->setData(percent);
        m_scaleActions->addAction(action);
    }
    connect(m_scaleActions, &QActionGroup::triggered, this, [this](QAction* action) {
        if (action != nullptr) {
            applyScale(action->data().toInt());
        }
    });
    scaleMenu->addSeparator();
    m_scaleReadoutAction = scaleMenu->addItem(tr("Current: %1%").arg(qRound(m_scalePercent)));
    m_scaleReadoutAction->setObjectName(QStringLiteral("screenshotPinnedScaleReadoutAction"));
    m_scaleReadoutAction->setEnabled(false);

    m_contextMenu->addSeparator();

    m_thumbnailAction = m_contextMenu->addItem(tr("Thumbnail Mode"), outlined_icons::Compress());
    setActionTranslationSource(m_thumbnailAction, "Thumbnail Mode");
    m_thumbnailAction->setObjectName(QStringLiteral("screenshotPinnedThumbnailAction"));
    m_thumbnailAction->setCheckable(true);
    connect(m_thumbnailAction, &QAction::toggled, this,
            [this](bool enabled) { setThumbnailMode(enabled); });

    auto* focusMenu = m_contextMenu->addSubMenu(tr("Focus Mode"), outlined_icons::Eye());
    setActionTranslationSource(focusMenu->menuAction(), "Focus Mode");
    focusMenu->setObjectName(QStringLiteral("screenshotPinnedFocusMenu"));
    QAction* showAllWindows = focusMenu->addItem(tr("Show All Windows"), outlined_icons::Expand());
    setActionTranslationSource(showAllWindows, "Show All Windows");
    connect(showAllWindows, &QAction::triggered, this,
            &ScreenshotPinnedWindow::showAllPinnedWindows);
    QAction* hideOtherWindows =
        focusMenu->addItem(tr("Hide Other Windows"), outlined_icons::EyeInvisible());
    setActionTranslationSource(hideOtherWindows, "Hide Other Windows");
    connect(hideOtherWindows, &QAction::triggered, this,
            &ScreenshotPinnedWindow::hideOtherPinnedWindows);
    QAction* closeOtherWindows =
        focusMenu->addItem(tr("Close Other Windows"), outlined_icons::Close());
    setActionTranslationSource(closeOtherWindows, "Close Other Windows");
    connect(closeOtherWindows, &QAction::triggered, this,
            &ScreenshotPinnedWindow::closeOtherPinnedWindows);
    QAction* closeAll = focusMenu->addItem(tr("Close All Windows"), outlined_icons::CloseCircle());
    setActionTranslationSource(closeAll, "Close All Windows");
    focusMenu->setActionDanger(closeAll);
    connect(closeAll, &QAction::triggered, this, &ScreenshotPinnedWindow::closeAllPinnedWindows);

    m_contextMenu->addSeparator();
    m_showMainInterfaceAction =
        m_contextMenu->addItem(tr("Show Main Interface"), custom_outlined_icons::Window());
    setActionTranslationSource(m_showMainInterfaceAction, "Show Main Interface");
    m_showMainInterfaceAction->setObjectName(
        QStringLiteral("screenshotPinnedShowMainInterfaceAction"));
    connect(m_showMainInterfaceAction, &QAction::triggered, this,
            &ScreenshotPinnedWindow::showMainWindowRequested);
    m_closeAction = m_contextMenu->addItem(tr("Close"), outlined_icons::Close());
    setActionTranslationSource(m_closeAction, "Close");
    m_closeAction->setObjectName(QStringLiteral("screenshotPinnedCloseAction"));
    m_contextMenu->setActionDanger(m_closeAction);
    connect(m_closeAction, &QAction::triggered, this, &QWidget::close);
    updateShowMainInterfaceAction();
    connect(m_contextMenu, &QMenu::aboutToShow, this, &ScreenshotPinnedWindow::refreshContextMenu);
}

void ScreenshotPinnedWindow::applyRuntimeBorderColor() {
    if (m_borderFrame == nullptr) {
        return;
    }
    const QColor color = configuredPinnedBorderColor();
    m_borderFrame->setProperty("borderColor", color);
    m_borderFrame->setStyleSheet(QStringLiteral("QFrame#screenshotPinnedBorder { "
                                                "border: 2px solid rgba(%1, %2, %3, %4); "
                                                "background: transparent; }")
                                     .arg(color.red())
                                     .arg(color.green())
                                     .arg(color.blue())
                                     .arg(color.alpha()));
    m_borderFrame->update();
}

void ScreenshotPinnedWindow::updateShowMainInterfaceAction() {
    if (m_contextMenu == nullptr || m_showMainInterfaceAction == nullptr ||
        m_closeAction == nullptr) {
        return;
    }
    const bool containsAction = m_contextMenu->actions().contains(m_showMainInterfaceAction);
    const bool shouldShowFallback = !configuredTrayEnabled() || !trayMenuShowsMainInterface();
    if (shouldShowFallback && !containsAction) {
        m_contextMenu->insertAction(m_closeAction, m_showMainInterfaceAction);
    } else if (!shouldShowFallback && containsAction) {
        m_contextMenu->removeAction(m_showMainInterfaceAction);
    }
}

void ScreenshotPinnedWindow::refreshContextMenu() {
    updateShowMainInterfaceAction();
    if (m_ocrAction != nullptr) {
        const bool activeText =
            m_recognitionSession != nullptr && m_recognitionSession->active() &&
            m_recognitionSession->mode() == ScreenshotRecognitionSessionController::Mode::Text;
        setActionDisplayText(m_ocrAction,
                             !m_ocrSupported
                                 ? tr(kOcrTooLargeDescription)
                                 : (m_recognitionSession != nullptr &&
                                            m_recognitionSession->busy(
                                                ScreenshotRecognitionSessionController::Mode::Text)
                                        ? tr("Recognizing text")
                                        : tr("Display Text Recognition Results")));
        m_ocrAction->setEnabled(m_formattedTextAvailable ||
                                (m_ocrSupported && m_recognition != nullptr));
        const QSignalBlocker blocker(m_ocrAction);
        m_ocrAction->setChecked(activeText);
    }
    if (m_drawingAction != nullptr) {
        m_drawingAction->setEnabled(m_editingEnabled);
        const QSignalBlocker blocker(m_drawingAction);
        m_drawingAction->setChecked(!m_ocrMode && m_editController != nullptr &&
                                    m_editController->editMode());
    }
    if (m_thumbnailAction != nullptr) {
        m_thumbnailAction->setChecked(m_thumbnailMode);
    }
    if (m_opacityActions != nullptr) {
        for (QAction* action : m_opacityActions->actions()) {
            action->setChecked(action->data().toInt() == m_opacityPercent);
        }
    }
    if (m_opacityReadoutAction != nullptr) {
        m_opacityReadoutAction->setText(tr("Current: %1%").arg(m_opacityPercent));
    }
    if (m_scaleActions != nullptr) {
        for (QAction* action : m_scaleActions->actions()) {
            action->setChecked(qAbs(action->data().toDouble() - m_scalePercent) < 0.001);
        }
    }
    if (m_scaleMenuAction != nullptr) {
        m_scaleMenuAction->setEnabled(!m_ocrMode);
    }
    if (m_scaleReadoutAction != nullptr) {
        m_scaleReadoutAction->setText(tr("Current: %1%").arg(qRound(m_scalePercent)));
    }
}

void ScreenshotPinnedWindow::setRuntimeBorderColor(const QColor& color) {
    configuredPinnedBorderColor() = color.isValid() ? color : kDefaultPinnedBorderColor;
    const auto windows = livePinnedWindows();
    for (const QPointer<ScreenshotPinnedWindow>& window : windows) {
        if (window != nullptr) {
            window->applyRuntimeBorderColor();
        }
    }
}

QColor ScreenshotPinnedWindow::runtimeBorderColor() {
    return configuredPinnedBorderColor();
}

void ScreenshotPinnedWindow::setRuntimeTrayEnabled(bool enabled) {
    configuredTrayEnabled() = enabled;
    const auto windows = livePinnedWindows();
    for (const QPointer<ScreenshotPinnedWindow>& window : windows) {
        if (window != nullptr) {
            window->updateShowMainInterfaceAction();
        }
    }
}

bool ScreenshotPinnedWindow::runtimeTrayEnabled() {
    return configuredTrayEnabled();
}

void ScreenshotPinnedWindow::showContextMenu(const QPoint& globalPosition) {
    if (m_contextMenu == nullptr || m_closing) {
        return;
    }
    refreshContextMenu();
    m_contextMenu->popupAt(globalPosition);
}

void ScreenshotPinnedWindow::updateCanvasViewport() {
    if (m_canvas == nullptr || !m_resultSurfaceCanvasRect.isValid() ||
        m_resultSurfaceCanvasRect.isEmpty() || m_canvas->width() <= 0 || m_canvas->height() <= 0) {
        return;
    }

    const QRect nativeGeometry = currentNativeGeometry();
    const qreal devicePixelRatio =
        m_canvas->devicePixelRatioF() > 0.0 ? m_canvas->devicePixelRatioF() : 1.0;
    const QSize physicalViewport = nativeGeometry.isValid() && !nativeGeometry.isEmpty()
                                       ? nativeGeometry.size()
                                       : QSize(qRound(m_canvas->width() * devicePixelRatio),
                                               qRound(m_canvas->height() * devicePixelRatio));
    m_screenshotRenderer->setImageViewportPhysicalSize({});
    const double zoomX = static_cast<double>(physicalViewport.width()) /
                         (devicePixelRatio * m_resultSurfaceCanvasRect.width());
    const double zoomY = static_cast<double>(physicalViewport.height()) /
                         (devicePixelRatio * m_resultSurfaceCanvasRect.height());
    const double zoom = qFuzzyCompare(zoomX, zoomY) ? zoomX : std::min(zoomX, zoomY);
    m_viewportZoom = zoom > 0.0 ? zoom : 1.0;
    m_viewportCenter = m_resultSurfaceCanvasRect.center();
    m_canvas->setViewportCamera(m_viewportCenter.x(), m_viewportCenter.y(), m_viewportZoom);
}

void ScreenshotPinnedWindow::updateControlsGeometry() {
    if (m_scaleLabel != nullptr) {
        m_scaleLabel->adjustSize();
        m_scaleLabel->move(kScaleReadoutInset,
                           std::max(0, height() - m_scaleLabel->height() - kScaleReadoutInset));
        m_scaleLabel->raise();
    }
    if (m_controlsPanel == nullptr) {
        return;
    }
    const bool editing = m_editController != nullptr && m_editController->editMode();
    const QSize nativeSize = currentNativeGeometry().size();
    const bool tooSmallForControls = !nativeSize.isValid() || nativeSize.isEmpty() ||
                                     nativeSize.width() < kControlsMinimumNativeDimension ||
                                     nativeSize.height() < kControlsMinimumNativeDimension;
    m_controlsPanel->adjustSize();
    const QSize panelSize = m_controlsPanel->sizeHint();
    m_controlsPanel->resize(panelSize);
    m_controlsPanel->move(std::max(0, width() - panelSize.width() - kControlsInset),
                          kControlsInset);
    const bool controlsVisible =
        m_pointerInside && !m_thumbnailMode && !editing && !tooSmallForControls;
    m_controlsPanel->setVisible(controlsVisible);
    if (!controlsVisible) {
        return;
    }
    m_controlsPanel->raise();
    if (m_scaleLabel != nullptr) {
        m_scaleLabel->raise();
    }
}

void ScreenshotPinnedWindow::destroyCanvas() {
    if (m_canvas == nullptr) {
        return;
    }

    SnowCanvasWidget* canvas = m_canvas;
    m_canvas = nullptr;
    canvas->removeEventFilter(this);
    if (canvas->customRenderer() == m_screenshotRenderer.get()) {
        canvas->setCustomRenderer(nullptr);
    }
    QObject::disconnect(canvas, nullptr, this, nullptr);
    canvas->setParent(nullptr);
    delete canvas;
    m_screenshotRenderer.reset();
}

void ScreenshotPinnedWindow::requestMaterializedImage(MaterializationCallback callback) {
    if (!callback) {
        return;
    }
    if (m_imageLoader) {
        m_materializationCallbacks.push_back(std::move(callback));
        if (m_materializationLoading) {
            SNOW_SHOT_PIN_PERF_COUNTER("materialization.coalesced", 1);
            return;
        }
        m_materializationLoading = true;
        const QPointer<ScreenshotPinnedWindow> receiver(this);
        m_imageLoader(this, [receiver](QImage image) {
            if (receiver.isNull() || !receiver->m_materializationLoading) {
                return;
            }
            ScreenshotExportTaskResult result;
            if (image.isNull()) {
                result = ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Render,
                    QStringLiteral("The pinned image could not be materialized"));
            } else {
                result.image = std::move(image);
            }
            receiver->finishMaterializedImage(std::move(result));
        });
        return;
    }
    if (!m_originalImage.isNull()) {
        QTimer::singleShot(0, this, [callback = std::move(callback)]() mutable { callback(true); });
        return;
    }
    if (!m_imageSource.isValid() || !m_originalPixelSize.isValid() ||
        m_originalPixelSize.isEmpty()) {
        SNOW_SHOT_PIN_PERF_COUNTER("materialization.failure", 1);
        QTimer::singleShot(0, this,
                           [callback = std::move(callback)]() mutable { callback(false); });
        return;
    }

    m_materializationCallbacks.push_back(std::move(callback));
    if (m_materializationJob.isValid()) {
        SNOW_SHOT_PIN_PERF_COUNTER("materialization.coalesced", 1);
        return;
    }

    const ScreenshotImageSource source = m_imageSource;
    const QRectF canvasRect = m_canvasSourceRect;
    const QSize pixelSize = m_originalPixelSize;
    const QPointer<ScreenshotPinnedWindow> receiver(this);
    m_materializationJob = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [source, canvasRect, pixelSize](const ScreenshotExportCancellation& cancellation) mutable {
            if (cancellation.isCancellationRequested()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Cancelled,
                    QStringLiteral("The pinned image materialization was cancelled"));
            }
            QImage image = materializeScreenshotImageSource(source, canvasRect, pixelSize);
            if (image.isNull()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Render,
                    QStringLiteral("The pinned image could not be materialized"));
            }
            ScreenshotExportTaskResult result;
            result.image = std::move(image);
            return result;
        },
        [receiver](ScreenshotExportTaskResult result) mutable {
            if (!receiver.isNull()) {
                receiver->finishMaterializedImage(std::move(result));
            }
        });
    if (!m_materializationJob.isValid()) {
        finishMaterializedImage(ScreenshotExportTaskResult::failure(
            ScreenshotExportFailureStage::Queue,
            QStringLiteral("The image processing queue is full")));
    }
}

void ScreenshotPinnedWindow::finishMaterializedImage(ScreenshotExportTaskResult result) {
    m_materializationJob = {};
    m_materializationLoading = false;
    if (m_closing) {
        m_imageLoader = {};
        m_materializationCallbacks.clear();
        return;
    }
    const bool succeeded = result.succeeded() && !result.image.isNull();
    if (succeeded) {
        SNOW_SHOT_PIN_PERF_SCOPE("window.materialize_image");
        m_imageLoader = {};
        m_originalImage = std::move(result.image);
        m_originalImage.setDevicePixelRatio(1.0);
        m_originalPixelSize = m_originalImage.size();
        m_ocrSupported = screenshotOcrImageWithinPixelLimit(m_originalPixelSize);
        m_transformedImage = ScreenshotResultCompositor::normalizeImage(m_originalImage);
        m_imageSource =
            ScreenshotImageSource::fromImage(m_transformedImage, m_backgroundCanvasRect);
        if (m_screenshotRenderer != nullptr) {
            m_screenshotRenderer->setImageSource(m_imageSource);
        }
        if (m_recognitionSession != nullptr) {
            m_recognitionSession->setTarget(ScreenshotRecognitionTarget{
                !m_recognitionResults.isEmpty()
                    ? m_recognitionResults.key
                    : QStringLiteral("pinned:%1").arg(reinterpret_cast<quintptr>(this)),
                m_originalImage,
                m_canvasSourceRect, m_formattedTextDocument, m_formattedPlainText});
            m_recognitionSession->seedRecognitionResults(m_recognitionResults);
            m_ocrReady = m_recognitionSession->hasTextResult();
        }
        SNOW_SHOT_PIN_PERF_COUNTER("materialization.count", 1);
        SNOW_SHOT_PIN_PERF_COUNTER("materialization.bytes", m_originalImage.sizeInBytes());
    } else {
        SNOW_SHOT_PIN_PERF_COUNTER("materialization.failure", 1);
    }

    std::vector<MaterializationCallback> callbacks = std::move(m_materializationCallbacks);
    m_materializationCallbacks.clear();
    for (MaterializationCallback& callback : callbacks) {
        if (callback) {
            callback(succeeded);
        }
    }
}

void ScreenshotPinnedWindow::finishPresentation(bool succeeded, QImage image) {
    if (!m_presentationCompletion) {
        return;
    }
    auto completion = std::move(m_presentationCompletion);
    completion(succeeded, std::move(image));
}

void ScreenshotPinnedWindow::ensureEditController() {
    if (m_editController != nullptr || m_canvas == nullptr || m_closing) {
        return;
    }

    m_editController =
        new ScreenshotPinnedEditController(*this, *m_canvas, *m_shortcutManager, this);
    connect(m_editController, &ScreenshotPinnedEditController::editModeChanged, this,
            [this](bool enabled) {
                if (m_drawingAction != nullptr) {
                    const QSignalBlocker blocker(m_drawingAction);
                    m_drawingAction->setChecked(enabled && !m_ocrMode);
                }
                updateControlsGeometry();
            });

    if (m_editController->toolbarWindow() != nullptr) {
        if (ScreenshotToolPalette* toolbar = m_editController->toolbarWindow()->palette()) {
            connect(toolbar, &ScreenshotToolPalette::saveRequested, this,
                    &ScreenshotPinnedWindow::saveAsFile);
            connect(toolbar, &ScreenshotToolPalette::copyRequested, this,
                    &ScreenshotPinnedWindow::copyEditToolbarContent);
        }
    }

    if (m_recognitionSession != nullptr && m_editController->toolbarWindow() != nullptr) {
        connect(m_editController, &ScreenshotPinnedEditController::textRecognitionRequested, this,
                [this]() {
                    m_translateAfterRecognition = false;
                    activateRecognitionMode(
                        static_cast<int>(ScreenshotRecognitionSessionController::Mode::Text));
                });
        connect(m_editController, &ScreenshotPinnedEditController::tableRecognitionRequested, this,
                [this]() {
                    m_translateAfterRecognition = false;
                    activateRecognitionMode(
                        static_cast<int>(ScreenshotRecognitionSessionController::Mode::Table));
                });
        connect(m_editController, &ScreenshotPinnedEditController::qrRecognitionRequested, this,
                [this]() {
                    m_translateAfterRecognition = false;
                    activateRecognitionMode(
                        static_cast<int>(ScreenshotRecognitionSessionController::Mode::Qr));
                });
        connect(m_editController, &ScreenshotPinnedEditController::textTranslationRequested, this,
                &ScreenshotPinnedWindow::activateTextTranslation);
        if (ScreenshotToolPalette* toolbar = m_editController->toolbarWindow()->palette()) {
            connect(toolbar, &ScreenshotToolPalette::textEditRequested, this,
                    &ScreenshotPinnedWindow::handleTextEditingRequested);
            connect(toolbar, &ScreenshotToolPalette::textTranslateRequested, this,
                    &ScreenshotPinnedWindow::handleTextTranslationRequested);
            connect(toolbar, &ScreenshotToolPalette::textResetRequested, this,
                    &ScreenshotPinnedWindow::handleTextResetRequested);
            connect(toolbar, &ScreenshotToolPalette::textSettingsRequested, this,
                    &ScreenshotPinnedWindow::handleTextSettingsRequested);
            connect(toolbar, &ScreenshotToolPalette::textFormattingRequested, this,
                    &ScreenshotPinnedWindow::handleTextFormattingRequested);
            connect(toolbar, &ScreenshotToolPalette::textPunctuationRequested, this,
                    &ScreenshotPinnedWindow::handleTextPunctuationRequested);
            connect(toolbar, &ScreenshotToolPalette::tableMergeRequested, this,
                    &ScreenshotPinnedWindow::handleTableMergeRequested);
            connect(toolbar, &ScreenshotToolPalette::tableSplitRequested, this,
                    &ScreenshotPinnedWindow::handleTableSplitRequested);
            connect(toolbar, &ScreenshotToolPalette::tableResetRequested, this,
                    &ScreenshotPinnedWindow::handleTableResetRequested);
            const auto leaveRecognition = [this]() { deactivateRecognition(); };
            connect(toolbar, &ScreenshotToolPalette::selectRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::shapeRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::arrowRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::lineRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::freeDrawRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::highlightRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::penHighlightRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::spotlightRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::eraserRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::filterRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::rectangleFilterRequested, this,
                    leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::penFilterRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::watermarkRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::textRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::serialNumberRequested, this, leaveRecognition);
            connect(toolbar, &ScreenshotToolPalette::confirmRequested, this, leaveRecognition);
            toolbar->setOcrEnabled(m_formattedTextAvailable ||
                                   (m_ocrSupported && m_recognition != nullptr));
            toolbar->setTableEnabled(m_ocrSupported && m_tableRecognition != nullptr);
            toolbar->setQrEnabled(m_ocrSupported && m_qrRecognition != nullptr);
        }
    }

    m_editController->updatePlacement();
    updateRecognitionToolbarState();
    SNOW_SHOT_PIN_PERF_MILESTONE("window.edit_controller_created");
}

void ScreenshotPinnedWindow::setEditMode(bool enabled) {
    if (enabled) {
        ensureEditController();
    }
    if (m_closing || m_editController == nullptr) {
        return;
    }
    if (enabled) {
        static_cast<void>(finishNativeGeometryInteraction());
        finishWindowMove();
        clearWindowDragCursor();
    }
    if (enabled && m_thumbnailMode) {
        setThumbnailMode(false);
        QVariantAnimation* restorationAnimation = m_geometryAnimation;
        if (restorationAnimation != nullptr &&
            restorationAnimation->state() == QAbstractAnimation::Running) {
            connect(
                restorationAnimation, &QVariantAnimation::finished, this,
                [this, restorationAnimation]() {
                    if (!m_closing && !m_thumbnailMode &&
                        m_geometryAnimation == restorationAnimation) {
                        setEditMode(true);
                    }
                },
                Qt::SingleShotConnection);
        } else {
            setEditMode(true);
        }
        return;
    }
    m_editController->setEditMode(enabled);
    if (m_drawingAction != nullptr && m_drawingAction->isChecked() != enabled) {
        m_drawingAction->setChecked(enabled);
    }
    updateControlsGeometry();
    refreshContextMenu();
    if (!enabled) {
        updateWindowDragCursor(mapFromGlobal(QCursor::pos()));
    }
}

void ScreenshotPinnedWindow::setOcrMode(bool enabled) {
    if (enabled) {
        setEditMode(true);
        activateRecognitionMode(
            static_cast<int>(ScreenshotRecognitionSessionController::Mode::Text));
    } else {
        deactivateRecognition();
    }
}

void ScreenshotPinnedWindow::updateRecognitionContentGeometry() {
    if (m_recognitionContent == nullptr || m_canvas == nullptr) {
        return;
    }
    m_recognitionContent->setGeometry(m_canvas->geometry());
    m_recognitionContent->raise();
    if (m_borderFrame != nullptr) {
        m_borderFrame->raise();
    }
}

void ScreenshotPinnedWindow::activateRecognitionMode(int mode) {
    if (m_closing || m_recognitionSession == nullptr) {
        return;
    }
    const auto selectedMode = static_cast<ScreenshotRecognitionSessionController::Mode>(mode);
    const bool canUseFormattedText =
        selectedMode == ScreenshotRecognitionSessionController::Mode::Text &&
        m_formattedTextAvailable;
    if (!m_ocrSupported && !canUseFormattedText) {
        return;
    }
    if (m_originalImage.isNull()) {
        requestMaterializedImage([this, mode](bool succeeded) {
            if (succeeded && !m_closing) {
                activateRecognitionMode(mode);
            } else if (!succeeded) {
                showPinnedRecognitionMessage(
                    this, translatePinnedText("The pinned image could not be prepared"), true);
            }
        });
        return;
    }
    ensureEditController();
    if (m_editController == nullptr) {
        return;
    }
    if (!m_editController->editMode()) {
        setEditMode(true);
    }
    m_recognitionSession->activate(selectedMode);
    refreshContextMenu();
}

void ScreenshotPinnedWindow::deactivateRecognition() {
    m_translateAfterRecognition = false;
    if (m_recognitionSession != nullptr) {
        m_recognitionSession->deactivate();
    }
    m_ocrMode = false;
    refreshContextMenu();
}

void ScreenshotPinnedWindow::updateRecognitionToolbarState() {
    if (m_recognitionSession == nullptr || m_editController == nullptr ||
        m_editController->toolbarWindow() == nullptr) {
        return;
    }
    if (ScreenshotToolPalette* toolbar = m_editController->toolbarWindow()->palette()) {
        toolbar->setOcrEnabled(m_formattedTextAvailable ||
                               (m_ocrSupported && m_recognition != nullptr));
        toolbar->setTableEnabled(m_ocrSupported && m_tableRecognition != nullptr);
        toolbar->setQrEnabled(m_ocrSupported && m_qrRecognition != nullptr);
        toolbar->setOcrBusy(
            m_recognitionSession->busy(ScreenshotRecognitionSessionController::Mode::Text));
        toolbar->setTableBusy(
            m_recognitionSession->busy(ScreenshotRecognitionSessionController::Mode::Table));
        toolbar->setQrBusy(
            m_recognitionSession->busy(ScreenshotRecognitionSessionController::Mode::Qr));
    }
}

void ScreenshotPinnedWindow::handleTextEditingRequested() {
    if (m_recognitionSession != nullptr) {
        if (m_recognitionSession->translating()) {
            m_recognitionSession->endTextEditing();
            m_recognitionSession->beginTextEditing();
        } else if (m_recognitionSession->editing()) {
            m_recognitionSession->endTextEditing();
        } else {
            m_recognitionSession->beginTextEditing();
        }
    }
}

void ScreenshotPinnedWindow::handleTextTranslationRequested() {
    if (m_recognitionSession != nullptr) {
        if (m_recognitionSession->translating()) {
            m_recognitionSession->endTextEditing();
        } else {
            m_recognitionSession->beginTextTranslation();
        }
    }
}

void ScreenshotPinnedWindow::handleTextResetRequested() {
    if (m_recognitionSession != nullptr) {
        m_recognitionSession->resetTextEditing();
    }
}

void ScreenshotPinnedWindow::handleTextSettingsRequested() {
    if (m_recognitionSession != nullptr) {
        m_recognitionSession->openTranslationSettings();
    }
}

void ScreenshotPinnedWindow::handleTextFormattingRequested(const QString& value) {
    if (m_recognitionSession != nullptr) {
        m_recognitionSession->applyTextFormatting(value);
    }
}

void ScreenshotPinnedWindow::handleTextPunctuationRequested(const QString& value) {
    if (m_recognitionSession != nullptr) {
        m_recognitionSession->applyTextPunctuation(value);
    }
}

void ScreenshotPinnedWindow::handleTableMergeRequested() {
    if (m_recognitionSession != nullptr) {
        m_recognitionSession->mergeTableSelection();
    }
}

void ScreenshotPinnedWindow::handleTableSplitRequested() {
    if (m_recognitionSession != nullptr) {
        m_recognitionSession->splitTableSelection();
    }
}

void ScreenshotPinnedWindow::handleTableResetRequested() {
    if (m_recognitionSession != nullptr) {
        m_recognitionSession->resetTable();
    }
}

void ScreenshotPinnedWindow::stopRecognition() {
    if (m_recognitionSession != nullptr) {
        m_recognitionSession->invalidate();
    }
}

void ScreenshotPinnedWindow::updateOcrPresentation() {
    if (!m_ocrReady || m_originalOcrPresentation == nullptr || m_screenshotRenderer == nullptr) {
        return;
    }
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = m_backgroundCanvasRect.toAlignedRect();
    const qreal originalScaleX = m_canvasSourceRect.width() > 0.0
                                     ? m_originalImage.width() / m_canvasSourceRect.width()
                                     : 1.0;
    const qreal originalScaleY = m_canvasSourceRect.height() > 0.0
                                     ? m_originalImage.height() / m_canvasSourceRect.height()
                                     : 1.0;
    const qreal transformedScaleX =
        m_transformedImage.width() > 0 ? m_backgroundCanvasRect.width() / m_transformedImage.width()
                                       : 1.0;
    const qreal transformedScaleY =
        m_transformedImage.height() > 0
            ? m_backgroundCanvasRect.height() / m_transformedImage.height()
            : 1.0;
    presentation->lines.reserve(m_originalOcrPresentation->lines.size());
    for (const ScreenshotOcrLine& originalLine : m_originalOcrPresentation->lines) {
        ScreenshotOcrLine line = originalLine;
        line.quad.clear();
        line.quad.reserve(originalLine.quad.size());
        for (const QPointF& point : originalLine.quad) {
            const QPointF imagePoint((point.x() - m_canvasSourceRect.left()) * originalScaleX,
                                     (point.y() - m_canvasSourceRect.top()) * originalScaleY);
            const QPointF transformed = m_imageTransform.map(imagePoint);
            line.quad.push_back(
                QPointF(m_backgroundCanvasRect.left() + transformed.x() * transformedScaleX,
                        m_backgroundCanvasRect.top() + transformed.y() * transformedScaleY));
        }
        presentation->lines.push_back(std::move(line));
    }
    presentation->prepareForRendering();
    m_displayOcrPresentation = std::move(presentation);
    if (m_ocrMode) {
        // The embedded recognition window owns the translucent OCR text layer
        // for pinned windows. Keep the canvas responsible for the immutable
        // screenshot and recognition fill only; installing another translucent
        // graphics-view child on the pinned canvas breaks the layered-window
        // backing surface on Windows and makes the fill disappear.
        m_screenshotRenderer->setOcrPresentation(
            m_displayOcrPresentation,
            ScreenshotCanvasRenderer::OcrPresentationMode::BackgroundOnly);
        if (m_recognitionContent != nullptr) {
            m_recognitionContent->setOcrPresentation(m_displayOcrPresentation);
            m_recognitionContent->raise();
            if (m_borderFrame != nullptr) {
                m_borderFrame->raise();
            }
        }
    }
}

QPointF ScreenshotPinnedWindow::canvasPositionForViewPosition(const QPointF& position) const {
    if (m_canvas == nullptr || m_canvas->width() <= 0 || m_canvas->height() <= 0 ||
        m_viewportZoom <= 0.0) {
        return {};
    }
    return m_viewportCenter + QPointF((position.x() - m_canvas->width() / 2.0) / m_viewportZoom,
                                      (position.y() - m_canvas->height() / 2.0) / m_viewportZoom);
}

QRectF ScreenshotPinnedWindow::visibleCanvasRect() const {
    if (m_canvas == nullptr || m_viewportZoom <= 0.0) {
        return m_backgroundCanvasRect;
    }
    const QRect nativeGeometry = currentNativeGeometry();
    const qreal devicePixelRatio =
        m_canvas->devicePixelRatioF() > 0.0 ? m_canvas->devicePixelRatioF() : 1.0;
    const QSizeF physicalViewport =
        nativeGeometry.isValid() && !nativeGeometry.isEmpty()
            ? QSizeF(nativeGeometry.size())
            : QSizeF(m_canvas->width() * devicePixelRatio, m_canvas->height() * devicePixelRatio);
    const QSizeF visibleSize(physicalViewport.width() / (devicePixelRatio * m_viewportZoom),
                             physicalViewport.height() / (devicePixelRatio * m_viewportZoom));
    const QPointF topLeft = m_viewportCenter - QPointF(m_canvas->width() / (2.0 * m_viewportZoom),
                                                       m_canvas->height() / (2.0 * m_viewportZoom));
    return QRectF(topLeft, visibleSize);
}

void ScreenshotPinnedWindow::copyEditToolbarContent() {
    if (m_recognitionSession == nullptr || !m_recognitionSession->active()) {
        copyCurrentViewport();
        return;
    }
    invalidatePendingCopy();
    if (m_recognitionSession->tableModeActive() && m_recognitionContent != nullptr) {
        m_recognitionContent->commitActiveTableEdit();
    }
    std::unique_ptr<QMimeData> mimeData =
        m_recognitionSession->recognitionClipboardMimeData(m_displayOcrPresentation.get());
    QClipboard* clipboard = QApplication::clipboard();
    if (mimeData == nullptr || clipboard == nullptr) {
        showPinnedRecognitionMessage(
            this,
            QCoreApplication::translate("ScreenshotController",
                                        "No recognized result is available to copy"),
            true);
        return;
    }
    clipboard->setMimeData(mimeData.release(), QClipboard::Clipboard);
}

void ScreenshotPinnedWindow::copyCurrentViewport() {
    if (m_ocrMode && m_recognitionSession != nullptr && m_recognitionSession->active()) {
        copyEditToolbarContent();
        return;
    }
    if (m_copyService == nullptr) {
        return;
    }
    if (m_originalImage.isNull()) {
        requestMaterializedImage([this](bool succeeded) {
            if (succeeded && !m_closing) {
                copyCurrentViewport();
            } else if (!succeeded) {
                showPinnedRecognitionMessage(
                    this, translatePinnedText("The pinned image could not be prepared"), true);
            }
        });
        return;
    }
    if (m_transformedImage.isNull()) {
        return;
    }
    if (m_resultSurfaceCanvasRect.isEmpty()) {
        return;
    }
    const double surfaceScale =
        m_thumbnailMode
            ? std::min(static_cast<double>(width()) / m_resultSurfaceCanvasRect.width(),
                       static_cast<double>(height()) / m_resultSurfaceCanvasRect.height())
            : m_scalePercent / 100.0;
    if (!(surfaceScale > 0.0)) {
        return;
    }
    const QSize contentPixelSize(
        std::max(1, qRound(m_backgroundCanvasRect.width() * surfaceScale)),
        std::max(1, qRound(m_backgroundCanvasRect.height() * surfaceScale)));
    if (m_canvas != nullptr && !m_canvas->resetEditingState()) {
        return;
    }
    QByteArray documentSession = m_runtime.serializeDocumentSession();
    ScreenshotResultStyle scaledStyle = m_resultStyle;
    scaledStyle.cornerRadius = qRound(scaledStyle.cornerRadius * surfaceScale);
    scaledStyle.shadowWidth = qRound(scaledStyle.shadowWidth * surfaceScale);
    ScreenshotPinnedViewportCopyRequest request{
        std::move(documentSession), m_transformedImage, m_backgroundCanvasRect,
        contentPixelSize,           scaledStyle,
    };
    if (!m_copyService->requestCurrentViewport(std::move(request), this,
                                               [this](ScreenshotClipboardPayload payload) {
                                                   commitClipboardPayload(std::move(payload));
                                               })) {
        showPinnedRecognitionMessage(
            this, translatePinnedText("The pinned image copy could not be started"), true);
    }
}

void ScreenshotPinnedWindow::activateTextTranslation() {
    if (m_closing || m_recognitionSession == nullptr ||
        (!m_ocrSupported && !m_formattedTextAvailable)) {
        m_translateAfterRecognition = false;
        return;
    }
    m_translateAfterRecognition = true;
    activateRecognitionMode(static_cast<int>(ScreenshotRecognitionSessionController::Mode::Text));
    if (m_editController != nullptr && m_editController->toolbarHost() != nullptr) {
        m_editController->toolbarHost()->setActiveTool(
            ScreenshotToolPalette::Tool::TextTranslation);
    }
    if (m_translateAfterRecognition && m_recognitionSession->active() &&
        m_recognitionSession->mode() == ScreenshotRecognitionSessionController::Mode::Text &&
        m_recognitionSession->hasTextResult()) {
        m_translateAfterRecognition = false;
        m_recognitionSession->beginTextTranslation();
    }
}

void ScreenshotPinnedWindow::copyOriginalContent() {
    if (!m_originalClipboardContent.isEmpty()) {
        invalidatePendingCopy();
        auto* mimeData = new QMimeData();
        if (!m_originalClipboardContent.html.isEmpty()) {
            mimeData->setHtml(m_originalClipboardContent.html);
        }
        if (!m_originalClipboardContent.text.isEmpty()) {
            mimeData->setText(m_originalClipboardContent.text);
        }
        QApplication::clipboard()->setMimeData(mimeData, QClipboard::Clipboard);
        return;
    }
    if (m_copyService == nullptr) {
        return;
    }
    if (m_originalImage.isNull()) {
        requestMaterializedImage([this](bool succeeded) {
            if (succeeded && !m_closing) {
                copyOriginalContent();
            } else if (!succeeded) {
                showPinnedRecognitionMessage(
                    this, translatePinnedText("The pinned image could not be prepared"), true);
            }
        });
        return;
    }
    if (!m_copyService->requestOriginalImage(m_originalImage, this,
                                             [this](ScreenshotClipboardPayload payload) {
                                                 commitClipboardPayload(std::move(payload));
                                             })) {
        showPinnedRecognitionMessage(
            this, translatePinnedText("The pinned image copy could not be started"), true);
    }
}

void ScreenshotPinnedWindow::saveAsFile() {
    if (m_copyService == nullptr) {
        return;
    }
    if (m_originalImage.isNull()) {
        requestMaterializedImage([this](bool succeeded) {
            if (succeeded && !m_closing) {
                saveAsFile();
            } else if (!succeeded) {
                showPinnedRecognitionMessage(
                    this, translatePinnedText("The pinned image could not be prepared"), true);
            }
        });
        return;
    }
    if (m_transformedImage.isNull() || m_backgroundCanvasRect.isEmpty()) {
        return;
    }

    const snow_shot::storage::ScreenshotSettings outputSettings;
    const QString directory = ScreenshotImageFileService::saveDialogDirectory(
        outputSettings.lastManualSaveDirectory(), outputSettings.imageSaveDirectory());
    static_cast<void>(QDir().mkpath(directory));
    const QString initialPath = QDir(directory).filePath(
        ScreenshotImageFileService::suggestedBaseName(outputSettings.manualSaveFilenameFormat()) +
        QStringLiteral(".png"));
    QString selectedFilter =
        ScreenshotImageFileService::dialogFilter(ScreenshotImageFileFormat::Png);
    const QString selectedPath = QFileDialog::getSaveFileName(
        this, translatePinnedText("Save as File"), initialPath,
        ScreenshotImageFileService::saveDialogFilter(), &selectedFilter);
    if (selectedPath.isEmpty()) {
        return;
    }
    static_cast<void>(
        outputSettings.setLastManualSaveDirectory(QFileInfo(selectedPath).absolutePath()));

    const ScreenshotImageFileFormat format =
        ScreenshotImageFileService::formatForDialogSelection(selectedPath, selectedFilter);
    const QString outputPath = ScreenshotImageFileService::normalizedPath(selectedPath, format);
    if (m_canvas != nullptr && !m_canvas->resetEditingState()) {
        return;
    }
    const QByteArray documentSession = m_runtime.serializeDocumentSession();
    const qreal renderScale = m_backgroundCanvasRect.width() > 0.0
                                  ? m_transformedImage.width() / m_backgroundCanvasRect.width()
                                  : 1.0;
    ScreenshotResultStyle scaledStyle = m_resultStyle;
    scaledStyle.cornerRadius = qRound(scaledStyle.cornerRadius * renderScale);
    scaledStyle.shadowWidth = qRound(scaledStyle.shadowWidth * renderScale);
    ScreenshotPinnedViewportCopyRequest request{
        documentSession,           m_transformedImage, m_backgroundCanvasRect,
        m_transformedImage.size(), scaledStyle,
    };
    if (!m_copyService->requestCurrentImage(
            std::move(request), this, [this, outputPath, format](QImage image) mutable {
                if (image.isNull() || m_closing) {
                    if (!m_closing) {
                        showPinnedRecognitionMessage(
                            this, translatePinnedText("The pinned image could not be prepared"),
                            true);
                    }
                    return;
                }
                m_fileSaveJob.cancel();
                m_fileSaveJob = ScreenshotExportCoordinator::shared().submit(
                    this, ScreenshotExportCoordinator::Priority::Foreground,
                    [image = std::move(image), outputPath,
                     format](const ScreenshotExportCancellation& cancellation) mutable {
                        if (cancellation.isCancellationRequested()) {
                            return ScreenshotExportTaskResult::failure(
                                ScreenshotExportFailureStage::Cancelled,
                                QStringLiteral("The pinned image save was cancelled"));
                        }
                        const ScreenshotImageFileSaveResult saved =
                            ScreenshotImageFileService::write(image, outputPath, format);
                        if (!saved.succeeded()) {
                            return ScreenshotExportTaskResult::failure(
                                ScreenshotExportFailureStage::File, saved.error);
                        }
                        ScreenshotExportTaskResult result;
                        result.savedPath = saved.path;
                        return result;
                    },
                    [this](ScreenshotExportTaskResult result) {
                        m_fileSaveJob = {};
                        if (!result.succeeded() &&
                            result.failureStage != ScreenshotExportFailureStage::Cancelled) {
                            showPinnedRecognitionMessage(
                                this,
                                QCoreApplication::translate("ScreenshotController",
                                                            "The screenshot could not be saved: %1")
                                    .arg(result.error),
                                true);
                        }
                    });
                if (!m_fileSaveJob.isValid()) {
                    m_fileSaveJob = {};
                    showPinnedRecognitionMessage(
                        this,
                        QCoreApplication::translate("ScreenshotController",
                                                    "The screenshot could not be saved: %1")
                            .arg(QStringLiteral("The screenshot export queue is full")),
                        true);
                }
            })) {
        showPinnedRecognitionMessage(
            this, translatePinnedText("The pinned image save could not be started"), true);
    }
}

void ScreenshotPinnedWindow::commitClipboardPayload(ScreenshotClipboardPayload payload) {
    m_clipboardCommit.cancel();
    m_clipboardCommit = ScreenshotClipboardService::commit(
        QApplication::clipboard(), this, std::move(payload),
        [this](ScreenshotClipboardCommitResult result) {
            if (!result.succeeded()) {
                showPinnedRecognitionMessage(
                    this,
                    QCoreApplication::translate("ScreenshotPinnedWindow",
                                                "The pinned image could not be copied: %1")
                        .arg(result.errorString()),
                    true);
            }
        });
    if (!m_clipboardCommit.isValid()) {
        showPinnedRecognitionMessage(
            this, translatePinnedText("The pinned image copy could not be started"), true);
    }
}

void ScreenshotPinnedWindow::invalidatePendingCopy() {
    if (m_copyService != nullptr) {
        m_copyService->invalidate();
    }
    m_clipboardCommit.cancel();
    m_clipboardCommit = {};
}

void ScreenshotPinnedWindow::applyImageOperation(const QTransform& operation,
                                                 int quarterTurnDelta) {
    if (m_originalImage.isNull()) {
        requestMaterializedImage([this, operation, quarterTurnDelta](bool succeeded) {
            if (succeeded && !m_closing) {
                applyImageOperation(operation, quarterTurnDelta);
            }
        });
        return;
    }
    restoreFromThumbnailImmediately();
    const QPoint nativeCenter = currentNativeGeometry().center();
    const QPolygonF sourceQuad({
        QPointF(0.0, 0.0),
        QPointF(m_originalImage.width(), 0.0),
        QPointF(m_originalImage.width(), m_originalImage.height()),
        QPointF(0.0, m_originalImage.height()),
    });
    QPolygonF targetQuad;
    targetQuad.reserve(sourceQuad.size());
    for (const QPointF& point : sourceQuad) {
        targetQuad.push_back(operation.map(m_imageTransform.map(point)));
    }
    QTransform combined;
    if (!QTransform::quadToQuad(sourceQuad, targetQuad, combined)) {
        return;
    }
    m_imageTransform = normalizedImageTransform(combined, m_originalImage.size());
    m_quarterTurns = (m_quarterTurns + quarterTurnDelta) % 4;
    if (m_quarterTurns < 0) {
        m_quarterTurns += 4;
    }
    rebuildTransformedImage();

    if (quarterTurnDelta != 0) {
        QSize nativeSize = orientedInitialPhysicalSize();
        nativeSize = QSize(std::max(1, qRound(nativeSize.width() * m_scalePercent / 100.0)),
                           std::max(1, qRound(nativeSize.height() * m_scalePercent / 100.0)));
        QRect nativeTarget(QPoint(), nativeSize);
        nativeTarget.moveCenter(nativeCenter);
        m_preserveScaleForSettledGeometry = true;
        static_cast<void>(applyWindowGeometry(nativeTarget, GeometryMutation::ImageTransform));
    }
    updateCanvasViewport();
    updateControlsGeometry();
    if (m_editController != nullptr) {
        m_editController->updatePlacement();
    }
}

void ScreenshotPinnedWindow::resetImageTransform() {
    if (m_imageTransform.isIdentity() && m_quarterTurns == 0) {
        return;
    }
    if (m_originalImage.isNull()) {
        requestMaterializedImage([this](bool succeeded) {
            if (succeeded && !m_closing) {
                resetImageTransform();
            }
        });
        return;
    }
    restoreFromThumbnailImmediately();
    const QPoint nativeCenter = currentNativeGeometry().center();
    const bool dimensionsChange = (m_quarterTurns % 2) != 0;
    m_imageTransform.reset();
    m_quarterTurns = 0;
    rebuildTransformedImage();
    if (dimensionsChange) {
        QSize nativeSize(
            std::max(1, qRound(m_initialPhysicalSize.width() * m_scalePercent / 100.0)),
            std::max(1, qRound(m_initialPhysicalSize.height() * m_scalePercent / 100.0)));
        QRect nativeTarget(QPoint(), nativeSize);
        nativeTarget.moveCenter(nativeCenter);
        m_preserveScaleForSettledGeometry = true;
        static_cast<void>(applyWindowGeometry(nativeTarget, GeometryMutation::ImageTransform));
    }
    updateCanvasViewport();
}

void ScreenshotPinnedWindow::rebuildTransformedImage() {
    if (m_originalImage.isNull()) {
        requestMaterializedImage([this](bool succeeded) {
            if (succeeded && !m_closing) {
                rebuildTransformedImage();
            }
        });
        return;
    }
    invalidatePendingCopy();
    m_transformedImage = m_originalImage.transformed(m_imageTransform, Qt::SmoothTransformation);
    m_transformedImage = ScreenshotResultCompositor::normalizeImage(m_transformedImage);
    const qreal scaleX =
        m_originalImage.width() > 0 ? m_canvasSourceRect.width() / m_originalImage.width() : 1.0;
    const qreal scaleY =
        m_originalImage.height() > 0 ? m_canvasSourceRect.height() / m_originalImage.height() : 1.0;
    m_backgroundCanvasRect =
        QRectF(m_canvasSourceRect.topLeft(),
               QSizeF(m_transformedImage.width() * scaleX, m_transformedImage.height() * scaleY));
    const qreal effect = m_resultStyle.shadowWidth;
    m_resultSurfaceCanvasRect = m_backgroundCanvasRect.adjusted(-effect, -effect, effect, effect);
    m_screenshotRenderer->setImage(m_transformedImage, m_backgroundCanvasRect);
    m_screenshotRenderer->setPinnedResultSurface(m_backgroundCanvasRect, m_resultSurfaceCanvasRect,
                                                 m_resultStyle);
    if (m_ocrReady) {
        updateOcrPresentation();
        if (m_ocrMode && m_recognitionSession != nullptr && m_recognitionSession->active() &&
            m_recognitionSession->mode() == ScreenshotRecognitionSessionController::Mode::Text) {
            m_recognitionSession->renderTextBackground();
        }
    }
}

void ScreenshotPinnedWindow::applyScale(int percent) {
    if (m_ocrMode || percent < kMinimumScalePercent || percent > kMaximumScalePercent) {
        return;
    }
    restoreFromThumbnailImmediately();
    QSize nativeSize = orientedInitialPhysicalSize();
    nativeSize = QSize(std::max(1, qRound(nativeSize.width() * percent / 100.0)),
                       std::max(1, qRound(nativeSize.height() * percent / 100.0)));
    const QRect currentGeometry = currentNativeGeometry();
    const QRect nativeTarget(currentGeometry.topLeft(), nativeSize);
    setEffectiveScale(percent, true);
    if (nativeTarget != currentGeometry) {
        // The native size is integer-valued and generally cannot encode the
        // requested percentage exactly. Do not turn that pixel rounding back
        // into a different scale when the resize event settles.
        m_preserveScaleForSettledGeometry = true;
    }
    static_cast<void>(applyWindowGeometry(nativeTarget, GeometryMutation::Scale));
    updateCanvasViewport();
    updateControlsGeometry();
    if (m_editController != nullptr) {
        m_editController->updatePlacement();
    }
    refreshContextMenu();
}

void ScreenshotPinnedWindow::applyWheelScale(int percent, const QPointF& nativeCursor) {
    if (m_ocrMode || percent < kMinimumScalePercent || percent > kMaximumScalePercent) {
        return;
    }
    restoreFromThumbnailImmediately();
    const QRect oldGeometry = currentNativeGeometry();
    if (!oldGeometry.isValid() || oldGeometry.isEmpty()) {
        applyScale(percent);
        return;
    }

    QSize nativeSize = orientedInitialPhysicalSize();
    nativeSize = QSize(std::max(1, qRound(nativeSize.width() * percent / 100.0)),
                       std::max(1, qRound(nativeSize.height() * percent / 100.0)));
    const resize_geometry::ScaleAnchor anchor =
        resize_geometry::scaleAnchorFromSetting(m_mouseWheelZoomMode);
    const QRect nativeTarget =
        resize_geometry::anchoredScaleRect(oldGeometry, nativeSize, anchor, nativeCursor);
    setEffectiveScale(percent, true);
    if (nativeTarget != oldGeometry) {
        // Keep wheel steps on their requested percentage. Re-adopting the
        // rounded native width can otherwise make the next notch target the
        // same scale level and leave the pin stuck in a narrow range.
        m_preserveScaleForSettledGeometry = true;
    }
    static_cast<void>(applyWindowGeometry(nativeTarget, GeometryMutation::Scale));
    updateCanvasViewport();
    updateControlsGeometry();
    if (m_editController != nullptr) {
        m_editController->updatePlacement();
    }
    refreshContextMenu();
}

bool ScreenshotPinnedWindow::handleOpacityWheel(QObject* watched, QWheelEvent* event) {
    Q_UNUSED(watched);
    if (event == nullptr || m_closing || !event->modifiers().testFlag(Qt::ControlModifier) ||
        (!m_ocrMode && m_editController != nullptr && m_editController->editMode())) {
        return false;
    }

    int steps = 0;
    if (event->pixelDelta().y() != 0) {
        steps = event->pixelDelta().y() > 0 ? 1 : -1;
    } else if (event->angleDelta().y() != 0) {
        m_opacityWheelAngleRemainder += event->angleDelta().y();
        steps = m_opacityWheelAngleRemainder / 120;
        m_opacityWheelAngleRemainder -= steps * 120;
    } else {
        return false;
    }

    event->accept();
    if (steps == 0) {
        return true;
    }

    const int targetPercent =
        qBound(kMinimumOpacityPercent, m_opacityPercent + steps * kWheelOpacityStep,
               kMaximumOpacityPercent);
    if (targetPercent != m_opacityPercent) {
        setOpacityPercent(targetPercent);
    }
    return true;
}

bool ScreenshotPinnedWindow::handleScaleWheel(QObject* watched, QWheelEvent* event) {
    if (event == nullptr || m_closing) {
        return false;
    }
    if (m_ocrMode) {
        event->accept();
        return true;
    }
    if (m_editController != nullptr && m_editController->editMode()) {
        return false;
    }

    int steps = 0;
    if (event->pixelDelta().y() != 0) {
        steps = event->pixelDelta().y() > 0 ? 1 : -1;
    } else if (event->angleDelta().y() != 0) {
        m_wheelAngleRemainder += event->angleDelta().y();
        steps = m_wheelAngleRemainder / 120;
        m_wheelAngleRemainder -= steps * 120;
    } else {
        return false;
    }

    event->accept();
    if (steps == 0) {
        return true;
    }

    const QPointF windowPosition = windowPositionForEvent(watched, event->position());
    const QPointF nativeCursor = nativePositionForWindowPosition(windowPosition);
    // Wheel scaling moves between fixed ten-percent levels. Keep arbitrary
    // values produced by native resizing, but use the next level in the
    // direction of travel instead of adding ten to the current value.
    const double level = steps > 0 ? std::floor(m_scalePercent / kWheelScaleStep)
                                   : std::ceil(m_scalePercent / kWheelScaleStep);
    const int targetPercent =
        qBound(kMinimumScalePercent, qRound(level * kWheelScaleStep + steps * kWheelScaleStep),
               kMaximumScalePercent);
    if (targetPercent != qRound(m_scalePercent)) {
        applyWheelScale(targetPercent, nativeCursor);
    }
    return true;
}

QSize ScreenshotPinnedWindow::orientedInitialPhysicalSize() const {
    QSize size = m_initialPhysicalSize;
    if ((m_quarterTurns % 2) != 0) {
        size.transpose();
    }
    return size;
}

QRect ScreenshotPinnedWindow::logicalRectForNativeRect(const QRect& nativeRect) const {
    QScreen* targetScreen = screen();
    if (targetScreen == nullptr) {
        targetScreen = QGuiApplication::screenAt(frameGeometry().center());
    }
    const QRect logical =
        ScreenshotGeometryMapper::logicalRectForPhysicalRect(nativeRect, targetScreen);
    return logical.isValid() && !logical.isEmpty() ? logical : nativeRect;
}

void ScreenshotPinnedWindow::setEffectiveScale(double percent, bool showReadout) {
    percent = qBound(static_cast<double>(kMinimumScalePercent), percent,
                     static_cast<double>(kMaximumScalePercent));
    const bool changed = qAbs(percent - m_scalePercent) > 0.001;
    if (changed) {
        invalidatePendingCopy();
    }
    m_scalePercent = percent;
    refreshContextMenu();
    if (changed && showReadout) {
        showScaleReadout();
    }
}

void ScreenshotPinnedWindow::showScaleReadout() {
    if (m_scaleLabel == nullptr || m_scaleLabelTimer == nullptr) {
        return;
    }
    m_scaleLabel->setText(tr("Scale: %1%").arg(qRound(m_scalePercent)));
    m_scaleLabel->adjustSize();
    updateControlsGeometry();
    m_scaleLabel->show();
    m_scaleLabel->raise();
    m_scaleLabelTimer->start();
}

void ScreenshotPinnedWindow::showOpacityReadout() {
    if (m_scaleLabel == nullptr || m_scaleLabelTimer == nullptr) {
        return;
    }
    m_scaleLabel->setText(tr("Opacity: %1%").arg(m_opacityPercent));
    m_scaleLabel->adjustSize();
    updateControlsGeometry();
    m_scaleLabel->show();
    m_scaleLabel->raise();
    m_scaleLabelTimer->start();
}

void ScreenshotPinnedWindow::scheduleNativeScaleAdoption() {
    if (m_nativeScaleSettleTimer != nullptr && !m_closing && m_presented) {
        m_nativeScaleSettleTimer->start();
    }
}

void ScreenshotPinnedWindow::adoptSettledNativeScale() {
    if (!m_presented || m_closing || m_thumbnailMode || m_geometryAnimating ||
        m_systemSizingActive) {
        return;
    }
    const QRect nativeGeometry = currentNativeGeometry();
    const QSize baseline = orientedInitialPhysicalSize();
    if (!nativeGeometry.isValid() || nativeGeometry.isEmpty() || baseline.width() <= 0) {
        return;
    }
    if (m_preserveScaleForSettledGeometry) {
        constexpr int platformRoundingTolerance = 2;
        const QSize requestedSize = m_nativeGeometryController != nullptr
                                        ? m_nativeGeometryController->committedGeometry().size()
                                        : QSize();
        if (qAbs(nativeGeometry.width() - requestedSize.width()) <= platformRoundingTolerance &&
            qAbs(nativeGeometry.height() - requestedSize.height()) <= platformRoundingTolerance) {
            updateCanvasViewport();
            updateControlsGeometry();
            if (m_editController != nullptr) {
                m_editController->updatePlacement();
            }
            return;
        }
        m_preserveScaleForSettledGeometry = false;
    }
    const double percent = 100.0 * nativeGeometry.width() / baseline.width();
    setEffectiveScale(percent, true);
    updateCanvasViewport();
    updateControlsGeometry();
    if (m_editController != nullptr) {
        m_editController->updatePlacement();
    }
}

void ScreenshotPinnedWindow::setOpacityPercent(int percent) {
    if (percent < kMinimumOpacityPercent || percent > kMaximumOpacityPercent) {
        return;
    }
    const bool changed = percent != m_opacityPercent;
    m_opacityPercent = percent;
    setWindowOpacity(percent / 100.0);
    refreshContextMenu();
    if (changed) {
        showOpacityReadout();
    }
}

void ScreenshotPinnedWindow::setThumbnailMode(bool enabled, bool animate) {
    if (m_closing || m_thumbnailMode == enabled) {
        return;
    }
    invalidatePendingCopy();
    static_cast<void>(finishNativeGeometryInteraction());
    finishWindowMove();
    clearWindowDragCursor();
    if (enabled) {
        setEditMode(false);
        m_preThumbnailNativeGeometry = currentNativeGeometry();
        QScreen* targetScreen =
            ScreenshotGeometryMapper::screenForPhysicalRect(m_preThumbnailNativeGeometry);
        const qreal scale = targetScreen != nullptr ? targetScreen->devicePixelRatio() : 1.0;
        const int nativeThumbnailSize = std::max(1, qRound(kThumbnailSize * scale));
        QRect nativeTarget(m_preThumbnailNativeGeometry.topLeft(),
                           QSize(nativeThumbnailSize, nativeThumbnailSize));
        if (targetScreen != nullptr) {
            const QRect bounds = ScreenshotGeometryMapper::physicalRectForScreen(*targetScreen);
            nativeTarget.moveLeft(qBound(bounds.left(), nativeTarget.left(),
                                         bounds.right() - nativeTarget.width() + 1));
            nativeTarget.moveTop(qBound(bounds.top(), nativeTarget.top(),
                                        bounds.bottom() - nativeTarget.height() + 1));
        }
        m_thumbnailMode = true;
        m_thumbnailAnimationTarget = true;
        updateControlsGeometry();
        if (animate) {
            animateGeometryTo(nativeTarget);
        } else {
            static_cast<void>(applyWindowGeometry(nativeTarget, GeometryMutation::Thumbnail));
        }
    } else {
        m_thumbnailMode = false;
        m_thumbnailAnimationTarget = false;
        updateControlsGeometry();
        if (animate) {
            animateGeometryTo(m_preThumbnailNativeGeometry);
        } else {
            static_cast<void>(
                applyWindowGeometry(m_preThumbnailNativeGeometry, GeometryMutation::Thumbnail));
        }
    }
    refreshContextMenu();
}

void ScreenshotPinnedWindow::restoreFromThumbnailImmediately() {
    if (!m_thumbnailMode) {
        return;
    }
    invalidatePendingCopy();
    if (m_geometryAnimation != nullptr) {
        m_geometryAnimation->stop();
    }
    m_geometryAnimating = false;
    m_thumbnailMode = false;
    m_thumbnailAnimationTarget = false;
    updateControlsGeometry();
    static_cast<void>(
        applyWindowGeometry(m_preThumbnailNativeGeometry, GeometryMutation::Thumbnail));
    refreshContextMenu();
    updateWindowDragCursor(mapFromGlobal(QCursor::pos()));
}

void ScreenshotPinnedWindow::animateGeometryTo(const QRect& nativeTarget) {
    if (!nativeTarget.isValid() || nativeTarget.isEmpty()) {
        return;
    }
    if (m_geometryAnimation != nullptr) {
        m_geometryAnimation->stop();
        delete m_geometryAnimation;
    }
    m_geometryAnimating = false;
    m_geometryAnimation = new QVariantAnimation(this);
    m_geometryAnimating = true;
    m_geometryAnimation->setObjectName(QStringLiteral("screenshotPinnedGeometryAnimation"));
    m_geometryAnimation->setDuration(kThumbnailAnimationDurationMs);
    m_geometryAnimation->setEasingCurve(QEasingCurve::InOutCubic);
    m_geometryAnimation->setStartValue(currentNativeGeometry());
    m_geometryAnimation->setEndValue(nativeTarget);
    connect(m_geometryAnimation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                static_cast<void>(applyWindowGeometry(value.toRect(), GeometryMutation::Animation));
            });
    connect(m_geometryAnimation, &QVariantAnimation::finished, this, [this, nativeTarget]() {
        m_geometryAnimating = false;
        static_cast<void>(applyWindowGeometry(nativeTarget, GeometryMutation::Animation));
        updateCanvasViewport();
        updateControlsGeometry();
        if (m_editController != nullptr) {
            m_editController->updatePlacement();
        }
        updateWindowDragCursor(mapFromGlobal(QCursor::pos()));
    });
    m_geometryAnimation->start();
}

bool ScreenshotPinnedWindow::applyWindowGeometry(const QRect& nativeGeometry,
                                                 GeometryMutation mutation) {
    if (!nativeGeometry.isValid() || nativeGeometry.isEmpty() ||
        m_nativeGeometryController == nullptr) {
        return false;
    }

    ScreenshotPinnedNativeGeometryController::Origin origin =
        ScreenshotPinnedNativeGeometryController::Origin::Scale;
    switch (mutation) {
    case GeometryMutation::Scale:
        origin = ScreenshotPinnedNativeGeometryController::Origin::Scale;
        break;
    case GeometryMutation::ImageTransform:
        origin = ScreenshotPinnedNativeGeometryController::Origin::ImageTransform;
        break;
    case GeometryMutation::Thumbnail:
        origin = ScreenshotPinnedNativeGeometryController::Origin::Thumbnail;
        break;
    case GeometryMutation::Animation:
        origin = ScreenshotPinnedNativeGeometryController::Origin::Animation;
        break;
    }

    if (!m_nativeGeometryController->beginProgrammatic(nativeGeometry, origin)) {
        return false;
    }

#if defined(Q_OS_WIN) || defined(_WIN32)
    if (!native::applyClientGeometry(winId(), nativeGeometry)) {
        static_cast<void>(restoreCommittedNativeGeometry());
        return false;
    }
#else
    setGeometry(logicalRectForNativeRect(nativeGeometry));
#endif
    const auto change = m_nativeGeometryController->commitTarget();
    if (change.sizeChanged || change.dpiChanged) {
        scheduleNativeScaleAdoption();
    }
    return true;
}

bool ScreenshotPinnedWindow::finishNativeGeometryInteraction() {
    if (m_nativeGeometryController == nullptr ||
        !m_nativeGeometryController->hasInteractiveTransaction()) {
        return false;
    }

    const QRect target = m_nativeGeometryController->finishInteractiveTarget();
    if (!target.isValid() || target.isEmpty()) {
        m_nativeGeometryController->cancelPendingInteraction();
        return false;
    }

#if defined(Q_OS_WIN) || defined(_WIN32)
    if (native::currentClientGeometry(winId()) != target &&
        !native::applyClientGeometry(winId(), target)) {
        static_cast<void>(restoreCommittedNativeGeometry());
        return false;
    }
#else
    setGeometry(logicalRectForNativeRect(target));
#endif
    const auto change = m_nativeGeometryController->commitTarget();
    if (change.sizeChanged || change.dpiChanged) {
        m_preserveScaleForSettledGeometry = false;
        scheduleNativeScaleAdoption();
    }
    return true;
}

bool ScreenshotPinnedWindow::reconcilePassiveNativeGeometry() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (!m_presented || m_closing || m_nativeGeometryController == nullptr) {
        return false;
    }

    const auto phase = m_nativeGeometryController->phase();
    const bool passive =
        phase == ScreenshotPinnedNativeGeometryController::Phase::Stable ||
        ((phase == ScreenshotPinnedNativeGeometryController::Phase::MovePending ||
          phase == ScreenshotPinnedNativeGeometryController::Phase::ResizePending) &&
         !m_nativeGeometryController->hasAcceptedInteractiveGeometry());
    const QRect target = m_nativeGeometryController->targetGeometry();
    if (!passive || !target.isValid() || target.isEmpty() ||
        native::currentClientGeometry(winId()) == target) {
        return false;
    }

    m_passiveGeometryReconciliationActive = true;
    const bool reconciled =
        native::applyClientGeometry(winId(), target, native::GeometryUpdate::DiscardClientPixels);
    m_passiveGeometryReconciliationActive = false;
    if (reconciled) {
        return true;
    }
    qCritical("Pinned window passive native geometry could not be reconciled");
    QTimer::singleShot(0, this, &QWidget::close);
#endif
    return false;
}

bool ScreenshotPinnedWindow::restoreCommittedNativeGeometry() {
    if (m_nativeGeometryController == nullptr) {
        return false;
    }

    m_nativeGeometryController->prepareRollback();
    const QRect committed = m_nativeGeometryController->targetGeometry();
    bool restored = committed.isValid() && !committed.isEmpty();
#if defined(Q_OS_WIN) || defined(_WIN32)
    restored = restored && native::applyClientGeometry(winId(), committed,
                                                       native::GeometryUpdate::DiscardClientPixels);
#else
    if (restored) {
        setGeometry(logicalRectForNativeRect(committed));
    }
#endif
    if (restored) {
        static_cast<void>(m_nativeGeometryController->finishRollback());
        return true;
    }

    qCritical("Pinned window native geometry could not be restored");
    QTimer::singleShot(0, this, &QWidget::close);
    return false;
}

QRect ScreenshotPinnedWindow::nativeRectForLogicalRect(const QRect& logical,
                                                       QScreen* targetScreen) const {
    if (targetScreen == nullptr) {
        return logical;
    }
    const QRect logicalScreen = targetScreen->geometry();
    const QRect physicalScreen = ScreenshotGeometryMapper::physicalRectForScreen(*targetScreen);
    if (logicalScreen.isEmpty() || physicalScreen.isEmpty()) {
        return logical;
    }
    const qreal scaleX = targetScreen->devicePixelRatio();
    const qreal scaleY = targetScreen->devicePixelRatio();
    return QRect(
        QPoint(physicalScreen.left() + qRound((logical.left() - logicalScreen.left()) * scaleX),
               physicalScreen.top() + qRound((logical.top() - logicalScreen.top()) * scaleY)),
        QSize(std::max(1, qRound(logical.width() * scaleX)),
              std::max(1, qRound(logical.height() * scaleY))));
}

void ScreenshotPinnedWindow::showAllPinnedWindows() {
    const auto windows = livePinnedWindows();
    for (const QPointer<ScreenshotPinnedWindow>& window : windows) {
        if (window != nullptr) {
            window->show();
            window->raise();
        }
    }
}

void ScreenshotPinnedWindow::hideOtherPinnedWindows() {
    const auto windows = livePinnedWindows();
    for (const QPointer<ScreenshotPinnedWindow>& window : windows) {
        if (window != nullptr && window != this) {
            window->hide();
        }
    }
    show();
    raise();
}

void ScreenshotPinnedWindow::closeOtherPinnedWindows() {
    const auto windows = livePinnedWindows();
    for (const QPointer<ScreenshotPinnedWindow>& window : windows) {
        if (window != nullptr && window != this) {
            window->close();
        }
    }
}

void ScreenshotPinnedWindow::closeAllPinnedWindows() {
    const auto windows = livePinnedWindows();
    for (const QPointer<ScreenshotPinnedWindow>& window : windows) {
        if (window != nullptr) {
            window->close();
        }
    }
}

std::optional<QPoint> ScreenshotPinnedWindow::physicalCursorPosition() const {
    return m_physicalCursor != nullptr ? m_physicalCursor->position() : std::nullopt;
}

bool ScreenshotPinnedWindow::cursorMovementEnabled() const {
    return !m_closing && (m_windowDragActive || (!m_ocrMode && m_editController != nullptr &&
                                                 m_editController->editMode()));
}

bool ScreenshotPinnedWindow::moveCursorOnePixel(
    snow_shot::platform::PhysicalCursorDirection direction) {
    if (!cursorMovementEnabled() || m_physicalCursor == nullptr) {
        return false;
    }
    const snow_shot::platform::PhysicalCursorMoveResult result =
        m_physicalCursor->moveOnePixel(direction);
    if (!result.commandApplied()) {
        return false;
    }
    if (!result.position.has_value()) {
        return true;
    }

    if (m_windowDragActive) {
        static_cast<void>(updateWindowMove(result.position.value()));
    } else if (m_editController != nullptr && m_editController->canvasColorSamplingActive()) {
        m_editController->updateCanvasColorSamplingAfterCursorMove(result.position.value());
    }
    return true;
}

bool ScreenshotPinnedWindow::startWindowMove() {
    if (!windowDragEnabled() || m_nativeGeometryController == nullptr) {
        return false;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    const WId nativeWindowId = winId();
    m_windowDragActive = true;
    if (!native::beginWindowMoveCapture(nativeWindowId)) {
        m_windowDragActive = false;
        return false;
    }
    m_windowMoveCaptureId = nativeWindowId;
    POINT cursor{};
    if (GetCursorPos(&cursor) == FALSE ||
        !m_nativeGeometryController->beginMove(QPoint(cursor.x, cursor.y))) {
        finishWindowMove();
        return false;
    }
#else
    const QPoint globalCursor = QCursor::pos();
    if (!m_nativeGeometryController->beginMove(globalCursor)) {
        return false;
    }
    m_windowDragActive = true;
#endif
    setWindowDragCursor(Qt::ClosedHandCursor);
#if defined(Q_OS_WIN) || defined(_WIN32)
    return true;
#else
    if (windowHandle()->startSystemMove()) {
        return true;
    }
    finishWindowMove();
    m_nativeGeometryController->cancelPendingInteraction();
    return false;
#endif
}

bool ScreenshotPinnedWindow::updateWindowMove(const QPoint& nativeCursorPosition) {
    if (!m_windowDragActive || m_nativeGeometryController == nullptr) {
        return false;
    }
    const QRect target = m_nativeGeometryController->updateMove({}, nativeCursorPosition);
    if (!target.isValid() || target.isEmpty()) {
        return false;
    }

#if defined(Q_OS_WIN) || defined(_WIN32)
    if (native::currentClientGeometry(winId()) == target) {
        return true;
    }
    if (native::applyClientGeometry(winId(), target)) {
        return true;
    }
    static_cast<void>(restoreCommittedNativeGeometry());
    finishWindowMove();
    return false;
#else
    Q_UNUSED(nativeCursorPosition);
    return true;
#endif
}

void ScreenshotPinnedWindow::finishWindowMove() {
    const bool wasActive = m_windowDragActive;
    m_windowDragActive = false;
    if (m_windowMoveCaptureId != 0) {
        native::endWindowMoveCapture(m_windowMoveCaptureId);
        m_windowMoveCaptureId = 0;
    }
    if (wasActive) {
        updateWindowDragCursor(mapFromGlobal(QCursor::pos()));
    }
}

bool ScreenshotPinnedWindow::windowDragEnabled() const {
    return !m_closing && !m_geometryAnimating &&
           (m_editController == nullptr || !m_editController->editMode()) && !m_ocrMode &&
           windowHandle() != nullptr;
}

void ScreenshotPinnedWindow::updateWindowDragCursor(const QPoint& position) {
    if (m_windowDragActive) {
        setWindowDragCursor(Qt::ClosedHandCursor);
        return;
    }
    if (windowDragEnabled() && !isControlsPanelPosition(position)) {
        setWindowDragCursor(Qt::OpenHandCursor);
        return;
    }
    clearWindowDragCursor();
}

void ScreenshotPinnedWindow::setWindowDragCursor(Qt::CursorShape shape) {
    const QCursor cursor(shape);
    if (m_canvas != nullptr) {
        m_canvas->setCursorForLayer(SnowCanvasCursorLayer::Host, cursor);
    }
    setCursor(cursor);
    if (QWindow* handle = windowHandle()) {
        handle->setCursor(cursor);
    }
    m_windowDragCursorSet = true;
}

void ScreenshotPinnedWindow::clearWindowDragCursor() {
    if (!m_windowDragCursorSet) {
        return;
    }
    if (m_canvas != nullptr) {
        m_canvas->clearCursorForLayer(SnowCanvasCursorLayer::Host);
    }
    unsetCursor();
    if (QWindow* handle = windowHandle()) {
        handle->unsetCursor();
    }
    m_windowDragCursorSet = false;
}

bool ScreenshotPinnedWindow::nativeTrackSizeConstraintsEnabled() const {
    // Drawing mode disables interactive resizing, but the existing native size
    // remains governed by the same bounds. Dropping those bounds lets Windows
    // clamp long pins to the work area when the edit toolbar takes ownership.
    return !m_closing && !m_thumbnailMode && !m_geometryAnimating;
}

bool ScreenshotPinnedWindow::interactiveResizingEnabled() const {
    return !m_closing && !m_thumbnailMode && !m_geometryAnimating && !m_ocrMode &&
           (m_editController == nullptr || !m_editController->editMode());
}

QPointF ScreenshotPinnedWindow::windowPositionForEvent(QObject* watched,
                                                       const QPointF& position) const {
    const auto* widget = qobject_cast<const QWidget*>(watched);
    if (widget == nullptr || widget == this) {
        return position;
    }
    return QPointF(widget->mapTo(this, QPoint())) + position;
}

QPointF ScreenshotPinnedWindow::nativePositionForWindowPosition(const QPointF& position) const {
    const QRect nativeGeometry = currentNativeGeometry();
    if (!nativeGeometry.isValid() || nativeGeometry.isEmpty() || width() <= 0 || height() <= 0) {
        return position;
    }
    return QPointF(nativeGeometry.left() + position.x() * nativeGeometry.width() / width(),
                   nativeGeometry.top() + position.y() * nativeGeometry.height() / height());
}

QPoint ScreenshotPinnedWindow::globalPositionForNativePosition(const QPoint& position) const {
    const QRect nativeGeometry = currentNativeGeometry();
    if (!nativeGeometry.isValid() || nativeGeometry.isEmpty() || width() <= 0 || height() <= 0) {
        return position;
    }
    const QPoint windowPosition(qRound((position.x() - nativeGeometry.left()) *
                                       static_cast<double>(width()) / nativeGeometry.width()),
                                qRound((position.y() - nativeGeometry.top()) *
                                       static_cast<double>(height()) / nativeGeometry.height()));
    return mapToGlobal(windowPosition);
}

bool ScreenshotPinnedWindow::isControlsPanelPosition(const QPoint& position) const {
    return m_controlsPanel != nullptr && m_controlsPanel->isVisible() &&
           m_controlsPanel->geometry().contains(position);
}
