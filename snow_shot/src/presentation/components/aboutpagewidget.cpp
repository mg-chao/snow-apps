#include "snow_shot/presentation/components/aboutpagewidget.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/presentation/components/pagecontainerwidget.h"
#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/update/updateservice.h"

#include "antd_icons.h"
#include "icon_renderer.h"
#include "widgets/button.h"
#include "widgets/button_style.h"
#include "widgets/divider.h"
#include "theme/theme.h"
#include "widgets/scroll_area.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>

// Force the artwork to be linked through the static settings library.
static void initializeAboutResources() {
    static const bool initialized = []() {
        Q_INIT_RESOURCE(snow_shot_about_assets);
        return true;
    }();
    Q_UNUSED(initialized)
}

namespace {
namespace styles = snow_shot::presentation::styles;
namespace custom = snow_shot::presentation::icons::custom;
namespace outlined = adqt::icons::antd::outlined;
using adqt::widgets::AdButton;
using adqt::widgets::AdDivider;

QLabel* aboutLabel(const QString& objectName, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

void styleAboutLabel(QLabel* label, int pixelSize, QFont::Weight weight, const QColor& color) {
    QFont font = label->font();
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    label->setFont(font);
    label->setForegroundRole(QPalette::WindowText);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, color);
    label->setPalette(palette);
}

QPixmap aboutIcon(const adqt::icons::IconRef& icon, const QSize& size, const QWidget* widget,
                  const QColor& color = {}) {
    adqt::icons::IconRenderRequest request;
    request.logicalSize = size;
    request.devicePixelRatio = widget->devicePixelRatioF();
    const auto colored =
        color.isValid() ? icon.withColors(adqt::icons::IconColors::primary(color)) : icon;
    return adqt::icons::renderIconPixmap(colored, request);
}

QColor blendAboutColor(const QColor& foreground, const QColor& background, qreal amount) {
    return QColor::fromRgbF(foreground.redF() * amount + background.redF() * (1 - amount),
                            foreground.greenF() * amount + background.greenF() * (1 - amount),
                            foreground.blueF() * amount + background.blueF() * (1 - amount));
}

class AboutHeroSurface final : public QFrame {
  public:
    explicit AboutHeroSurface(QWidget* parent) : QFrame(parent) {}

    void setTheme(const styles::ThemeColorScheme& scheme) {
        const bool dark = scheme.appearance == styles::ThemeAppearance::Dark;
        const QColor sky(dark ? "#497da9" : "#c5e3fa");
        m_start = blendAboutColor(sky, scheme.map.colorBgContainer, dark ? 0.22 : 0.30);
        m_end = blendAboutColor(sky, scheme.map.colorBgContainer, dark ? 0.05 : 0.04);
        m_radius = styles::buildMainWindowComponentMetricToken(scheme).cardRadius;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()), m_radius, m_radius);
        path.addRect(QRectF(0, m_radius, width(), height() - m_radius));
        path.setFillRule(Qt::WindingFill);
        QLinearGradient gradient(rect().topLeft(), QPointF(rect().left(), rect().bottom()));
        gradient.setColorAt(0, m_start);
        gradient.setColorAt(1, m_end);
        painter.fillPath(path, gradient);
    }

  private:
    QColor m_start;
    QColor m_end;
    int m_radius = 10;
};

class AboutArtwork final : public QWidget {
  public:
    explicit AboutArtwork(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("aboutArtwork"));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        QFile file(QStringLiteral(":/snow_shot/about/illustration.svg"));
        if (file.open(QIODevice::ReadOnly)) {
            m_source = file.readAll();
        }
    }

    void refresh(const styles::ThemeColorScheme& scheme) {
        QByteArray svg = m_source;
        const auto translated = [](const char* source) {
            return QCoreApplication::translate("AboutPageWidget", source).toHtmlEscaped().toUtf8();
        };
        svg.replace("{{product}}", translated(QT_TRANSLATE_NOOP("AboutPageWidget", "Snow Shot")));
        svg.replace("{{moment}}",
                    translated(QT_TRANSLATE_NOOP("AboutPageWidget", "Make every moment clear.")));
        svg.replace("{{ocr}}",
                    translated(QT_TRANSLATE_NOOP("AboutPageWidget", "Text recognition")));
        svg.replace("{{pixels}}", translated(QT_TRANSLATE_NOOP("AboutPageWidget",
                                                               "Turn pixels into usable text")));
        svg.replace("{{font}}", font().family().toHtmlEscaped().toUtf8());
        if (scheme.appearance == styles::ThemeAppearance::Dark) {
            // Scene colors complement the dark theme without changing the original brand mark.
            svg.replace("#DDEFFF", "#253E57");
            svg.replace("#F5F9FF", "#253349");
            svg.replace("#D3DEFA", "#485879");
            svg.replace("#B7CDEF", "#374C6A");
            svg.replace("#F8FBFF", "#9BACCD");
            svg.replace("#F3F8FF", "#526684");
            svg.replace("#E5D6FF", "#5B427B");
            svg.replace("#9254DE", "#B58AEC");
            svg.replace("#F3EBFC", "#443357");
            svg.replace("#ECF7E7", "#2C4333");
            svg.replace("#579C33", "#9AC776");
            svg.replace("#57714A", "#ADCE9B");
            svg.replace("#343C4C", "#E1E6F0");
            svg.replace("#34384A", "#E1E6F0");
            svg.replace("#717B8C", "#AEBBD0");
            svg.replace("#6A7890", "#AAB8CF");
            svg.replace("#D9E3EF", "#48566B");
            svg.replace("#EDF1F7", "#3C4A60");
            svg.replace("#E0E6EF", "#48566B");
            svg.replace("#E1E9F2", "#48566B");
            svg.replace("#8AA4BE", "#0B1220");
            svg.replace("fill=\"white\"", "fill=\"#263246\"");
        }
        m_renderer.load(svg);
        m_renderer.setAspectRatioMode(Qt::KeepAspectRatio);
        setAccessibleName(QCoreApplication::translate(
            "AboutPageWidget", "Screenshot selection, annotation tools, and recognized text"));
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        m_renderer.render(&painter, QRectF(rect()));
    }

  private:
    QByteArray m_source;
    QSvgRenderer m_renderer;
};

class AboutResourceButton final : public AdButton {
  public:
    AboutResourceButton(const QString& objectName, const adqt::icons::IconRef& icon,
                        QWidget* parent)
        : AdButton(parent), m_icon(icon) {
        setObjectName(objectName);
        setButtonStyle(ButtonStyle::Outline);
        setAccentRole(AccentRole::Neutral);
        setShape(Shape::Rounded);
        setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* row = new QHBoxLayout(this);
        m_iconLabel = new QLabel(this);
        row->addWidget(m_iconLabel, 0, Qt::AlignVCenter);
        auto* copy = new QVBoxLayout;
        copy->setSpacing(3);
        m_title = aboutLabel(objectName + QStringLiteral("Title"), this);
        m_description = aboutLabel(objectName + QStringLiteral("Description"), this);
        copy->addWidget(m_title);
        copy->addWidget(m_description);
        row->addLayout(copy, 1);
        m_arrow = new QLabel(this);
        row->addWidget(m_arrow, 0, Qt::AlignTop);
        for (QLabel* label : {m_iconLabel, m_title, m_description, m_arrow}) {
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
        }
    }

    void setCopy(const QString& title, const QString& description, const QUrl& url) {
        setAccessibleName(title);
        setAccessibleDescription(description);
        setToolTip(url.toDisplayString());
        m_title->setText(title);
        m_description->setText(description);
    }

    void setTheme(const styles::ThemeColorScheme& scheme) {
        m_scheme = scheme;
        const auto& metric = scheme.metricAlias;
        layout()->setContentsMargins(metric.paddingSM, metric.paddingXS, metric.paddingSM,
                                     metric.paddingXS);
        layout()->setSpacing(metric.paddingSM);
        styleAboutLabel(m_title, metric.fontSizeSM, QFont::DemiBold, scheme.map.colorText);
        styleAboutLabel(m_description, metric.fontSizeSM - 2, QFont::Normal, scheme.map.colorText);
        const QSize iconSize(metric.fontSizeLG, metric.fontSizeLG);
        m_iconLabel->setFixedSize(iconSize);
        m_iconLabel->setPixmap(aboutIcon(m_icon, iconSize, this, scheme.map.colorTextSecondary));
        m_arrow->setFixedSize(metric.fontSizeSM, metric.fontSizeSM);
        m_arrow->setPixmap(
            aboutIcon(outlined::Export(), m_arrow->size(), this, scheme.map.colorTextTertiary));
        m_contentColor = {};
        updateGeometry();
        update();
    }

    QSize sizeHint() const override {
        return layout()->sizeHint();
    }
    QSize minimumSizeHint() const override {
        return {0, layout()->minimumSize().height()};
    }

  protected:
    void enterEvent(QEnterEvent* event) override {
        m_hovered = true;
        AdButton::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        m_hovered = false;
        AdButton::leaveEvent(event);
    }

    void hideEvent(QHideEvent* event) override {
        m_hovered = false;
        AdButton::hideEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        // AdButton owns the surface, focus ring and click wave. Only the multiline
        // child content needs its colors synchronized with the standard button state.
        adqt::widgets::detail::ButtonStyleInput input;
        input.buttonStyle = buttonStyle();
        input.accentRole = accentRole();
        input.sizeClass = sizeClass();
        input.baseFont = font();
        const auto visual = adqt::widgets::detail::resolveButtonVisualStyle(
            input, adqt::theme::ThemeManager::instance().resolve(this));
        const auto& state = !isEnabled() ? visual.disabled
                            : isDown()   ? visual.active
                            : m_hovered  ? visual.hover
                                         : visual.normal;
        if (m_contentColor != state.text || m_contentDpr != devicePixelRatioF()) {
            m_contentColor = state.text;
            m_contentDpr = devicePixelRatioF();
            const auto& metric = m_scheme.metricAlias;
            styleAboutLabel(m_title, metric.fontSizeSM, QFont::DemiBold, state.text);
            styleAboutLabel(m_description, metric.fontSizeSM - 2, QFont::Normal, state.text);
            m_iconLabel->setPixmap(aboutIcon(m_icon, m_iconLabel->size(), this, state.text));
            m_arrow->setPixmap(aboutIcon(outlined::Export(), m_arrow->size(), this, state.text));
        }
        AdButton::paintEvent(event);
    }

  private:
    adqt::icons::IconRef m_icon;
    QLabel* m_iconLabel = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_description = nullptr;
    QLabel* m_arrow = nullptr;
    styles::ThemeColorScheme m_scheme;
    QColor m_contentColor;
    qreal m_contentDpr = 0;
    bool m_hovered = false;
};

QUrl aboutProjectUrl(const QString& suffix = {}) {
    QUrl url(QStringLiteral(SNOW_SHOT_PROJECT_URL));
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    url.setPath(path + suffix);
    return url;
}
} // namespace

struct AboutPageWidget::Ui {
    styles::ThemeColorScheme scheme;
    PageContainerWidget* container = nullptr;
    AboutHeroSurface* hero = nullptr;
    QBoxLayout* heroLayout = nullptr;
    QWidget* heroCopy = nullptr;
    QBoxLayout* identityLayout = nullptr;
    QLabel* logo = nullptr;
    QLabel* productName = nullptr;
    QLabel* openSource = nullptr;
    QLabel* tagline = nullptr;
    QLabel* description = nullptr;
    AboutArtwork* artwork = nullptr;
    QWidget* body = nullptr;
    QVBoxLayout* bodyLayout = nullptr;
    QGridLayout* featureLayout = nullptr;
    std::array<QWidget*, 6> features{};
    std::array<QLabel*, 6> featureIcons{};
    std::array<QLabel*, 6> featureLabels{};
    std::array<AdDivider*, 6> featureSeparators{};
    std::array<adqt::icons::IconRef, 6> featureRefs{
        custom::twotone::ScreenshotFeature(),  custom::outlined::ToolFreeDraw(),
        custom::outlined::ToolRecognizeText(), custom::outlined::RecordScreen(),
        custom::outlined::PinToScreen(),       outlined::History()};
    AdDivider* featureDivider = nullptr;
    QFrame* versionPanel = nullptr;
    QVBoxLayout* versionPanelLayout = nullptr;
    QBoxLayout* versionLayout = nullptr;
    QLabel* versionCaption = nullptr;
    QLabel* versionValue = nullptr;
    QLabel* previewBadge = nullptr;
    QBoxLayout* versionActions = nullptr;
    AdButton* releaseNotes = nullptr;
    AdButton* copyButton = nullptr;
    QTimer* copyFeedbackTimer = nullptr;
    snow_shot::update::UpdateService* updates = nullptr;
    QLabel* updateStatus = nullptr;
    QLabel* updateIcon = nullptr;
    AdDivider* updateDivider = nullptr;
    QBoxLayout* updateLayout = nullptr;
    QProgressBar* updateProgress = nullptr;
    AdButton* updateAction = nullptr;
    AdButton* updateCancel = nullptr;
    QBoxLayout* updateActions = nullptr;
    QGridLayout* resourceLayout = nullptr;
    std::array<AboutResourceButton*, 3> resources{};
    QLabel* linkError = nullptr;
    AdDivider* footerDivider = nullptr;
    QBoxLayout* footerLayout = nullptr;
    QLabel* community = nullptr;
    QLabel* heart = nullptr;
    QLabel* license = nullptr;
    QLabel* copyright = nullptr;
    QLabel* slogan = nullptr;
    int featureColumns = 0;
    int resourceColumns = 0;
};

AboutPageWidget::AboutPageWidget(QWidget* parent, UrlOpener urlOpener,
                                 snow_shot::update::UpdateService* updates)
    : QWidget(parent), m_version(QCoreApplication::applicationVersion()),
      m_urlOpener(urlOpener ? std::move(urlOpener) : QDesktopServices::openUrl),
      m_ui(std::make_unique<Ui>()) {
    initializeAboutResources();
    setObjectName(QStringLiteral("aboutPage"));
    const auto& themeManager = styles::ThemeManager::instance();
    m_ui->scheme = themeManager.themeColorScheme();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    m_ui->container = new PageContainerWidget(m_ui->scheme.metricAlias, this);
    root->addWidget(m_ui->container);
    auto* content = m_ui->container->contentWidget();
    auto* layout = m_ui->container->contentLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_ui->hero = new AboutHeroSurface(content);
    m_ui->hero->setObjectName(QStringLiteral("aboutHero"));
    m_ui->heroLayout = new QBoxLayout(QBoxLayout::LeftToRight, m_ui->hero);
    m_ui->heroCopy = new QWidget(m_ui->hero);
    m_ui->heroCopy->setMinimumWidth(0);
    m_ui->heroCopy->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* copyLayout = new QVBoxLayout(m_ui->heroCopy);
    copyLayout->setContentsMargins(0, 0, 0, 0);
    copyLayout->setAlignment(Qt::AlignVCenter);
    m_ui->identityLayout = new QBoxLayout(QBoxLayout::LeftToRight);
    m_ui->logo = new QLabel(m_ui->heroCopy);
    m_ui->logo->setObjectName(QStringLiteral("aboutLogo"));
    m_ui->identityLayout->addWidget(m_ui->logo, 0, Qt::AlignVCenter);
    m_ui->productName = aboutLabel(QStringLiteral("aboutProductName"), m_ui->heroCopy);
    m_ui->productName->setWordWrap(false);
    m_ui->productName->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    m_ui->identityLayout->addWidget(m_ui->productName, 0, Qt::AlignVCenter);
    m_ui->openSource = aboutLabel(QStringLiteral("aboutOpenSource"), m_ui->heroCopy);
    m_ui->openSource->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
    m_ui->identityLayout->addWidget(m_ui->openSource, 0, Qt::AlignVCenter);
    m_ui->identityLayout->addStretch(1);
    copyLayout->addLayout(m_ui->identityLayout);
    m_ui->tagline = aboutLabel(QStringLiteral("aboutTagline"), m_ui->heroCopy);
    m_ui->tagline->setTextFormat(Qt::RichText);
    copyLayout->addWidget(m_ui->tagline);
    m_ui->description = aboutLabel(QStringLiteral("aboutDescription"), m_ui->heroCopy);
    copyLayout->addWidget(m_ui->description);
    m_ui->heroLayout->addWidget(m_ui->heroCopy, 1);
    m_ui->artwork = new AboutArtwork(m_ui->hero);
    m_ui->heroLayout->addWidget(m_ui->artwork, 0, Qt::AlignCenter);
    layout->addWidget(m_ui->hero);

    m_ui->body = new QWidget(content);
    m_ui->bodyLayout = new QVBoxLayout(m_ui->body);
    m_ui->featureLayout = new QGridLayout;
    for (size_t i = 0; i < m_ui->features.size(); ++i) {
        auto* feature = new QWidget(m_ui->body);
        feature->setObjectName(QStringLiteral("aboutFeature%1").arg(i));
        feature->setMinimumWidth(0);
        auto* row = new QHBoxLayout(feature);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        auto* cell = new QWidget(feature);
        auto* featureLayout = new QVBoxLayout(cell);
        featureLayout->setContentsMargins(0, 0, 0, 0);
        m_ui->featureIcons[i] = new QLabel(cell);
        m_ui->featureIcons[i]->setObjectName(QStringLiteral("aboutFeatureIcon%1").arg(i));
        featureLayout->addWidget(m_ui->featureIcons[i], 0, Qt::AlignHCenter);
        m_ui->featureLabels[i] = aboutLabel(QStringLiteral("aboutFeatureLabel%1").arg(i), cell);
        m_ui->featureLabels[i]->setAlignment(Qt::AlignCenter);
        featureLayout->addWidget(m_ui->featureLabels[i]);
        row->addWidget(cell, 1);
        auto* separator = new AdDivider(feature);
        separator->setObjectName(QStringLiteral("aboutFeatureSeparator%1").arg(i));
        separator->setOrientation(AdDivider::Orientation::Vertical);
        separator->setDividerSize(AdDivider::Size::Small);
        // The dense feature grid keeps the minimal 3px rail box so labels keep their width.
        AdDivider::MetricTokens separatorMetrics;
        separatorMetrics.verticalMarginInline = 1;
        separator->setComponentTokens({{}, separatorMetrics});
        row->addWidget(separator, 0, Qt::AlignVCenter);
        m_ui->features[i] = feature;
        m_ui->featureSeparators[i] = separator;
    }
    m_ui->bodyLayout->addLayout(m_ui->featureLayout);
    m_ui->featureDivider = new AdDivider(m_ui->body);
    m_ui->featureDivider->setObjectName(QStringLiteral("aboutFeatureDivider"));
    m_ui->featureDivider->setDividerSize(AdDivider::Size::Small);
    m_ui->bodyLayout->addWidget(m_ui->featureDivider);

    m_ui->versionPanel = new QFrame(m_ui->body);
    m_ui->versionPanel->setObjectName(QStringLiteral("aboutVersionPanel"));
    m_ui->versionPanelLayout = new QVBoxLayout(m_ui->versionPanel);
    m_ui->versionLayout = new QBoxLayout(QBoxLayout::LeftToRight);
    m_ui->versionPanelLayout->addLayout(m_ui->versionLayout);
    auto* versionText = new QVBoxLayout;
    versionText->setSpacing(2);
    m_ui->versionCaption = aboutLabel(QStringLiteral("aboutVersionCaption"), m_ui->versionPanel);
    versionText->addWidget(m_ui->versionCaption);
    auto* versionValue = new QHBoxLayout;
    m_ui->versionValue = aboutLabel(QStringLiteral("aboutVersionValue"), m_ui->versionPanel);
    m_ui->versionValue->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                                Qt::TextSelectableByKeyboard);
    m_ui->versionValue->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    versionValue->addWidget(m_ui->versionValue);
    m_ui->previewBadge = aboutLabel(QStringLiteral("aboutPreviewBadge"), m_ui->versionPanel);
    m_ui->previewBadge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
    versionValue->addWidget(m_ui->previewBadge, 0, Qt::AlignVCenter);
    versionValue->addStretch(1);
    versionText->addLayout(versionValue);
    m_ui->versionLayout->addLayout(versionText, 1);
    m_ui->versionActions = new QBoxLayout(QBoxLayout::LeftToRight);
    m_ui->releaseNotes = new AdButton(m_ui->versionPanel);
    m_ui->releaseNotes->setObjectName(QStringLiteral("aboutReleaseNotes"));
    m_ui->releaseNotes->setButtonStyle(AdButton::ButtonStyle::Link);
    m_ui->releaseNotes->setIconRef(outlined::Export());
    m_ui->releaseNotes->setIconPosition(AdButton::IconPosition::Trailing);
    m_ui->releaseNotes->setSizeClass(AdButton::SizeClass::Small);
    m_ui->releaseNotes->setFocusPolicy(Qt::StrongFocus);
    m_ui->versionActions->addWidget(m_ui->releaseNotes);
    m_ui->copyButton = new AdButton(m_ui->versionPanel);
    m_ui->copyButton->setObjectName(QStringLiteral("aboutCopyVersion"));
    m_ui->copyButton->setButtonStyle(AdButton::ButtonStyle::Outline);
    m_ui->copyButton->setSizeClass(AdButton::SizeClass::Small);
    m_ui->copyButton->setFocusPolicy(Qt::StrongFocus);
    m_ui->versionActions->addWidget(m_ui->copyButton);
    m_ui->versionLayout->addLayout(m_ui->versionActions);
    m_ui->bodyLayout->addWidget(m_ui->versionPanel);

    m_ui->updates =
        updates != nullptr ? updates : qApp->findChild<snow_shot::update::UpdateService*>();
    if (m_ui->updates != nullptr) {
        m_ui->updateDivider = new AdDivider(m_ui->versionPanel);
        m_ui->updateDivider->setObjectName(QStringLiteral("aboutUpdateDivider"));
        m_ui->updateDivider->setDividerSize(AdDivider::Size::Small);
        m_ui->versionPanelLayout->addWidget(m_ui->updateDivider);
        m_ui->updateLayout = new QBoxLayout(QBoxLayout::LeftToRight);
        auto* statusRow = new QHBoxLayout;
        m_ui->updateIcon = new QLabel(m_ui->versionPanel);
        m_ui->updateIcon->setObjectName(QStringLiteral("aboutUpdateIcon"));
        statusRow->addWidget(m_ui->updateIcon, 0, Qt::AlignVCenter);
        m_ui->updateStatus = aboutLabel(QStringLiteral("aboutUpdateStatus"), m_ui->versionPanel);
        statusRow->addWidget(m_ui->updateStatus, 1);
        m_ui->updateLayout->addLayout(statusRow, 1);
        m_ui->versionPanelLayout->addLayout(m_ui->updateLayout);
        m_ui->updateProgress = new QProgressBar(m_ui->versionPanel);
        m_ui->updateProgress->setObjectName(QStringLiteral("aboutUpdateProgress"));
        m_ui->updateProgress->setRange(0, 1000);
        m_ui->updateProgress->setTextVisible(false);
        m_ui->versionPanelLayout->addWidget(m_ui->updateProgress);
        m_ui->updateActions = new QBoxLayout(QBoxLayout::LeftToRight);
        auto* actions = m_ui->updateActions;
        m_ui->updateAction = new AdButton(m_ui->versionPanel);
        m_ui->updateAction->setObjectName(QStringLiteral("aboutUpdateAction"));
        m_ui->updateCancel = new AdButton(m_ui->versionPanel);
        m_ui->updateCancel->setObjectName(QStringLiteral("aboutUpdateCancel"));
        actions->addWidget(m_ui->updateAction);
        actions->addWidget(m_ui->updateCancel);
        for (auto* button : {m_ui->updateAction, m_ui->updateCancel}) {
            button->setButtonStyle(AdButton::ButtonStyle::Outline);
            button->setSizeClass(AdButton::SizeClass::Small);
            button->setFocusPolicy(Qt::StrongFocus);
        }
        m_ui->updateLayout->addLayout(actions);
        connect(m_ui->updates, &snow_shot::update::UpdateService::statusChanged, this,
                &AboutPageWidget::refreshUpdateStatus);
        connect(m_ui->updateCancel, &adqt::widgets::AdButton::clicked, m_ui->updates,
                &snow_shot::update::UpdateService::cancel);
        connect(m_ui->updateAction, &adqt::widgets::AdButton::clicked, this, [this] {
            using snow_shot::update::UpdateState;
            switch (m_ui->updates->status().state) {
            case UpdateState::Available:
                m_ui->updates->download();
                break;
            case UpdateState::Ready:
                m_ui->updates->requestRestart();
                break;
            default:
                m_ui->updates->check();
                break;
            }
        });
    }

    m_ui->resourceLayout = new QGridLayout;
    m_ui->resources = {
        new AboutResourceButton(QStringLiteral("aboutWebsite"), outlined::Global(), m_ui->body),
        new AboutResourceButton(QStringLiteral("aboutSourceCode"), outlined::Code(), m_ui->body),
        new AboutResourceButton(QStringLiteral("aboutFeedback"), outlined::Comment(), m_ui->body)};
    m_ui->bodyLayout->addLayout(m_ui->resourceLayout);
    m_ui->linkError = aboutLabel(QStringLiteral("aboutLinkError"), m_ui->body);
    m_ui->linkError->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                             Qt::TextSelectableByKeyboard);
    m_ui->linkError->hide();
    m_ui->bodyLayout->addWidget(m_ui->linkError);
    m_ui->footerDivider = new AdDivider(m_ui->body);
    m_ui->footerDivider->setObjectName(QStringLiteral("aboutFooterDivider"));
    m_ui->footerDivider->setDividerSize(AdDivider::Size::Small);
    m_ui->bodyLayout->addWidget(m_ui->footerDivider);
    m_ui->footerLayout = new QBoxLayout(QBoxLayout::LeftToRight);
    auto* footerText = new QVBoxLayout;
    footerText->setSpacing(4);
    auto* community = new QHBoxLayout;
    m_ui->heart = new QLabel(m_ui->body);
    community->addWidget(m_ui->heart, 0, Qt::AlignTop);
    m_ui->community = aboutLabel(QStringLiteral("aboutCommunity"), m_ui->body);
    community->addWidget(m_ui->community, 1);
    footerText->addLayout(community);
    m_ui->license = aboutLabel(QStringLiteral("aboutLicense"), m_ui->body);
    footerText->addWidget(m_ui->license);
    m_ui->footerLayout->addLayout(footerText, 1);
    auto* legalWidget = new QWidget(m_ui->body);
    auto* legal = new QVBoxLayout(legalWidget);
    legal->setContentsMargins(0, 0, 0, 0);
    legal->setSpacing(4);
    m_ui->copyright = aboutLabel(QStringLiteral("aboutCopyright"), legalWidget);
    m_ui->copyright->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    legal->addWidget(m_ui->copyright);
    m_ui->slogan = aboutLabel(QStringLiteral("aboutSlogan"), legalWidget);
    m_ui->slogan->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    legal->addWidget(m_ui->slogan);
    m_ui->footerLayout->addWidget(legalWidget, 0, Qt::AlignTop);
    m_ui->bodyLayout->addLayout(m_ui->footerLayout);
    layout->addWidget(m_ui->body);
    layout->addStretch(1);

    m_ui->copyFeedbackTimer = new QTimer(this);
    m_ui->copyFeedbackTimer->setObjectName(QStringLiteral("aboutCopyFeedbackTimer"));
    m_ui->copyFeedbackTimer->setSingleShot(true);
    m_ui->copyFeedbackTimer->setInterval(2000);
    connect(m_ui->copyButton, &AdButton::clicked, this, [this]() {
        if (!m_version.trimmed().isEmpty()) {
            QApplication::clipboard()->setText(m_version);
            m_ui->copyFeedbackTimer->start();
            retranslateUi();
        }
    });
    connect(m_ui->copyFeedbackTimer, &QTimer::timeout, this, &AboutPageWidget::retranslateUi);
    connect(m_ui->releaseNotes, &AdButton::clicked, this,
            [this]() { openProjectLink(aboutProjectUrl(QStringLiteral("/releases"))); });
    connect(m_ui->resources[0], &QAbstractButton::clicked, this,
            [this]() { openProjectLink(QUrl(QStringLiteral(SNOW_SHOT_WEBSITE_URL))); });
    connect(m_ui->resources[1], &QAbstractButton::clicked, this,
            [this]() { openProjectLink(aboutProjectUrl()); });
    connect(m_ui->resources[2], &QAbstractButton::clicked, this,
            [this]() { openProjectLink(aboutProjectUrl(QStringLiteral("/issues"))); });
    connect(&themeManager, &styles::ThemeManager::themeChanged, this, &AboutPageWidget::applyTheme);
    applyTheme(m_ui->scheme);
    content->installEventFilter(this);
    m_ui->container->scrollArea()->viewport()->installEventFilter(this);
}

AboutPageWidget::~AboutPageWidget() {
    m_ui->container->contentWidget()->removeEventFilter(this);
    m_ui->container->scrollArea()->viewport()->removeEventFilter(this);
}

void AboutPageWidget::applyTheme(const styles::ThemeColorScheme& scheme) {
    m_ui->scheme = scheme;
    const auto& metric = scheme.metricAlias;
    const auto& colors = scheme.map;
    const QColor violet(scheme.appearance == styles::ThemeAppearance::Dark ? "#b58aec" : "#7052d8");
    m_ui->hero->setTheme(scheme);
    m_ui->heroLayout->setContentsMargins(metric.paddingLG, metric.paddingXXS, metric.paddingSM,
                                         metric.paddingXXS);
    m_ui->heroLayout->setSpacing(metric.paddingXS);
    m_ui->heroCopy->layout()->setSpacing(metric.paddingXS);
    m_ui->identityLayout->setSpacing(metric.paddingSM);
    m_ui->bodyLayout->setContentsMargins(metric.paddingLG, metric.paddingXXS, metric.paddingLG,
                                         metric.paddingXS);
    // Section dividers carry the Ant Design clearance in their own margins; the body only
    // spaces the divider-less junctions so the page keeps its default window height budget.
    m_ui->bodyLayout->setSpacing(0);
    m_ui->featureLayout->setHorizontalSpacing(metric.paddingXS);
    m_ui->featureLayout->setVerticalSpacing(metric.paddingSM);
    m_ui->resourceLayout->setSpacing(metric.paddingSM);
    m_ui->resourceLayout->setContentsMargins(0, metric.paddingXS, 0, 0);
    m_ui->linkError->setContentsMargins(0, metric.paddingXS, 0, metric.paddingXS);
    m_ui->footerLayout->setSpacing(metric.paddingSM);
    m_ui->versionPanelLayout->setContentsMargins(metric.padding, metric.paddingXXS, metric.padding,
                                                 metric.paddingXS);
    m_ui->versionPanelLayout->setSpacing(metric.paddingXXS);
    m_ui->versionLayout->setSpacing(metric.paddingSM);
    m_ui->versionActions->setSpacing(metric.paddingXS);
    if (m_ui->updateStatus != nullptr) {
        // The version card's stylesheet marks the divider's palette as customized, which would
        // reroute the rail through QPalette::Mid and fill the widget rect. Pin the theme split
        // color and a transparent root explicitly, and budget the card's height with a
        // half-token margin that doubles the updates module's clearance around the rail.
        AdDivider::ComponentTokens dividerTokens;
        dividerTokens.colors.splitColor = colors.colorSplit;
        dividerTokens.metrics.horizontalMarginSmall = metric.paddingXXS;
        m_ui->updateDivider->setComponentTokens(dividerTokens);
        AdDivider::SemanticStyles dividerSemantics;
        dividerSemantics.root.backgroundColor = Qt::transparent;
        m_ui->updateDivider->setSemanticStyles(dividerSemantics);
        m_ui->updateLayout->setSpacing(metric.paddingSM);
        m_ui->updateLayout->itemAt(0)->layout()->setSpacing(metric.paddingXS);
        m_ui->updateActions->setSpacing(metric.paddingXS);
        const int progressHeight = qMax(4, metric.paddingXXS);
        m_ui->updateProgress->setFixedHeight(progressHeight);
        m_ui->updateProgress->setStyleSheet(
            QStringLiteral("QProgressBar#aboutUpdateProgress { background: %1; border: none; "
                           "border-radius: %3px; } QProgressBar#aboutUpdateProgress::chunk { "
                           "background: %2; border-radius: %3px; }")
                .arg(colors.colorFillSecondary.name(QColor::HexArgb),
                     colors.colorPrimary.name(QColor::HexArgb))
                .arg(progressHeight / 2));
    }
    const QColor versionBackground =
        blendAboutColor(colors.colorBgLayout, colors.colorBgContainer, 0.8);
    m_ui->versionPanel->setStyleSheet(
        QStringLiteral("QFrame#aboutVersionPanel { background-color: %1; border: 1px solid %2; "
                       "border-radius: %3px; }")
            .arg(versionBackground.name(QColor::HexArgb),
                 colors.colorBorderSecondary.name(QColor::HexArgb))
            .arg(metric.borderRadiusLG));
    const QString badgeStyle =
        QStringLiteral("QLabel { background-color: %1; border: 1px solid %2; "
                       "border-radius: %3px; padding: 1px 8px; }")
            .arg(blendAboutColor(violet, colors.colorBgContainer, 0.08).name(QColor::HexArgb),
                 blendAboutColor(violet, colors.colorBgContainer, 0.22).name(QColor::HexArgb))
            .arg(metric.borderRadiusSM);
    m_ui->openSource->setStyleSheet(badgeStyle);
    m_ui->previewBadge->setStyleSheet(badgeStyle);
    styleAboutLabel(m_ui->openSource, metric.fontSizeSM - 2, QFont::Normal, violet);
    styleAboutLabel(m_ui->previewBadge, metric.fontSizeSM - 2, QFont::Normal, violet);
    styleAboutLabel(m_ui->productName, metric.fontSizeXL, QFont::DemiBold, colors.colorText);
    styleAboutLabel(m_ui->tagline, metric.fontSizeXL, QFont::DemiBold, colors.colorText);
    styleAboutLabel(m_ui->description, metric.fontSizeSM, QFont::Normal, colors.colorTextSecondary);
    styleAboutLabel(m_ui->versionCaption, metric.fontSizeSM, QFont::Normal,
                    colors.colorTextTertiary);
    styleAboutLabel(m_ui->versionValue, metric.fontSizeLG, QFont::DemiBold, colors.colorText);
    styleAboutLabel(m_ui->community, metric.fontSizeSM, QFont::Normal, colors.colorTextSecondary);
    styleAboutLabel(m_ui->copyright, metric.fontSizeSM, QFont::Normal, colors.colorTextSecondary);
    for (QLabel* label : {m_ui->license, m_ui->slogan}) {
        styleAboutLabel(label, metric.fontSizeSM - 2, QFont::Normal, colors.colorTextTertiary);
    }
    styleAboutLabel(m_ui->linkError, metric.fontSizeSM, QFont::Normal, colors.colorErrorText);
    const int logoSize = metric.controlHeight;
    m_ui->logo->setFixedSize(logoSize, logoSize);
    m_ui->logo->setPixmap(aboutIcon(custom::app::ApplicationIcon(), m_ui->logo->size(), this));
    m_ui->heart->setFixedSize(metric.fontSizeSM, metric.fontSizeSM);
    m_ui->heart->setPixmap(aboutIcon(outlined::Heart(), m_ui->heart->size(), this, violet));
    const std::array<QColor, 6> tones{violet,
                                      colors.colorWarningText,
                                      colors.colorSuccessText,
                                      colors.colorErrorText,
                                      colors.colorInfoText,
                                      colors.colorTextSecondary};
    for (size_t i = 0; i < m_ui->features.size(); ++i) {
        m_ui->featureIcons[i]->parentWidget()->layout()->setContentsMargins(metric.paddingXS, 0,
                                                                            metric.paddingXS, 0);
        m_ui->featureIcons[i]->parentWidget()->layout()->setSpacing(metric.paddingXXS);
        m_ui->featureIcons[i]->setFixedSize(metric.fontSizeLG, metric.fontSizeLG);
        m_ui->featureIcons[i]->setPixmap(
            aboutIcon(m_ui->featureRefs[i], m_ui->featureIcons[i]->size(), this, tones[i]));
        styleAboutLabel(m_ui->featureLabels[i], metric.fontSizeSM, QFont::Normal, colors.colorText);
    }
    for (auto* resource : m_ui->resources) {
        resource->setTheme(scheme);
    }
    retranslateUi();
}

void AboutPageWidget::retranslateUi() {
    refreshUpdateStatus();
    const bool hasVersion = !m_version.trimmed().isEmpty();
    setAccessibleName(tr("About Snow Shot"));
    m_ui->productName->setText(tr("Snow Shot"));
    m_ui->logo->setAccessibleName(tr("Snow Shot logo"));
    m_ui->openSource->setText(tr("Free · Open source"));
    const QColor violet(m_ui->scheme.appearance == styles::ThemeAppearance::Dark ? "#b58aec"
                                                                                 : "#7052d8");
    m_ui->tagline->setText(QStringLiteral("<span style=\"color:%1\">%2</span>%3")
                               .arg(violet.name(), tr("Elegant screenshots").toHtmlEscaped(),
                                    tr(", excellent work.").toHtmlEscaped()));
    m_ui->description->setText(
        tr("Capture, annotate, recognize text, and record your screen,\n"
           "so every moment on screen can be expressed clearly and shared easily."));
    const std::array<QString, 6> features{tr("Screenshot capture"), tr("Easy annotation"),
                                          tr("Text recognition"),   tr("Screen recording"),
                                          tr("Pin to screen"),      tr("Screenshot history")};
    for (size_t i = 0; i < features.size(); ++i) {
        m_ui->featureLabels[i]->setText(features[i]);
    }
    m_ui->versionCaption->setText(tr("Current version"));
    m_ui->versionValue->setText(hasVersion ? m_version : tr("Unavailable"));
    m_ui->versionValue->setAccessibleName(
        tr("Current version: %1").arg(m_ui->versionValue->text()));
    m_ui->previewBadge->setText(tr("Preview"));
    static const QRegularExpression prerelease(
        QStringLiteral("^\\d+\\.\\d+\\.\\d+-[^+]+(?:\\+.*)?$"));
    m_ui->previewBadge->setVisible(prerelease.match(m_version).hasMatch());
    m_ui->copyButton->setEnabled(hasVersion);
    m_ui->copyButton->setText(m_ui->copyFeedbackTimer->isActive() ? tr("Copied")
                                                                  : tr("Copy version number"));
    m_ui->copyButton->setAccessibleName(tr("Copy version number"));
    m_ui->copyButton->setToolTip(tr("Copy the version number to the clipboard"));
    m_ui->copyButton->setIconRef(m_ui->copyFeedbackTimer->isActive() ? outlined::Check()
                                                                     : outlined::Copy());
    m_ui->releaseNotes->setText(tr("Changelog"));
    m_ui->releaseNotes->setAccessibleName(tr("Changelog"));
    m_ui->releaseNotes->setToolTip(aboutProjectUrl(QStringLiteral("/releases")).toDisplayString());
    m_ui->resources[0]->setCopy(tr("Official website"),
                                tr("Discover more features and ways to use it"),
                                QUrl(QStringLiteral(SNOW_SHOT_WEBSITE_URL)));
    m_ui->resources[1]->setCopy(tr("GitHub"), tr("View the source and improve it together"),
                                aboutProjectUrl());
    m_ui->resources[2]->setCopy(tr("Feedback and suggestions"),
                                tr("Make the next experience better"),
                                aboutProjectUrl(QStringLiteral("/issues")));
    m_ui->community->setText(tr("Built for daily work, and growing with the community."));
    m_ui->license->setText(
        tr("%1 · %2").arg(tr("GNU General Public License v3.0 or later"),
                          tr("Free and open-source software. Distributed without any warranty.")));
    m_ui->copyright->setText(
        tr("Copyright © %1 %2").arg(QStringLiteral("2025–2026"), QStringLiteral("mg-chao")));
    m_ui->slogan->setText(tr("Snow Shot · Make expression clearer"));
    m_ui->linkError->setText(m_failedUrl.isEmpty()
                                 ? QString()
                                 : tr("Could not open the link. Open %1 in your browser.")
                                       .arg(m_failedUrl.toDisplayString()));
    m_ui->linkError->setVisible(!m_failedUrl.isEmpty());
    m_ui->artwork->refresh(m_ui->scheme);
    updateLayout();
}

void AboutPageWidget::updateLayout() {
    const auto& metric = m_ui->scheme.metricAlias;
    const qreal scale = metric.fontSize / 14.0;
    const int width = m_ui->container->contentWidget()->width();
    const bool wide = width >= qRound(600 * scale);
    m_ui->heroLayout->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    const int availableWidth = std::max(1, width - m_ui->heroLayout->contentsMargins().left() -
                                               m_ui->heroLayout->contentsMargins().right());
    const int artWidth =
        std::min(availableWidth,
                 qRound((wide ? std::clamp(width / scale * 0.29, 170.0, 218.0) : 210.0) * scale));
    const bool tiny = width < qRound(350 * scale);
    m_ui->identityLayout->setDirection(tiny ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_ui->versionLayout->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    const int versionWidth = width - m_ui->bodyLayout->contentsMargins().left() -
                             m_ui->bodyLayout->contentsMargins().right() -
                             m_ui->versionPanelLayout->contentsMargins().left() -
                             m_ui->versionPanelLayout->contentsMargins().right();
    const int versionActionsWidth = m_ui->releaseNotes->sizeHint().width() +
                                    m_ui->copyButton->sizeHint().width() +
                                    m_ui->versionActions->spacing();
    // An oversized minimum width makes Qt calculate wrapped text heights at the wrong width.
    // Stack the version actions before they can force the shared card wider than its viewport.
    m_ui->versionActions->setDirection(tiny || versionWidth < versionActionsWidth
                                           ? QBoxLayout::TopToBottom
                                           : QBoxLayout::LeftToRight);
    if (m_ui->updateActions != nullptr) {
        const bool stackUpdate =
            tiny || versionWidth < m_ui->updateActions->sizeHint().width() + qRound(220 * scale);
        m_ui->updateLayout->setDirection(stackUpdate ? QBoxLayout::TopToBottom
                                                     : QBoxLayout::LeftToRight);
        m_ui->updateActions->setDirection(tiny ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    }
    m_ui->footerLayout->setDirection(wide ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    m_ui->copyright->setWordWrap(!wide);
    m_ui->slogan->setWordWrap(!wide);
    const auto footerAlign = wide ? Qt::AlignRight | Qt::AlignTop : Qt::AlignLeft | Qt::AlignTop;
    m_ui->copyright->setAlignment(footerAlign);
    m_ui->slogan->setAlignment(footerAlign);
    const int featureColumns = wide ? 6 : (tiny ? 2 : 3);
    if (m_ui->featureColumns != featureColumns) {
        for (int i = 0; i < 6; ++i) {
            m_ui->featureLayout->setColumnStretch(i, i < featureColumns ? 1 : 0);
            m_ui->featureLayout->addWidget(m_ui->features[static_cast<size_t>(i)],
                                           i / featureColumns, i % featureColumns);
        }
        m_ui->featureColumns = featureColumns;
    }
    const int separatorHeight = qMax(metric.fontSizeXL + metric.fontSizeSM + metric.paddingXXS,
                                     m_ui->featureIcons.front()->height() + metric.paddingSM);
    for (int i = 0; i < 6; ++i) {
        auto* separator = m_ui->featureSeparators[static_cast<size_t>(i)];
        const bool lastInRow = featureColumns <= 1 || (i % featureColumns) == featureColumns - 1;
        separator->setVisible(!lastInRow);
        separator->setFixedHeight(separatorHeight);
    }
    const int resourceColumns = wide ? 3 : 1;
    if (m_ui->resourceColumns != resourceColumns) {
        for (int i = 0; i < 3; ++i) {
            m_ui->resourceLayout->setColumnStretch(i, i < resourceColumns ? 1 : 0);
            m_ui->resourceLayout->addWidget(m_ui->resources[static_cast<size_t>(i)],
                                            i / resourceColumns, i % resourceColumns);
        }
        m_ui->resourceColumns = resourceColumns;
    }

    int artHeight = qRound(artWidth * 340.0 / 460.0);
    if (wide) {
        // The scroll area uses the content's preferred height. Budget decorative artwork
        // against the viewport so changes in text metrics do not force unnecessary scrolling.
        const int bodyHeight = m_ui->body->hasHeightForWidth() ? m_ui->body->heightForWidth(width)
                                                               : m_ui->body->sizeHint().height();
        const int copyWidth = qMax(1, availableWidth - artWidth - m_ui->heroLayout->spacing());
        const int copyHeight = m_ui->heroCopy->hasHeightForWidth()
                                   ? m_ui->heroCopy->heightForWidth(copyWidth)
                                   : m_ui->heroCopy->sizeHint().height();
        const auto margins = m_ui->heroLayout->contentsMargins();
        const int availableHeight = m_ui->container->scrollArea()->viewport()->height() -
                                    bodyHeight - margins.top() - margins.bottom();
        artHeight = qMin(artHeight, qMax(copyHeight, availableHeight));
    }
    m_ui->artwork->setFixedSize(artWidth, artHeight);
}

void AboutPageWidget::openProjectLink(const QUrl& url) {
    m_failedUrl = m_urlOpener(url) ? QUrl() : url;
    retranslateUi();
    if (!m_failedUrl.isEmpty()) {
        // Let the newly visible error take its place in the responsive layout before scrolling.
        QTimer::singleShot(0, this, [this]() {
            if (!m_failedUrl.isEmpty()) {
                m_ui->container->scrollArea()->ensureWidgetVisible(m_ui->linkError);
            }
        });
    }
}

void AboutPageWidget::refreshUpdateStatus() {
    if (m_ui->updates == nullptr || m_ui->updateStatus == nullptr) {
        return;
    }
    using snow_shot::update::UpdateState;
    const auto& status = m_ui->updates->status();
    const auto& colors = m_ui->scheme.map;
    QColor statusColor = colors.colorTextSecondary;
    auto statusIcon = outlined::InfoCircle();
    auto actionIcon = outlined::Sync();
    QString text;
    QString action = tr("Check for updates");
    switch (status.state) {
    case UpdateState::Unavailable:
        text = tr("Automatic updates are unavailable for this copy.");
        break;
    case UpdateState::Idle:
        text = status.version.isEmpty() ? tr("Check for a newer version of Snow Shot.")
                                        : tr("You are up to date.");
        if (!status.version.isEmpty()) {
            statusIcon = outlined::CheckCircle();
            statusColor = colors.colorSuccessText;
        }
        break;
    case UpdateState::Checking:
        text = tr("Checking for updates…");
        statusIcon = outlined::Sync();
        break;
    case UpdateState::Available:
        text = tr("Update available: %1").arg(status.version);
        action = tr("Download update");
        statusIcon = outlined::CloudDownload();
        actionIcon = outlined::Download();
        statusColor = colors.colorInfoText;
        break;
    case UpdateState::Downloading:
        text = status.total > 0 ? tr("Downloading %1 of %2 MB")
                                      .arg(status.received / 1048576)
                                      .arg(status.total / 1048576)
                                : tr("Downloading update…");
        statusIcon = outlined::CloudDownload();
        statusColor = colors.colorInfoText;
        break;
    case UpdateState::Verifying:
        text = tr("Verifying update…");
        statusIcon = outlined::SafetyCertificate();
        break;
    case UpdateState::Ready:
        text = tr("Ready to install %1").arg(status.version);
        action = tr("Restart and update");
        statusIcon = outlined::CheckCircle();
        actionIcon = outlined::Reload();
        statusColor = colors.colorSuccessText;
        break;
    case UpdateState::Applying:
        text = tr("Preparing to restart and update…");
        statusIcon = outlined::Sync();
        break;
    case UpdateState::Failed:
        text = tr("Update failed: %1").arg(status.error);
        statusIcon = outlined::ExclamationCircle();
        statusColor = colors.colorErrorText;
        break;
    }
    if (!status.error.isEmpty() && status.state == UpdateState::Ready) {
        text += u'\n' + status.error;
        statusIcon = outlined::ExclamationCircle();
        statusColor = colors.colorWarningText;
    }
    styleAboutLabel(m_ui->updateStatus, m_ui->scheme.metricAlias.fontSizeSM, QFont::Normal,
                    statusColor);
    const int iconSize = m_ui->scheme.metricAlias.fontSize;
    m_ui->updateIcon->setFixedSize(iconSize, iconSize);
    m_ui->updateIcon->setPixmap(aboutIcon(statusIcon, m_ui->updateIcon->size(), this, statusColor));
    m_ui->updateStatus->setText(text);
    m_ui->updateStatus->setAccessibleName(text);
    m_ui->updateAction->setText(action);
    m_ui->updateAction->setAccessibleName(action);
    m_ui->updateAction->setIconRef(actionIcon);
    const bool primaryAction =
        status.state == UpdateState::Available || status.state == UpdateState::Ready;
    m_ui->updateAction->setButtonStyle(primaryAction ? AdButton::ButtonStyle::Solid
                                                     : AdButton::ButtonStyle::Outline);
    m_ui->updateAction->setAccentRole(primaryAction ? AdButton::AccentRole::Primary
                                                    : AdButton::AccentRole::Neutral);
    m_ui->updateAction->setEnabled(
        status.state == UpdateState::Idle || status.state == UpdateState::Failed ||
        status.state == UpdateState::Available || status.state == UpdateState::Ready);
    m_ui->updateAction->setVisible(m_ui->updateAction->isEnabled());
    m_ui->updateCancel->setText(tr("Cancel download"));
    m_ui->updateCancel->setVisible(status.state == UpdateState::Downloading);
    m_ui->updateProgress->setVisible(status.state == UpdateState::Downloading);
    m_ui->updateProgress->setRange(0, status.total > 0 ? 1000 : 0);
    m_ui->updateProgress->setValue(
        status.total > 0
            ? qRound(std::clamp(static_cast<double>(status.received) / status.total, 0.0, 1.0) *
                     1000)
            : 0);
    m_ui->updateProgress->setAccessibleName(tr("Update download progress"));
    updateLayout();
}

void AboutPageWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (m_ui && event->type() == QEvent::LanguageChange) {
        retranslateUi();
    } else if (m_ui && event->type() == QEvent::DevicePixelRatioChange) {
        applyTheme(m_ui->scheme);
    }
}

bool AboutPageWidget::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == m_ui->container->contentWidget() ||
         watched == m_ui->container->scrollArea()->viewport()) &&
        event->type() == QEvent::Resize) {
        updateLayout();
    }
    return QWidget::eventFilter(watched, event);
}
