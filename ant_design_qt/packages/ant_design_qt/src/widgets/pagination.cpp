#include "pagination.h"
#include "detail/focus_reason.h"

#include "antd_icons.h"
#include "combo_box.h"
#include "input_line_edit.h"
#include "pagination_style.h"
#include "theme/theme_manager.h"

#include <QAbstractButton>
#include <QBoxLayout>
#include <QEnterEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QIntValidator>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionFocusRect>

#include <algorithm>
#include <limits>
#include <utility>

namespace adqt::widgets {

QRectF detail::deviceAlignedPaginationBorderRect(const QRectF& bounds, qreal borderWidth, qreal dpr,
                                                 const QPointF& origin) {
  if (dpr <= 0.0) {
    const qreal inset = borderWidth / 2.0;
    return bounds.adjusted(inset, inset, -inset, -inset);
  }

  const qreal left = qCeil((bounds.left() + origin.x()) * dpr) / dpr - origin.x();
  const qreal top = qCeil((bounds.top() + origin.y()) * dpr) / dpr - origin.y();
  const qreal right = qFloor((bounds.right() + origin.x()) * dpr) / dpr - origin.x();
  const qreal bottom = qFloor((bounds.bottom() + origin.y()) * dpr) / dpr - origin.y();
  const qreal inset = borderWidth / 2.0;

  return QRectF(left, top, std::max<qreal>(0.0, right - left), std::max<qreal>(0.0, bottom - top))
      .adjusted(inset, inset, -inset, -inset);
}

namespace {

namespace outlined_icons = adqt::icons::antd::outlined;

using detail::PaginationVisualStyle;

QPointF widgetPaintOrigin(const QWidget* widget) {
  if (!widget) {
    return QPointF();
  }

  const QWidget* const hostWindow = widget->window();
  if (!hostWindow) {
    return QPointF();
  }

  return QPointF(widget->mapTo(hostWindow, QPoint(0, 0)));
}

class PaginationButton final : public QAbstractButton {
 public:
  explicit PaginationButton(QWidget* parent) : QAbstractButton(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  }

  int targetPage() const { return targetPage_; }

  void setItem(AdPagination::ItemType type, int targetPage, const QString& text) {
    type_ = type;
    targetPage_ = targetPage;
    text_ = text;
  }

  void setAppearance(const PaginationVisualStyle& style, bool active) {
    style_ = style;
    active_ = active;
    const bool isPage = type_ == AdPagination::ItemType::Page;
    setCheckable(isPage);
    setChecked(isPage && active);
    setFont(style.font);
    setFixedSize(style.itemSize, style.itemSize);
    update();
  }

 protected:
  void nextCheckState() override {
    // The pagination model owns the current-page state; activation must not toggle it locally.
  }

  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool hover = underMouse() && isEnabled();
    QColor background = style_.itemBackground;
    QColor foreground = isEnabled() ? style_.text : style_.disabledText;
    QColor border = Qt::transparent;

    if (active_) {
      background = isEnabled() ? style_.activeBackground : style_.activeDisabledBackground;
      foreground =
          isEnabled() ? (hover ? style_.activeHoverText : style_.activeText) : style_.disabledText;
      border =
          isEnabled() ? (hover ? style_.activeHoverText : style_.activeBorder) : Qt::transparent;
    } else if (isDown() && isEnabled()) {
      background = style_.itemPressedBackground;
    } else if (hover) {
      background = style_.itemHoverBackground;
    }

    const qreal dpr =
        painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();
    const QPointF paintOrigin = widgetPaintOrigin(this);
    const bool hasVisibleBorder = style_.borderWidth > 0 && border.alpha() > 0;
    const qreal borderInset = style_.borderWidth / 2.0;
    const QRectF itemRect =
        hasVisibleBorder
            ? detail::deviceAlignedPaginationBorderRect(QRectF(rect()), style_.borderWidth, dpr,
                                                        paintOrigin)
            : QRectF(rect()).adjusted(borderInset, borderInset, -borderInset, -borderInset);
    QPainterPath itemPath;
    itemPath.addRoundedRect(itemRect, style_.radius, style_.radius);
    painter.fillPath(itemPath, background);
    if (hasVisibleBorder) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(border, style_.borderWidth, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
      painter.drawPath(itemPath);
    }

    if (hasFocus() && detail::isKeyboardFocusReason(focusReason_)) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(style_.focusOutline, 2));
      painter.drawRoundedRect(
          detail::deviceAlignedPaginationBorderRect(QRectF(rect()), 2.0, dpr, paintOrigin),
          style_.radius, style_.radius);
    }

    if (!text_.isEmpty()) {
      QFont drawFont = font();
      drawFont.setBold(active_);
      painter.setFont(drawFont);
      painter.setPen(foreground);
      painter.drawText(rect(), Qt::AlignCenter, text_);
      return;
    }

    if ((type_ == AdPagination::ItemType::JumpPrevious ||
         type_ == AdPagination::ItemType::JumpNext) &&
        !hover) {
      painter.setPen(foreground);
      painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("..."));
      return;
    }

    const adqt::icons::IconColors colors = adqt::icons::IconColors::primary(foreground);
    const bool rtl = layoutDirection() == Qt::RightToLeft;
    adqt::icons::IconRef icon;
    switch (type_) {
      case AdPagination::ItemType::Previous:
        icon = rtl ? outlined_icons::Right(colors) : outlined_icons::Left(colors);
        break;
      case AdPagination::ItemType::Next:
        icon = rtl ? outlined_icons::Left(colors) : outlined_icons::Right(colors);
        break;
      case AdPagination::ItemType::JumpPrevious:
        icon = rtl ? outlined_icons::DoubleRight(colors) : outlined_icons::DoubleLeft(colors);
        break;
      case AdPagination::ItemType::JumpNext:
        icon = rtl ? outlined_icons::DoubleLeft(colors) : outlined_icons::DoubleRight(colors);
        break;
      case AdPagination::ItemType::Page:
        break;
    }
    if (!icon.isValid()) {
      return;
    }
    const int iconSide = std::max(12, std::min(16, style_.itemSize - 8));
    const QPixmap pixmap = adqt::icons::renderIconPixmap(
        icon, {QSize(iconSide, iconSide), devicePixelRatioF(),
               isEnabled() ? QIcon::Normal : QIcon::Disabled, QIcon::Off});
    const QPoint topLeft((width() - iconSide) / 2, (height() - iconSide) / 2);
    painter.drawPixmap(topLeft, pixmap);
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
      click();
      event->accept();
      return;
    }
    QAbstractButton::keyPressEvent(event);
  }

  void focusInEvent(QFocusEvent* event) override {
    focusReason_ = event->reason();
    QAbstractButton::focusInEvent(event);
    update();
  }

  void focusOutEvent(QFocusEvent* event) override {
    QAbstractButton::focusOutEvent(event);
    update();
  }

  void enterEvent(QEnterEvent* event) override {
    QAbstractButton::enterEvent(event);
    update();
  }

  void leaveEvent(QEvent* event) override {
    QAbstractButton::leaveEvent(event);
    update();
  }

 private:
  AdPagination::ItemType type_ = AdPagination::ItemType::Page;
  int targetPage_ = 1;
  QString text_;
  PaginationVisualStyle style_;
  bool active_ = false;
  Qt::FocusReason focusReason_ = Qt::OtherFocusReason;
};

struct PageItemSpec {
  AdPagination::ItemType type = AdPagination::ItemType::Page;
  int page = 1;
};

QVector<PageItemSpec> pageItems(int current, int allPages, bool showLessItems) {
  QVector<PageItemSpec> items;
  const int buffer = showLessItems ? 1 : 2;
  if (allPages <= 3 + buffer * 2) {
    if (allPages == 0) {
      items.append({AdPagination::ItemType::Page, 1});
    } else {
      for (int page = 1; page <= allPages; ++page) {
        items.append({AdPagination::ItemType::Page, page});
      }
    }
    return items;
  }

  int left = std::max(1, current - buffer);
  int right = static_cast<int>(std::min<qint64>(static_cast<qint64>(current) + buffer, allPages));
  if (current - 1 <= buffer) {
    right = 1 + buffer * 2;
  }
  if (allPages - current <= buffer) {
    left = allPages - buffer * 2;
  }

  if (left != 1) {
    items.append({AdPagination::ItemType::Page, 1});
  }
  if (current - 1 >= buffer * 2 && current != 3) {
    items.append(
        {AdPagination::ItemType::JumpPrevious, std::max(1, current - (showLessItems ? 3 : 5))});
  }
  for (int page = left; page <= right; ++page) {
    items.append({AdPagination::ItemType::Page, page});
  }
  if (allPages - current >= buffer * 2 && current != allPages - 2) {
    const int jump = showLessItems ? 3 : 5;
    items.append(
        {AdPagination::ItemType::JumpNext,
         static_cast<int>(std::min<qint64>(allPages, static_cast<qint64>(current) + jump))});
  }
  if (right != allPages) {
    items.append({AdPagination::ItemType::Page, allPages});
  }
  return items;
}

QString defaultAccessibleName(AdPagination::ItemType type, int page) {
  switch (type) {
    case AdPagination::ItemType::Previous:
      return AdPagination::tr("Previous page");
    case AdPagination::ItemType::Next:
      return AdPagination::tr("Next page");
    case AdPagination::ItemType::JumpPrevious:
      return AdPagination::tr("Previous %1 pages").arg(page);
    case AdPagination::ItemType::JumpNext:
      return AdPagination::tr("Next %1 pages").arg(page);
    case AdPagination::ItemType::Page:
    default:
      return AdPagination::tr("Page %1").arg(page);
  }
}

}  // namespace

struct AdPagination::Private {
  explicit Private(AdPagination* owner) : q(owner) {}

  AdPagination* q = nullptr;
  QHBoxLayout* layout = nullptr;
  QLabel* totalLabel = nullptr;
  QLabel* quickLabel = nullptr;
  AdLineEdit* quickInput = nullptr;
  QLabel* quickSuffixLabel = nullptr;
  AdComboBox* sizeChanger = nullptr;
  QLabel* simpleCurrentLabel = nullptr;
  AdLineEdit* simpleInput = nullptr;
  QLabel* simpleSlashLabel = nullptr;
  QLabel* simplePageCountLabel = nullptr;
  QVector<PaginationButton*> buttons;

  int currentPage = 1;
  int total = 0;
  int pageSize = 10;
  ControlSize controlSize = ControlSize::Medium;
  ControlSize lastEffectiveSize = ControlSize::Medium;
  Alignment alignment = Alignment::Start;
  SizeChangerMode sizeChangerMode = SizeChangerMode::Auto;
  int totalBoundaryShowSizeChanger = 50;
  QVector<int> pageSizeOptions = {10, 20, 50, 100};
  bool hideOnSinglePage = false;
  bool showQuickJumper = false;
  bool showLessItems = false;
  bool showTitle = true;
  bool simple = false;
  bool simpleReadOnly = false;
  bool responsive = false;
  bool rebuilding = false;
  bool lastDisabled = false;
  TotalTextFormatter totalFormatter;
  ItemTextFormatter itemFormatter;
  ComponentTokens componentTokens;
  SemanticStyles semanticStyles;
  PaginationVisualStyle visualStyle;
};

AdPagination::AdPagination(QWidget* parent) : QWidget(parent), d_(std::make_unique<Private>(this)) {
  d_->lastDisabled = disabled();
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setAccessibleName(tr("Pagination"));
  connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
          [this]() { rebuild(); });

  d_->layout = new QHBoxLayout(this);
  d_->layout->setContentsMargins(0, 0, 0, 0);

  d_->totalLabel = new QLabel(this);
  d_->totalLabel->setObjectName(QStringLiteral("paginationTotal"));

  d_->quickLabel = new QLabel(tr("Go to"), this);
  d_->quickLabel->setObjectName(QStringLiteral("paginationQuickLabel"));
  d_->quickInput = new AdLineEdit(this);
  d_->quickInput->setObjectName(QStringLiteral("paginationQuickInput"));
  d_->quickInput->setAccessibleName(tr("Page number"));
  d_->quickInput->setValidator(
      new QIntValidator(1, std::numeric_limits<int>::max(), d_->quickInput));
  d_->quickSuffixLabel = new QLabel(tr("Page"), this);
  d_->quickSuffixLabel->setObjectName(QStringLiteral("paginationQuickSuffix"));
  connect(d_->quickInput, &QLineEdit::editingFinished, this, [this]() {
    if (!d_->rebuilding && !d_->quickInput->text().isEmpty()) {
      navigateFromUser(d_->quickInput->text().toInt());
      d_->quickInput->clear();
    }
  });

  d_->sizeChanger = new AdComboBox(this);
  d_->sizeChanger->setObjectName(QStringLiteral("paginationSizeChanger"));
  d_->sizeChanger->setAccessibleName(tr("Items per page"));
  d_->sizeChanger->setPopupWidthMode(AdComboBox::PopupWidthMode::ContentWidth);
  connect(d_->sizeChanger, &AdComboBox::currentValueChanged, this, [this](const QVariant& value) {
    if (!d_->rebuilding && value.isValid()) {
      applyPageSizeFromUser(value.toInt());
    }
  });

  d_->simpleCurrentLabel = new QLabel(this);
  d_->simpleCurrentLabel->setObjectName(QStringLiteral("paginationSimpleCurrent"));
  d_->simpleInput = new AdLineEdit(this);
  d_->simpleInput->setObjectName(QStringLiteral("paginationSimpleInput"));
  d_->simpleInput->setAccessibleName(tr("Page number"));
  d_->simpleInput->setAlignment(Qt::AlignCenter);
  d_->simpleInput->setValidator(
      new QIntValidator(1, std::numeric_limits<int>::max(), d_->simpleInput));
  connect(d_->simpleInput, &QLineEdit::editingFinished, this, [this]() {
    if (!d_->rebuilding && !d_->simpleInput->text().isEmpty()) {
      navigateFromUser(d_->simpleInput->text().toInt());
    }
  });
  d_->simpleSlashLabel = new QLabel(QStringLiteral("/"), this);
  d_->simplePageCountLabel = new QLabel(this);

  rebuild();
}

AdPagination::~AdPagination() = default;

int AdPagination::currentPage() const { return d_->currentPage; }

void AdPagination::setCurrentPage(int value) {
  const int bounded = boundedPage(value);
  if (d_->currentPage == bounded) {
    return;
  }
  d_->currentPage = bounded;
  emit currentPageChanged(bounded);
  rebuild();
}

int AdPagination::total() const { return d_->total; }

void AdPagination::setTotal(int value) {
  value = std::max(0, value);
  if (d_->total == value) {
    return;
  }
  const int oldPageCount = pageCount();
  d_->total = value;
  emit totalChanged(value);
  const int bounded = boundedPage(d_->currentPage);
  if (bounded != d_->currentPage) {
    d_->currentPage = bounded;
    emit currentPageChanged(bounded);
  }
  if (pageCount() != oldPageCount) {
    emit pageCountChanged(pageCount());
  }
  rebuild();
}

int AdPagination::pageSize() const { return d_->pageSize; }

void AdPagination::setPageSize(int value) {
  value = std::max(1, value);
  if (d_->pageSize == value) {
    return;
  }
  const int oldPageCount = pageCount();
  d_->pageSize = value;
  emit pageSizeChanged(value);
  const int bounded = boundedPage(d_->currentPage);
  if (bounded != d_->currentPage) {
    d_->currentPage = bounded;
    emit currentPageChanged(bounded);
  }
  if (pageCount() != oldPageCount) {
    emit pageCountChanged(pageCount());
  }
  rebuild();
}

int AdPagination::pageCount() const {
  return d_->total == 0 ? 0 : (d_->total - 1) / d_->pageSize + 1;
}

AdPagination::Range AdPagination::visibleItemRange() const {
  if (d_->total == 0) {
    return {};
  }
  const qint64 first = static_cast<qint64>(d_->currentPage - 1) * d_->pageSize + 1;
  const qint64 last =
      std::min<qint64>(d_->total, static_cast<qint64>(d_->currentPage) * d_->pageSize);
  return {static_cast<int>(first), static_cast<int>(last)};
}

AdPagination::ControlSize AdPagination::controlSize() const { return d_->controlSize; }

void AdPagination::setControlSize(ControlSize value) {
  if (d_->controlSize == value) {
    return;
  }
  d_->controlSize = value;
  emit controlSizeChanged(value);
  rebuild();
}

AdPagination::Alignment AdPagination::alignment() const { return d_->alignment; }

void AdPagination::setAlignment(Alignment value) {
  if (d_->alignment == value) {
    return;
  }
  d_->alignment = value;
  emit alignmentChanged(value);
  rebuild();
}

AdPagination::SizeChangerMode AdPagination::sizeChangerMode() const { return d_->sizeChangerMode; }

void AdPagination::setSizeChangerMode(SizeChangerMode value) {
  if (d_->sizeChangerMode == value) {
    return;
  }
  d_->sizeChangerMode = value;
  emit sizeChangerModeChanged(value);
  rebuild();
}

bool AdPagination::isSizeChangerVisible() const {
  switch (d_->sizeChangerMode) {
    case SizeChangerMode::Always:
      return true;
    case SizeChangerMode::Never:
      return false;
    case SizeChangerMode::Auto:
    default:
      return d_->total > d_->totalBoundaryShowSizeChanger;
  }
}

void AdPagination::setShowSizeChanger(bool value) {
  setSizeChangerMode(value ? SizeChangerMode::Always : SizeChangerMode::Never);
}

int AdPagination::totalBoundaryShowSizeChanger() const { return d_->totalBoundaryShowSizeChanger; }

void AdPagination::setTotalBoundaryShowSizeChanger(int value) {
  value = std::max(0, value);
  if (d_->totalBoundaryShowSizeChanger == value) {
    return;
  }
  d_->totalBoundaryShowSizeChanger = value;
  emit totalBoundaryShowSizeChangerChanged(value);
  rebuild();
}

QVector<int> AdPagination::pageSizeOptions() const { return d_->pageSizeOptions; }

void AdPagination::setPageSizeOptions(const QVector<int>& values) {
  QVector<int> normalized;
  normalized.reserve(values.size());
  for (int value : values) {
    if (value > 0 && !normalized.contains(value)) {
      normalized.append(value);
    }
  }
  if (normalized.isEmpty()) {
    normalized = {10, 20, 50, 100};
  }
  if (d_->pageSizeOptions == normalized) {
    return;
  }
  d_->pageSizeOptions = normalized;
  emit pageSizeOptionsChanged(normalized);
  rebuild();
}

bool AdPagination::disabled() const { return !isEnabled(); }

void AdPagination::setDisabled(bool value) {
  if (disabled() == value) {
    return;
  }
  QWidget::setDisabled(value);
}

#define ADQT_PAGINATION_BOOL_PROPERTY(Getter, member, Setter, Signal) \
  bool AdPagination::Getter() const { return d_->member; }            \
  void AdPagination::Setter(bool value) {                             \
    if (d_->member == value) {                                        \
      return;                                                         \
    }                                                                 \
    d_->member = value;                                               \
    emit Signal(value);                                               \
    rebuild();                                                        \
  }

ADQT_PAGINATION_BOOL_PROPERTY(hideOnSinglePage, hideOnSinglePage, setHideOnSinglePage,
                              hideOnSinglePageChanged)
ADQT_PAGINATION_BOOL_PROPERTY(showQuickJumper, showQuickJumper, setShowQuickJumper,
                              showQuickJumperChanged)
ADQT_PAGINATION_BOOL_PROPERTY(showLessItems, showLessItems, setShowLessItems, showLessItemsChanged)
ADQT_PAGINATION_BOOL_PROPERTY(showTitle, showTitle, setShowTitle, showTitleChanged)
ADQT_PAGINATION_BOOL_PROPERTY(simple, simple, setSimple, simpleChanged)
ADQT_PAGINATION_BOOL_PROPERTY(simpleReadOnly, simpleReadOnly, setSimpleReadOnly,
                              simpleReadOnlyChanged)
ADQT_PAGINATION_BOOL_PROPERTY(responsive, responsive, setResponsive, responsiveChanged)

#undef ADQT_PAGINATION_BOOL_PROPERTY

AdPagination::TotalTextFormatter AdPagination::totalTextFormatter() const {
  return d_->totalFormatter;
}

void AdPagination::setTotalTextFormatter(TotalTextFormatter formatter) {
  d_->totalFormatter = std::move(formatter);
  emit totalTextFormatterChanged();
  rebuild();
}

AdPagination::ItemTextFormatter AdPagination::itemTextFormatter() const {
  return d_->itemFormatter;
}

void AdPagination::setItemTextFormatter(ItemTextFormatter formatter) {
  d_->itemFormatter = std::move(formatter);
  emit itemTextFormatterChanged();
  rebuild();
}

AdPagination::ComponentTokens AdPagination::componentTokens() const { return d_->componentTokens; }

void AdPagination::setComponentTokens(const ComponentTokens& tokens) {
  d_->componentTokens = tokens;
  emit componentTokensChanged();
  rebuild();
}

void AdPagination::resetComponentTokens() { setComponentTokens({}); }

AdPagination::SemanticStyles AdPagination::semanticStyles() const { return d_->semanticStyles; }

void AdPagination::setSemanticStyles(const SemanticStyles& styles) {
  d_->semanticStyles = styles;
  emit semanticStylesChanged();
  rebuild();
}

QSize AdPagination::sizeHint() const {
  if (d_->hideOnSinglePage && d_->total <= d_->pageSize) {
    return QSize(0, 0);
  }
  return d_->layout->sizeHint();
}

QSize AdPagination::minimumSizeHint() const { return sizeHint(); }

void AdPagination::previousPage() { navigateFromUser(d_->currentPage - 1); }

void AdPagination::nextPage() { navigateFromUser(d_->currentPage + 1); }

void AdPagination::jumpToPage(int page) { navigateFromUser(page); }

void AdPagination::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!event) {
    return;
  }
  switch (event->type()) {
    case QEvent::ApplicationPaletteChange:
    case QEvent::PaletteChange:
    case QEvent::FontChange:
    case QEvent::StyleChange:
    case QEvent::LayoutDirectionChange:
    case QEvent::EnabledChange:
      if (d_ && !d_->rebuilding) {
        const bool disabledNow = disabled();
        if (disabledNow != d_->lastDisabled) {
          d_->lastDisabled = disabledNow;
          emit disabledChanged(disabledNow);
        }
        rebuild();
      }
      break;
    case QEvent::LanguageChange:
      if (d_ && !d_->rebuilding) {
        d_->quickLabel->setText(tr("Go to"));
        d_->quickSuffixLabel->setText(tr("Page"));
        d_->quickInput->setAccessibleName(tr("Page number"));
        d_->simpleInput->setAccessibleName(tr("Page number"));
        d_->sizeChanger->setAccessibleName(tr("Items per page"));
        setAccessibleName(tr("Pagination"));
        rebuild();
      }
      break;
    default:
      break;
  }
}

void AdPagination::paintEvent(QPaintEvent* event) {
  QWidget::paintEvent(event);
  if (!d_->visualStyle.hasRootBorder) {
    return;
  }
  QPainter painter(this);
  const qreal dpr = painter.device() ? painter.device()->devicePixelRatioF() : devicePixelRatioF();
  const QRectF borderRect = detail::deviceAlignedPaginationBorderRect(
      QRectF(rect()), d_->visualStyle.borderWidth, dpr, widgetPaintOrigin(this));
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(d_->visualStyle.rootBorder, d_->visualStyle.borderWidth, Qt::SolidLine,
                      Qt::SquareCap, Qt::MiterJoin));
  painter.drawRect(borderRect);
}

void AdPagination::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  const ControlSize effective = effectiveControlSize();
  if (effective != d_->lastEffectiveSize && !d_->rebuilding) {
    rebuild();
  }
}

void AdPagination::applyPageSizeFromUser(int value) {
  value = std::max(1, value);
  if (value == d_->pageSize || disabled()) {
    return;
  }
  const int oldCurrent = d_->currentPage;
  const int oldPageCount = pageCount();
  d_->pageSize = value;
  d_->currentPage = boundedPage(oldCurrent);
  emit pageSizeSelected(oldCurrent, value);
  emit pageSizeChanged(value);
  if (d_->currentPage != oldCurrent) {
    emit currentPageChanged(d_->currentPage);
  }
  if (pageCount() != oldPageCount) {
    emit pageCountChanged(pageCount());
  }
  emit changed(d_->currentPage, d_->pageSize);
  rebuild();
}

void AdPagination::navigateFromUser(int page) {
  if (disabled() || d_->total <= 0) {
    return;
  }
  const int bounded = boundedPage(page);
  if (bounded == d_->currentPage) {
    rebuild();
    return;
  }
  d_->currentPage = bounded;
  emit currentPageChanged(bounded);
  emit changed(bounded, d_->pageSize);
  rebuild();
}

int AdPagination::boundedPage(int page) const {
  return std::clamp(page, 1, std::max(1, pageCount()));
}

AdPagination::ControlSize AdPagination::effectiveControlSize() const {
  if (!d_->responsive || d_->controlSize != ControlSize::Medium) {
    return d_->controlSize;
  }
  const int breakpoint = d_->componentTokens.metrics.responsiveBreakpoint.value_or(576);
  return width() > 0 && width() < std::max(1, breakpoint) ? ControlSize::Small
                                                          : ControlSize::Medium;
}

void AdPagination::rebuild() {
  if (d_->rebuilding) {
    return;
  }
  d_->rebuilding = true;

  while (QLayoutItem* item = d_->layout->takeAt(0)) {
    delete item;
  }
  const bool hidden = d_->hideOnSinglePage && d_->total <= d_->pageSize;
  const QList<QWidget*> persistentWidgets = {
      d_->totalLabel,       d_->quickLabel,       d_->quickInput,
      d_->quickSuffixLabel, d_->sizeChanger,      d_->simpleCurrentLabel,
      d_->simpleInput,      d_->simpleSlashLabel, d_->simplePageCountLabel};
  if (hidden) {
    for (QWidget* widget : persistentWidgets) {
      widget->hide();
    }
    for (PaginationButton* button : std::as_const(d_->buttons)) {
      button->hide();
    }
    d_->visualStyle.hasRootBorder = false;
    setAutoFillBackground(false);
    d_->rebuilding = false;
    updateGeometry();
    return;
  }

  const ControlSize effectiveSize = effectiveControlSize();
  d_->lastEffectiveSize = effectiveSize;
  detail::PaginationStyleInput styleInput;
  styleInput.controlSize = effectiveSize;
  styleInput.baseFont = font();
  styleInput.componentTokens = d_->componentTokens;
  styleInput.semanticStyles = d_->semanticStyles;
  const PaginationVisualStyle style = detail::resolvePaginationVisualStyle(
      styleInput, adqt::theme::ThemeManager::instance().resolve(this));
  d_->visualStyle = style;

  d_->layout->setSpacing(style.spacing);
  d_->layout->setDirection(QBoxLayout::LeftToRight);

  QPalette rootPalette = palette();
  rootPalette.setColor(QPalette::Window, style.rootBackground);
  rootPalette.setColor(QPalette::WindowText, style.text);
  setPalette(rootPalette);
  setAutoFillBackground(d_->semanticStyles.root.backgroundColor.has_value());

  const auto configureTextWidget = [&style](QWidget* widget) {
    QPalette p = widget->palette();
    p.setColor(QPalette::WindowText, style.text);
    p.setColor(QPalette::Text, style.text);
    widget->setPalette(p);
    widget->setFont(style.font);
  };
  for (QWidget* widget : persistentWidgets) {
    configureTextWidget(widget);
    widget->setEnabled(!disabled());
  }

  const auto addStartAlignment = [this]() {
    if (d_->alignment == Alignment::Center || d_->alignment == Alignment::End) {
      d_->layout->addStretch(1);
    }
  };
  const auto addEndAlignment = [this]() {
    if (d_->alignment == Alignment::Center || d_->alignment == Alignment::Start) {
      d_->layout->addStretch(1);
    }
  };
  addStartAlignment();

  if (d_->totalFormatter) {
    d_->totalLabel->setText(d_->totalFormatter(d_->total, visibleItemRange()));
    d_->totalLabel->show();
    d_->layout->addWidget(d_->totalLabel);
  } else {
    d_->totalLabel->hide();
  }

  const auto controlSizeForInput = [effectiveSize]() {
    switch (effectiveSize) {
      case ControlSize::Large:
        return AdLineEdit::ControlSize::Large;
      case ControlSize::Small:
        return AdLineEdit::ControlSize::Small;
      case ControlSize::Medium:
      default:
        return AdLineEdit::ControlSize::Medium;
    }
  };
  const auto controlSizeForSelect = [effectiveSize]() {
    switch (effectiveSize) {
      case ControlSize::Large:
        return AdComboBox::ControlSize::Large;
      case ControlSize::Small:
        return AdComboBox::ControlSize::Small;
      case ControlSize::Medium:
      default:
        return AdComboBox::ControlSize::Middle;
    }
  };

  d_->quickInput->setControlSize(controlSizeForInput());
  d_->simpleInput->setControlSize(controlSizeForInput());
  d_->sizeChanger->setControlSize(controlSizeForSelect());

  int usedButtonCount = 0;
  const auto addButton = [this, &style, &usedButtonCount](ItemType type, int targetPage,
                                                          bool active = false) {
    QString text;
    if (d_->itemFormatter) {
      text = d_->itemFormatter(targetPage, type);
    } else if (type == ItemType::Page) {
      text = QString::number(targetPage);
    }
    PaginationButton* button = nullptr;
    if (usedButtonCount < d_->buttons.size()) {
      button = d_->buttons.at(usedButtonCount);
    } else {
      button = new PaginationButton(this);
      connect(button, &QAbstractButton::clicked, this,
              [this, button]() { navigateFromUser(button->targetPage()); });
      d_->buttons.append(button);
    }
    ++usedButtonCount;
    button->setItem(type, targetPage, text);
    button->setLayoutDirection(layoutDirection());
    button->setAppearance(style, active);
    button->setEnabled(!disabled());
    button->setObjectName(
        type == ItemType::Page
            ? QStringLiteral("paginationPage%1").arg(targetPage)
            : QStringLiteral("paginationNavigation%1").arg(static_cast<int>(type)));
    button->setAccessibleIdentifier(button->objectName());
    if (type == ItemType::JumpPrevious || type == ItemType::JumpNext) {
      const int distance = std::abs(targetPage - d_->currentPage);
      button->setAccessibleName(type == ItemType::JumpPrevious
                                    ? tr("Previous %1 pages").arg(distance)
                                    : tr("Next %1 pages").arg(distance));
    } else {
      button->setAccessibleName(defaultAccessibleName(type, targetPage));
    }
    button->setAccessibleDescription(active ? tr("Current page") : QString());
    button->setToolTip(QString());
    if (d_->showTitle) {
      if (type == ItemType::JumpPrevious || type == ItemType::JumpNext) {
        const int distance = std::abs(targetPage - d_->currentPage);
        button->setToolTip(type == ItemType::JumpPrevious ? tr("Previous %1 pages").arg(distance)
                                                          : tr("Next %1 pages").arg(distance));
      } else {
        button->setToolTip(defaultAccessibleName(type, targetPage));
      }
    }
    button->show();
    d_->layout->addWidget(button);
    return button;
  };

  auto* previous = addButton(ItemType::Previous, d_->currentPage - 1);
  previous->setEnabled(!disabled() && d_->currentPage > 1 && pageCount() > 0);

  if (d_->simple) {
    d_->simpleCurrentLabel->setText(QString::number(d_->currentPage));
    d_->simpleInput->setText(QString::number(d_->currentPage));
    const int digits =
        std::max(2, static_cast<int>(QString::number(std::max(1, pageCount())).size()));
    d_->simpleInput->setFixedWidth(std::max(
        style.quickJumperWidth,
        QFontMetrics(style.font).horizontalAdvance(QString(digits, QLatin1Char('8'))) + 20));
    d_->simplePageCountLabel->setText(QString::number(pageCount()));
    if (d_->simpleReadOnly) {
      d_->simpleCurrentLabel->show();
      d_->layout->addWidget(d_->simpleCurrentLabel);
    } else {
      d_->simpleInput->show();
      d_->layout->addWidget(d_->simpleInput);
    }
    d_->simpleSlashLabel->show();
    d_->simplePageCountLabel->show();
    d_->layout->addWidget(d_->simpleSlashLabel);
    d_->layout->addWidget(d_->simplePageCountLabel);
  } else {
    d_->simpleCurrentLabel->hide();
    d_->simpleInput->hide();
    d_->simpleSlashLabel->hide();
    d_->simplePageCountLabel->hide();
    const QVector<PageItemSpec> specs = pageItems(d_->currentPage, pageCount(), d_->showLessItems);
    for (const PageItemSpec& spec : specs) {
      auto* button = addButton(spec.type, spec.page,
                               spec.type == ItemType::Page && spec.page == d_->currentPage);
      if (d_->total == 0) {
        button->setEnabled(false);
      }
    }
  }

  const int nextPage = static_cast<int>(
      std::min<qint64>(std::max(1, pageCount()), static_cast<qint64>(d_->currentPage) + 1));
  auto* next = addButton(ItemType::Next, nextPage);
  next->setEnabled(!disabled() && d_->currentPage < pageCount());

  if (!d_->simple && isSizeChangerVisible()) {
    QVector<int> options = d_->pageSizeOptions;
    if (!options.contains(d_->pageSize)) {
      options.append(d_->pageSize);
      std::sort(options.begin(), options.end());
    }
    QVector<AdComboBox::Option> selectOptions;
    selectOptions.reserve(options.size());
    int optionTextWidth = 0;
    for (int value : options) {
      AdComboBox::Option option;
      option.value = value;
      option.label = tr("%1 / page").arg(value);
      optionTextWidth =
          std::max(optionTextWidth, QFontMetrics(style.font).horizontalAdvance(option.label));
      selectOptions.append(option);
    }
    const QSignalBlocker blocker(d_->sizeChanger);
    d_->sizeChanger->setOptions(selectOptions);
    d_->sizeChanger->setCurrentValue(d_->pageSize);
    d_->sizeChanger->setFixedWidth(std::max(96, optionTextWidth + 42));
    d_->sizeChanger->show();
    d_->layout->addWidget(d_->sizeChanger);
  } else {
    d_->sizeChanger->hide();
  }

  if (!d_->simple && d_->showQuickJumper && d_->total > d_->pageSize) {
    d_->quickInput->setFixedWidth(style.quickJumperWidth);
    d_->quickLabel->show();
    d_->quickInput->show();
    d_->quickSuffixLabel->show();
    d_->layout->addWidget(d_->quickLabel);
    d_->layout->addWidget(d_->quickInput);
    d_->layout->addWidget(d_->quickSuffixLabel);
  } else {
    d_->quickLabel->hide();
    d_->quickInput->hide();
    d_->quickSuffixLabel->hide();
  }

  for (int index = usedButtonCount; index < d_->buttons.size(); ++index) {
    PaginationButton* button = d_->buttons.at(index);
    button->setObjectName(QString());
    button->setAccessibleIdentifier(QString());
    button->setAccessibleName(QString());
    button->setAccessibleDescription(QString());
    button->setToolTip(QString());
    button->hide();
  }

  addEndAlignment();
  d_->rebuilding = false;
  updateGeometry();
  update();
}

}  // namespace adqt::widgets
