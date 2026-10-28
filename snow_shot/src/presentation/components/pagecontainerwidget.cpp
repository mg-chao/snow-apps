#include "snow_shot/presentation/components/pagecontainerwidget.h"

#include "snow_shot/presentation/components/settingspageutils.h"

#include "widgets/scroll_area.h"

#include <QEvent>
#include <QPointer>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

namespace {
// Keep normal-flow geometry in the layout while offsetting the actual widget.
// QWidgetItem retains Qt's visibility, size-hint and layout invalidation handling;
// keeping the widget's parent stable also preserves focus and popup anchors.
class StickyPageItem final : public QObject, public QWidgetItem {
  public:
    StickyPageItem(QWidget* widget, QWidget* content, QWidget* viewport, int topMargin)
        : QWidgetItem(widget), m_widget(widget), m_viewport(viewport), m_topMargin(topMargin) {
        content->installEventFilter(this);
        viewport->installEventFilter(this);
    }

    void setGeometry(const QRect& rectangle) override {
        m_normalGeometry = rectangle;
        updatePosition();
    }

    QRect geometry() const override {
        return m_normalGeometry;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize) {
            updatePosition();
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    void updatePosition() {
        if (!m_widget || !m_viewport || m_widget->isHidden() || !m_normalGeometry.isValid()) {
            return;
        }
        const auto* content = m_widget->parentWidget();
        const int stickyTop = content->mapFrom(m_viewport, QPoint()).y() + m_topMargin;
        const int bottom = content->height() - content->layout()->contentsMargins().bottom() -
                           m_normalGeometry.height();
        QRect rectangle = m_normalGeometry;
        rectangle.moveTop(std::min(std::max(rectangle.top(), stickyTop), bottom));
        QWidgetItem::setGeometry(rectangle);
        m_widget->raise();
        const bool sticky = rectangle.top() != m_normalGeometry.top();
        if (m_widget->property("pageSticky").toBool() != sticky) {
            m_widget->setProperty("pageSticky", sticky);
            m_widget->update();
        }
    }

    QPointer<QWidget> m_widget;
    QPointer<QWidget> m_viewport;
    int m_topMargin;
    QRect m_normalGeometry;
};
} // namespace

PageContainerWidget::PageContainerWidget(
    const snow_shot::presentation::styles::ThemeAliasMetricToken& metric, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("pageContainer"));
    setAutoFillBackground(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_scrollArea = new adqt::widgets::AdScrollArea(this);
    m_scrollArea->setObjectName(QStringLiteral("pageScrollArea"));
    snow_shot::presentation::components::configureSettingsScrollArea(m_scrollArea, metric);

    m_contentWidget = new QWidget(m_scrollArea);
    m_contentWidget->setObjectName(QStringLiteral("pageContent"));
    m_contentWidget->setAutoFillBackground(false);
    m_contentLayout = new QVBoxLayout(m_contentWidget);
    m_contentLayout->setContentsMargins(metric.paddingLG, metric.paddingXXS, metric.paddingLG,
                                        metric.paddingLG);
    m_contentLayout->setSpacing(0);

    m_scrollArea->setContentWidget(m_contentWidget);
    rootLayout->addWidget(m_scrollArea, 1);
}

adqt::widgets::AdScrollArea* PageContainerWidget::scrollArea() const {
    return m_scrollArea;
}

QWidget* PageContainerWidget::contentWidget() const {
    return m_contentWidget;
}

QVBoxLayout* PageContainerWidget::contentLayout() const {
    return m_contentLayout;
}

void PageContainerWidget::addStickyWidget(QWidget* widget, int topMargin) {
    widget->setParent(m_contentWidget);
    m_contentLayout->addItem(
        new StickyPageItem(widget, m_contentWidget, m_scrollArea->viewport(), topMargin));
}
