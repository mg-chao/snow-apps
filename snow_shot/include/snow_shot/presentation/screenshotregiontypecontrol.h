#pragma once

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/screenshotregionpreferences.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "widgets/button.h"

#include <QButtonGroup>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

#include <array>
#include <functional>

class ScreenshotRegionTypeControl final : public QWidget {
  public:
    explicit ScreenshotRegionTypeControl(QWidget* parent, bool showHint = false)
        : QWidget(parent), m_floating(showHint) {
        setObjectName(QStringLiteral("screenshotRegionTypeControl"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(showHint ? 8 : 0, showHint ? 8 : 0, showHint ? 8 : 0,
                                   showHint ? 8 : 0);
        layout->setSpacing(6);
        auto* row = new QHBoxLayout;
        row->setSpacing(2);
        auto* group = new QButtonGroup(this);
        group->setExclusive(true);
        namespace icons = snow_shot::presentation::icons::custom::outlined;
        const std::array refs{icons::ScreenshotRegionRectangle(), icons::ScreenshotRegionPolyline(),
                              icons::ScreenshotRegionCurved(), icons::ScreenshotRegionFreehand()};
        for (int i = 0; i < 4; ++i) {
            auto* button = new adqt::widgets::AdButton(this);
            button->setObjectName(QStringLiteral("screenshotRegionType_") +
                                  screenshotRegionTypeId(ScreenshotRegionType(i)));
            button->setCheckable(true);
            button->setFixedSize(32, 32);
            button->setIconSize(QSize(24, 24));
            button->setIconRef(refs[static_cast<std::size_t>(i)]);
            button->setFocusPolicy(showHint ? Qt::StrongFocus : Qt::NoFocus);
            group->addButton(button, i);
            row->addWidget(button);
            m_buttons[static_cast<std::size_t>(i)] = button;
            connect(button, &QPushButton::clicked, this, [this, i] {
                if (typeChanged)
                    typeChanged(ScreenshotRegionType(i));
            });
        }
        layout->addLayout(row);
        if (showHint) {
            m_hint = new QLabel(this);
            m_hint->setAlignment(Qt::AlignCenter);
            m_hint->setWordWrap(true);
            layout->addWidget(m_hint);
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
    std::function<void(ScreenshotRegionType)> typeChanged;
    void setType(ScreenshotRegionType type) {
        for (int i = 0; i < 4; ++i) {
            auto* button = m_buttons[static_cast<std::size_t>(i)];
            const bool selected = i == int(type);
            button->setChecked(selected);
            button->setButtonStyle(selected ? adqt::widgets::AdButton::ButtonStyle::Tonal
                                            : adqt::widgets::AdButton::ButtonStyle::Text);
            button->setAccentRole(selected ? adqt::widgets::AdButton::AccentRole::Primary
                                           : adqt::widgets::AdButton::AccentRole::Neutral);
        }
    }

  protected:
    void changeEvent(QEvent* event) override {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::LanguageChange)
            retranslate();
    }
    void paintEvent(QPaintEvent* event) override {
        if (m_floating) {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(palette().color(QPalette::Mid));
            painter.setBrush(palette().color(QPalette::Window));
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
        }
        QWidget::paintEvent(event);
    }

  private:
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
};
