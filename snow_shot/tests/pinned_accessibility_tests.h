// SPDX-License-Identifier: GPL-3.0-or-later
// Included after the pinned-window and live-source fixtures.
#include <QAccessible>
#include <QAccessibleWidget>

namespace pinned_accessibility_tests {
using Access = ScreenshotPinnedWindowTestAccess;

QAccessibleInterface* content(QAccessibleInterface* root) {
    require(root && root->isValid(), "pin has a valid accessible root");
    QAccessibleInterface* result = nullptr;
    for (int i = 0; i < root->childCount(); ++i) {
        auto* child = root->child(i);
        require(child && child->parent() == root && root->indexOfChild(child) == i,
                "accessible children have consistent parent and index relationships");
        if (child->text(QAccessible::Identifier) == QStringLiteral("screenshotPinnedContent")) {
            require(!result, "pin exposes exactly one semantic content child");
            result = child;
        }
    }
    require(result && result->role() == QAccessible::Graphic,
            "pin content has a semantic image role on both platforms");
    return result;
}

void checkBounds(ScreenshotPinnedWindow& window, QAccessibleInterface* root,
                 QAccessibleInterface* image) {
    const QRect expected = snow_shot::presentation::pinnedLogicalRect(
        Access::contentGeometry(window), window.screen());
    const QRect actual = image->rect();
    require(!actual.isEmpty() && root->rect().contains(actual) && actual != root->rect(),
            "content bounds exclude the permanent window padding");
    // Native-to-Qt origins and the accessibility API each round to integer DIPs.
    require((actual.topLeft() - expected.topLeft()).manhattanLength() <= 2 &&
                std::abs(actual.width() - expected.width()) <= 1 &&
                std::abs(actual.height() - expected.height()) <= 1,
            "content bounds match the native content in desktop DIPs");
    // Zoom readouts and hover controls can cover the center of a small pin at
    // high DPI. Use the clear inset above those controls for the image hit.
    const QPoint imagePoint = actual.topLeft() + QPoint(2, 2);
    require(root->childAt(imagePoint.x(), imagePoint.y()) == image,
            "hit testing reaches semantic content through the full-window canvas");
    const QPoint padding = root->rect().topLeft() + QPoint(1, 1);
    require(!root->childAt(padding.x(), padding.y()),
            "decoration padding is not exposed as an image or canvas hit");
}

QVector<QAccessible::Event>* recordedEvents = nullptr;
QObject* recordedObject = nullptr;
void recordEvent(QAccessibleEvent* event) {
    if (recordedEvents && event->object() == recordedObject)
        recordedEvents->push_back(event->type());
}

void imagePin() {
    auto config = pinned_crop_tests::config();
    QAccessible::Id contentId = 0;
    QVector<QAccessible::Event> events;
    const auto oldHandler = QAccessible::installUpdateHandler(recordEvent);
    const auto restoreHandler = qScopeGuard([&] {
        QAccessible::installUpdateHandler(oldHandler);
        recordedEvents = nullptr;
        recordedObject = nullptr;
    });
    recordedEvents = &events;
    {
        ScreenshotPinnedWindow window;
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        auto* root = QAccessible::queryAccessibleInterface(&window);
        auto* image = content(root);
        contentId = QAccessible::uniqueId(image);
        recordedObject = image->object();
        require(image->state().invisible && image->rect().isEmpty(),
                "unpresented pins do not expose visible content");
        require(window.present(config), "present the accessibility image fixture");
        QCoreApplication::processEvents();
        checkBounds(window, root, image);
        require(image->text(QAccessible::Name) == QStringLiteral("Pinned content") &&
                    !image->state().focusable && !image->state().focused && image->state().readOnly,
                "content is named without adding a keyboard focus target");
        require(events.contains(QAccessible::ObjectShow), "content announces presentation");
        require(root->child(-1) == nullptr && root->child(root->childCount()) == nullptr &&
                    root->indexOfChild(nullptr) == -1,
                "accessible child lookup rejects invalid indices");
        auto* canvas = window.findChild<SnowCanvasWidget*>();
        require(canvas && QAccessible::queryAccessibleInterface(canvas)->rect() == image->rect(),
                "canvas accessibility also excludes padding for enumerating clients");
        require(root->indexOfChild(
                    QAccessible::queryAccessibleInterface(Access::decoration(window))) == -1,
                "the painted border cannot mask content during child enumeration");

        // Preserve real controls and editable canvas descendants in the hit path.
        QPushButton button(QStringLiteral("Fixture action"), &window);
        button.setGeometry(QRect(window.mapFromGlobal(image->rect().center()), QSize(50, 25)));
        button.show();
        button.raise();
        auto* buttonAccessible = QAccessible::queryAccessibleInterface(&button);
        const QPoint buttonPoint = buttonAccessible->rect().center();
        require(root->childAt(buttonPoint.x(), buttonPoint.y()) == buttonAccessible,
                "real pinned controls take priority over the content image");
        button.hide();
        QLineEdit editor(canvas);
        editor.setGeometry(QRect(canvas->mapFromGlobal(image->rect().center()), QSize(60, 25)));
        editor.show();
        auto* editorAccessible = QAccessible::queryAccessibleInterface(&editor);
        const QPoint editorPoint = editorAccessible->rect().center();
        require(root->childAt(editorPoint.x(), editorPoint.y()) == editorAccessible,
                "canvas text editors remain reachable through presentation containers");
        editor.hide();

        events.clear();
        const QRect before = image->rect();
        require(Access::moveWindow(window, window.currentNativeGeometry().translated(41, 29)),
                "move accessibility fixture");
        ScreenshotLivePinnedWindowTestAccess::scale(window, 150);
        QCoreApplication::processEvents();
        checkBounds(window, root, image);
        require(image->rect() != before && content(root) == image &&
                    events.contains(QAccessible::LocationChanged),
                "move and zoom retain the child identity and announce new bounds");
        require(Access::crop(window, {110, 210, 60, 50}), "crop accessibility fixture");
        QCoreApplication::processEvents();
        checkBounds(window, root, image);
        Access::shadowState(window, false, false);
        checkBounds(window, root, image);

        class ContentTranslator final : public QTranslator {
          public:
            QString translate(const char* context, const char* source, const char*,
                              int) const override {
                return QByteArray(context) == "ScreenshotPinnedWindow" &&
                               QByteArray(source) == "Pinned content"
                           ? QStringLiteral("Translated pinned content")
                           : QString();
            }
        } translator;
        QCoreApplication::installTranslator(&translator);
        const auto removeTranslator =
            qScopeGuard([&] { QCoreApplication::removeTranslator(&translator); });
        QEvent languageChange(QEvent::LanguageChange);
        QCoreApplication::sendEvent(&window, &languageChange);
        require(image->text(QAccessible::Name) == QStringLiteral("Translated pinned content") &&
                    events.contains(QAccessible::NameChanged),
                "content name retranslates and notifies accessibility clients");
        events.clear();
        window.hide();
        require(image->state().invisible && image->state().offscreen && image->rect().isEmpty() &&
                    events.contains(QAccessible::ObjectHide),
                "hidden pins withdraw their accessible content");
        window.show();
        checkBounds(window, root, image);
        require(content(root) == image && events.contains(QAccessible::ObjectShow),
                "restoring the pin preserves its content identity");
    }
    require(QAccessible::accessibleInterface(contentId) == nullptr &&
                events.contains(QAccessible::ObjectDestroyed),
            "destroying a pin retires its accessible child and cached ID");
}

void livePin() {
    auto source = std::make_shared<live_pinned_tests::FakeSource>();
    source->region = QRect(180, 160, 320, 240);
    ScreenshotPinnedWindow window;
    live_pinned_tests::present(window, live_pinned_tests::config(source));
    auto* root = QAccessible::queryAccessibleInterface(&window);
    auto* image = content(root);
    auto* canvas = window.findChild<SnowCanvasWidget*>();
    for (const bool native : {false, true, false}) {
        source->setNativePresentation(native);
        QCoreApplication::processEvents();
        require(canvas->isVisible() != native, "fixture switches native and canvas presentation");
        require(content(root) == image && !image->state().invisible,
                "live presentation keeps one visible content element when the canvas is hidden");
        checkBounds(window, root, image);
    }
    window.hide();
}

void run() {
    const bool wasActive = QAccessible::isActive();
    QAccessible::setActive(true);
    const auto restoreActive = qScopeGuard([wasActive] { QAccessible::setActive(wasActive); });
    imagePin();
    livePin();
}
} // namespace pinned_accessibility_tests
