// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/imageeditingcanvas.h"
#include "snow_shot/presentation/imageeditingwindow.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshottableeditor.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "widgets/modal.h"

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScopeGuard>
#include <QThread>
#include <memory>
#include <stdexcept>

#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
namespace {
using adqt::widgets::AdModal;
using snow_shot::presentation::ImageEditingSource;
using snow_shot::presentation::ImageEditingWindow;
using snow_shot::presentation::ImageEditingWindowCoordinator;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void drainEvents() {
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

void awaitCondition(const std::function<bool()>& condition, const char* message) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 15000) {
        drainEvents();
        QThread::msleep(1);
    }
    require(condition(), message);
}

ImageEditingSource tableSource() {
    ImageEditingSource result;
    result.image = QImage(16, 12, QImage::Format_ARGB32);
    result.image.fill(Qt::white);
    result.canvasRect = QRectF(QPointF(), QSizeF(result.image.size()));
    result.recognition.key = QStringLiteral("image-editing-close-table");
    result.recognition.table =
        SnowShotTableResult{QStringLiteral("<table><tr><td>Original</td></tr></table>"), {}, {}, 0};
    result.recognition.tableModelSelection = QStringLiteral("dedicated");
    return result;
}

ScreenshotTableEditor* presentTable(ImageEditingWindow& window) {
    window.present(tableSource());
    window.palette()->tableRequested();
    auto* surface = window.canvas()->parentWidget();
    awaitCondition(
        [&] {
            const auto* table = surface->findChild<ScreenshotTableEditor*>();
            return table && table->model()->index(0, 0).data(Qt::EditRole).toString() ==
                                QStringLiteral("Original");
        },
        "cached table recognition opens the populated table editor");
    auto* table = surface->findChild<ScreenshotTableEditor*>();
    require(table->model()->index(0, 0).data(Qt::EditRole).toString() == QStringLiteral("Original"),
            "table fixture starts with its recognized cell contents");
    return table;
}

ImageEditingWindow* openTable(ImageEditingWindowCoordinator& coordinator) {
    coordinator.presentSource(ScreenshotExportSource::fromImage(tableSource().image));
    require(coordinator.windows().size() == 1, "coordinator owns one table editing window");
    const QPointer<ImageEditingWindow> window = coordinator.windows().first();
    awaitCondition([&] { return window && !window->document()->image().isNull(); },
                   "coordinator finishes loading the table image");
    static_cast<void>(presentTable(*window));
    return window;
}

void copyTable(ImageEditingWindow& window, const QString& expected) {
    int exports = 0;
    bool succeeded = false;
    const auto connection =
        QObject::connect(&window, &ImageEditingWindow::exportFinished, &window, [&](bool success) {
            ++exports;
            succeeded = success;
        });
    window.palette()->copyRequested();
    QObject::disconnect(connection);
    const auto* mime = QApplication::clipboard()->mimeData();
    require(exports == 1 && succeeded && mime && mime->text().contains(expected) &&
                mime->html().contains(expected),
            "table Copy exports the current cell contents once as text and HTML");
    require(!window.dirty(), "successful table export establishes a clean content checkpoint");
}

QPointer<AdModal> confirmation(ImageEditingWindow& window) {
    for (auto* dialog : window.findChildren<AdModal*>())
        if (dialog != window.modal() && dialog->isOpen())
            return dialog;
    return {};
}

enum class CellEdit { Active, Deferred };

QString editCell(ScreenshotTableEditor& table, CellEdit kind, const char* scenario) {
    const auto index = table.model()->index(0, 0);
    const auto before = index.data(Qt::EditRole).toString();
    const auto text = kind == CellEdit::Active ? QStringLiteral("Unsaved active cell")
                                               : QStringLiteral("Unsaved deferred cell");
    QString afterDraft;
    if (kind == CellEdit::Active) {
        table.setCurrentIndex(index);
        table.edit(index);
        auto* cell = table.findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"));
        require(cell && table.isEditingCell(), "table opens an active cell editor");
        cell->setPlainText(text);
        afterDraft = index.data(Qt::EditRole).toString();
    } else {
        // Return can submit the cell again during focus-out and synchronously flush the
        // first write. Submit through the real public delegate exactly once to retain
        // its queued model replacement without leaving a live view editor.
        QPlainTextEdit cell;
        cell.setPlainText(text);
        afterDraft = index.data(Qt::EditRole).toString();
        table.itemDelegate()->setModelData(&cell, table.model(), index);
        require(!table.isEditingCell(), "deferred delegate commit has no active cell editor");
    }
    // Deliberately avoid processing events: the model still precedes the active draft or
    // the delegate's queued replacement, so each entry point must commit it itself.
    const auto afterDelegate = index.data(Qt::EditRole).toString();
    if (afterDelegate != QStringLiteral("Original"))
        throw std::runtime_error(
            QStringLiteral("%1/%2 pending-cell fixture: model before '%3', after draft '%4', "
                           "after delegate '%5'; expected an uncommitted Original model")
                .arg(QString::fromLatin1(scenario),
                     kind == CellEdit::Active ? QStringLiteral("active")
                                              : QStringLiteral("deferred"),
                     before, afterDraft, afterDelegate)
                .toStdString());
    return text;
}

void cleanExportedTableClosesWithoutPrompt() {
    ImageEditingWindow window;
    static_cast<void>(presentTable(window));
    copyTable(window, QStringLiteral("Original"));
    int closes = 0;
    QObject::connect(&window, &ImageEditingWindow::closed, &window, [&] { ++closes; });
    window.requestClose();
    require(closes == 1 && !window.modal()->isOpen() && !confirmation(window),
            "closing an exported unchanged table needs no discard confirmation");
}

void importedTablesCloseWithoutPrompt() {
    for (const bool draft : {false, true}) {
        for (const bool asynchronous : {false, true}) {
            auto source = tableSource();
            ScreenshotTableRecognitionEntry entry;
            entry.model = QStringLiteral("dedicated");
            entry.result = *source.recognition.table;
            if (asynchronous)
                entry.result.html.prepend(QString(20000, u' '));
            if (draft)
                entry.draftHtml = QStringLiteral("<table><tr><td>Imported draft</td></tr></table>");
            source.recognition.tableEntries = {entry};
            source.recognition.table.reset();
            ImageEditingWindow window;
            window.present(source);
            window.palette()->tableRequested();
            auto* surface = window.canvas()->parentWidget();
            const QString expected =
                draft ? QStringLiteral("Imported draft") : QStringLiteral("Original");
            awaitCondition(
                [&] {
                    const auto* table = surface->findChild<ScreenshotTableEditor*>();
                    return table &&
                           table->model()->index(0, 0).data(Qt::EditRole).toString() == expected;
                },
                "an imported table opens with its existing draft and lazy parsing policy");
            require(!window.dirty(), "viewing an imported table preserves its clean checkpoint");
            int closes = 0;
            QObject::connect(&window, &ImageEditingWindow::closed, &window, [&] { ++closes; });
            window.requestClose();
            require(closes == 1 && !window.modal()->isOpen() && !confirmation(window),
                    "an untouched imported table closes without a discard confirmation");
        }
    }
}

void importedTableCheckpointsFollowDraftsAcrossModels() {
    using Session = ScreenshotRecognitionSessionController;
    const snow_shot::storage::ScreenshotRecognitionModelSettings settings;
    const QString previousModel = settings.tableModel();
    const auto restore = qScopeGuard([&] { settings.setTableModel(previousModel); });
    Session session(nullptr, nullptr, nullptr, {});
    session.setIndependentEditing(true);
    const auto image = tableSource().image;
    ScreenshotRecognitionResults imported;
    imported.key = QStringLiteral("imported-table-checkpoints");
    ScreenshotTableRecognitionEntry first;
    first.model = QStringLiteral("imported-table-model-a");
    first.result.html =
        QString(20000, u' ') + QStringLiteral("<table><tr><td>First recognized</td></tr></table>");
    first.draftHtml = QStringLiteral("<table><tr><td>First imported draft</td></tr></table>");
    auto second = first;
    second.model = QStringLiteral("imported-table-model-b");
    second.result.html = QStringLiteral("<table><tr><td>Second recognized</td></tr></table>");
    second.draftHtml = QStringLiteral("<table><tr><td>Second imported draft</td></tr></table>");
    imported.tableEntries = {first, second};
    imported.tableModelSelection = first.model;
    session.setTarget({imported.key, image, QRectF(QPointF(), QSizeF(image.size()))});
    session.seedRecognitionResults(imported);
    const auto clean = session.contentCheckpoints();
    require(clean.size() == 2, "both imported models have checkpoints before table parsing");
    session.activate(Session::Mode::Table);
    require(session.busy(Session::Mode::Table),
            "large imported tables retain asynchronous parsing");
    const auto activeText = [&] {
        const auto cells = session.workflowResult().value(QStringLiteral("cells")).toArray();
        return cells.isEmpty() ? QString{}
                               : cells.first().toObject().value(QStringLiteral("text")).toString();
    };
    awaitCondition([&] { return activeText() == QStringLiteral("First imported draft"); },
                   "the first model materializes its imported draft");
    require(session.contentCheckpoints() == clean,
            "canonical table parsing leaves both source checkpoints unchanged");
    const QString firstKey = session.activeContentKey();
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_cell")},
                                  {QStringLiteral("row"), 0},
                                  {QStringLiteral("column"), 0},
                                  {QStringLiteral("text"), QStringLiteral("First edited")}}),
            "the first imported draft can be edited");
    const auto edited = session.contentCheckpoints();
    require(edited.size() == 2 && edited.value(firstKey) != clean.value(firstKey),
            "a real edit changes only its model's checkpoint");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("undo")}}) &&
                session.contentCheckpoints() == clean,
            "undo to the imported draft restores the original source checkpoint");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("redo")}}) &&
                session.contentCheckpoints() == edited,
            "redo restores the edited checkpoint");
    session.setRecognitionModel(second.model);
    require(activeText() == QStringLiteral("Second imported draft") &&
                session.contentCheckpoints() == edited,
            "viewing another imported model preserves every model's checkpoint");
    const QString secondKey = session.activeContentKey();
    require(secondKey != firstKey && edited.value(secondKey) == clean.value(secondKey),
            "the models retain independent checkpoint identities");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("reset_table")}}) &&
                activeText() == QStringLiteral("Second recognized") &&
                session.contentCheckpoints().value(secondKey) != clean.value(secondKey),
            "resetting an imported draft to recognized content is an unsaved edit");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("undo")}}) &&
                session.contentCheckpoints() == edited,
            "undoing Reset restores that model's imported draft checkpoint");
    session.setRecognitionModel(first.model);
    require(activeText() == QStringLiteral("First edited") &&
                session.contentCheckpoints() == edited,
            "returning to a model restores its retained edits and checkpoint");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("undo")}}) &&
                session.contentCheckpoints() == clean,
            "undo restores all imported content after switching models");
}

void closeCommitsPendingCellBeforeReview() {
    for (const auto kind : {CellEdit::Active, CellEdit::Deferred}) {
        ImageEditingWindowCoordinator coordinator({});
        QPointer<ImageEditingWindow> window = openTable(coordinator);
        auto* table = window->canvas()->parentWidget()->findChild<ScreenshotTableEditor*>();
        copyTable(*window, QStringLiteral("Original"));
        const auto text = editCell(*table, kind, "requestClose");
        window->requestClose();
        require(window && window->modal()->isOpen() && coordinator.windows().size() == 1,
                "uncommitted table text prevents the editor from closing silently");
        auto decision = confirmation(*window);
        require(decision && table->model()->index(0, 0).data(Qt::EditRole).toString() == text,
                "close commits the table draft before presenting the discard decision");
        decision->reject();
        awaitCondition([&] { return !decision; }, "Continue Editing dismisses its decision");
        require(window && window->modal()->isOpen() && window->dirty() &&
                    table->model()->index(0, 0).data(Qt::EditRole).toString() == text,
                "cancelling close preserves the pending cell text and its unsaved state");
        window->requestClose();
        decision = confirmation(*window);
        require(decision, "the preserved draft still requires a discard decision");
        decision->accept();
        awaitCondition([&] { return !window && coordinator.windows().isEmpty(); },
                       "accepted discard deletes the coordinator-owned table editor");
    }
}

void shutdownReviewCommitsPendingCellBeforeCleanScan() {
    for (const auto kind : {CellEdit::Active, CellEdit::Deferred}) {
        ImageEditingWindowCoordinator coordinator({});
        QPointer<ImageEditingWindow> window = openTable(coordinator);
        auto* table = window->canvas()->parentWidget()->findChild<ScreenshotTableEditor*>();
        copyTable(*window, QStringLiteral("Original"));
        const auto text = editCell(*table, kind, "confirmShutdown");
        bool reviewed = false, accepted = false;
        coordinator.confirmShutdown([&](bool value) {
            reviewed = true;
            accepted = value;
        });
        require(!reviewed && window && window->modal()->isOpen(),
                "quit review cannot approve a clean checkpoint while a table draft is pending");
        auto decision = confirmation(*window);
        require(decision, "quit review prompts for an active or deferred table cell edit");
        require(
            !coordinator.prepareForUpdateHandoff() && confirmation(*window) == decision,
            "final update admission refuses an outstanding review without replacing its prompt");
        decision->reject();
        require(reviewed && !accepted && window->dirty() &&
                    table->model()->index(0, 0).data(Qt::EditRole).toString() == text,
                "Continue Editing cancels quit while preserving the cell draft");
        awaitCondition([&] { return !decision; }, "cancelled quit releases its discard decision");
        reviewed = false;
        coordinator.confirmShutdown([&](bool value) {
            reviewed = true;
            accepted = value;
        });
        decision = confirmation(*window);
        require(decision && !reviewed, "a later quit still reviews the retained cell changes");
        decision->accept();
        require(reviewed && accepted && window,
                "accepting the current cell checkpoint permits coordinated shutdown");
        coordinator.shutdown();
        awaitCondition([&] { return !window && coordinator.windows().isEmpty(); },
                       "approved shutdown deletes the table editor and its draft");
    }
}

void updateHandoffPreparesPendingCellsWithoutPrompting() {
    for (const auto kind : {CellEdit::Active, CellEdit::Deferred}) {
        ImageEditingWindowCoordinator coordinator({});
        QPointer<ImageEditingWindow> window = openTable(coordinator);
        auto* table = window->canvas()->parentWidget()->findChild<ScreenshotTableEditor*>();
        copyTable(*window, QStringLiteral("Original"));
        require(coordinator.prepareForUpdateHandoff(),
                "a clean stable editor permits final update preparation");
        const auto text = editCell(*table, kind, "prepareForUpdateHandoff");
        require(!coordinator.prepareForUpdateHandoff() && window && window->dirty() &&
                    table->model()->index(0, 0).data(Qt::EditRole).toString() == text &&
                    !confirmation(*window) && coordinator.windows().size() == 1,
                "final update admission commits pending cell edits and refuses without a prompt");
        copyTable(*window, text);
        require(coordinator.prepareForUpdateHandoff(),
                "an explicitly exported draft permits a later final update attempt");
    }
}

void updateHandoffCommitsCanvasDraftAndRejectsDetachedRuntime() {
    ImageEditingWindowCoordinator coordinator({});
    QImage image(200, 160, QImage::Format_ARGB32);
    image.fill(Qt::white);
    coordinator.presentSource(ScreenshotExportSource::fromImage(image));
    QPointer<ImageEditingWindow> window = coordinator.windows().first();
    awaitCondition([&] { return window && !window->document()->image().isNull(); },
                   "canvas draft editor finishes loading its clean source");
    window->canvas()->setMode(snow_shot::presentation::ImageEditingCanvas::Mode::Draw);
    require(window->canvas()->setCanvasTool(SnowCanvasTool::Text),
            "final handoff fixture activates pending canvas text");
    const QPointF point = window->canvas()->canvasToViewTransform().map(QPointF(80, 70));
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, point, window->canvas()->mapToGlobal(point), Qt::LeftButton,
                          type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                          Qt::NoModifier);
        QApplication::sendEvent(window->canvas(), &event);
    }
    require(window->canvas()->hasActiveTextEditing(), "canvas owns a pending text draft");
    QKeyEvent input(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("Pending text"));
    QApplication::sendEvent(window->canvas(), &input);
    require(!coordinator.prepareForUpdateHandoff() && window &&
                !window->canvas()->hasActiveTextEditing() && window->dirty() &&
                !confirmation(*window),
            "final admission commits pending canvas text and refuses its unsaved contents");
    coordinator.shutdown();
    awaitCondition([&] { return !window; }, "canvas draft fixture releases its editor");
    window = openTable(coordinator);
    copyTable(*window, QStringLiteral("Original"));
    window->document()->runtime().destroyAsync();
    require(!coordinator.prepareForUpdateHandoff() && window && !confirmation(*window),
            "failed canvas preparation cannot approve final handoff from a clean checkpoint");
}

void updateHandoffRejectsReentrantMembershipChanges() {
    enum class Change { AddWindow, DestroyWindow, DestroyCoordinator, ReenterPreparation };
    for (const auto change : {Change::AddWindow, Change::DestroyWindow, Change::DestroyCoordinator,
                              Change::ReenterPreparation}) {
        auto coordinator = std::make_unique<ImageEditingWindowCoordinator>(
            snow_shot::presentation::ImageEditingServices{});
        QPointer<ImageEditingWindow> window = openTable(*coordinator);
        copyTable(*window, QStringLiteral("Original"));
        require(window->canvas()->setCanvasTool(SnowCanvasTool::Shape) && !window->dirty(),
                "a transient tool choice leaves the editor clean before preparation");
        bool changed = false;
        bool reentrantAccepted = true;
        const auto connection =
            QObject::connect(window->canvas(), &SnowCanvasWidget::activeToolChanged, qApp, [&] {
                if (changed)
                    return;
                changed = true;
                if (change == Change::AddWindow)
                    coordinator->presentSource(
                        ScreenshotExportSource::fromImage(tableSource().image));
                else if (change == Change::DestroyWindow)
                    delete window.data();
                else if (change == Change::DestroyCoordinator)
                    coordinator.reset();
                else
                    reentrantAccepted = coordinator->prepareForUpdateHandoff();
            });
        const bool accepted = coordinator->prepareForUpdateHandoff();
        QObject::disconnect(connection);
        require(changed, "draft preparation publishes its transient canvas tool reset");
        if (change == Change::ReenterPreparation) {
            require(accepted && !reentrantAccepted && coordinator && window && !window->dirty(),
                    "nested final preparation refuses without disturbing the original clean scan");
        } else {
            require(!accepted, "changed editor membership cannot reuse the earlier final scan");
            if (change == Change::AddWindow) {
                require(coordinator && coordinator->windows().size() == 2 &&
                            !coordinator->hasUnsavedChanges(),
                        "even a newly opened clean window invalidates the preparation snapshot");
                const QPointer<ImageEditingWindow> added = coordinator->windows().last();
                awaitCondition([&] { return added && !added->document()->image().isNull(); },
                               "new editor finishes loading before a fresh final scan");
                require(coordinator->prepareForUpdateHandoff(),
                        "a later stable scan prepares both current clean windows");
            } else if (change == Change::DestroyWindow) {
                require(coordinator && !window && coordinator->windows().isEmpty(),
                        "destroyed windows leave no stale membership entries");
            } else {
                require(!coordinator, "preparation safely returns after coordinator destruction");
            }
        }
    }
}

void exportCommitsActiveAndDeferredCells() {
    for (const auto kind : {CellEdit::Active, CellEdit::Deferred}) {
        ImageEditingWindow window;
        auto* table = presentTable(window);
        copyTable(window, QStringLiteral("Original"));
        const auto text = editCell(*table, kind, "export");
        copyTable(window, text);
        require(table->model()->index(0, 0).data(Qt::EditRole).toString() == text &&
                    window.modal()->isOpen(),
                "export commits the exact pending cell text and keeps the editor open");
        int closes = 0;
        QObject::connect(&window, &ImageEditingWindow::closed, &window, [&] { ++closes; });
        window.requestClose();
        require(closes == 1 && !confirmation(window),
                "the exported cell checkpoint closes without another discard prompt");
    }
}

void discardCompletionMayDestroyItsEditor() {
    for (const bool discard : {false, true}) {
        auto window = std::make_unique<ImageEditingWindow>();
        const QPointer<ImageEditingWindow> lifetime(window.get());
        auto* table = presentTable(*window);
        copyTable(*window, QStringLiteral("Original"));
        static_cast<void>(editCell(*table, CellEdit::Active, "discard completion destruction"));
        bool completed = false, accepted = !discard;
        window->confirmDiscard([&](bool value) {
            completed = true;
            accepted = value;
            window.reset();
        });
        require(lifetime && window && !completed,
                "dirty editor retains itself until the discard decision is answered");
        auto decision = confirmation(*window);
        require(decision && !completed, "dirty editor defers its discard completion until review");
        if (discard)
            decision->accept();
        else
            decision->reject();
        require(completed && accepted == discard && !lifetime && !window,
                "discard completion may synchronously destroy its editor for either decision");
        awaitCondition([&] { return !decision; },
                       "discard decision is released after its completion destroys the editor");
    }
}

void shutdownReviewCompletionMayDestroyItsCoordinator() {
    auto coordinator = std::make_unique<ImageEditingWindowCoordinator>(
        snow_shot::presentation::ImageEditingServices{});
    const QPointer<ImageEditingWindowCoordinator> lifetime(coordinator.get());
    const QPointer<ImageEditingWindow> window(openTable(*coordinator));
    auto* table = window->canvas()->parentWidget()->findChild<ScreenshotTableEditor*>();
    copyTable(*window, QStringLiteral("Original"));
    static_cast<void>(editCell(*table, CellEdit::Deferred, "coordinator completion destruction"));
    bool completed = false, accepted = false;
    coordinator->confirmShutdown([&](bool value) {
        completed = true;
        accepted = value;
        coordinator.reset();
    });
    require(lifetime && window && !completed,
            "coordinator retains the editor until its table changes have been reviewed");
    auto decision = confirmation(*window);
    require(decision && !completed, "coordinator review waits for the unsaved table decision");
    decision->accept();
    require(completed && accepted && !lifetime && !coordinator,
            "approved shutdown completion may synchronously destroy its coordinator");
    awaitCondition([&] { return !window && !decision; },
                   "destroying the coordinator in its completion releases the editor and decision");
}

void shutdownMayDestroyEditorThroughPendingDiscardCompletion() {
    auto window = std::make_unique<ImageEditingWindow>();
    const QPointer<ImageEditingWindow> lifetime(window.get());
    auto* table = presentTable(*window);
    copyTable(*window, QStringLiteral("Original"));
    static_cast<void>(editCell(*table, CellEdit::Active, "shutdown completion destruction"));
    bool completed = false, accepted = true;
    window->confirmDiscard([&](bool value) {
        completed = true;
        accepted = value;
        window.reset();
    });
    require(lifetime && window && !completed,
            "pending table draft keeps the editor alive until review or shutdown");
    auto decision = confirmation(*window);
    require(decision && !completed, "editor shutdown starts with an outstanding discard decision");
    window->shutdown();
    require(completed && !accepted && !lifetime && !window,
            "shutdown may reject a pending review whose completion destroys the editor");
    awaitCondition([&] { return !decision; },
                   "reentrant editor destruction releases the pending discard decision");
}
} // namespace
#endif

void runImageEditingCloseTests() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    importedTablesCloseWithoutPrompt();
    importedTableCheckpointsFollowDraftsAcrossModels();
    cleanExportedTableClosesWithoutPrompt();
    closeCommitsPendingCellBeforeReview();
    shutdownReviewCommitsPendingCellBeforeCleanScan();
    updateHandoffPreparesPendingCellsWithoutPrompting();
    updateHandoffCommitsCanvasDraftAndRejectsDetachedRuntime();
    updateHandoffRejectsReentrantMembershipChanges();
    exportCommitsActiveAndDeferredCells();
    discardCompletionMayDestroyItsEditor();
    shutdownReviewCompletionMayDestroyItsCoordinator();
    shutdownMayDestroyEditorThroughPendingDiscardCompletion();
#endif
}
