#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/storage/pinnedwindowstackingorder.h"
#include "snow_shot/presentation/pinnedwindowstackingcoordinator.h"
#include "presentation/pinned/pinnedwindowstackingplatform.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>
#include <QElapsedTimer>
#include <QWindow>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <atomic>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#elif defined(Q_OS_MACOS)
#import <AppKit/AppKit.h>
#endif

namespace storage = snow_shot::storage;
namespace presentation = snow_shot::presentation;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
QString id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
storage::PinnedWindowRecord record(const QString& key,
                                   const QString& group = QStringLiteral("default")) {
    storage::PinnedWindowRecord value;
    value.id = key;
    value.groupId = group;
    value.sourceKind = storage::PinnedWindowSourceKind::ClipboardText;
    value.originalText = QStringLiteral("Stacking fixture");
    value.nativeGeometry = QRect(0, 0, 64, 32);
    value.initialWindowSize = QSize(64, 32);
    value.canvasSourceRect = value.contentCanvasRect = value.surfaceCanvasRect =
        QRectF(0, 0, 64, 32);
    return value;
}
void processEvents() {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
QJsonObject manifest(const QString& root) {
    QFile file(root + QStringLiteral("/pinned_windows_v2/index.json"));
    require(file.open(QIODevice::ReadOnly), "read stacking manifest");
    return QJsonDocument::fromJson(file.readAll()).object();
}
void writeManifest(const QString& root, const QJsonObject& object) {
    QFile file(root + QStringLiteral("/pinned_windows_v2/index.json"));
    require(file.open(QIODevice::WriteOnly), "write stacking fixture manifest");
    require(file.write(QJsonDocument(object).toJson()) > 0, "write manifest bytes");
}
void model() {
    storage::PinnedWindowStackingOrder order;
    for (const auto& key :
         {QStringLiteral("d"), QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("a")})
        order.add(key, QStringLiteral("one"));
    const auto unchanged = order.revision();
    require(!order.observe("one", {"a", "a", "missing", "c"}) && order.revision() == unchanged,
            "duplicate, stale and unchanged observations are no-ops");
    require(order.observe("one", {"c", "a"}) &&
                order.order("one") == QStringList({"c", "b", "a", "d"}),
            "unobserved slots retain hidden members");
    order.add("x", "two");
    order.move({"a", "c"}, "two");
    require(order.order("two") == QStringList({"c", "a", "x"}) &&
                order.order("one") == QStringList({"b", "d"}),
            "batch transfers preserve source order");
    require(!order.observe("two", {"d", "missing", "a"}), "wrong-group IDs cannot move");
    require(order.promote("x") && order.order("two") == QStringList({"x", "c", "a"}),
            "promote member");
    order.remove("c");
    require(order.order("two") == QStringList({"x", "a"}),
            "remove after observe and splice preserves iterators");
}
void persistence() {
    QTemporaryDir directory;
    const QString a = id(), b = id(), c = id(), pending = id();
    QStringList expected{b, c, a};
    {
        int payloadReads = 0, notifications = 0;
        storage::PinnedWindowRepository repository(
            directory.path(), true, 60000, [&](storage::PinnedWindowOperation operation) {
                if (operation == storage::PinnedWindowOperation::PayloadRead)
                    ++payloadReads;
            });
        for (const auto& key : {a, b, c})
            require(repository.create(record(key)).success, "seed stacking records");
        require(repository.flush().success, "flush sources");
        const auto preview = repository.previewSourceRevision(a);
        const auto source = directory.filePath("pinned_windows_v2/pins/" + a + "/original.txt");
        const auto modified = QFileInfo(source).lastModified();
        require(QFileInfo(source).exists() && modified.isValid(), "source payload exists on disk");
        const auto membership = repository.membershipRevision(),
                   visibility = repository.visibilityRevision();
        repository.setChangedCallback([&] { ++notifications; });
        require(repository.updateStackingOrder("default", expected).success,
                "persist visual order");
        require(repository.flush().success && payloadReads == 0 && notifications == 0 &&
                    repository.previewSourceRevision(a) == preview &&
                    repository.membershipRevision() == membership &&
                    repository.visibilityRevision() == visibility &&
                    QFileInfo(source).lastModified() == modified,
                "stacking writes only metadata without unrelated invalidation");
        const auto revision = repository.revision();
        require(repository.updateStackingOrder("default", expected).success &&
                    repository.revision() == revision,
                "unchanged order does not dirty storage");
        auto updated = record(a);
        updated.updatedUtc = QDateTime::currentDateTimeUtc().addDays(1);
        require(repository.updateState(updated).success &&
                    repository.stackingOrder("default") == expected,
                "late state saves cannot replace order");
        require(repository.setRecordsHidden({c}, true).success &&
                    repository.markClosedDeferred(b).success,
                "hide and close fixture");
        require(repository.stackingOrder("default") == expected, "visibility does not erase order");
        repository.reserveCreation(pending);
        require(repository.promoteStackingWindow(a).success, "promote while first save is pending");
        require(repository.createReserved(record(pending)).success, "complete pending save");
        require(repository.stackingOrder("default") == QStringList({a, pending, b, c}),
                "first save retains newer stacking decisions");
        repository.reserveCreation(id());
        require(repository.removeMany({pending}).success, "delete stacking member");
        expected = {a, b, c};
        require(repository.flush().success, "flush final order");
    }
    {
        storage::PinnedWindowRepository repository(directory.path());
        require(repository.stackingOrder("default") == expected,
                "restart retains order and prunes unsaved reservations");
        const QString group = id();
        require(repository
                    .setGroups({{"default", "Default", true}, {group, "Second", false}}, "default")
                    .success,
                "create second group");
        require(repository.setRecordsGroup({c, a}, group).success &&
                    repository.stackingOrder(group) == QStringList({a, c}),
                "group move uses saved order instead of selection order");
        repository.reserveCreation(pending, QDateTime::currentDateTimeUtc(), group);
        repository.cancelCreation(pending);
        require(repository.stackingOrder(group) == QStringList({a, c}), "cancel pending member");
        require(repository.removeGroupAndRecords(group).success &&
                    repository.stackingOrder(group).isEmpty(),
                "group destruction removes order");
        require(repository.clearClosed().success && repository.stackingOrder("default").isEmpty(),
                "closed retention removes order");
        const QString expired = id();
        const auto now = QDateTime::currentDateTimeUtc();
        require(repository.create(record(expired)).success &&
                    repository.markClosedDeferred(expired, now.addDays(-365)).success &&
                    repository.enforcePolicy(now).success &&
                    repository.stackingOrder("default").isEmpty(),
                "retention expiration removes the saved stacking member");
    }
}
void legacyAndExchange() {
    QTemporaryDir directory, other;
    const QString a = id(), b = id(), c = id();
    {
        storage::PinnedWindowRepository repository(directory.path());
        int sequence = 0;
        for (const auto& key : {a, b, c}) {
            auto value = record(key);
            value.updatedUtc = QDateTime::fromMSecsSinceEpoch(1700000000000LL + sequence++);
            require(repository.create(value).success, "legacy records");
        }
    }
    auto object = manifest(directory.path());
    object.remove("stacking_order");
    writeManifest(directory.path(), object);
    {
        storage::PinnedWindowRepository repository(directory.path());
        require(repository.stackingOrder("default") == QStringList({c, b, a}),
                "legacy timestamp fallback");
    }
    object["stacking_order"] = QJsonObject{{"default", QJsonArray{b, b, "stale"}}};
    writeManifest(directory.path(), object);
    storage::PinnedWindowRepository repository(directory.path()), prepared(other.path());
    require(repository.stackingOrder("default") == QStringList({b, c, a}),
            "partial orders keep valid prefix and append fallback");
    require(prepared.create(record(a)).success && prepared.flush().success, "prepared storage");
    const auto revision = repository.stackingRevision();
    repository.suspendWrites(true);
    prepared.suspendWrites(true);
    repository.exchangeStorage(prepared);
    repository.suspendWrites(false);
    require(repository.stackingOrder("default") == QStringList({a}) &&
                repository.stackingRevision() > revision,
            "storage exchange replaces ordering generation");
}

struct FakeDesktop {
    presentation::PinnedStackingWindows windows;
    int captures = 0, placements = 0, applications = 0;
    bool fail = false;
    presentation::PinnedWindowStackingMetrics metrics;
    static qint64 level(QWidget* window) {
        return window->property("stackingLevel").toLongLong();
    }
    presentation::PinnedWindowStackingPlatform port(bool native = false) {
        if (native) {
            auto backend = presentation::createPinnedWindowStackingPlatform(&metrics);
            require(bool(backend.capture), "native stacking backend is available");
            return {[this, capture = backend.capture](const auto& candidates) {
                        ++captures;
                        return capture(candidates);
                    },
                    backend.level,
                    [this, place = backend.place](auto* window, auto* above, auto* below) {
                        ++placements;
                        return place(window, above, below);
                    },
                    [this, apply = backend.apply](const auto& desired) {
                        ++applications;
                        return apply(desired);
                    }};
        }
        return {
            [this](const auto& candidates) -> std::optional<presentation::PinnedStackingWindows> {
                ++captures;
                presentation::PinnedStackingWindows result;
                for (const auto& window : windows)
                    if (window && window->isVisible() && candidates.contains(window))
                        result.append(window);
                return result;
            },
            level,
            [this](QWidget* window, QWidget* above, QWidget* below) {
                ++placements;
                if (fail)
                    return false;
                if (!above && !below)
                    return true;
                windows.removeAll(window);
                const auto index = above ? windows.indexOf(above) + 1 : windows.indexOf(below);
                require(index >= 0, "restore neighbor is available");
                windows.insert(index, window);
                return true;
            },
            [this](const auto& desired) {
                ++applications;
                if (fail)
                    return false;
                windows = desired;
                std::stable_sort(windows.begin(), windows.end(),
                                 [](const auto& a, const auto& b) { return level(a) > level(b); });
                return true;
            }};
    }
};
struct Fixture {
    QTemporaryDir directory;
    std::atomic_int manifestWrites{0};
    storage::PinnedWindowRepository repository{
        directory.path(), true, 60000, [this](auto operation) {
            if (operation == storage::PinnedWindowOperation::ManifestWrite)
                ++manifestWrites;
        }};
    FakeDesktop desktop;
    presentation::PinnedWindowStackingCoordinator coordinator;
    QStringList ids;
    std::vector<std::unique_ptr<QWidget>> widgets;
    explicit Fixture(int count = 4, bool native = false)
        : coordinator(&repository, desktop.port(native)) {
        for (int i = 0; i < count; ++i) {
            ids.append(id());
            require(repository.create(record(ids.last())).success, "seed restore fixture");
            auto window =
                std::make_unique<QWidget>(nullptr, Qt::Tool | Qt::WindowDoesNotAcceptFocus);
            window->setAttribute(Qt::WA_ShowWithoutActivating);
            window->setGeometry(30000, 30000, 32, 32);
            coordinator.track(window.get(), ids.last(), "default");
            widgets.push_back(std::move(window));
        }
        require(repository.updateStackingOrder("default", ids).success,
                "set desired restore order");
    }
    QWidget* window(int index) {
        return widgets.at(static_cast<size_t>(index)).get();
    }
    void present(int index, bool passive = true) {
        auto* widget = window(index);
        widget->show();
        desktop.windows.prepend(widget);
        coordinator.presented(widget, passive);
    }
    QStringList observed() const {
        QStringList result;
        for (const auto& window : desktop.windows)
            for (qsizetype i = 0; i < ids.size(); ++i)
                if (window == widgets.at(static_cast<size_t>(i)).get())
                    result.append(ids.at(i));
        return result;
    }
};
void completionPermutations() {
    std::array<int, 4> completion{0, 1, 2, 3};
    do {
        Fixture fixture;
        const auto batch = fixture.coordinator.beginRestore("default", fixture.ids);
        for (const int index : completion) {
            fixture.present(index);
            fixture.coordinator.settleRestore(batch, fixture.ids.at(index));
        }
        processEvents();
        require(fixture.observed() == fixture.ids && fixture.desktop.placements == 4 &&
                    fixture.desktop.applications == 1,
                "out-of-order completions restore once with one placement per arrival");
    } while (std::next_permutation(completion.begin(), completion.end()));
}

void presentationsDuringMutation() {
    for (bool batched : {false, true}) {
        Fixture fixture;
        const auto batch = batched ? fixture.coordinator.beginRestore("default", fixture.ids) : 0;
        fixture.coordinator.beginMutation();
        fixture.coordinator.beginMutation();
        for (int index : {0, 2, 3, 1}) {
            fixture.present(index);
            fixture.coordinator.settleRestore(batch, fixture.ids.at(index));
        }
        processEvents();
        require(fixture.desktop.placements == 0 && fixture.desktop.applications == 0 &&
                    fixture.desktop.captures == 0,
                "presentations and completed restore batches do no native work during mutation");
        fixture.coordinator.endMutation();
        processEvents();
        require(fixture.desktop.applications == 0,
                "nested mutations defer reconciliation until the outermost mutation ends");
        fixture.coordinator.endMutation();
        // A checkpoint before queued reconciliation must not save the temporary arrival order.
        fixture.coordinator.checkpoint();
        processEvents();
        require(fixture.observed() == fixture.ids && fixture.desktop.applications == 1 &&
                    fixture.repository.stackingOrder("default") == fixture.ids,
                "resume reconciles every presented surface once without overwriting intent");
        fixture.desktop.windows.removeAll(fixture.window(3));
        fixture.desktop.windows.prepend(fixture.window(3));
        fixture.coordinator.promote(fixture.window(3));
        processEvents();
        require(fixture.repository.stackingOrder("default").first() == fixture.ids.at(3),
                "a surface presented during suspension participates in later activation");
        std::reverse(fixture.desktop.windows.begin(), fixture.desktop.windows.end());
        fixture.coordinator.nativeOrderChanged(fixture.window(0));
        processEvents();
        require(fixture.repository.stackingOrder("default") == fixture.observed(),
                "a surface presented during suspension participates in later native observation");
    }
}

void reconciliationInterruptedByMutation() {
    Fixture fixture;
    const auto batch = fixture.coordinator.beginRestore("default", fixture.ids);
    for (int index : {0, 2, 3, 1})
        fixture.present(index);
    // Completion has queued reconciliation, but the mutation starts before its callback runs.
    for (const auto& key : fixture.ids)
        fixture.coordinator.settleRestore(batch, key);
    fixture.coordinator.beginMutation();
    std::reverse(fixture.desktop.windows.begin(), fixture.desktop.windows.end());
    processEvents();
    require(fixture.desktop.applications == 0, "queued reconciliation stays suspended");
    fixture.coordinator.endMutation();
    processEvents();
    require(fixture.observed() == fixture.ids && fixture.desktop.applications == 1,
            "a reconciliation callback interrupted by mutation is replayed on resume");
}

void suspendedPresentationsRespectLifecycle() {
    Fixture fixture;
    fixture.coordinator.beginMutation();
    fixture.present(0);
    fixture.present(1, false);
    fixture.coordinator.retiring(fixture.window(1));
    fixture.window(1)->hide();
    fixture.widgets[2].reset();
    fixture.coordinator.endMutation();
    processEvents();
    require(fixture.observed() == QStringList({fixture.ids.at(0)}) &&
                fixture.repository.stackingOrder("default") == fixture.ids,
            "resume excludes retired and destroyed surfaces and their deferred promotions");
    fixture.coordinator.promote(fixture.window(0));
    require(fixture.repository.stackingOrder("default").first() == fixture.ids.at(0),
            "remaining surface stays ready after its suspended peers retire");
}

void canceledSuspendedPresentations() {
    for (bool shutdown : {false, true}) {
        Fixture fixture;
        const auto batch = fixture.coordinator.beginRestore("default", fixture.ids);
        fixture.coordinator.beginMutation();
        for (int index : {0, 1, 2, 3}) {
            fixture.present(index, index != 3);
            fixture.coordinator.settleRestore(batch, fixture.ids.at(index));
        }
        processEvents();
        if (shutdown)
            fixture.coordinator.shutdown();
        else
            fixture.coordinator.cancelGroup("default");
        fixture.coordinator.endMutation();
        processEvents();
        require(fixture.desktop.applications == 0 &&
                    fixture.repository.stackingOrder("default") == fixture.ids,
                "group cancellation and shutdown discard suspended reconciliation");
    }
}

void explicitPresentationDuringMutation() {
    Fixture fixture;
    fixture.coordinator.beginMutation();
    fixture.present(3, false);
    fixture.present(1, false);
    fixture.present(2);
    fixture.coordinator.endMutation();
    require(fixture.repository.stackingOrder("default") ==
                QStringList(
                    {fixture.ids.at(1), fixture.ids.at(3), fixture.ids.at(0), fixture.ids.at(2)}),
            "resume preserves explicit presentation intent in acceptance order");
    processEvents();
    require(fixture.observed() ==
                QStringList({fixture.ids.at(1), fixture.ids.at(3), fixture.ids.at(2)}),
            "explicit suspended presentations reconcile with passive peers");
}

void newerIntentDuringMutation() {
    Fixture fixture;
    fixture.coordinator.beginMutation();
    fixture.present(3, false);
    // An explicit request reserves foreground order before its payload finishes loading.
    require(fixture.repository.promoteStackingWindow(fixture.ids.at(1)).success,
            "reserve a newer foreground request during suspension");
    const auto desired = fixture.repository.stackingOrder("default");
    fixture.coordinator.endMutation();
    require(fixture.repository.stackingOrder("default") == desired,
            "deferred presentation cannot override newer accepted foreground intent");
    fixture.present(1, false);
    processEvents();
    require(fixture.observed() == QStringList({fixture.ids.at(1), fixture.ids.at(3)}),
            "the newer request retains foreground order after both presentations finish");
}

void partialRestoreDuringMutation() {
    Fixture fixture;
    const auto batch = fixture.coordinator.beginRestore("default", fixture.ids);
    fixture.coordinator.beginMutation();
    fixture.present(2);
    fixture.coordinator.settleRestore(batch, fixture.ids.at(2));
    // Complete the first arrival's presentation notifications before resuming the other loads.
    processEvents();
    fixture.coordinator.endMutation();
    processEvents();
    require(fixture.observed() == QStringList({fixture.ids.at(2)}) &&
                fixture.desktop.applications == 1 &&
                fixture.repository.stackingOrder("default") == fixture.ids,
            "resume reconciles ready surfaces while other payloads are still loading");
    for (int index : {0, 3, 1}) {
        fixture.present(index);
        fixture.coordinator.settleRestore(batch, fixture.ids.at(index));
    }
    processEvents();
    require(fixture.observed() == fixture.ids && fixture.desktop.applications == 2,
            "later arrivals join the resumed surface and complete the original restore batch");
}

void presentationDuringNativePlacement() {
    Fixture fixture;
    fixture.coordinator.shutdown();
    auto port = fixture.desktop.port();
    const auto apply = port.apply;
    presentation::PinnedWindowStackingCoordinator* receiver = nullptr;
    bool arrived = false;
    port.apply = [&](const auto& desired) {
        const bool success = apply(desired);
        if (!arrived) {
            arrived = true;
            fixture.present(0);
            receiver->presented(fixture.window(0), true);
        }
        return success;
    };
    presentation::PinnedWindowStackingCoordinator coordinator(&fixture.repository, port);
    receiver = &coordinator;
    for (int i : {0, 1, 2})
        coordinator.track(fixture.window(i), fixture.ids.at(i), "default");
    const auto batch = coordinator.beginRestore("default", {fixture.ids.at(1), fixture.ids.at(2)});
    for (int i : {1, 2}) {
        fixture.present(i);
        coordinator.presented(fixture.window(i), true);
        coordinator.settleRestore(batch, fixture.ids.at(i));
    }
    processEvents();
    processEvents();
    require(fixture.observed() == fixture.ids.mid(0, 3),
            "a reentrant presentation remains ready and reconciles after native placement");
    require(fixture.repository.promoteStackingWindow(fixture.ids.at(1)).success,
            "move another pin ahead before testing reentrant surface activation");
    coordinator.promote(fixture.window(0));
    require(fixture.repository.stackingOrder("default").first() == fixture.ids.at(0),
            "a presentation during native placement participates in later activation");
    coordinator.shutdown();
}

void interactionsAndLifecycle() {
    Fixture fixture;
    auto batch = fixture.coordinator.beginRestore("default", fixture.ids);
    fixture.present(2);
    fixture.desktop.windows = {fixture.window(2)};
    fixture.coordinator.promote(fixture.window(2));
    for (const int index : {0, 1, 3})
        fixture.present(index);
    for (const auto& key : fixture.ids)
        fixture.coordinator.settleRestore(batch, key);
    processEvents();
    require(fixture.observed().first() == fixture.ids.at(2),
            "late completions respect user activation");
    const auto captures = fixture.desktop.captures;
    fixture.desktop.windows = {fixture.window(3), fixture.window(2), fixture.window(1),
                               fixture.window(0)};
    for (int i = 0; i < 100; ++i)
        fixture.coordinator.nativeOrderChanged(fixture.window(3));
    processEvents();
    require(fixture.desktop.captures == captures + 1 &&
                fixture.repository.stackingOrder("default") == fixture.observed(),
            "native changes coalesce and persist actual order");
    fixture.coordinator.beginMutation();
    const auto saved = fixture.repository.stackingOrder("default");
    for (auto& widget : fixture.widgets) {
        fixture.coordinator.retiring(widget.get());
        widget->hide();
    }
    fixture.coordinator.endMutation();
    processEvents();
    require(fixture.repository.stackingOrder("default") == saved, "bulk teardown retains order");
    fixture.desktop.windows.clear();
    batch = fixture.coordinator.beginRestore("default", fixture.ids);
    for (int i = 0; i < 4; ++i) {
        fixture.coordinator.track(fixture.window(i), fixture.ids.at(i), "default");
        fixture.present(i);
        fixture.coordinator.settleRestore(batch, fixture.ids.at(i));
    }
    processEvents();
    require(fixture.observed() == saved, "reused shells restore saved order");
    std::reverse(fixture.desktop.windows.begin(), fixture.desktop.windows.end());
    const auto shutdownOrder = fixture.observed();
    fixture.coordinator.nativeOrderChanged(fixture.window(0));
    fixture.coordinator.shutdown();
    require(fixture.repository.stackingOrder("default") == shutdownOrder,
            "shutdown captures the last queued reorder synchronously");
    const auto finalRevision = fixture.repository.stackingRevision();
    fixture.desktop.windows.clear();
    processEvents();
    require(fixture.repository.stackingRevision() == finalRevision,
            "shutdown freezes queued observations");
}
void cancellationAndFailure() {
    Fixture fixture;
    const auto oldBatch = fixture.coordinator.beginRestore("default", fixture.ids);
    fixture.coordinator.cancelGroup("default");
    fixture.coordinator.settleRestore(oldBatch, fixture.ids.first());
    const auto first =
        fixture.coordinator.beginRestore("default", {fixture.ids.at(0), fixture.ids.at(1)});
    const auto second =
        fixture.coordinator.beginRestore("default", {fixture.ids.at(2), fixture.ids.at(3)});
    fixture.present(1);
    fixture.coordinator.settleRestore(first, fixture.ids.at(0)); // failed load
    fixture.coordinator.settleRestore(first, fixture.ids.at(1));
    processEvents();
    require(fixture.desktop.applications == 0, "overlapping batches wait for remaining peers");
    fixture.present(3);
    fixture.coordinator.settleRestore(second, fixture.ids.at(2)); // canceled load
    fixture.desktop.fail = true;
    fixture.coordinator.settleRestore(second, fixture.ids.at(3));
    processEvents();
    const auto saved = fixture.repository.stackingOrder("default");
    fixture.desktop.windows = {fixture.window(3), fixture.window(1)};
    fixture.coordinator.checkpoint();
    require(fixture.repository.stackingOrder("default") == saved,
            "failed native restore cannot overwrite desired order");
    fixture.coordinator.promote(fixture.window(1));
    const auto promoted = fixture.repository.stackingOrder("default");
    processEvents();
    require(promoted.first() == fixture.ids.at(1) &&
                fixture.repository.stackingOrder("default") == promoted,
            "user intent updates failed restoration without saving its intermediate stack");
    fixture.desktop.fail = false;
    fixture.coordinator.promote(fixture.window(1));
    processEvents();
    require(fixture.observed() == QStringList({fixture.ids.at(1), fixture.ids.at(3)}),
            "a later explicit action can successfully reconcile the retained desired order");
}

void activationDuringNativePlacement() {
    Fixture fixture;
    fixture.coordinator.shutdown();
    auto port = fixture.desktop.port();
    const auto apply = port.apply;
    presentation::PinnedWindowStackingCoordinator* receiver = nullptr;
    bool activated = false;
    port.apply = [&](const auto& desired) {
        const bool success = apply(desired);
        if (!activated) {
            activated = true;
            receiver->promote(fixture.window(3));
        }
        return success;
    };
    presentation::PinnedWindowStackingCoordinator coordinator(&fixture.repository, port);
    receiver = &coordinator;
    const auto batch = coordinator.beginRestore("default", fixture.ids);
    for (int i = 0; i < 4; ++i) {
        coordinator.track(fixture.window(i), fixture.ids.at(i), "default");
        fixture.present(i);
        coordinator.presented(fixture.window(i), true);
        coordinator.settleRestore(batch, fixture.ids.at(i));
    }
    processEvents();
    processEvents();
    require(fixture.repository.stackingOrder("default").first() == fixture.ids.at(3) &&
                fixture.observed().first() == fixture.ids.at(3),
            "native placement suppression never discards a reentrant user activation");
    coordinator.shutdown();
}

void canceledQueuedReconciliation() {
    Fixture fixture;
    const auto stale = fixture.coordinator.beginRestore("default", {fixture.ids.at(0)});
    fixture.present(0);
    fixture.coordinator.settleRestore(stale, fixture.ids.at(0));
    fixture.coordinator.cancelGroup("default");
    const auto replacement = fixture.coordinator.beginRestore("default", {fixture.ids.at(1)});
    processEvents();
    require(fixture.desktop.applications == 0,
            "queued completion from a canceled group generation cannot reconcile its replacement");
    fixture.present(1);
    fixture.coordinator.settleRestore(replacement, fixture.ids.at(1));
    processEvents();
    require(fixture.desktop.applications == 1, "replacement generation reconciles exactly once");

    const QString second = id();
    require(fixture.repository
                .setGroups({{"default", "Default", true}, {second, "Second", false}}, "default")
                .success,
            "create group-change fixture");
    const auto pending = fixture.coordinator.beginRestore("default", {fixture.ids.at(2)});
    fixture.coordinator.beginMutation();
    fixture.coordinator.cancelGroup("default");
    require(fixture.repository.setActiveGroup(second).success, "switch group during load");
    fixture.coordinator.endMutation();
    const auto placements = fixture.desktop.placements;
    fixture.present(2);
    fixture.coordinator.settleRestore(pending, fixture.ids.at(2));
    processEvents();
    require(fixture.desktop.applications == 1 && fixture.desktop.placements == placements,
            "stale load cannot reorder another active group");
}

void bandsNewPinsAndReplacement() {
    Fixture fixture;
    fixture.window(0)->setProperty("stackingLevel", 1);
    fixture.window(2)->setProperty("stackingLevel", 1);
    const auto batch = fixture.coordinator.beginRestore("default", fixture.ids);
    fixture.present(3);
    fixture.present(2);
    const QString fresh = id();
    fixture.repository.reserveCreation(fresh);
    auto added = std::make_unique<QWidget>(nullptr, Qt::Tool | Qt::WindowDoesNotAcceptFocus);
    added->setAttribute(Qt::WA_ShowWithoutActivating);
    fixture.coordinator.track(added.get(), fresh, "default");
    added->show();
    fixture.desktop.windows.prepend(added.get());
    fixture.coordinator.presented(added.get(), false);
    fixture.present(1);
    fixture.present(0);
    for (const auto& key : fixture.ids)
        fixture.coordinator.settleRestore(batch, key);
    processEvents();
    require(fixture.desktop.windows ==
                presentation::PinnedStackingWindows{fixture.window(0), fixture.window(2),
                                                    added.get(), fixture.window(1),
                                                    fixture.window(3)},
            "late completion honors a new pin within each native level");
    require(fixture.repository.createReserved(record(fresh)).success,
            "new foreground pin completes first save");
    processEvents();
    fixture.coordinator.checkpoint();
    const auto before = fixture.desktop.captures;
    for (int i = 0; i < 20; ++i) {
        fixture.window(1)->move(i, i);
        fixture.window(1)->resize(32 + i, 32);
    }
    processEvents();
    require(fixture.desktop.captures == before, "ordinary geometry changes never scan stacking");

    // Replace one ID's widget, then deliver events and destruction from its obsolete shell.
    auto replacement = std::make_unique<QWidget>(nullptr, Qt::Tool | Qt::WindowDoesNotAcceptFocus);
    replacement->setAttribute(Qt::WA_ShowWithoutActivating);
    fixture.coordinator.track(replacement.get(), fixture.ids.at(1), "default");
    replacement->show();
    fixture.desktop.windows.removeAll(fixture.window(1));
    fixture.desktop.windows.prepend(replacement.get());
    fixture.coordinator.presented(replacement.get(), true);
    fixture.coordinator.nativeOrderChanged(fixture.window(1));
    fixture.widgets[1].reset();
    fixture.coordinator.promote(replacement.get());
    require(fixture.repository.stackingOrder("default").first() == fixture.ids.at(1),
            "destroying an obsolete shell cannot unregister its replacement");

    // Rebind the same widget to a different persistence ID, as a pooled shell would be.
    fixture.coordinator.retiring(replacement.get());
    const QString reused = id();
    fixture.repository.reserveCreation(reused);
    fixture.coordinator.track(replacement.get(), reused, "default");
    fixture.coordinator.presented(replacement.get(), false);
    require(fixture.repository.stackingOrder("default").first() == reused,
            "pooled widget promotion uses the current persistence ID");
    fixture.coordinator.shutdown();
}

void nativeOrdering() {
    auto platform = presentation::createPinnedWindowStackingPlatform();
    require(bool(platform.capture), "native test requires Windows or Cocoa");
#if defined(Q_OS_WIN)
    const auto foreground = GetForegroundWindow();
#elif defined(Q_OS_MACOS)
    const auto foreground = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    NSWindow* const keyWindow = NSApp.keyWindow;
#endif
    std::vector<std::unique_ptr<QWidget>> owners;
    presentation::PinnedStackingWindows windows;
    std::array<QRect, 4> geometries;
    for (int i = 0; i < 4; ++i) {
        auto window = std::make_unique<QWidget>(
            nullptr, Qt::Window | Qt::FramelessWindowHint |
                         (i < 2 ? Qt::WindowStaysOnTopHint : Qt::WindowFlags()));
        window->setAttribute(Qt::WA_ShowWithoutActivating);
        window->setGeometry(30000, 30000, 64, 32);
        window->show();
        windows.append(window.get());
        owners.push_back(std::move(window));
    }
    processEvents();
    for (size_t i = 0; i < owners.size(); ++i)
        geometries[i] = owners[i]->geometry();
    std::array<int, 4> permutation{0, 1, 2, 3};
    do {
        for (int i : permutation)
            owners.at(static_cast<size_t>(i))->raise();
        require(platform.apply(windows), "native batch restore succeeds");
        const auto observed = platform.capture(windows);
        require(observed && *observed == windows, "native front-to-back order matches saved order");
        for (size_t i = 0; i < owners.size(); ++i)
            require(owners[i]->geometry() == geometries[i], "stacking preserves window geometry");
        require(platform.level(owners[0].get()) == platform.level(owners[1].get()) &&
                    platform.level(owners[0].get()) > platform.level(owners[2].get()),
                "stacking preserves native levels");
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    owners[1]->lower();
    require(platform.place(owners[1].get(), owners[0].get(), nullptr) && platform.apply(windows),
            "activation-free insertion restores a lowered pin");
    owners[2]->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    owners[2]->show();
    processEvents();
    require(platform.level(owners[2].get()) == platform.level(owners[0].get()) &&
                platform.apply(windows),
            "topmost toggles are reconciled within the new native band");
    QWidget auxiliary(nullptr, Qt::Tool | Qt::WindowDoesNotAcceptFocus);
    auxiliary.setAttribute(Qt::WA_ShowWithoutActivating);
    auxiliary.setGeometry(30000, 30000, 32, 32);
    static_cast<void>(auxiliary.winId());
    auxiliary.windowHandle()->setTransientParent(owners[0]->windowHandle());
    auxiliary.show();
    owners[1]->setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
    owners[1]->show();
    processEvents();
    require(platform.apply(windows) &&
                auxiliary.windowHandle()->transientParent() == owners[0]->windowHandle(),
            "surface rebinding preserves auxiliary ownership without assigning auxiliary ranks");
#if defined(Q_OS_WIN)
    require(GetWindow(reinterpret_cast<HWND>(auxiliary.winId()), GW_OWNER) ==
                reinterpret_cast<HWND>(owners[0]->winId()),
            "native reconciliation preserves the auxiliary owner HWND");
#endif
#if defined(Q_OS_WIN)
    require(GetForegroundWindow() == foreground, "native restoration preserves foreground focus");
#elif defined(Q_OS_MACOS)
    require(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == foreground &&
                NSApp.keyWindow == keyWindow,
            "Cocoa restoration preserves the frontmost application and key window");
#endif
}

void benchmark(const QString& output) {
    QJsonArray reports;
    for (int count : {1, 10, 100, 500}) {
        Fixture fixture(count, true);
        require(fixture.repository.flush().success,
                "benchmark flushes initial payloads before timing");
        fixture.manifestWrites = 0;
        QVector<double> samples;
        int captures = 0, placements = 0, applications = 0;
        presentation::PinnedWindowStackingMetrics totals;
        for (int cycle = -1; cycle < 20; ++cycle) {
            fixture.coordinator.beginMutation();
            for (auto& widget : fixture.widgets) {
                fixture.coordinator.retiring(widget.get());
                widget->hide();
            }
            fixture.coordinator.endMutation();
            fixture.desktop.windows.clear();
            fixture.desktop.captures = fixture.desktop.placements = fixture.desktop.applications =
                0;
            fixture.desktop.metrics = {};
            QElapsedTimer timer;
            timer.start();
            const auto batch = fixture.coordinator.beginRestore("default", fixture.ids);
            // Front-first completion requires an insertion for every subsequent arrival.
            for (int i = 0; i < count; ++i) {
                fixture.present(i);
                fixture.coordinator.settleRestore(batch, fixture.ids.at(i));
            }
            processEvents();
            if (cycle >= 0) {
                samples.append(double(timer.nsecsElapsed()) / 1'000'000.0);
                captures += fixture.desktop.captures;
                placements += fixture.desktop.placements;
                applications += fixture.desktop.applications;
                totals.snapshots += fixture.desktop.metrics.snapshots;
                totals.placements += fixture.desktop.metrics.placements;
                totals.deferredBatches += fixture.desktop.metrics.deferredBatches;
            }
        }
        require(fixture.repository.flush().success, "benchmark drains metadata");
        std::sort(samples.begin(), samples.end());
        reports.append(QJsonObject{{"windows", count},
                                   {"samples", samples.size()},
                                   {"median_ms", samples.at(samples.size() / 2)},
                                   {"p95_ms", samples.at(samples.size() - 2)},
                                   {"coordinator_snapshots", captures},
                                   {"placement_requests", placements},
                                   {"batch_applications", applications},
                                   {"native_snapshots", qint64(totals.snapshots)},
                                   {"native_placements", qint64(totals.placements)},
                                   {"native_batches", qint64(totals.deferredBatches)},
                                   {"manifest_writes", fixture.manifestWrites.load()}});
    }
    const QByteArray bytes = QJsonDocument(reports).toJson();
    if (!output.isEmpty()) {
        QFile file(output);
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
                "write stacking benchmark");
    }
    std::cout << bytes.constData();
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    try {
        if (app.arguments().contains("--native")) {
            nativeOrdering();
            return 0;
        }
        if (app.arguments().contains("--benchmark")) {
            const auto index = app.arguments().indexOf("--output");
            benchmark(index >= 0 ? app.arguments().value(index + 1) : QString());
            return 0;
        }
        model();
        persistence();
        legacyAndExchange();
        completionPermutations();
        presentationsDuringMutation();
        reconciliationInterruptedByMutation();
        suspendedPresentationsRespectLifecycle();
        canceledSuspendedPresentations();
        explicitPresentationDuringMutation();
        newerIntentDuringMutation();
        partialRestoreDuringMutation();
        presentationDuringNativePlacement();
        interactionsAndLifecycle();
        cancellationAndFailure();
        activationDuringNativePlacement();
        canceledQueuedReconciliation();
        bandsNewPinsAndReplacement();
        std::cout << "Pinned stacking tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
