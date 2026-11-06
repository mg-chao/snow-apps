// SPDX-License-Identifier: GPL-3.0-or-later
#include "screenshotpinnedaccessibility.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QAccessible>
#include <QAccessibleObject>
#include <QAccessibleWidget>
#include <QCoreApplication>
#include <QFrame>
#include <QVector>

namespace {
class PinnedContentAccessible final : public QAccessibleObject {
  public:
    explicit PinnedContentAccessible(ScreenshotPinnedAccessibility* content)
        : QAccessibleObject(content) {}

    QWindow* window() const override {
        return content()->owner()->windowHandle();
    }
    QAccessibleInterface* parent() const override {
        return QAccessible::queryAccessibleInterface(content()->owner());
    }
    QAccessibleInterface* child(int) const override {
        return nullptr;
    }
    int childCount() const override {
        return 0;
    }
    int indexOfChild(const QAccessibleInterface*) const override {
        return -1;
    }
    QString text(QAccessible::Text kind) const override {
        if (kind == QAccessible::Name)
            return QCoreApplication::translate("ScreenshotPinnedWindow", "Pinned content");
        if (kind == QAccessible::Identifier)
            return QStringLiteral("screenshotPinnedContent");
        return {};
    }
    QRect rect() const override {
        return content()->rect();
    }
    QAccessible::Role role() const override {
        return QAccessible::Graphic;
    }
    QAccessible::State state() const override {
        QAccessible::State result;
        result.readOnly = true;
        result.invisible = !content()->visible();
        result.offscreen = result.invisible;
        result.disabled = !content()->owner()->isEnabled();
        return result;
    }

  private:
    ScreenshotPinnedAccessibility* content() const {
        return static_cast<ScreenshotPinnedAccessibility*>(object());
    }
};

class PinnedCanvasAccessible final : public QAccessibleWidget {
  public:
    explicit PinnedCanvasAccessible(QWidget* canvas) : QAccessibleWidget(canvas) {}
    QRect rect() const override {
        const auto* content = ScreenshotPinnedAccessibility::forCanvas(object());
        return content && widget()->isVisible() ? content->rect() : QRect();
    }
};

class PinnedWindowAccessible final : public QAccessibleWidget {
  public:
    explicit PinnedWindowAccessible(QWidget* window) : QAccessibleWidget(window) {}
    int childCount() const override {
        return static_cast<int>(children().size());
    }
    QAccessibleInterface* child(int index) const override {
        const auto items = children();
        return index >= 0 && index < items.size() ? items.at(index) : nullptr;
    }
    int indexOfChild(const QAccessibleInterface* child) const override {
        return static_cast<int>(children().indexOf(child));
    }
    QAccessibleInterface* childAt(int x, int y) const override {
        const auto items = children();
        // Match QWidget stacking order. Empty presentation containers must not obscure
        // the semantic content, but their buttons and canvas text editors remain reachable.
        for (auto it = items.crbegin(); it != items.crend(); ++it) {
            if (auto* hit = hitElement(*it, x, y, 0))
                return hit;
        }
        return nullptr;
    }

  private:
    QVector<QAccessibleInterface*> children() const {
        QVector<QAccessibleInterface*> result;
        auto* content = ScreenshotPinnedAccessibility::forWindow(object());
        if (!content)
            return result;
        result.push_back(content->accessibleInterface());
        const int count = QAccessibleWidget::childCount();
        for (int index = 0; index < count; ++index) {
            auto* item = QAccessibleWidget::child(index);
            if (item && !content->isDecoration(item->object()))
                result.push_back(item);
        }
        return result;
    }
    static QAccessibleInterface* hitElement(QAccessibleInterface* item, int x, int y, int depth) {
        if (!item || !item->isValid() || item->state().invisible || !item->rect().contains(x, y) ||
            depth >= 32)
            return nullptr;
        const auto role = item->role();
        if (role != QAccessible::Client && role != QAccessible::Border && role != QAccessible::Pane)
            return item;
        for (int index = item->childCount() - 1; index >= 0; --index) {
            if (auto* hit = hitElement(item->child(index), x, y, depth + 1))
                return hit;
        }
        return nullptr;
    }
};

QAccessibleInterface* pinnedAccessibilityFactory(const QString&, QObject* object) {
    if (qobject_cast<ScreenshotPinnedWindow*>(object))
        return new PinnedWindowAccessible(static_cast<QWidget*>(object));
    if (ScreenshotPinnedAccessibility::forCanvas(object))
        return new PinnedCanvasAccessible(static_cast<QWidget*>(object));
    return nullptr;
}
} // namespace

ScreenshotPinnedAccessibility::ScreenshotPinnedAccessibility(ScreenshotPinnedWindow& window)
    : QObject(&window), m_window(window) {
    static const bool installed = [] {
        QAccessible::installFactory(pinnedAccessibilityFactory);
        return true;
    }();
    Q_UNUSED(installed);
    QAccessible::registerAccessibleInterface(new PinnedContentAccessible(this));
}

ScreenshotPinnedAccessibility::~ScreenshotPinnedAccessibility() {
    QAccessibleEvent event(this, QAccessible::ObjectDestroyed);
    QAccessible::updateAccessibility(&event);
}

ScreenshotPinnedAccessibility* ScreenshotPinnedAccessibility::forWindow(QObject* object) {
    auto* window = qobject_cast<ScreenshotPinnedWindow*>(object);
    return window ? window->m_accessibility.get() : nullptr;
}

ScreenshotPinnedAccessibility* ScreenshotPinnedAccessibility::forCanvas(QObject* object) {
    auto* window = object ? qobject_cast<ScreenshotPinnedWindow*>(object->parent()) : nullptr;
    // Qt can query accessibility during the canvas constructor, before m_canvas is assigned.
    return window && qobject_cast<SnowCanvasWidget*>(object) ? window->m_accessibility.get()
                                                             : nullptr;
}

QWidget* ScreenshotPinnedAccessibility::owner() const {
    return &m_window;
}

bool ScreenshotPinnedAccessibility::visible() const {
    return m_window.isVisible() && !m_window.isMinimized() && m_window.m_presented &&
           !m_window.m_closing && !m_window.m_destroying;
}

QRect ScreenshotPinnedAccessibility::rect() const {
    if (!visible())
        return {};
    const QRectF local = m_window.contentViewRect();
    if (local.isEmpty())
        return {};
    // Qt accessibility uses integer desktop DIPs. Round outward once; platform
    // bridges perform their own conversion to UIA pixels or AppKit screen points.
    return QRectF(m_window.mapToGlobal(local.topLeft()), local.size()).toAlignedRect();
}

bool ScreenshotPinnedAccessibility::isDecoration(QObject* object) const {
    return object == m_window.m_borderFrame;
}

QAccessibleInterface* ScreenshotPinnedAccessibility::accessibleInterface() const {
    return QAccessible::queryAccessibleInterface(const_cast<ScreenshotPinnedAccessibility*>(this));
}

void ScreenshotPinnedAccessibility::update(bool languageChanged) {
    const bool nowVisible = visible();
    const QRect nowRect = rect();
    const bool visibilityChanged = nowVisible != m_lastVisible;
    const bool geometryChanged = nowRect != m_lastRect;
    m_lastVisible = nowVisible;
    m_lastRect = nowRect;
    const auto notify = [this](QAccessible::Event type) {
        QAccessibleEvent event(this, type);
        QAccessible::updateAccessibility(&event);
    };
    if (visibilityChanged)
        notify(nowVisible ? QAccessible::ObjectShow : QAccessible::ObjectHide);
    if (geometryChanged && nowVisible)
        notify(QAccessible::LocationChanged);
    if (languageChanged)
        notify(QAccessible::NameChanged);
}
