#include "snow_shot/presentation/screenshotmessageservice.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "widgets/message.h"

#include <QWidget>

#include <utility>

namespace {
enum class MessageKind { Warning, Error, Loading };

void showMessage(MessageKind kind, adqt::widgets::AdMessage::Request request, QWidget* owner) {
    if (owner == nullptr) {
        return;
    }
    switch (kind) {
    case MessageKind::Warning:
        adqt::widgets::AdMessageService::warning(std::move(request), owner);
        return;
    case MessageKind::Error:
        adqt::widgets::AdMessageService::error(std::move(request), owner);
        return;
    case MessageKind::Loading:
        adqt::widgets::AdMessageService::loading(std::move(request), owner);
        return;
    }
}
} // namespace

ScreenshotMessageService::ScreenshotMessageService(
    ScreenshotDisplaySession& displaySession, ScreenshotGeometryMapper& geometry,
    ScreenshotSelectionModel& selection, std::function<QWidget*()> toolbarFallback)
    : m_displaySession(displaySession),
      m_geometry(geometry),
      m_selection(selection),
      m_toolbarFallback(std::move(toolbarFallback)) {}

void ScreenshotMessageService::warning(const QString& key, const QString& message,
                                       const QRectF& canvasRect, QWidget* preferredOwner) const {
    if (message.isEmpty()) {
        return;
    }
    adqt::widgets::AdMessage::Request request;
    request.key = key;
    request.content = message;
    QWidget* owner = ownerFor(canvasRect, preferredOwner);
    rememberOwner(key, owner);
    showMessage(MessageKind::Warning, std::move(request), owner);
}

void ScreenshotMessageService::error(const QString& key, const QString& message,
                                     const QRectF& canvasRect, QWidget* preferredOwner) const {
    if (message.isEmpty()) {
        return;
    }
    adqt::widgets::AdMessage::Request request;
    request.key = key;
    request.content = message;
    QWidget* owner = ownerFor(canvasRect, preferredOwner);
    rememberOwner(key, owner);
    showMessage(MessageKind::Error, std::move(request), owner);
}

void ScreenshotMessageService::loading(const QString& key, const QString& message,
                                       const QRectF& canvasRect, QWidget* preferredOwner) const {
    if (message.isEmpty()) {
        return;
    }
    adqt::widgets::AdMessage::Request request;
    request.key = key;
    request.content = message;
    request.durationMs = 0;
    QWidget* owner = ownerFor(canvasRect, preferredOwner);
    rememberOwner(key, owner);
    showMessage(MessageKind::Loading, std::move(request), owner);
}

void ScreenshotMessageService::destroy(const QString& key) const {
    QWidget* owner = m_messageOwners.value(key);
    if (owner == nullptr) {
        owner = ownerFor({}, nullptr);
    }
    if (owner != nullptr) {
        adqt::widgets::AdMessageService::destroy(key, owner);
    }
    m_messageOwners.remove(key);
}

void ScreenshotMessageService::loadingFor(QWidget* owner, const QString& key,
                                          const QString& message) {
    if (owner == nullptr || message.isEmpty()) {
        return;
    }
    adqt::widgets::AdMessage::Request request;
    request.key = key;
    request.content = message;
    request.durationMs = 0;
    showMessage(MessageKind::Loading, std::move(request), owner);
}


void ScreenshotMessageService::destroyFor(QWidget* owner, const QString& key) {
    if (owner != nullptr) {
        adqt::widgets::AdMessageService::destroy(key, owner);
    }
}

void ScreenshotMessageService::rememberOwner(const QString& key, QWidget* owner) const {
    if (owner == nullptr) {
        return;
    }
    const QPointer<QWidget> previous = m_messageOwners.value(key);
    if (previous != nullptr && previous != owner) {
        adqt::widgets::AdMessageService::destroy(key, previous);
    }
    m_messageOwners.insert(key, owner);
}

QWidget* ScreenshotMessageService::ownerFor(const QRectF& canvasRect,
                                            QWidget* preferredOwner) const {
    if (preferredOwner != nullptr) {
        return preferredOwner;
    }

    QRectF target = canvasRect;
    if (target.isNull() || target.isEmpty()) {
        target = QRectF(m_selection.pixelSelection());
    }
    const CapturedDisplayModel* display =
        m_geometry.displayForCanvasRect(m_displaySession, target);
    if (ScreenshotOverlayWindow* overlay = m_displaySession.overlayForDisplay(display)) {
        return overlay;
    }
    return m_toolbarFallback ? m_toolbarFallback() : nullptr;
}
