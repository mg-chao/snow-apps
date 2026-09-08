#include "snow_shot/presentation/components/aboutpagewidget.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/components/pagecontainerwidget.h"
#include "snow_shot/presentation/styles/thememanager.h"

#include "antd_icons.h"
#include "icon_renderer.h"
#include "widgets/button.h"
#include "widgets/descriptions.h"

#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int VERSION_ACTION_INLINE_MIN_WIDTH = 480;

QLabel* aboutLabel(const QString& objectName, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return label;
}

void styleAboutLabel(QLabel* label, int pixelSize, QFont::Weight weight, const QColor& color) {
    QFont font = label->font();
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    label->setFont(font);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, color);
    label->setPalette(palette);
}
} // namespace

AboutPageWidget::AboutPageWidget(QWidget* parent)
    : QWidget(parent), m_version(QCoreApplication::applicationVersion()) {
    setObjectName(QStringLiteral("aboutPage"));
    const auto& themeManager = snow_shot::presentation::styles::ThemeManager::instance();
    const auto scheme = themeManager.themeColorScheme();
    const auto& metric = scheme.metricAlias;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    m_container = new PageContainerWidget(metric, this);
    root->addWidget(m_container);
    auto* content = m_container->contentWidget();
    auto* layout = m_container->contentLayout();

    auto* identity = new QHBoxLayout;
    m_logo = new QLabel(content);
    m_logo->setObjectName(QStringLiteral("aboutLogo"));
    m_logo->setFixedSize(64, 64);
    identity->addWidget(m_logo, 0, Qt::AlignTop);
    auto* identityText = new QVBoxLayout;
    identityText->setSpacing(metric.paddingXS);
    m_productName = aboutLabel(QStringLiteral("aboutProductName"), content);
    m_description = aboutLabel(QStringLiteral("aboutDescription"), content);
    identityText->addWidget(m_productName);
    identityText->addWidget(m_description);
    identity->addLayout(identityText, 1);
    identity->setSpacing(metric.padding);
    layout->addLayout(identity);

    m_versionPanel = new QFrame(content);
    m_versionPanel->setObjectName(QStringLiteral("aboutVersionPanel"));
    m_versionLayout = new QBoxLayout(QBoxLayout::LeftToRight, m_versionPanel);
    auto* versionTextLayout = new QVBoxLayout;
    versionTextLayout->setSpacing(metric.paddingXS);
    m_versionCaption = aboutLabel(QStringLiteral("aboutVersionCaption"), m_versionPanel);
    m_versionValue = aboutLabel(QStringLiteral("aboutVersionValue"), m_versionPanel);
    m_versionValue->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                            Qt::TextSelectableByKeyboard);
    versionTextLayout->addWidget(m_versionCaption);
    versionTextLayout->addWidget(m_versionValue);
    m_versionLayout->addLayout(versionTextLayout, 1);

    m_copyButton = new adqt::widgets::AdButton(m_versionPanel);
    m_copyButton->setObjectName(QStringLiteral("aboutCopyVersion"));
    m_copyButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Outline);
    m_copyButton->setFocusPolicy(Qt::StrongFocus);
    m_versionLayout->addWidget(m_copyButton, 0, Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(m_versionPanel);

    auto* footerLayout = new QVBoxLayout;
    footerLayout->setSpacing(metric.paddingXS);
    m_details = new adqt::widgets::AdDescriptions(content);
    m_details->setObjectName(QStringLiteral("aboutDetails"));
    m_details->setColumn(1);
    adqt::widgets::AdDescriptions::Item license;
    license.key = QStringLiteral("license");
    m_details->addItem(license);
    footerLayout->addWidget(m_details);

    m_licenseNote = aboutLabel(QStringLiteral("aboutLicenseNote"), content);
    footerLayout->addWidget(m_licenseNote);
    m_copyright = aboutLabel(QStringLiteral("aboutCopyright"), content);
    footerLayout->addWidget(m_copyright);
    layout->addLayout(footerLayout);
    layout->addStretch(1);

    m_copyFeedbackTimer = new QTimer(this);
    m_copyFeedbackTimer->setObjectName(QStringLiteral("aboutCopyFeedbackTimer"));
    m_copyFeedbackTimer->setSingleShot(true);
    m_copyFeedbackTimer->setInterval(2000);
    connect(m_copyButton, &adqt::widgets::AdButton::clicked, this, [this]() {
        if (m_version.trimmed().isEmpty()) {
            return;
        }
        QApplication::clipboard()->setText(m_version);
        m_copyFeedbackTimer->start();
        retranslateUi();
    });
    connect(m_copyFeedbackTimer, &QTimer::timeout, this, &AboutPageWidget::retranslateUi);
    connect(&themeManager, &snow_shot::presentation::styles::ThemeManager::themeChanged, this,
            &AboutPageWidget::applyTheme);
    retranslateUi();
    applyTheme(scheme);
}

void AboutPageWidget::applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
    const auto& metric = scheme.metricAlias;
    const auto& colors = scheme.map;
    m_container->contentLayout()->setContentsMargins(metric.paddingLG, metric.paddingLG,
                                                     metric.paddingLG, metric.paddingLG);
    m_container->contentLayout()->setSpacing(metric.paddingLG);
    m_versionPanel->layout()->setContentsMargins(metric.paddingLG, metric.padding, metric.paddingLG,
                                                 metric.padding);
    m_versionPanel->layout()->setSpacing(metric.paddingSM);
    m_versionPanel->setStyleSheet(
        QStringLiteral("QFrame#aboutVersionPanel { background-color: %1; border: %2px solid %3; "
                       "border-radius: %4px; }")
            .arg(colors.colorPrimaryBg.name(QColor::HexArgb))
            .arg(metric.lineWidth)
            .arg(colors.colorPrimaryBorder.name(QColor::HexArgb))
            .arg(metric.borderRadiusLG));

    styleAboutLabel(m_productName, metric.fontSizeHeading2, QFont::DemiBold, colors.colorText);
    styleAboutLabel(m_description, metric.fontSize, QFont::Normal, colors.colorTextSecondary);
    styleAboutLabel(m_versionCaption, metric.fontSize, QFont::Normal, colors.colorTextSecondary);
    styleAboutLabel(m_versionValue, metric.fontSizeHeading2, QFont::DemiBold, colors.colorText);
    styleAboutLabel(m_licenseNote, metric.fontSize, QFont::Normal, colors.colorTextSecondary);
    styleAboutLabel(m_copyright, metric.fontSizeSM, QFont::Normal, colors.colorTextTertiary);

    adqt::icons::IconRenderRequest request;
    request.logicalSize = m_logo->size();
    request.devicePixelRatio = devicePixelRatioF();
    m_logo->setPixmap(adqt::icons::renderIconPixmap(
        snow_shot::presentation::icons::custom::app::ApplicationIcon(), request));
}

void AboutPageWidget::retranslateUi() {
    const bool hasVersion = !m_version.trimmed().isEmpty();
    setAccessibleName(tr("About Snow Shot"));
    m_productName->setText(tr("Snow Shot"));
    m_description->setText(tr("Capture, annotate, and share your screen."));
    m_logo->setAccessibleName(tr("Snow Shot logo"));
    m_versionCaption->setText(tr("Installed version"));
    m_versionValue->setText(hasVersion ? m_version : tr("Unavailable"));
    m_versionValue->setAccessibleName(tr("Installed version: %1").arg(m_versionValue->text()));
    m_copyButton->setEnabled(hasVersion);
    m_copyButton->setText(m_copyFeedbackTimer->isActive() ? tr("Copied") : tr("Copy version"));
    m_copyButton->setAccessibleName(tr("Copy version"));
    m_copyButton->setToolTip(tr("Copy the version number to the clipboard"));
    m_copyButton->setIconRef(m_copyFeedbackTimer->isActive() ? adqt::icons::antd::outlined::Check()
                                                             : adqt::icons::antd::outlined::Copy());
    m_details->setItemLabel(0, tr("License"));
    m_details->setItemContent(0, tr("GNU General Public License v3.0 or later"));
    m_licenseNote->setText(tr("Free and open-source software. Distributed without any warranty."));
    m_copyright->setText(
        tr("Copyright © %1 %2").arg(QStringLiteral("2025–2026"), QStringLiteral("mg-chao")));
}

void AboutPageWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
}

void AboutPageWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    m_versionLayout->setDirection(width() < VERSION_ACTION_INLINE_MIN_WIDTH
                                      ? QBoxLayout::TopToBottom
                                      : QBoxLayout::LeftToRight);
}
