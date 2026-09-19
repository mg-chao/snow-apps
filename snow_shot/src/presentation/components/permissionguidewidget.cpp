#include "snow_shot/presentation/components/permissionguidewidget.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QDrag>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStyleHints>
#include <QVBoxLayout>

namespace snow_shot::presentation {
namespace {
QString guideText(const char* text) {
    return QCoreApplication::translate("PermissionGuide", text);
}
bool validBundle(const QUrl& url) {
    const QFileInfo info(url.toLocalFile());
    return url.isLocalFile() && info.isDir() &&
           info.suffix().compare(QStringLiteral("app"), Qt::CaseInsensitive) == 0 &&
           QFileInfo(info.filePath() + QStringLiteral("/Contents/Info.plist")).isFile();
}
} // namespace
class PermissionGuideDragRow final : public QAbstractButton {
  public:
    PermissionGuideDragRow(PermissionGuideWidget& owner, const PermissionGuideApplication& app)
        : QAbstractButton(&owner), m_owner(owner), m_icon(app.icon) {
        setText(app.name);
        setMinimumHeight(46);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::OpenHandCursor);
        setObjectName(QStringLiteral("permissionGuideApp"));
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = m_owner.m_dark;
        painter.setPen(Qt::NoPen);
        painter.setBrush(dark ? QColor(255, 255, 255, underMouse() ? 24 : 14)
                              : QColor(underMouse() ? "#e6edf7" : "#f0f4f8"));
        painter.drawRoundedRect(rect(), 6, 6);
        m_icon.paint(&painter, QRect(8, (height() - 32) / 2, 32, 32));
        painter.setPen(dark ? QColor("#f1f1f1") : QColor("#202124"));
        painter.drawText(QRect(48, 0, width() - 60, height()), Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, width() - 60));
        if (hasFocus()) {
            painter.setPen(QPen(QColor("#4080ff"), 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 6, 6);
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_origin = event->position().toPoint();
            m_owner.setInteracting(true);
        }
        QAbstractButton::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        QAbstractButton::mouseReleaseEvent(event);
        m_owner.setInteracting(false);
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton) || !isDown() ||
            (event->position().toPoint() - m_origin).manhattanLength() <
                QApplication::startDragDistance()) {
            QAbstractButton::mouseMoveEvent(event);
            return;
        }
        auto* mime = m_owner.createDragMimeData();
        if (!mime)
            return;
        setDown(false);
        // Keep the source alive through the native drag's nested event loop.
        QPointer<PermissionGuideDragRow> guard(this);
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(m_icon.pixmap(QSize(48, 48), devicePixelRatioF()));
        drag->setHotSpot(QPoint(24, 24));
        drag->exec(Qt::CopyAction, Qt::CopyAction);
        if (!guard)
            return;
        drag->deleteLater();
        m_owner.setInteracting(false);
    }

  private:
    PermissionGuideWidget& m_owner;
    QIcon m_icon;
    QPoint m_origin;
};

PermissionGuideWidget::PermissionGuideWidget(PermissionGuideApplication application)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowDoesNotAcceptFocus),
      m_application(std::move(application)) {
    setObjectName(QStringLiteral("permissionGuide"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_QuitOnClose, false);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 10, 16, 14);
    layout->setSpacing(8);
    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    auto* arrow = new QLabel(QString::fromUtf8("⬆"), this);
    arrow->setObjectName(QStringLiteral("permissionGuideArrow"));
    arrow->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    arrow->setFixedWidth(28);
    m_instruction = new QLabel(this);
    m_instruction->setObjectName(QStringLiteral("permissionGuideInstruction"));
    m_instruction->setWordWrap(true);
    m_instruction->setTextFormat(Qt::PlainText);
    m_close = new QPushButton(QString::fromUtf8("×"), this);
    m_close->setObjectName(QStringLiteral("permissionGuideClose"));
    m_close->setFixedSize(24, 24);
    m_close->setCursor(Qt::PointingHandCursor);
    header->addWidget(arrow, 0, Qt::AlignTop);
    header->addWidget(m_instruction, 1);
    header->addWidget(m_close, 0, Qt::AlignTop);
    layout->addLayout(header);
    m_row = new PermissionGuideDragRow(*this, m_application);
    layout->addWidget(m_row);
    m_request = new QPushButton(this);
    m_request->setObjectName(QStringLiteral("permissionGuideRequest"));
    m_request->setCursor(Qt::PointingHandCursor);
    layout->addWidget(m_request, 0, Qt::AlignRight);
    connect(m_close, &QPushButton::clicked, this, &PermissionGuideWidget::dismissed);
    connect(m_request, &QPushButton::clicked, this, &PermissionGuideWidget::requestAccess);
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            &PermissionGuideWidget::setColorScheme);
    retranslate();
    setColorScheme(QGuiApplication::styleHints()->colorScheme());
}
void PermissionGuideWidget::setPermission(AppPermission permission, AppPermissionStatus status,
                                          bool pending, bool fallback) {
    if (permission == m_permission && status == m_status && pending == m_pending &&
        fallback == m_fallback)
        return;
    m_permission = permission;
    m_status = status;
    m_pending = pending;
    m_fallback = fallback;
    retranslate();
}
int PermissionGuideWidget::heightForGuideWidth(int width) const {
    return std::max(100, layout()->totalHeightForWidth(width));
}
QMimeData* PermissionGuideWidget::createDragMimeData() const {
    if (m_permission == AppPermission::Microphone || m_status == AppPermissionStatus::Restricted ||
        !validBundle(m_application.bundleUrl))
        return nullptr;
    auto* mime = new QMimeData;
    mime->setUrls({m_application.bundleUrl});
    return mime;
}
void PermissionGuideWidget::setInteracting(bool interacting) {
    if (m_interacting == interacting)
        return;
    m_interacting = interacting;
    if (!interacting)
        emit interactionFinished();
}
void PermissionGuideWidget::retranslate() {
    const bool microphone = m_permission == AppPermission::Microphone;
    const bool bundled = validBundle(m_application.bundleUrl);
    QString instruction;
    if (m_status == AppPermissionStatus::Restricted) {
        instruction = guideText(QT_TRANSLATE_NOOP(
            "PermissionGuide", "This permission is restricted by your Mac's administrator."));
    } else if (microphone && m_status == AppPermissionStatus::Error) {
        instruction = guideText(QT_TRANSLATE_NOOP(
            "PermissionGuide",
            "Microphone access is unavailable. Open the installed application and try again."));
    } else if (microphone && m_status == AppPermissionStatus::NotDetermined) {
        instruction = guideText(QT_TRANSLATE_NOOP(
            "PermissionGuide", "Request microphone access, then allow it in the macOS prompt."));
    } else if (microphone) {
        instruction =
            guideText(QT_TRANSLATE_NOOP("PermissionGuide",
                                        "Turn on microphone access for ‘%1’ in System Settings."))
                .arg(m_application.name);
    } else if (!bundled) {
        instruction = guideText(
            QT_TRANSLATE_NOOP("PermissionGuide",
                              "Open the installed application to add it to the permissions list."));
    } else if (m_fallback) {
        instruction = guideText(QT_TRANSLATE_NOOP("PermissionGuide",
                                                  "Drag ‘%1’ into the permissions list in System "
                                                  "Settings. If it is already listed, turn it on."))
                          .arg(m_application.name);
    } else {
        instruction = guideText(QT_TRANSLATE_NOOP("PermissionGuide",
                                                  "Drag ‘%1’ into the permissions list above. If "
                                                  "it is already listed, turn it on."))
                          .arg(m_application.name);
    }
    setWindowTitle(guideText(QT_TRANSLATE_NOOP("PermissionGuide", "Permission setup")));
    m_instruction->setText(instruction);
    m_close->setAccessibleName(guideText(QT_TRANSLATE_NOOP("PermissionGuide", "Close guidance")));
    m_close->setToolTip(m_close->accessibleName());
    m_row->setVisible(!microphone && bundled && m_status != AppPermissionStatus::Restricted);
    m_row->setAccessibleName(
        guideText(QT_TRANSLATE_NOOP("PermissionGuide", "Drag %1 to System Settings"))
            .arg(m_application.name));
    m_row->setAccessibleDescription(instruction);
    m_row->setToolTip(m_row->accessibleName());
    m_request->setText(guideText(m_pending
                                     ? QT_TRANSLATE_NOOP("PermissionGuide", "Requesting…")
                                     : QT_TRANSLATE_NOOP("PermissionGuide", "Request Access")));
    m_request->setVisible(microphone && m_status == AppPermissionStatus::NotDetermined);
    m_request->setEnabled(!m_pending);
    layout()->invalidate();
    emit contentSizeChanged();
}
void PermissionGuideWidget::setColorScheme(Qt::ColorScheme scheme) {
    m_dark = scheme == Qt::ColorScheme::Dark;
    const bool dark = m_dark;
    setStyleSheet(
        QStringLiteral("QLabel { color: %1; background: transparent; font-size: 13px; }"
                       "QLabel#permissionGuideArrow { color: #4080ff; font-size: 25px; }"
                       "QPushButton#permissionGuideClose { color: %2; background: transparent; "
                       "border: none; border-radius: 12px; font-size: 20px; }"
                       "QPushButton#permissionGuideClose:hover { background: %3; }"
                       "QPushButton#permissionGuideRequest { color: white; background: #3478f6; "
                       "border: none; border-radius: 6px; padding: 6px 14px; }"
                       "QPushButton#permissionGuideRequest:disabled { background: %2; }")
            .arg(dark ? QStringLiteral("#f1f1f1") : QStringLiteral("#202124"),
                 dark ? QStringLiteral("#aaaaaa") : QStringLiteral("#888888"),
                 dark ? QStringLiteral("#444444") : QStringLiteral("#eeeeee")));
    update();
    m_row->update();
    emit contentSizeChanged();
}
void PermissionGuideWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        retranslate();
}
void PermissionGuideWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(m_dark ? QColor("#626262") : QColor("#bfc2c7"));
    painter.setBrush(m_dark ? QColor("#292929") : QColor("#ffffff"));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 11, 11);
}
void PermissionGuideWidget::closeEvent(QCloseEvent* event) {
    event->ignore();
    emit dismissed();
}
} // namespace snow_shot::presentation
