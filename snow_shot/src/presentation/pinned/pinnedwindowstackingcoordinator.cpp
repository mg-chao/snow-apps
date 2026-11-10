#include "snow_shot/presentation/pinnedwindowstackingcoordinator.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "pinnedwindowstackingplatform.h"

#include <QEvent>
#include <QScopedValueRollback>
#include <QSet>
#include <QTimer>
#include <QDebug>
#include <algorithm>
#include <map>
#include <limits>
#include <utility>

namespace snow_shot::presentation {
struct PinnedWindowStackingCoordinator::Impl {
    struct Surface {
        QPointer<QWidget> window;
        QString group;
        bool ready = false;
        quint64 pendingPromotion = 0;
        quint64 promotionRevision = 0;
    };
    struct Batch {
        QString group;
        QSet<QString> pending;
    };
    PinnedWindowStackingCoordinator& owner;
    storage::PinnedWindowRepository* repository;
    PinnedWindowStackingPlatform platform;
    QHash<QString, Surface> surfaces;
    QHash<QObject*, QString> ids;
    QSet<QObject*> watched;
    QHash<quint64, Batch> batches;
    QHash<QString, quint64> groupEpochs;
    quint64 nextBatch = 0;
    quint64 nextPromotion = 0;
    // Zero requests lifecycle reconciliation; nonzero guards a completed restore epoch.
    QHash<QString, quint64> pendingReconciliations;
    bool queued = false;
    bool dirty = false;
    bool stopped = false;
    bool applying = false;
    int mutationDepth = 0;
    QSet<QString> unreconciledGroups;
    quint64 indexRevision = (std::numeric_limits<quint64>::max)();
    QString indexGroup;
    QStringList order;
    QHash<QString, qsizetype> ranks;
    std::map<qint64, std::map<qsizetype, QString>> available;

    Impl(PinnedWindowStackingCoordinator& owner_, storage::PinnedWindowRepository* repository_,
         PinnedWindowStackingPlatform platform_)
        : owner(owner_), repository(repository_), platform(std::move(platform_)) {}

    bool observing() const {
        return repository && !stopped && !applying && mutationDepth == 0;
    }
    void invalidateIndex() {
        indexRevision = (std::numeric_limits<quint64>::max)();
    }
    bool visible(const Surface& surface) const {
        return surface.ready && surface.window && surface.window->isVisible() &&
               !surface.window->isMinimized();
    }
    void ensureIndex(const QString& group) {
        const quint64 revision = repository->stackingRevision();
        if (indexRevision == revision && indexGroup == group)
            return;
        indexRevision = revision;
        indexGroup = group;
        order = repository->stackingOrder(group);
        ranks.clear();
        available.clear();
        for (qsizetype i = 0; i < order.size(); ++i) {
            const auto& id = order.at(i);
            ranks.insert(id, i);
            const auto surface = surfaces.constFind(id);
            if (surface != surfaces.cend() && surface->group == group && visible(*surface) &&
                platform.level)
                available[platform.level(surface->window)].emplace(i, id);
        }
    }
    void schedule() {
        if (queued || !observing())
            return;
        queued = true;
        QTimer::singleShot(0, &owner, [this] {
            queued = false;
            if (!observing())
                return;
            const auto pending = std::exchange(pendingReconciliations, {});
            for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
                if (!observing()) {
                    requestReconciliation(it.key(), it.value());
                    continue;
                }
                const bool restoring = hasPendingRestores(it.key());
                if (it.value() != 0 && (groupEpochs.value(it.key()) != it.value() || restoring))
                    continue;
                if (!restoring)
                    groupEpochs.remove(it.key());
                apply(it.key());
            }
            if (dirty)
                owner.checkpoint();
            if (!pendingReconciliations.isEmpty())
                schedule();
        });
    }
    void scheduleObservation() {
        dirty = true;
        schedule();
    }
    bool hasPendingRestores(const QString& group) const {
        return std::any_of(batches.cbegin(), batches.cend(),
                           [&group](const auto& batch) { return batch.group == group; });
    }
    void requestReconciliation(const QString& group, quint64 epoch = 0) {
        if (!repository || stopped)
            return;
        const auto pending = pendingReconciliations.constFind(group);
        if (pending == pendingReconciliations.cend() || *pending != 0)
            pendingReconciliations.insert(group, epoch);
        schedule();
    }
    void resumePromotions() {
        QVector<std::pair<quint64, QPointer<QWidget>>> promotions;
        const QString activeGroup = repository->activeGroupId();
        const auto revision = repository->stackingRevision();
        for (auto& surface : surfaces) {
            const auto sequence = std::exchange(surface.pendingPromotion, 0);
            // A newer accepted request or storage generation owns the foreground intent.
            if (sequence != 0 && surface.promotionRevision == revision && surface.ready &&
                surface.window && surface.group == activeGroup)
                promotions.append({sequence, surface.window});
        }
        std::sort(promotions.begin(), promotions.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& promotion : promotions)
            if (promotion.second)
                owner.promote(promotion.second);
    }
    void apply(const QString& group) {
        if (!observing() || !platform.apply || repository->activeGroupId() != group)
            return;
        ensureIndex(group);
        PinnedStackingWindows windows;
        windows.reserve(order.size());
        for (const auto& id : order) {
            const auto found = surfaces.constFind(id);
            if (found != surfaces.cend() && found->group == group && visible(*found))
                windows.append(found->window);
        }
        const QScopedValueRollback guard(applying, true);
        if (!platform.apply(windows)) {
            unreconciledGroups.insert(group);
            qWarning("Pinned window stacking restoration failed");
        } else {
            unreconciledGroups.remove(group);
        }
        if (repository->stackingRevision() != indexRevision) {
            // An activation changed intent inside the native operation. Do not let a
            // queued observation save the superseded intermediate stack.
            unreconciledGroups.insert(group);
            requestReconciliation(group);
        }
    }
};

PinnedWindowStackingCoordinator::PinnedWindowStackingCoordinator(
    storage::PinnedWindowRepository* repository, QObject* parent)
    : PinnedWindowStackingCoordinator(repository, createPinnedWindowStackingPlatform(), parent) {}
PinnedWindowStackingCoordinator::PinnedWindowStackingCoordinator(
    storage::PinnedWindowRepository* repository, PinnedWindowStackingPlatform platform,
    QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, repository, std::move(platform))) {}
PinnedWindowStackingCoordinator::~PinnedWindowStackingCoordinator() = default;

void PinnedWindowStackingCoordinator::track(QWidget* window, const QString& id,
                                            const QString& group) {
    auto& s = *m_impl;
    if (!window || id.isEmpty() || !s.repository || s.stopped)
        return;
    if (s.ids.contains(window)) {
        const auto oldId = s.ids.value(window);
        if (oldId == id) {
            if (s.surfaces.value(id).group != group) {
                s.surfaces[id].group = group;
                s.surfaces[id].pendingPromotion = 0;
                s.invalidateIndex();
            }
            return;
        }
        s.surfaces.remove(oldId);
        s.ids.remove(window);
        s.invalidateIndex();
    }
    if (const auto old = s.surfaces.value(id).window; old && old != window) {
        old->removeEventFilter(this);
        s.ids.remove(old);
        s.invalidateIndex();
    }
    s.surfaces.insert(id, {window, group, false});
    s.ids.insert(window, id);
    window->installEventFilter(this);
    if (s.watched.contains(window))
        return;
    s.watched.insert(window);
    connect(window, &QObject::destroyed, this, [this, window] {
        auto& state = *m_impl;
        state.watched.remove(window);
        const auto id = state.ids.take(window);
        if (!id.isEmpty()) {
            state.surfaces.remove(id);
            state.invalidateIndex();
        }
    });
}

void PinnedWindowStackingCoordinator::presented(QWidget* window, bool preserveFocus) {
    auto& s = *m_impl;
    const auto id = s.ids.value(window);
    auto found = s.surfaces.find(id);
    if (found == s.surfaces.end() || !s.repository || s.stopped)
        return;
    // Lifecycle state survives suspension; only native observation and placement pause.
    found->ready = true;
    const QString group = found->group;
    if (group != s.repository->activeGroupId()) {
        s.invalidateIndex();
        return;
    }
    if (!preserveFocus && s.mutationDepth == 0) {
        promote(window);
        s.invalidateIndex();
        if (s.applying)
            s.requestReconciliation(group);
        return;
    }
    if (!s.observing()) {
        if (!preserveFocus) {
            found->pendingPromotion = ++s.nextPromotion;
            found->promotionRevision = s.repository->stackingRevision();
        }
        s.invalidateIndex();
        s.requestReconciliation(group);
        return;
    }
    if (!s.platform.level || !s.platform.place)
        return;
    s.ensureIndex(group);
    const auto rank = s.ranks.constFind(id);
    if (rank == s.ranks.cend() || !s.visible(*found))
        return;
    auto& peers = s.available[s.platform.level(window)];
    const auto [position, inserted] = peers.insert_or_assign(*rank, id);
    Q_UNUSED(inserted);
    QWidget* above = position == peers.begin()
                         ? nullptr
                         : s.surfaces.value(std::prev(position)->second).window.data();
    const auto next = std::next(position);
    QWidget* below = next == peers.end() ? nullptr : s.surfaces.value(next->second).window.data();
    {
        const QScopedValueRollback guard(s.applying, true);
        if (!s.platform.place(window, above, below)) {
            s.unreconciledGroups.insert(group);
            qWarning("Pinned window stacking placement failed");
        }
        if (s.repository->stackingRevision() != s.indexRevision) {
            s.unreconciledGroups.insert(group);
            s.requestReconciliation(group);
        }
    }
    if (!s.pendingReconciliations.isEmpty())
        s.schedule();
}

void PinnedWindowStackingCoordinator::retiring(QWidget* window) {
    auto& s = *m_impl;
    const auto found = s.surfaces.find(s.ids.value(window));
    if (found == s.surfaces.end() || !found->ready)
        return;
    checkpoint();
    found->ready = false;
    found->pendingPromotion = 0;
    s.invalidateIndex();
}

void PinnedWindowStackingCoordinator::nativeOrderChanged(QWidget* window) {
    auto& s = *m_impl;
    if (s.observing() && s.surfaces.value(s.ids.value(window)).ready) {
        s.invalidateIndex();
        s.scheduleObservation();
    }
}
void PinnedWindowStackingCoordinator::promote(QWidget* window) {
    auto& s = *m_impl;
    const auto id = s.ids.value(window);
    const auto found = s.surfaces.constFind(id);
    // Our native placement calls never activate. A genuine activation delivered
    // reentrantly during placement must still outrank the restore in progress.
    if (!s.repository || s.stopped || s.mutationDepth != 0 || found == s.surfaces.cend() ||
        !found->ready || found->group != s.repository->activeGroupId())
        return;
    if (!s.repository->promoteStackingWindow(id).success)
        qWarning("Pinned window stacking promotion could not be saved");
    s.scheduleObservation();
    if (s.unreconciledGroups.contains(found->group)) {
        s.requestReconciliation(found->group);
    }
}
void PinnedWindowStackingCoordinator::checkpoint() {
    auto& s = *m_impl;
    if (!s.observing() || !s.platform.capture)
        return;
    s.dirty = false;
    PinnedStackingWindows windows;
    const QString activeGroup = s.repository->activeGroupId();
    for (const auto& surface : s.surfaces)
        if (surface.group == activeGroup && s.visible(surface))
            windows.append(surface.window);
    if (windows.size() < 2 || s.unreconciledGroups.contains(activeGroup) ||
        s.pendingReconciliations.contains(activeGroup))
        return;
    const auto observed = s.platform.capture(windows);
    if (!observed) {
        qWarning("Pinned window stacking order could not be observed");
        return;
    }
    QStringList ids;
    for (const auto& window : *observed)
        if (window && s.ids.contains(window))
            ids.append(s.ids.value(window));
    if (!s.repository->updateStackingOrder(activeGroup, ids).success)
        qWarning("Pinned window stacking order could not be saved");
}
void PinnedWindowStackingCoordinator::shutdown() {
    checkpoint();
    m_impl->stopped = true;
    m_impl->batches.clear();
    m_impl->pendingReconciliations.clear();
    m_impl->dirty = false;
}
void PinnedWindowStackingCoordinator::beginMutation() {
    if (m_impl->mutationDepth == 0)
        checkpoint();
    ++m_impl->mutationDepth;
}
void PinnedWindowStackingCoordinator::endMutation() {
    Q_ASSERT(m_impl->mutationDepth > 0);
    --m_impl->mutationDepth;
    m_impl->invalidateIndex();
    if (m_impl->mutationDepth == 0 && m_impl->repository && !m_impl->stopped) {
        m_impl->resumePromotions();
        if (m_impl->dirty || !m_impl->pendingReconciliations.isEmpty())
            m_impl->schedule();
    }
}
quint64 PinnedWindowStackingCoordinator::beginRestore(const QString& group,
                                                      const QStringList& ids) {
    auto& s = *m_impl;
    if (!s.repository || s.stopped || ids.isEmpty())
        return 0;
    const auto batch = ++s.nextBatch;
    s.groupEpochs.insert(group, batch);
    s.batches.insert(batch, {group, QSet<QString>(ids.cbegin(), ids.cend())});
    s.ensureIndex(group);
    return batch;
}
void PinnedWindowStackingCoordinator::settleRestore(quint64 batch, const QString& id) {
    auto& s = *m_impl;
    auto found = s.batches.find(batch);
    if (found == s.batches.end() || !found->pending.remove(id) || !found->pending.isEmpty())
        return;
    const QString group = found->group;
    s.batches.erase(found);
    if (!s.hasPendingRestores(group))
        s.requestReconciliation(group, s.groupEpochs.value(group));
}
void PinnedWindowStackingCoordinator::cancelGroup(const QString& group) {
    auto& s = *m_impl;
    s.groupEpochs.remove(group);
    s.pendingReconciliations.remove(group);
    for (auto& surface : s.surfaces)
        if (surface.group == group)
            surface.pendingPromotion = 0;
    for (auto it = s.batches.begin(); it != s.batches.end();)
        if (it->group == group)
            it = s.batches.erase(it);
        else
            ++it;
    s.invalidateIndex();
}
bool PinnedWindowStackingCoordinator::eventFilter(QObject* object, QEvent* event) {
    auto& s = *m_impl;
    auto* window = qobject_cast<QWidget*>(object);
    switch (event->type()) {
    case QEvent::WindowActivate:
        if (s.platform.capture)
            promote(window);
        break;
    case QEvent::ZOrderChange:
        nativeOrderChanged(window);
        break;
    case QEvent::Hide:
    case QEvent::Show:
    case QEvent::WinIdChange:
    case QEvent::WindowStateChange:
        if (s.surfaces.value(s.ids.value(window)).ready)
            s.invalidateIndex();
        nativeOrderChanged(window);
        break;
    default:
        break;
    }
    return false;
}
} // namespace snow_shot::presentation
