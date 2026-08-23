#include "snow_shot/presentation/components/settingscustomwidget.h"

#include "snow_shot/presentation/components/drawingtoolbareditorsettingswidget.h"
#include "snow_shot/presentation/components/storagestatussettingswidget.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/settings/settingsruntimebindings.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include "widgets/button.h"
#include "widgets/checkbox.h"
#include "widgets/divider.h"

#include <QApplication>
#include <QBoxLayout>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace {
namespace toolbar_layout = snow_shot::presentation::toolbar_layout;
namespace storage = snow_shot::storage;

constexpr char kToolbarItemMimeType[] = "application/x-snow-shot-toolbar-item";
constexpr char kToolbarItemProperty[] = "screenshotToolbarItemId";
constexpr int kToolbarButtonSize = 32;
constexpr int kToolbarIconSize = 24;
constexpr int kToolbarHorizontalMargin = 12;
constexpr int kToolbarVerticalMargin = 4;
constexpr int kToolbarPositionSpacing = 8;
constexpr int kToolbarStackSpacing = 4;
constexpr int kToolbarRadius = 8;
constexpr int kDropIndicatorThickness = 3;
constexpr int kHiddenZoneHeight = 56;
[[maybe_unused]] constexpr const char* kEditorTranslations[] = {
    QT_TRANSLATE_NOOP(
        "DrawingToolbarEditorSettingsWidget",
        "Drop beside a tool to create a position. Drop above a tool to stack it. The bottom "
        "tool stays on the main toolbar row."),
    QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Drawing toolbar preview"),
    QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Hidden tools"),
    QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget",
                      "Drag tools here to hide them from the screenshot toolbar."),
    QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "No hidden tools"),
    QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Hidden drawing toolbar tools"),
};

QString translatedToolbarText(const char* sourceText) {
    return QApplication::translate("DrawingToolbarEditorSettingsWidget", sourceText);
}

QString cssColor(const QColor& color) {
    if (color.alpha() == 255) {
        return color.name(QColor::HexRgb);
    }
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

class ToolbarDragButton final : public adqt::widgets::AdButton {
  public:
    explicit ToolbarDragButton(const QString& itemId, QWidget* parent)
        : adqt::widgets::AdButton(parent), m_itemId(itemId) {
        setProperty(kToolbarItemProperty, itemId);
        setObjectName(QStringLiteral("settings-drawing-toolbar-item-%1").arg(itemId));
        setButtonStyle(ButtonStyle::Text);
        setAccentRole(AccentRole::Neutral);
        setCursor(Qt::OpenHandCursor);
        setFixedSize(kToolbarButtonSize, kToolbarButtonSize);
        setIconSize(QSize(kToolbarIconSize, kToolbarIconSize));
    }

  protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event != nullptr && event->button() == Qt::LeftButton) {
            m_pressPosition = event->position().toPoint();
        }
        adqt::widgets::AdButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (event == nullptr || !(event->buttons() & Qt::LeftButton) ||
            (event->position().toPoint() - m_pressPosition).manhattanLength() <
                QApplication::startDragDistance()) {
            adqt::widgets::AdButton::mouseMoveEvent(event);
            return;
        }

        auto* mimeData = new QMimeData();
        mimeData->setData(kToolbarItemMimeType, m_itemId.toUtf8());
        auto* drag = new QDrag(this);
        drag->setMimeData(mimeData);
        const qreal pixelRatio = devicePixelRatioF();
        QPixmap preview(qRound(width() * pixelRatio), qRound(height() * pixelRatio));
        preview.setDevicePixelRatio(pixelRatio);
        preview.fill(Qt::transparent);
        render(&preview);
        drag->setPixmap(preview);
        drag->setHotSpot(m_pressPosition);
        setCursor(Qt::ClosedHandCursor);
        static_cast<void>(drag->exec(Qt::MoveAction));
        setCursor(Qt::OpenHandCursor);
    }

  private:
    QString m_itemId;
    QPoint m_pressPosition;
};

class ToolbarPositionWidget final : public QWidget {
  public:
    explicit ToolbarPositionWidget(int index, QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("settings-drawing-toolbar-position-%1").arg(index));
        setProperty("screenshotToolbarPosition", true);
        setProperty("screenshotToolbarPositionIndex", index);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 0, 0, 0);
        m_layout->setSpacing(kToolbarStackSpacing);
        m_layout->setAlignment(Qt::AlignBottom | Qt::AlignHCenter);
        m_layout->setSizeConstraint(QLayout::SetFixedSize);
    }

    [[nodiscard]] QVBoxLayout* contentLayout() const {
        return m_layout;
    }

    [[nodiscard]] int insertionIndex(const QPoint& surfacePosition, const QWidget* surface) const {
        const QPoint localPosition = mapFrom(surface, surfacePosition);
        for (int index = 0; index < m_layout->count(); ++index) {
            QWidget* widget = m_layout->itemAt(index)->widget();
            if (widget != nullptr && localPosition.y() < widget->geometry().center().y()) {
                return index;
            }
        }
        return m_layout->count();
    }

  private:
    QVBoxLayout* m_layout = nullptr;
};

class ToolbarDropSurface final : public QFrame {
  public:
    enum class DropKind {
        NewPosition,
        Stack,
    };

    struct DropLocation {
        DropKind kind = DropKind::NewPosition;
        int positionIndex = 0;
        int itemIndex = 0;
    };

    using DropHandler = std::function<void(const QString&, const DropLocation&)>;

    explicit ToolbarDropSurface(DropHandler handler, QWidget* parent)
        : QFrame(parent), m_dropHandler(std::move(handler)) {
        setObjectName(QStringLiteral("settings-drawing-toolbar-surface"));
        setAcceptDrops(true);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setMinimumSize(kToolbarButtonSize + kToolbarHorizontalMargin * 2,
                       kToolbarButtonSize + kToolbarVerticalMargin * 2);
        setAttribute(Qt::WA_StyledBackground, false);
        m_layout = new QHBoxLayout(this);
        m_layout->setContentsMargins(kToolbarHorizontalMargin, kToolbarVerticalMargin,
                                     kToolbarHorizontalMargin, kToolbarVerticalMargin);
        m_layout->setSpacing(kToolbarPositionSpacing);
        m_layout->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
        m_layout->setSizeConstraint(QLayout::SetFixedSize);

        m_positionIndicator = new QFrame(this);
        m_positionIndicator->setObjectName(
            QStringLiteral("settings-drawing-toolbar-position-indicator"));
        m_positionIndicator->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_positionIndicator->hide();
        m_stackIndicator = new QFrame(this);
        m_stackIndicator->setObjectName(QStringLiteral("settings-drawing-toolbar-stack-indicator"));
        m_stackIndicator->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_stackIndicator->hide();

        auto* shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(18.0);
        shadow->setOffset(0.0, 3.0);
        shadow->setColor(QColor(0, 0, 0, 90));
        setGraphicsEffect(shadow);
    }

    [[nodiscard]] QHBoxLayout* contentLayout() const {
        return m_layout;
    }

    void setPositions(const QVector<ToolbarPositionWidget*>& positions) {
        m_positions = positions;
        hideIndicators();
        m_layout->invalidate();
        m_layout->activate();
        updateGeometry();
    }

    void applyTheme(const QColor& surface, const QColor& accent) {
        m_surfaceColor = surface;
        m_accentColor = accent;
        const QString indicatorStyle =
            QStringLiteral("QFrame { background: %1; border: 0; border-radius: 1px; }")
                .arg(cssColor(accent));
        m_positionIndicator->setStyleSheet(indicatorStyle);
        m_stackIndicator->setStyleSheet(indicatorStyle);
        update();
    }

    void hideIndicators() {
        m_positionIndicator->hide();
        m_stackIndicator->hide();
    }

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (hasToolbarItem(event != nullptr ? event->mimeData() : nullptr)) {
            m_dragActive = true;
            updateIndicator(event->position().toPoint());
            update();
            event->acceptProposedAction();
            return;
        }
        QFrame::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (hasToolbarItem(event != nullptr ? event->mimeData() : nullptr)) {
            updateIndicator(event->position().toPoint());
            event->acceptProposedAction();
            return;
        }
        QFrame::dragMoveEvent(event);
    }

    void dragLeaveEvent(QDragLeaveEvent* event) override {
        m_dragActive = false;
        hideIndicators();
        update();
        QFrame::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent* event) override {
        m_dragActive = false;
        hideIndicators();
        update();
        if (!hasToolbarItem(event != nullptr ? event->mimeData() : nullptr)) {
            QFrame::dropEvent(event);
            return;
        }
        const QString itemId = QString::fromUtf8(event->mimeData()->data(kToolbarItemMimeType));
        if (toolbar_layout::descriptor(itemId) == nullptr) {
            event->ignore();
            return;
        }
        if (m_dropHandler) {
            m_dropHandler(itemId, dropLocation(event->position().toPoint()));
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(m_surfaceColor.isValid() ? m_surfaceColor : QColor(Qt::white));
        if (m_dragActive && m_accentColor.isValid()) {
            QColor outline = m_accentColor;
            outline.setAlpha(110);
            painter.setPen(QPen(outline, 1.0));
        } else {
            painter.setPen(Qt::NoPen);
        }
        const int bottomBarHeight = kToolbarButtonSize + kToolbarVerticalMargin * 2;
        painter.drawRoundedRect(
            QRectF(0, qMax(0, height() - bottomBarHeight), width(), bottomBarHeight)
                .adjusted(0.5, 0.5, -0.5, -0.5),
            kToolbarRadius, kToolbarRadius);
        for (ToolbarPositionWidget* position : std::as_const(m_positions)) {
            if (position == nullptr || position->height() <= kToolbarButtonSize) {
                continue;
            }
            const QRect geometry = position->geometry();
            const int extensionTop = qMax(0, geometry.top() - kToolbarVerticalMargin);
            painter.drawRoundedRect(QRectF(geometry.left() - kToolbarVerticalMargin, extensionTop,
                                           geometry.width() + kToolbarVerticalMargin * 2,
                                           height() - extensionTop)
                                        .adjusted(0.5, 0.5, -0.5, -0.5),
                                    kToolbarRadius, kToolbarRadius);
        }
    }

  private:
    static bool hasToolbarItem(const QMimeData* mimeData) {
        return mimeData != nullptr && mimeData->hasFormat(kToolbarItemMimeType);
    }

    [[nodiscard]] DropLocation dropLocation(const QPoint& position) const {
        for (int index = 0; index < m_positions.size(); ++index) {
            ToolbarPositionWidget* toolbarPosition = m_positions.at(index);
            if (toolbarPosition == nullptr) {
                continue;
            }
            const QRect geometry = toolbarPosition->geometry();
            if (position.x() >= geometry.left() && position.x() <= geometry.right()) {
                return {DropKind::Stack, index, toolbarPosition->insertionIndex(position, this)};
            }
            if (position.x() < geometry.left()) {
                return {DropKind::NewPosition, index, 0};
            }
        }
        return {DropKind::NewPosition, static_cast<int>(m_positions.size()), 0};
    }

    void updateIndicator(const QPoint& position) {
        const DropLocation location = dropLocation(position);
        if (location.kind == DropKind::NewPosition) {
            int indicatorX = kToolbarHorizontalMargin;
            if (!m_positions.isEmpty()) {
                if (location.positionIndex <= 0) {
                    indicatorX =
                        (kToolbarHorizontalMargin + m_positions.constFirst()->geometry().left()) /
                        2;
                } else if (location.positionIndex >= m_positions.size()) {
                    indicatorX = (m_positions.constLast()->geometry().right() + width() -
                                  kToolbarHorizontalMargin) /
                                 2;
                } else {
                    indicatorX = (m_positions.at(location.positionIndex - 1)->geometry().right() +
                                  m_positions.at(location.positionIndex)->geometry().left()) /
                                 2;
                }
            }
            const int indicatorY = height() - kToolbarVerticalMargin - kToolbarButtonSize;
            m_positionIndicator->setGeometry(indicatorX - kDropIndicatorThickness / 2, indicatorY,
                                             kDropIndicatorThickness, kToolbarButtonSize);
            m_stackIndicator->hide();
            m_positionIndicator->show();
            m_positionIndicator->raise();
            return;
        }

        ToolbarPositionWidget* toolbarPosition = m_positions.value(location.positionIndex, nullptr);
        if (toolbarPosition == nullptr || toolbarPosition->contentLayout() == nullptr ||
            toolbarPosition->contentLayout()->count() == 0) {
            hideIndicators();
            return;
        }
        QVBoxLayout* positionLayout = toolbarPosition->contentLayout();
        int localY = 0;
        if (location.itemIndex <= 0) {
            localY = positionLayout->itemAt(0)->widget()->geometry().top() -
                     (kToolbarStackSpacing + kDropIndicatorThickness) / 2;
        } else if (location.itemIndex >= positionLayout->count()) {
            localY =
                positionLayout->itemAt(positionLayout->count() - 1)->widget()->geometry().bottom() +
                (kToolbarStackSpacing - kDropIndicatorThickness) / 2 + 1;
        } else {
            const QRect previousGeometry =
                positionLayout->itemAt(location.itemIndex - 1)->widget()->geometry();
            const QRect nextGeometry =
                positionLayout->itemAt(location.itemIndex)->widget()->geometry();
            localY =
                (previousGeometry.bottom() + nextGeometry.top() - kDropIndicatorThickness + 1) / 2;
        }
        const QPoint surfacePoint = toolbarPosition->mapTo(this, QPoint(0, localY));
        const int lineWidth = qMax(16, toolbarPosition->width() - 8);
        m_stackIndicator->setGeometry(toolbarPosition->geometry().center().x() - lineWidth / 2,
                                      surfacePoint.y(), lineWidth, kDropIndicatorThickness);
        m_positionIndicator->hide();
        m_stackIndicator->show();
        m_stackIndicator->raise();
    }

    DropHandler m_dropHandler;
    QHBoxLayout* m_layout = nullptr;
    QVector<ToolbarPositionWidget*> m_positions;
    QFrame* m_positionIndicator = nullptr;
    QFrame* m_stackIndicator = nullptr;
    QColor m_surfaceColor;
    QColor m_accentColor;
    bool m_dragActive = false;
};

class ToolbarHiddenDropZone final : public QFrame {
  public:
    using DropHandler = std::function<void(const QString&, int)>;

    explicit ToolbarHiddenDropZone(DropHandler handler, QWidget* parent)
        : QFrame(parent), m_dropHandler(std::move(handler)) {
        setObjectName(QStringLiteral("settings-drawing-toolbar-hidden-zone"));
        setAcceptDrops(true);
        setAttribute(Qt::WA_StyledBackground, false);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumHeight(kHiddenZoneHeight);

        m_layout = new QHBoxLayout(this);
        m_layout->setContentsMargins(12, 10, 12, 10);
        m_layout->setSpacing(kToolbarPositionSpacing);
        m_layout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        m_emptyLabel = new QLabel(this);
        m_emptyLabel->setObjectName(QStringLiteral("settings-drawing-toolbar-hidden-empty"));
        m_emptyLabel->setAlignment(Qt::AlignCenter);
        m_layout->addWidget(m_emptyLabel, 1);
    }

    void setButtons(const QVector<ToolbarDragButton*>& buttons) {
        for (ToolbarDragButton* button : std::as_const(m_buttons)) {
            if (button != nullptr) {
                m_layout->removeWidget(button);
            }
        }
        m_buttons = buttons;
        m_layout->removeWidget(m_emptyLabel);
        m_emptyLabel->setVisible(m_buttons.isEmpty());
        if (m_buttons.isEmpty()) {
            m_layout->addWidget(m_emptyLabel, 1);
        } else {
            for (ToolbarDragButton* button : std::as_const(m_buttons)) {
                if (button == nullptr) {
                    continue;
                }
                button->setParent(this);
                button->show();
                m_layout->addWidget(button, 0, Qt::AlignVCenter);
            }
            m_layout->addStretch(1);
        }
        updateGeometry();
        update();
    }

    void releaseButtons(QWidget* parent) {
        while (m_layout->count() > 0) {
            QLayoutItem* item = m_layout->takeAt(0);
            if (ToolbarDragButton* button = dynamic_cast<ToolbarDragButton*>(item->widget())) {
                button->setParent(parent);
                button->hide();
            }
            delete item;
        }
        m_buttons.clear();
        m_emptyLabel->setParent(this);
        m_emptyLabel->hide();
    }

    void applyTheme(const QColor& background, const QColor& border, const QColor& accent,
                    const QColor& emptyText) {
        m_backgroundColor = background;
        m_borderColor = border;
        m_accentColor = accent;
        QPalette palette = m_emptyLabel->palette();
        palette.setColor(QPalette::WindowText, emptyText);
        m_emptyLabel->setPalette(palette);
        update();
    }

    void setEmptyText(const QString& text) {
        m_emptyLabel->setText(text);
    }

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (hasToolbarItem(event != nullptr ? event->mimeData() : nullptr)) {
            m_dragActive = true;
            update();
            event->acceptProposedAction();
            return;
        }
        QFrame::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (hasToolbarItem(event != nullptr ? event->mimeData() : nullptr)) {
            event->acceptProposedAction();
            return;
        }
        QFrame::dragMoveEvent(event);
    }

    void dragLeaveEvent(QDragLeaveEvent* event) override {
        m_dragActive = false;
        update();
        QFrame::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent* event) override {
        m_dragActive = false;
        update();
        if (!hasToolbarItem(event != nullptr ? event->mimeData() : nullptr)) {
            QFrame::dropEvent(event);
            return;
        }
        const QString itemId = QString::fromUtf8(event->mimeData()->data(kToolbarItemMimeType));
        if (toolbar_layout::descriptor(itemId) == nullptr) {
            event->ignore();
            return;
        }
        if (m_dropHandler) {
            m_dropHandler(itemId, insertionIndex(event->position().toPoint()));
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(m_backgroundColor.isValid() ? m_backgroundColor : QColor(Qt::transparent));
        QColor outline = m_dragActive && m_accentColor.isValid() ? m_accentColor : m_borderColor;
        painter.setPen(outline.isValid() ? QPen(outline, m_dragActive ? 1.5 : 1.0) : Qt::NoPen);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75),
                                kToolbarRadius, kToolbarRadius);
    }

  private:
    static bool hasToolbarItem(const QMimeData* mimeData) {
        return mimeData != nullptr && mimeData->hasFormat(kToolbarItemMimeType);
    }

    [[nodiscard]] int insertionIndex(const QPoint& position) const {
        for (int index = 0; index < m_buttons.size(); ++index) {
            const ToolbarDragButton* button = m_buttons.at(index);
            if (button != nullptr && position.x() < button->geometry().center().x()) {
                return index;
            }
        }
        return m_buttons.size();
    }

    DropHandler m_dropHandler;
    QHBoxLayout* m_layout = nullptr;
    QLabel* m_emptyLabel = nullptr;
    QVector<ToolbarDragButton*> m_buttons;
    QColor m_backgroundColor;
    QColor m_borderColor;
    QColor m_accentColor;
    bool m_dragActive = false;
};

void clearPosition(ToolbarPositionWidget* position, QWidget* buttonParent) {
    if (position == nullptr || position->contentLayout() == nullptr) {
        return;
    }
    while (QLayoutItem* item = position->contentLayout()->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->setParent(buttonParent);
            widget->hide();
        }
        delete item;
    }
}
} // namespace

class TrayMenuOptionsSettingsWidget final : public SettingsCustomWidget {
  public:
    TrayMenuOptionsSettingsWidget(
        const snow_shot::presentation::settings::SettingsCatalog& catalog,
        const snow_shot::presentation::settings::SettingsItemDefinition& definition,
        snow_shot::presentation::settings::SettingsRuntimeBindings& runtimeBindings,
        QWidget* parent = nullptr)
        : SettingsCustomWidget(parent), m_catalog(catalog), m_definition(definition),
          m_runtimeBindings(runtimeBindings) {
        initialize();
    }

    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) override {
        QFont titleFont = m_title->font();
        titleFont.setPixelSize(scheme.metricAlias.fontSizeLG);
        titleFont.setWeight(QFont::Medium);
        m_title->setFont(titleFont);
        QPalette titlePalette = m_title->palette();
        titlePalette.setColor(QPalette::WindowText, scheme.alias.textPrimary);
        m_title->setPalette(titlePalette);

        QFont descriptionFont = m_description->font();
        descriptionFont.setPixelSize(scheme.metricAlias.fontSize);
        descriptionFont.setWeight(QFont::Normal);
        m_description->setFont(descriptionFont);
        QPalette descriptionPalette = m_description->palette();
        descriptionPalette.setColor(QPalette::WindowText, scheme.alias.textMuted);
        m_description->setPalette(descriptionPalette);
    }

    void retranslateUi() override {
        if (m_title == nullptr) {
            return;
        }
        m_title->setText(m_definition.title.translated());
        m_description->setText(m_definition.description.translated());
        const auto groups = m_catalog.trayMenuGroups();
        for (const auto& group : groups) {
            for (const auto& option : group.options) {
                if (auto* checkbox = m_checkboxes.value(option.id)) {
                    const QString label = translatedOptionLabel(option);
                    checkbox->setText(label);
                    checkbox->setAccessibleName(label);
                }
            }
        }
    }

  protected:
    void changeEvent(QEvent* event) override {
        SettingsCustomWidget::changeEvent(event);
        if (event != nullptr && event->type() == QEvent::LanguageChange) {
            retranslateUi();
        }
    }

  private:
    QString translatedOptionLabel(
        const snow_shot::presentation::settings::SettingsTrayMenuOptionDefinition& option) const {
        if (option.kind ==
            snow_shot::presentation::settings::SettingsTrayMenuOptionKind::QuickAction) {
            const QString title = m_catalog.shortcutActionTitle(
                option.shortcutAction,
                m_runtimeBindings.integerValue(
                    snow_shot::presentation::settings::SettingsIntegerBinding::ScreenshotDelaySeconds));
            Q_ASSERT(!title.isEmpty());
            return title;
        }
        return option.label.translated();
    }

    void initialize() {
        setObjectName(QStringLiteral("settings-tray-menu-options"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(6);

        m_title = new QLabel(this);
        m_title->setObjectName(QStringLiteral("settings-tray-menu-options-title"));
        rootLayout->addWidget(m_title);

        m_description = new QLabel(this);
        m_description->setObjectName(QStringLiteral("settings-tray-menu-options-description"));
        m_description->setWordWrap(true);
        rootLayout->addWidget(m_description);

        auto* options = new QWidget(this);
        options->setObjectName(QStringLiteral("settings-tray-menu-options-grid"));
        auto* grid = new QGridLayout(options);
        grid->setContentsMargins(0, 6, 0, 0);
        grid->setHorizontalSpacing(
            snow_shot::presentation::styles::ThemeManager::instance()
                .themeColorScheme()
                .metricAlias.marginLG);
        grid->setVerticalSpacing(6);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);

        int row = 0;
        bool firstGroup = true;
        for (const auto& group : m_catalog.trayMenuGroups()) {
            if (group.options.isEmpty()) {
                continue;
            }
            if (!firstGroup) {
                auto* separator = new adqt::widgets::AdDivider(options);
                separator->setObjectName(
                    QStringLiteral("settings-tray-menu-options-separator-%1").arg(group.id));
                separator->setDividerSize(adqt::widgets::AdDivider::Size::Small);
                grid->addWidget(separator, row++, 0, 1, 2);
            }

            const int optionCount = static_cast<int>(group.options.size());
            for (int index = 0; index < optionCount; ++index) {
                const auto& option = group.options.at(index);
                auto* checkbox = new adqt::widgets::AdCheckbox(options);
                checkbox->setObjectName(
                    QStringLiteral("settings-tray-menu-option-%1").arg(option.id));
                checkbox->setProperty("trayMenuOptionId", option.id);
                checkbox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                m_checkboxes.insert(option.id, checkbox);
                connect(checkbox, &QAbstractButton::toggled, this,
                        [this](bool) { applySelection(); });
                grid->addWidget(checkbox, row + index / 2, index % 2);
            }
            row += (optionCount + 1) / 2;
            firstGroup = false;
        }
        rootLayout->addWidget(options);

        connect(&m_runtimeBindings,
                &snow_shot::presentation::settings::SettingsRuntimeBindings::synchronized, this,
                [this]() {
                    retranslateUi();
                    syncFromRuntime();
                });
        retranslateUi();
        applyTheme(snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme());
        syncFromRuntime();
    }

    void syncFromRuntime() {
        const QVariantList values = m_runtimeBindings.multiSelectValue(
            snow_shot::presentation::settings::SettingsMultiSelectBinding::TrayMenuOptions);
        QSet<QString> selected;
        for (const QVariant& value : values) {
            selected.insert(value.toString());
        }
        m_syncing = true;
        for (auto it = m_checkboxes.cbegin(); it != m_checkboxes.cend(); ++it) {
            const QSignalBlocker blocker(it.value());
            it.value()->setChecked(selected.contains(it.key()));
        }
        m_syncing = false;
    }

    void applySelection() {
        if (m_syncing) {
            return;
        }
        QVariantList values;
        for (const auto& group : m_catalog.trayMenuGroups()) {
            for (const auto& option : group.options) {
                const auto* checkbox = m_checkboxes.value(option.id);
                if (checkbox != nullptr && checkbox->isChecked()) {
                    values.push_back(option.id);
                }
            }
        }
        if (!m_runtimeBindings.applyMultiSelectValue(
                snow_shot::presentation::settings::SettingsMultiSelectBinding::TrayMenuOptions,
                values)) {
            syncFromRuntime();
        }
    }

    const snow_shot::presentation::settings::SettingsCatalog& m_catalog;
    const snow_shot::presentation::settings::SettingsItemDefinition& m_definition;
    snow_shot::presentation::settings::SettingsRuntimeBindings& m_runtimeBindings;
    QLabel* m_title = nullptr;
    QLabel* m_description = nullptr;
    QHash<QString, adqt::widgets::AdCheckbox*> m_checkboxes;
    bool m_syncing = false;
};

struct DrawingToolbarEditorSettingsWidget::Private {
    Private(DrawingToolbarEditorSettingsWidget& sourceOwner,
            snow_shot::presentation::settings::SettingsRuntimeBindings& sourceRuntimeBindings)
        : owner(sourceOwner), runtimeBindings(sourceRuntimeBindings),
          colorScheme(
              snow_shot::presentation::styles::ThemeManager::instance().themeColorScheme()) {}

    void initialize() {
        owner.setObjectName(QStringLiteral("settings-drawing-toolbar-editor"));
        auto* rootLayout = new QVBoxLayout(&owner);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(8);

        previewStage = new QWidget(&owner);
        previewStage->setObjectName(QStringLiteral("settings-drawing-toolbar-preview-stage"));
        previewStage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* previewLayout = new QHBoxLayout(previewStage);
        previewLayout->setContentsMargins(24, 24, 24, 28);
        previewLayout->setSpacing(0);
        previewLayout->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
        toolbarSurface = new ToolbarDropSurface(
            [this](const QString& itemId, const ToolbarDropSurface::DropLocation& location) {
                applyDrop(itemId, location);
            },
            previewStage);
        previewLayout->addWidget(toolbarSurface, 0, Qt::AlignHCenter | Qt::AlignBottom);
        rootLayout->addWidget(previewStage);

        instructionLabel = new QLabel(&owner);
        instructionLabel->setObjectName(QStringLiteral("settings-drawing-toolbar-instruction"));
        instructionLabel->setWordWrap(true);
        instructionLabel->setAlignment(Qt::AlignHCenter);
        rootLayout->addWidget(instructionLabel);

        hiddenSection = new QWidget(&owner);
        hiddenSection->setObjectName(QStringLiteral("settings-drawing-toolbar-hidden-section"));
        auto* hiddenLayout = new QVBoxLayout(hiddenSection);
        hiddenLayout->setContentsMargins(0, 8, 0, 0);
        hiddenLayout->setSpacing(4);
        hiddenTitleLabel = new QLabel(hiddenSection);
        hiddenTitleLabel->setObjectName(QStringLiteral("settings-drawing-toolbar-hidden-title"));
        hiddenLayout->addWidget(hiddenTitleLabel);
        hiddenDescriptionLabel = new QLabel(hiddenSection);
        hiddenDescriptionLabel->setObjectName(
            QStringLiteral("settings-drawing-toolbar-hidden-description"));
        hiddenDescriptionLabel->setWordWrap(true);
        hiddenLayout->addWidget(hiddenDescriptionLabel);
        hiddenZone = new ToolbarHiddenDropZone(
            [this](const QString& itemId, int index) { applyHiddenDrop(itemId, index); },
            hiddenSection);
        hiddenLayout->addWidget(hiddenZone);
        rootLayout->addWidget(hiddenSection);

        for (const toolbar_layout::Descriptor& descriptor : toolbar_layout::descriptors()) {
            const QString itemId = QString::fromLatin1(descriptor.id);
            auto* button = new ToolbarDragButton(itemId, &owner);
            button->setIconRef(toolbar_layout::icon(descriptor.icon));
            buttons.insert(itemId, button);
        }

        QObject::connect(&runtimeBindings,
                         &snow_shot::presentation::settings::SettingsRuntimeBindings::synchronized,
                         &owner, [this]() { syncFromRuntime(); });

        layout = toolbar_layout::normalizedLayout(runtimeBindings.toolbarLayout());
        owner.retranslateUi();
        owner.applyTheme(colorScheme);
        rebuild();
    }

    void rebuild() {
        toolbarSurface->hideIndicators();
        hiddenZone->releaseButtons(&owner);
        for (ToolbarPositionWidget* position : std::as_const(positionWidgets)) {
            clearPosition(position, &owner);
            toolbarSurface->contentLayout()->removeWidget(position);
            delete position;
        }
        positionWidgets.clear();

        for (ToolbarDragButton* button : std::as_const(buttons)) {
            button->setProperty("screenshotToolbarMainButton", false);
            button->hide();
        }
        for (int positionIndex = 0; positionIndex < layout.positions.size(); ++positionIndex) {
            const QStringList& itemIds = layout.positions.at(positionIndex);
            auto* position = new ToolbarPositionWidget(positionIndex, toolbarSurface);
            for (const QString& itemId : itemIds) {
                ToolbarDragButton* button = buttons.value(itemId);
                if (button == nullptr) {
                    continue;
                }
                button->show();
                position->contentLayout()->addWidget(button, 0, Qt::AlignHCenter);
            }
            if (position->contentLayout()->count() == 0) {
                delete position;
                continue;
            }
            if (QWidget* mainButton = position->contentLayout()
                                          ->itemAt(position->contentLayout()->count() - 1)
                                          ->widget()) {
                mainButton->setProperty("screenshotToolbarMainButton", true);
            }
            positionWidgets.push_back(position);
            toolbarSurface->contentLayout()->addWidget(position, 0,
                                                       Qt::AlignBottom | Qt::AlignHCenter);
        }
        toolbarSurface->setPositions(positionWidgets);
        toolbarSurface->contentLayout()->activate();
        toolbarSurface->adjustSize();
        QVector<ToolbarDragButton*> hiddenButtons;
        hiddenButtons.reserve(layout.hidden.size());
        for (const QString& itemId : layout.hidden) {
            if (ToolbarDragButton* button = buttons.value(itemId)) {
                hiddenButtons.push_back(button);
            }
        }
        hiddenZone->setButtons(hiddenButtons);
        previewStage->updateGeometry();
        owner.updateGeometry();
    }

    void syncFromRuntime() {
        const storage::ScreenshotToolbarLayout synchronized =
            toolbar_layout::normalizedLayout(runtimeBindings.toolbarLayout());
        if (synchronized != layout) {
            layout = synchronized;
            rebuild();
        }
    }

    void applyDrop(const QString& itemId, const ToolbarDropSurface::DropLocation& dropLocation) {
        if (toolbar_layout::descriptor(itemId) == nullptr) {
            return;
        }
        const storage::ScreenshotToolbarLayout previous = layout;
        storage::ScreenshotToolbarLayout candidate = previous;

        int sourcePositionIndex = -1;
        int sourceItemIndex = -1;
        for (int positionIndex = 0; positionIndex < candidate.positions.size(); ++positionIndex) {
            const int itemIndex = candidate.positions.at(positionIndex).indexOf(itemId);
            if (itemIndex >= 0) {
                sourcePositionIndex = positionIndex;
                sourceItemIndex = itemIndex;
                break;
            }
        }
        const int sourceHiddenIndex = candidate.hidden.indexOf(itemId);
        if (sourcePositionIndex < 0 && sourceHiddenIndex < 0) {
            return;
        }
        if (sourceHiddenIndex >= 0) {
            candidate.hidden.removeAt(sourceHiddenIndex);
        }

        if (dropLocation.kind == ToolbarDropSurface::DropKind::NewPosition) {
            int targetPositionIndex = dropLocation.positionIndex;
            if (sourcePositionIndex >= 0) {
                candidate.positions[sourcePositionIndex].removeAt(sourceItemIndex);
                if (candidate.positions.at(sourcePositionIndex).isEmpty()) {
                    candidate.positions.removeAt(sourcePositionIndex);
                    if (sourcePositionIndex < targetPositionIndex) {
                        --targetPositionIndex;
                    }
                }
            }
            targetPositionIndex =
                std::clamp(targetPositionIndex, 0, static_cast<int>(candidate.positions.size()));
            candidate.positions.insert(targetPositionIndex, QStringList{itemId});
        } else {
            if (candidate.positions.isEmpty()) {
                candidate.positions.push_back({itemId});
                persistLayout(previous, candidate);
                return;
            }
            int targetPositionIndex = std::clamp(dropLocation.positionIndex, 0,
                                                 static_cast<int>(candidate.positions.size()) - 1);
            int targetItemIndex = dropLocation.itemIndex;
            if (sourcePositionIndex >= 0 && sourcePositionIndex == targetPositionIndex) {
                candidate.positions[sourcePositionIndex].removeAt(sourceItemIndex);
                if (sourceItemIndex < targetItemIndex) {
                    --targetItemIndex;
                }
            } else if (sourcePositionIndex >= 0) {
                candidate.positions[sourcePositionIndex].removeAt(sourceItemIndex);
                if (candidate.positions.at(sourcePositionIndex).isEmpty()) {
                    candidate.positions.removeAt(sourcePositionIndex);
                    if (sourcePositionIndex < targetPositionIndex) {
                        --targetPositionIndex;
                    }
                }
            }
            targetItemIndex =
                std::clamp(targetItemIndex, 0,
                           static_cast<int>(candidate.positions.at(targetPositionIndex).size()));
            candidate.positions[targetPositionIndex].insert(targetItemIndex, itemId);
        }
        persistLayout(previous, candidate);
    }

    void applyHiddenDrop(const QString& itemId, int targetIndex) {
        if (toolbar_layout::descriptor(itemId) == nullptr) {
            return;
        }
        const storage::ScreenshotToolbarLayout previous = layout;
        storage::ScreenshotToolbarLayout candidate = previous;

        for (int positionIndex = 0; positionIndex < candidate.positions.size(); ++positionIndex) {
            const int itemIndex = candidate.positions.at(positionIndex).indexOf(itemId);
            if (itemIndex < 0) {
                continue;
            }
            candidate.positions[positionIndex].removeAt(itemIndex);
            if (candidate.positions.at(positionIndex).isEmpty()) {
                candidate.positions.removeAt(positionIndex);
            }
            break;
        }
        const int sourceHiddenIndex = candidate.hidden.indexOf(itemId);
        if (sourceHiddenIndex >= 0) {
            candidate.hidden.removeAt(sourceHiddenIndex);
            if (sourceHiddenIndex < targetIndex) {
                --targetIndex;
            }
        }
        targetIndex = std::clamp(targetIndex, 0, static_cast<int>(candidate.hidden.size()));
        candidate.hidden.insert(targetIndex, itemId);
        persistLayout(previous, candidate);
    }

    void persistLayout(const storage::ScreenshotToolbarLayout& previous,
                       storage::ScreenshotToolbarLayout candidate) {
        candidate = toolbar_layout::normalizedLayout(candidate);

        if (candidate == previous) {
            return;
        }
        if (!runtimeBindings.applyToolbarLayout(candidate)) {
            layout = previous;
            rebuild();
            return;
        }
        if (layout != candidate) {
            layout = candidate;
            rebuild();
        }
    }

    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
        colorScheme = scheme;
        const QColor border = scheme.map.colorBorder.isValid() ? scheme.map.colorBorder
                                                               : QColor(QStringLiteral("#D9D9D9"));
        const QColor surface =
            scheme.map.colorBgContainer.isValid() ? scheme.map.colorBgContainer : QColor(Qt::white);
        const QColor activeBorder = scheme.map.colorPrimary.isValid()
                                        ? scheme.map.colorPrimary
                                        : QColor(QStringLiteral("#1677FF"));
        Q_UNUSED(border);
        toolbarSurface->applyTheme(surface, activeBorder);
        const QColor hiddenBackground = scheme.map.colorFillSecondary.isValid()
                                            ? scheme.map.colorFillSecondary
                                            : QColor(0, 0, 0, 10);
        const QColor hiddenBorder = scheme.map.colorBorderSecondary.isValid()
                                        ? scheme.map.colorBorderSecondary
                                        : border;
        hiddenZone->applyTheme(hiddenBackground, hiddenBorder, activeBorder,
                               scheme.map.colorTextTertiary);

        QFont hiddenTitleFont = hiddenTitleLabel->font();
        hiddenTitleFont.setPixelSize(scheme.metricAlias.fontSizeLG);
        hiddenTitleFont.setWeight(QFont::DemiBold);
        hiddenTitleLabel->setFont(hiddenTitleFont);

        QPalette palette = instructionLabel->palette();
        palette.setColor(QPalette::WindowText, scheme.map.colorTextSecondary);
        instructionLabel->setPalette(palette);
        QPalette descriptionPalette = hiddenDescriptionLabel->palette();
        descriptionPalette.setColor(QPalette::WindowText, scheme.map.colorTextSecondary);
        hiddenDescriptionLabel->setPalette(descriptionPalette);
        owner.update();
    }

    void retranslateUi() {
        instructionLabel->setText(translatedToolbarText(
            "Drop beside a tool to create a position. Drop above a tool to stack it. The bottom "
            "tool stays on the main toolbar row."));
        toolbarSurface->setAccessibleName(translatedToolbarText("Drawing toolbar preview"));
        hiddenTitleLabel->setText(translatedToolbarText("Hidden tools"));
        hiddenDescriptionLabel->setText(translatedToolbarText(
            "Drag tools here to hide them from the screenshot toolbar."));
        hiddenZone->setEmptyText(translatedToolbarText("No hidden tools"));
        hiddenZone->setAccessibleName(translatedToolbarText("Hidden drawing toolbar tools"));
        for (const toolbar_layout::Descriptor& descriptor : toolbar_layout::descriptors()) {
            ToolbarDragButton* button = buttons.value(QString::fromLatin1(descriptor.id));
            if (button != nullptr) {
                const QString label = translatedToolbarText(descriptor.label);
                button->setToolTip(label);
                button->setAccessibleName(label);
            }
        }
    }

    DrawingToolbarEditorSettingsWidget& owner;
    snow_shot::presentation::settings::SettingsRuntimeBindings& runtimeBindings;
    snow_shot::presentation::styles::ThemeColorScheme colorScheme;
    storage::ScreenshotToolbarLayout layout;
    QWidget* previewStage = nullptr;
    QLabel* instructionLabel = nullptr;
    QWidget* hiddenSection = nullptr;
    QLabel* hiddenTitleLabel = nullptr;
    QLabel* hiddenDescriptionLabel = nullptr;
    ToolbarDropSurface* toolbarSurface = nullptr;
    ToolbarHiddenDropZone* hiddenZone = nullptr;
    QVector<ToolbarPositionWidget*> positionWidgets;
    QHash<QString, ToolbarDragButton*> buttons;
};

DrawingToolbarEditorSettingsWidget::DrawingToolbarEditorSettingsWidget(
    snow_shot::presentation::settings::SettingsRuntimeBindings& runtimeBindings, QWidget* parent)
    : SettingsCustomWidget(parent), m_private(std::make_unique<Private>(*this, runtimeBindings)) {
    m_private->initialize();
}

DrawingToolbarEditorSettingsWidget::~DrawingToolbarEditorSettingsWidget() = default;

void DrawingToolbarEditorSettingsWidget::applyTheme(
    const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    m_private->applyTheme(scheme);
}

void DrawingToolbarEditorSettingsWidget::retranslateUi() {
    m_private->retranslateUi();
}

void DrawingToolbarEditorSettingsWidget::changeEvent(QEvent* event) {
    SettingsCustomWidget::changeEvent(event);
    if (event != nullptr && event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

SettingsCustomWidget* createSettingsCustomWidget(
    snow_shot::presentation::settings::SettingsCustomRenderer renderer,
    const snow_shot::presentation::settings::SettingsCatalog& catalog,
    const snow_shot::presentation::settings::SettingsItemDefinition& definition,
    snow_shot::presentation::settings::SettingsRuntimeBindings& runtimeBindings, QWidget* parent) {
    using snow_shot::presentation::settings::SettingsCustomRenderer;
    switch (renderer) {
    case SettingsCustomRenderer::StorageStatus:
        return new StorageStatusSettingsWidget(runtimeBindings, parent);
    case SettingsCustomRenderer::DrawingToolbarEditor:
        return new DrawingToolbarEditorSettingsWidget(runtimeBindings, parent);
    case SettingsCustomRenderer::TrayMenuOptions:
        return new TrayMenuOptionsSettingsWidget(catalog, definition, runtimeBindings, parent);
    }
    return nullptr;
}
