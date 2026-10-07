#include "snow_shot/presentation/windowgroupswitcherpopup.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "widgets/detail/native_window_chrome.h"
#include "widgets/scroll_area.h"
#include <QAbstractListModel>
#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QListView>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace snow_shot::presentation {
namespace {
QString pickerText(const char* source) {
    return QCoreApplication::translate("WindowGroupSwitcher", source);
}
class GroupListModel final : public QAbstractListModel {
  public:
    using QAbstractListModel::QAbstractListModel;
    QVector<WindowGroupDisplayEntry> groups;
    QString activeId;
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(groups.size());
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= groups.size())
            return {};
        const auto& group = groups.at(index.row());
        if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
            return group.name;
        if (role == Qt::UserRole)
            return group.id;
        if (role == Qt::AccessibleTextRole || role == Qt::AccessibleDescriptionRole) {
            QString text = pickerText(QT_TRANSLATE_NOOP("WindowGroupSwitcher",
                                                        "%1, %2 not closed windows, %3 total"))
                               .arg(group.name)
                               .arg(group.counts.nonIgnored)
                               .arg(group.counts.total);
            if (group.id == activeId)
                text += QStringLiteral(", ") +
                        pickerText(QT_TRANSLATE_NOOP("WindowGroupSwitcher", "Current"));
            return text;
        }
        return {};
    }
    void reset(QVector<WindowGroupDisplayEntry> value, const QString& active) {
        beginResetModel();
        groups = std::move(value);
        activeId = active;
        endResetModel();
    }
};
class GroupDelegate final : public QStyledItemDelegate {
  public:
    explicit GroupDelegate(GroupListModel& model, QObject* parent)
        : QStyledItemDelegate(parent), m_model(model) {}
    styles::ThemeColorScheme scheme;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return {0, std::max(scheme.metricAlias.controlHeightLG,
                            QFontMetrics(option.font).height() + 20) +
                       4};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto& group = m_model.groups.at(index.row());
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool current = group.id == m_model.activeId;
        const auto& colors = scheme.map;
        const QRect row = option.rect.adjusted(0, 2, -2, -2);
        painter->save();
        painter->setClipRect(option.rect);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        if (selected || option.state.testFlag(QStyle::State_MouseOver)) {
            painter->setBrush(selected ? colors.colorPrimaryBg : colors.colorFillTertiary);
            painter->drawRoundedRect(row, scheme.metricAlias.borderRadius,
                                     scheme.metricAlias.borderRadius);
        }
        if (selected) {
            painter->setBrush(colors.colorPrimary);
            painter->drawRoundedRect(QRectF(row.left() + 3, row.center().y() - 8, 3, 16), 1.5, 1.5);
        }
        QFont font = option.font;
        font.setWeight(selected ? QFont::DemiBold : QFont::Normal);
        painter->setFont(font);
        const QFontMetrics fm(font);
        int countWidth = fm.horizontalAdvance(QStringLiteral("0 / 0"));
        // The model computes no data during cycling; this width is supplied by the popup.
        countWidth = std::max(countWidth, m_countWidth);
        const QRect countRect(row.right() - countWidth - 12, row.top(), countWidth, row.height());
        painter->setPen(selected ? colors.colorPrimaryText : colors.colorTextSecondary);
        painter->drawText(
            countRect, Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("%1 / %2").arg(group.counts.nonIgnored).arg(group.counts.total));
        int nameRight = countRect.left() - 16;
        if (current) {
            QFont smallFont = option.font;
            smallFont.setPixelSize(scheme.metricAlias.fontSizeSM);
            const QString label = pickerText(QT_TRANSLATE_NOOP("WindowGroupSwitcher", "Current"));
            const QFontMetrics smallMetrics(smallFont);
            const int width = smallMetrics.horizontalAdvance(label) + 12;
            const QRect badge(nameRight - width, row.center().y() - (smallMetrics.height() + 6) / 2,
                              width, smallMetrics.height() + 6);
            painter->setPen(Qt::NoPen);
            painter->setBrush(colors.colorFillSecondary);
            painter->drawRoundedRect(badge, 4, 4);
            painter->setFont(smallFont);
            painter->setPen(colors.colorTextSecondary);
            painter->drawText(badge, Qt::AlignCenter, label);
            nameRight = badge.left() - 10;
            painter->setFont(font);
        }
        painter->setPen(selected ? colors.colorPrimaryText : colors.colorText);
        const QRect nameRect(row.left() + 14, row.top(), std::max(0, nameRight - row.left() - 14),
                             row.height());
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          fm.elidedText(group.name, Qt::ElideRight, nameRect.width()));
        painter->restore();
    }
    int m_countWidth = 0;

  private:
    GroupListModel& m_model;
};
} // namespace
class WindowGroupSwitcherPopup::Impl {
  public:
    explicit Impl(WindowGroupSwitcherPopup& owner) : q(owner) {
        auto* layout = new QVBoxLayout(&q);
        layout->setSizeConstraint(QLayout::SetNoConstraint);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(10);
        title = new QLabel(&q);
        caption = new QLabel(&q);
        caption->setAlignment(Qt::AlignRight);
        caption->setWordWrap(true);
        list = new QListView(&q);
        list->setObjectName(QStringLiteral("windowGroupSwitcherList"));
        model = new GroupListModel(list);
        delegate = new GroupDelegate(*model, list);
        list->setModel(model);
        list->setItemDelegate(delegate);
        list->setUniformItemSizes(true);
        list->setMinimumSize(0, 0);
        list->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
        list->setFrameShape(QFrame::NoFrame);
        list->setFocusPolicy(Qt::NoFocus);
        list->setMouseTracking(true);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setEditTriggers(QAbstractItemView::NoEditTriggers);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setVerticalScrollBar(new adqt::widgets::AdScrollBar(Qt::Vertical, list));
        list->setCursor(Qt::PointingHandCursor);
        footer = new QLabel(&q);
        footer->setWordWrap(true);
        for (QLabel* label : {title, caption, footer})
            label->setTextFormat(Qt::PlainText);
        layout->addWidget(title);
        layout->addWidget(caption);
        layout->addWidget(list, 1);
        layout->addWidget(footer);
        QObject::connect(list, &QListView::clicked, &q, [this](const QModelIndex& index) {
            emit q.groupClicked(index.data(Qt::UserRole).toString());
        });
        QObject::connect(&styles::ThemeManager::instance(), &styles::ThemeManager::themeChanged, &q,
                         [this] {
                             applyTheme();
                             q.updatePlacement();
                         });
        applyTheme();
        retranslate();
    }
    void retranslate() {
        title->setText(pickerText(QT_TRANSLATE_NOOP("WindowGroupSwitcher", "Switch Window Group")));
        q.setWindowTitle(title->text());
        q.setAccessibleName(title->text());
        list->setAccessibleName(title->text());
        caption->setText(
            pickerText(QT_TRANSLATE_NOOP("WindowGroupSwitcher", "Windows · not closed / total")));
        footer->setText(
            shortcutMode ? pickerText(QT_TRANSLATE_NOOP(
                               "WindowGroupSwitcher",
                               "Release shortcut keys or click a group to switch. Esc to cancel."))
                         : pickerText(QT_TRANSLATE_NOOP(
                               "WindowGroupSwitcher", "Click a group to switch. Esc to cancel.")));
        q.setAccessibleDescription(footer->text());
    }
    void applyTheme() {
        const auto scheme = styles::ThemeManager::instance().themeColorScheme();
        delegate->scheme = scheme;
        QFont font = QApplication::font();
        const QString family = styles::ThemeManager::instance().appFontFamily();
        if (!family.isEmpty())
            font.setFamily(family);
        font.setPixelSize(scheme.metricAlias.fontSize);
        q.setFont(font);
        QFont heading = font;
        heading.setPixelSize(scheme.metricAlias.fontSizeLG);
        heading.setWeight(QFont::DemiBold);
        title->setFont(heading);
        QFont smallFont = font;
        smallFont.setPixelSize(scheme.metricAlias.fontSizeSM);
        caption->setFont(smallFont);
        footer->setFont(smallFont);
        QPalette palette = q.palette();
        palette.setColor(QPalette::WindowText, scheme.map.colorText);
        palette.setColor(QPalette::Window, scheme.map.colorBgElevated);
        palette.setColor(QPalette::Text, scheme.map.colorText);
        palette.setColor(QPalette::Base, Qt::transparent);
        q.setPalette(palette);
        list->setPalette(palette);
        palette.setColor(QPalette::WindowText, scheme.map.colorTextSecondary);
        caption->setPalette(palette);
        footer->setPalette(palette);
        updateCountWidth();
        list->doItemsLayout();
        list->viewport()->update();
    }
    void updateCountWidth() {
        QFont font = list->font();
        font.setWeight(QFont::DemiBold);
        const QFontMetrics fm(font);
        delegate->m_countWidth = 0;
        for (const auto& group : model->groups)
            delegate->m_countWidth = std::max(delegate->m_countWidth,
                                              fm.horizontalAdvance(QStringLiteral("%1 / %2")
                                                                       .arg(group.counts.nonIgnored)
                                                                       .arg(group.counts.total)));
    }
    WindowGroupSwitcherPopup& q;
    QLabel* title = nullptr;
    QLabel* caption = nullptr;
    QLabel* footer = nullptr;
    QListView* list = nullptr;
    GroupListModel* model = nullptr;
    GroupDelegate* delegate = nullptr;
    QPointer<QScreen> screen;
    bool shortcutMode = false;
};
WindowGroupSwitcherPopup::WindowGroupSwitcherPopup() {
    setObjectName(QStringLiteral("windowGroupSwitcherPopup"));
    setWindowFlags(Qt::Tool | adqt::widgets::detail::nativeWindowChromeFlags() |
                   Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_QuitOnClose, false);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::NoFocus);
#ifdef Q_OS_MACOS
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
#endif
    m_impl = std::make_unique<Impl>(*this);
}
WindowGroupSwitcherPopup::~WindowGroupSwitcherPopup() = default;
void WindowGroupSwitcherPopup::setGroups(QVector<WindowGroupDisplayEntry> groups,
                                         const QString& activeId) {
    m_impl->model->reset(std::move(groups), activeId);
    m_impl->updateCountWidth();
    updatePlacement();
}
void WindowGroupSwitcherPopup::setSelectedGroup(const QString& id) {
    for (int row = 0; row < m_impl->model->rowCount(); ++row) {
        if (m_impl->model->groups.at(row).id != id)
            continue;
        const auto index = m_impl->model->index(row);
        m_impl->list->setCurrentIndex(index);
        m_impl->list->scrollTo(index, QAbstractItemView::EnsureVisible);
        break;
    }
}
void WindowGroupSwitcherPopup::setShortcutMode(bool enabled) {
    if (m_impl->shortcutMode == enabled)
        return;
    m_impl->shortcutMode = enabled;
    m_impl->retranslate();
    updatePlacement();
}
void WindowGroupSwitcherPopup::showOnScreen(QScreen* screen) {
    if (m_impl->screen)
        disconnect(m_impl->screen, nullptr, this, nullptr);
    m_impl->screen = screen;
    if (screen) {
        connect(screen, &QScreen::availableGeometryChanged, this,
                &WindowGroupSwitcherPopup::updatePlacement);
        connect(screen, &QScreen::logicalDotsPerInchChanged, this,
                &WindowGroupSwitcherPopup::updatePlacement);
    }
    createWinId();
    if (windowHandle())
        windowHandle()->setScreen(screen);
    updatePlacement();
    show();
    // Cocoa can replace the native window during show.
    adqt::widgets::detail::applyNativeWindowChrome(this);
    raise();
    m_impl->list->scrollToTop();
}
void WindowGroupSwitcherPopup::updatePlacement() {
    if (!m_impl || !m_impl->screen)
        return;
    const QRect available = m_impl->screen->availableGeometry();
    const int width =
        std::min(available.width(), std::max(400, m_impl->title->sizeHint().width() + 32));
    const int bodyWidth = std::max(1, width - 32);
    const int rowHeight = std::max(m_impl->delegate->scheme.metricAlias.controlHeightLG,
                                   fontMetrics().height() + 20) +
                          4;
    const int heightLimit = std::min(static_cast<int>(available.height() * 0.6), 560);
    const int textHeight = m_impl->title->sizeHint().height() +
                           m_impl->caption->heightForWidth(bodyWidth) +
                           m_impl->footer->heightForWidth(bodyWidth);
    // Preserve a complete selectable row when enlarged fonts meet a small logical display.
    const bool compact = textHeight + 32 + 30 + rowHeight > heightLimit;
    const int verticalPadding = compact ? 8 : 16;
    const int spacing = compact ? 6 : 10;
    auto* layout = qobject_cast<QVBoxLayout*>(this->layout());
    layout->setContentsMargins(16, verticalPadding, 16, verticalPadding);
    layout->setSpacing(spacing);
    const int fixedHeight = verticalPadding * 2 + spacing * 3 + textHeight;
    const int height = std::min(heightLimit, fixedHeight + rowHeight * m_impl->model->rowCount());
    setGeometry(QRect(available.topLeft() + QPoint((available.width() - width) / 2,
                                                   (available.height() - height) / 2),
                      QSize(width, std::max(1, height))));
}
void WindowGroupSwitcherPopup::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (m_impl && event->type() == QEvent::LanguageChange) {
        m_impl->retranslate();
        emit languageChanged();
        updatePlacement();
    }
}
void WindowGroupSwitcherPopup::showEvent(QShowEvent* event) {
    adqt::widgets::detail::applyNativeWindowChrome(this);
    QWidget::showEvent(event);
}
bool WindowGroupSwitcherPopup::nativeEvent(const QByteArray& type, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (static_cast<MSG*>(message)->message == WM_NCCALCSIZE) {
        // Keep the native frame decoration while our content fills the client area.
        *result = 0;
        return true;
    }
    if (static_cast<MSG*>(message)->message == WM_NCHITTEST) {
        *result = HTCLIENT;
        return true;
    }
    if (static_cast<MSG*>(message)->message == WM_MOUSEACTIVATE) {
        *result = MA_NOACTIVATE;
        return true;
    }
#endif
    return QWidget::nativeEvent(type, message, result);
}
} // namespace snow_shot::presentation
