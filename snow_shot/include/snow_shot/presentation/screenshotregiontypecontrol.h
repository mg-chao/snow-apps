#pragma once

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotregionpreferences.h"
#include "snow_shot/presentation/screenshotshortcuthints.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "widgets/button.h"

#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QVBoxLayout>

#include <array>
#include <algorithm>
#include <optional>

class ScreenshotRegionTypeControl final : public QWidget {
  public:
    explicit ScreenshotRegionTypeControl(QWidget* parent, bool showHint = false)
        : QWidget(parent), m_floating(showHint) {
        setObjectName(QStringLiteral("screenshotRegionTypeControl"));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(showHint ? 8 : 0, showHint ? 8 : 0, showHint ? 8 : 0,
                                   showHint ? 8 : 0);
        layout->setSpacing(6);
        auto* row = new QHBoxLayout;
        row->setSpacing(2);
        namespace icons = snow_shot::presentation::icons::custom::outlined;
        const std::array refs{icons::ScreenshotRegionRectangle(), icons::ScreenshotRegionPolyline(),
                              icons::ScreenshotRegionCurved(), icons::ScreenshotRegionFreehand()};
        for (int i = 0; i < 4; ++i) {
            auto* button = new adqt::widgets::AdButton(this);
            button->setObjectName(QStringLiteral("screenshotRegionType_") +
                                  screenshotRegionTypeId(ScreenshotRegionType(i)));
            button->setFixedSize(32, 32);
            button->setIconSize(QSize(24, 24));
            button->setIconRef(refs[static_cast<std::size_t>(i)]);
            button->setFocusPolicy(Qt::NoFocus);
            button->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            row->addWidget(button);
            m_buttons[static_cast<std::size_t>(i)] = button;
        }
        layout->addLayout(row);
        if (showHint) {
            m_hint = new QLabel(this);
            m_hint->setAlignment(Qt::AlignCenter);
            m_hint->setWordWrap(true);
            layout->addWidget(m_hint);
            if (QCoreApplication::instance() != nullptr)
                QCoreApplication::instance()->installEventFilter(this);
        }
        retranslate();
        setType(screenshotRegionPreference());
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        if (storage.isInitialized())
            connect(&storage.configuration(), &snow_shot::storage::ConfigurationStore::valueChanged,
                    this, [this](const QString& key, const QJsonValue& value) {
                        if (key == QStringLiteral("screenshot_selection/region_type"))
                            setType(screenshotRegionTypeFromId(value.toString()));
                    });
        if (m_floating) {
            const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
            connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged,
                    this, [this](const snow_shot::presentation::styles::ThemeColorScheme&) {
                        updateHintColor();
                        update();
                    });
        }
        updateHintColor();
    }
    QSize sizeHint() const override {
        const auto base = QWidget::sizeHint();
        if (!m_hint)
            return base;
        const int width =
            std::min(maximumWidth(),
                     std::max(base.width(),
                              m_hint->fontMetrics().boundingRect(m_hint->text()).width() + 32));
        return QSize(width, layout()->hasHeightForWidth() ? layout()->heightForWidth(width)
                                                          : base.height());
    }
    void setPresentationVisible(bool visible, const QRectF& selectionGlobal,
                                const QPointF& cursorGlobal) {
        m_requestedVisible = visible;
        m_selectionGlobal = selectionGlobal;
        refreshVisibility(cursorGlobal);
    }

    void refreshVisibility(const QPointF& cursorGlobal) {
        if (!m_floating || !m_requestedVisible) {
            if (!isHidden())
                hide();
            return;
        }
        const QRectF hintArea(mapToGlobal(QPoint()), size());
        const bool obscured =
            screenshotShortcutHintAreaIsObscured(hintArea, m_selectionGlobal, cursorGlobal);
        if (isHidden() != obscured) {
            setVisible(!obscured);
            if (!obscured)
                raise();
        }
    }
    void setType(ScreenshotRegionType type) {
        if (m_type == type)
            return;
        m_type = type;
        for (int i = 0; i < 4; ++i) {
            auto* button = m_buttons[static_cast<std::size_t>(i)];
            const bool selected = i == int(type);
            button->setButtonStyle(selected ? adqt::widgets::AdButton::ButtonStyle::Solid
                                            : adqt::widgets::AdButton::ButtonStyle::Text);
            button->setAccentRole(selected ? adqt::widgets::AdButton::AccentRole::Primary
                                           : adqt::widgets::AdButton::AccentRole::Neutral);
        }
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (m_floating && m_requestedVisible && event != nullptr &&
            event->type() == QEvent::MouseMove) {
            refreshVisibility(static_cast<QMouseEvent*>(event)->globalPosition());
        }
        return QWidget::eventFilter(watched, event);
    }
    void changeEvent(QEvent* event) override {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            retranslate();
    }
    void paintEvent(QPaintEvent* event) override {
        if (m_floating) {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
            painter.setPen(Qt::NoPen);
            painter.setBrush(scheme.map.colorBgContainer.isValid() ? scheme.map.colorBgContainer
                                                                   : QColor(Qt::white));
            painter.drawRoundedRect(QRectF(rect()), 8, 8);
        }
        QWidget::paintEvent(event);
    }

  private:
    void updateHintColor() {
        if (!m_hint)
            return;
        QPalette palette = m_hint->palette();
        const auto scheme = snow_shot::presentation::styles::generateThemeColorScheme();
        palette.setColor(QPalette::WindowText, scheme.map.colorTextSecondary);
        m_hint->setPalette(palette);
    }
    void retranslate() {
        const char* names[] = {QT_TRANSLATE_NOOP("ScreenshotRegionTypeControl", "Rectangle region"),
                               QT_TRANSLATE_NOOP("ScreenshotRegionTypeControl", "Polyline region"),
                               QT_TRANSLATE_NOOP("ScreenshotRegionTypeControl", "Curve region"),
                               QT_TRANSLATE_NOOP("ScreenshotRegionTypeControl", "Freehand region")};
        for (int i = 0; i < 4; ++i) {
            const auto text = QCoreApplication::translate("ScreenshotRegionTypeControl", names[i]);
            m_buttons[static_cast<std::size_t>(i)]->setToolTip(text);
            m_buttons[static_cast<std::size_t>(i)]->setAccessibleName(text);
        }
        if (m_hint)
            m_hint->setText(QCoreApplication::translate("ScreenshotRegionTypeControl",
                                                        "Ctrl+Tab to switch region type"));
        adjustSize();
    }
    std::array<adqt::widgets::AdButton*, 4> m_buttons{};
    QLabel* m_hint = nullptr;
    bool m_floating = false;
    bool m_requestedVisible = false;
    QRectF m_selectionGlobal;
    std::optional<ScreenshotRegionType> m_type;
};
