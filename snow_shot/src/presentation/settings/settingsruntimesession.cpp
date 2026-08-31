#include "snow_shot/presentation/settings/settingsruntimesession.h"

#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QMetaType>

#include <utility>

namespace snow_shot::presentation::settings {
namespace {
QVariantList stringListVariant(const QStringList& values) {
    QVariantList result;
    result.reserve(values.size());
    for (const QString& value : values) {
        result.push_back(value);
    }
    return result;
}

QStringList stringListValue(const QVariant& value) {
    QStringList result;
    if (value.metaType() == QMetaType::fromType<QStringList>()) {
        return value.toStringList();
    }
    for (const QVariant& item : value.toList()) {
        result.push_back(item.toString());
    }
    return result;
}

QString localShortcutKey(SettingsLocalShortcutScope scope, const QString& shortcutId) {
    return QString::number(static_cast<int>(scope)) + QLatin1Char('\x1f') + shortcutId;
}

bool sameStorageStatus(const storage::StorageStatus& first,
                       const storage::StorageStatus& second) {
    return first.requestedDirectory == second.requestedDirectory &&
           first.effectiveDirectory == second.effectiveDirectory &&
           first.fallbackReason == second.fallbackReason &&
           first.effectiveMode == second.effectiveMode &&
           first.configurationCompatibility == second.configurationCompatibility &&
           first.readAvailable == second.readAvailable &&
           first.writeAvailable == second.writeAvailable &&
           first.historyUsage == second.historyUsage &&
           first.historyPolicyUpdating == second.historyPolicyUpdating &&
           first.historyClearing == second.historyClearing &&
           first.lastConfigurationError == second.lastConfigurationError &&
           first.lastHistoryError == second.lastHistoryError;
}
} // namespace

SettingsRuntimeSession::SettingsRuntimeSession(const SettingsRegistry& registry,
                                               SettingsBackend& backend,
                                               QObject* parent)
    : QObject(parent), m_registry(registry), m_backend(backend) {
    qRegisterMetaType<SettingsFieldState>();
    qRegisterMetaType<SettingsOptions>();
    qRegisterMetaType<SettingsCommandState>();
    qRegisterMetaType<SettingsWritePhase>();
    qRegisterMetaType<SettingsCommand>();
    qRegisterMetaType<SettingsCommandKind>();
    qRegisterMetaType<storage::ScreenshotToolbarLayout>();
    connect(&m_backend, &SettingsBackend::synchronized, this, [this]() {
        refreshAll();
    }, Qt::QueuedConnection);
    connect(&m_backend, &SettingsBackend::shortcutStateChanged, this,
            [this](GlobalShortcutAction action,
                   const GlobalShortcutRegistrationState&) {
                if (const auto* descriptor = descriptorForShortcut(action)) {
                    refreshField(descriptor->id);
                }
                // Registration status can change without the shortcut list
                // changing, so fieldChanged alone is not sufficient here.
                emit shortcutStateChanged(action, shortcutState(action));
            }, Qt::QueuedConnection);
    refreshAll();
}

const SettingsRegistry& SettingsRuntimeSession::registry() const {
    return m_registry;
}

SettingsFieldState SettingsRuntimeSession::state(const QString& fieldId) const {
    const auto found = m_states.constFind(fieldId);
    if (found != m_states.cend()) {
        return found.value();
    }
    SettingsFieldState result;
    if (const auto* descriptor = descriptorFor(fieldId)) {
        result.acceptedValue = readValue(*descriptor);
        result.draftValue = result.acceptedValue;
        result.enabled = descriptor->definition != nullptr && !isReadOnly(*descriptor);
    } else {
        result.enabled = false;
        result.visible = false;
    }
    return result;
}

bool SettingsRuntimeSession::hasState(const QString& fieldId) const {
    return m_states.contains(fieldId);
}

SettingsOptions SettingsRuntimeSession::options(const QString& fieldId) const {
    const auto cached = m_optionsCache.constFind(fieldId);
    if (cached != m_optionsCache.cend()) {
        return cached.value();
    }
    const auto* descriptor = descriptorFor(fieldId);
    if (descriptor == nullptr || descriptor->definition == nullptr) {
        SettingsOptions result;
        result.error = QStringLiteral("Unknown settings field");
        return result;
    }
    const SettingsOptions result = buildOptions(*descriptor);
    const_cast<SettingsRuntimeSession*>(this)->m_optionsCache.insert(fieldId, result);
    return result;
}

bool SettingsRuntimeSession::hasDirtyFields() const {
    for (auto it = m_states.cbegin(); it != m_states.cend(); ++it) {
        if (it.value().dirty) {
            return true;
        }
    }
    return false;
}

bool SettingsRuntimeSession::hasPendingWrites() const {
    for (auto it = m_states.cbegin(); it != m_states.cend(); ++it) {
        if (it.value().busy) {
            return true;
        }
    }
    return false;
}

QStringList SettingsRuntimeSession::dirtyFieldIds() const {
    QStringList result;
    result.reserve(m_states.size());
    // QHash iteration order is intentionally unspecified. Expose the stable
    // catalog order so callers can render a predictable pending/dirty summary.
    for (const SettingsFieldDescriptor& descriptor : m_registry.fields()) {
        const auto found = m_states.constFind(descriptor.id);
        if (found != m_states.cend() && found->dirty) {
            result.push_back(descriptor.id);
        }
    }
    return result;
}

bool SettingsRuntimeSession::submitDraft(const QString& fieldId, const QVariant& value) {
    return submitDraftInternal(fieldId, value, false);
}

bool SettingsRuntimeSession::submitDraftInternal(const QString& fieldId, const QVariant& value,
                                                  bool forceWrite) {
    const auto* descriptor = descriptorFor(fieldId);
    if (descriptor == nullptr || isReadOnly(*descriptor)) {
        return false;
    }

    SettingsFieldState next = state(fieldId);
    const QVariant previousAccepted = next.acceptedValue;
    if (!forceWrite && valuesEqual(*descriptor, value, previousAccepted) && !next.dirty &&
        !next.busy) {
        // A no-op must not cause a storage write or a new revision. Clear a
        // stale presentation error, but leave the accepted value untouched.
        if (!next.error.isEmpty() || next.phase != SettingsWritePhase::Clean ||
            next.conflicted) {
            next.error.clear();
            next.phase = SettingsWritePhase::Clean;
            next.conflicted = false;
            updateState(fieldId, next);
        }
        return true;
    }
    if (next.busy && valuesEqual(*descriptor, value, next.draftValue)) {
        // Do not enqueue duplicate writes while an asynchronous request is in
        // flight. Callers can explicitly use retry after a failure.
        return true;
    }

    next.draftValue = value;
    // Retry is an explicit persistence attempt. The backend may already expose
    // the target in memory even though the previous write failed to persist;
    // keep a real operation generation in that case instead of collapsing it
    // into a no-op because the values happen to match.
    next.dirty = forceWrite ||
                 !valuesEqual(*descriptor, next.draftValue, next.acceptedValue);
    next.conflicted = false;
    next.error.clear();
    next.phase = next.dirty ? SettingsWritePhase::Pending : SettingsWritePhase::Clean;
    next.busy = next.dirty;
    ++next.revision;

    // A user edit supersedes an accepted asynchronous reset for this field.
    // Retire that reset generation so its late completion cannot overwrite the
    // newer draft.
    if (const auto reset = m_pendingResets.constFind(fieldId);
        reset != m_pendingResets.cend()) {
        retireWrite(fieldId, reset.value());
        m_pendingResets.erase(reset);
    }

    // A field can only have one active write. Older requests remain in a
    // bounded retired list so a late completion cannot overwrite the newer
    // draft when the backend exposes only a generic synchronized() signal.
    if (const auto pending = m_pendingWrites.constFind(fieldId);
        pending != m_pendingWrites.cend()) {
        retireWrite(fieldId, pending.value());
        m_pendingWrites.erase(pending);
    }
    // Once the user explicitly asks for a retired target again, accepting any
    // matching completion is correct regardless of which backend generation
    // produced it.
    forgetRetiredTarget(*descriptor, value);
    updateState(fieldId, next);

    if (!next.dirty) {
        refreshCommandStates();
        return true;
    }

    // Install the generation before invoking the backend: several built-in
    // adapters emit synchronized synchronously from their setter.
    PendingWrite operation;
    operation.target = value;
    operation.baseline = previousAccepted;
    operation.revision = next.revision;
    operation.fieldError = m_backend.fieldError(fieldId);
    const storage::StorageStatus beforeStatus = storageStatus();
    operation.configurationError = beforeStatus.lastConfigurationError;
    operation.historyError = beforeStatus.lastHistoryError;
    m_pendingWrites.insert(fieldId, operation);

    const bool accepted = writeValue(*descriptor, value);
    const QVariant current = readValue(*descriptor);
    const bool pending = accepted && (isPending(*descriptor) ||
                                      !valuesEqual(*descriptor, current, value));
    const bool operationFailed = accepted && operationErrorChanged(*descriptor, operation);
    if (!accepted || operationFailed) {
        if (accepted && operationFailed && pending) {
            retireWrite(fieldId, operation);
        }
        next.busy = false;
        next.dirty = true;
        next.phase = !accepted ? SettingsWritePhase::Rejected : SettingsWritePhase::Failed;
        next.error = writeError(*descriptor);
        m_pendingWrites.remove(fieldId);
    } else if (pending) {
        next.acceptedValue = pending ? previousAccepted : current;
        next.dirty = !valuesEqual(*descriptor, next.draftValue, next.acceptedValue);
        next.busy = true;
        next.phase = SettingsWritePhase::Pending;
    } else {
        next.acceptedValue = current;
        next.draftValue = current;
        next.dirty = false;
        next.busy = false;
        next.conflicted = false;
        next.phase = SettingsWritePhase::Clean;
        next.error.clear();
        m_pendingWrites.remove(fieldId);
    }
    updateState(fieldId, next);
    if (accepted && pending) {
        // The backend will normally emit synchronized on completion. A
        // queued refresh also covers lightweight plugin backends that only
        // expose a future/pending hint.
        const quint64 revision = next.revision;
        QMetaObject::invokeMethod(
            this, [this, fieldId, revision]() { refreshField(fieldId, revision); },
            Qt::QueuedConnection);
    }
    refreshCommandStates();
    return accepted;
}

bool SettingsRuntimeSession::retry(const QString& fieldId) {
    const SettingsFieldState current = state(fieldId);
    if (!current.dirty) {
        return true;
    }
    return submitDraftInternal(fieldId, current.draftValue, true);
}

bool SettingsRuntimeSession::discard(const QString& fieldId) {
    const auto* descriptor = descriptorFor(fieldId);
    if (descriptor == nullptr) {
        return false;
    }
    SettingsFieldState next = state(fieldId);
    if (!next.dirty && !next.busy && next.phase == SettingsWritePhase::Clean) {
        return true;
    }
    if (const auto pending = m_pendingWrites.constFind(fieldId);
        pending != m_pendingWrites.cend()) {
        retireWrite(fieldId, pending.value());
        m_pendingWrites.erase(pending);
    }
    if (const auto reset = m_pendingResets.constFind(fieldId);
        reset != m_pendingResets.cend()) {
        retireWrite(fieldId, reset.value());
        m_pendingResets.erase(reset);
    }
    next.draftValue = next.acceptedValue;
    next.dirty = false;
    next.busy = false;
    next.conflicted = false;
    next.phase = SettingsWritePhase::Clean;
    next.error.clear();
    ++next.revision;
    updateState(fieldId, next);
    refreshCommandStates();
    return true;
}

bool SettingsRuntimeSession::reset(SettingsSectionReset resetGroup) {
    const QVector<int>& indexes = m_registry.fieldsForReset(resetGroup);
    QHash<QString, SettingsFieldState> beforeStates;
    beforeStates.reserve(indexes.size());
    for (const int index : indexes) {
        if (index < 0 || index >= m_registry.fields().size()) {
            continue;
        }
        const SettingsFieldDescriptor& descriptor = m_registry.fields().at(index);
        beforeStates.insert(descriptor.id, state(descriptor.id));
    }
    const storage::StorageStatus beforeStatus = storageStatus();
    const bool accepted = m_backend.resetSection(resetGroup);
    for (int index : indexes) {
        if (index < 0 || index >= m_registry.fields().size()) {
            continue;
        }
        const SettingsFieldDescriptor& descriptor = m_registry.fields().at(index);
        if (accepted) {
            SettingsFieldState next = beforeStates.value(descriptor.id);
            if (const auto pending = m_pendingWrites.constFind(descriptor.id);
                pending != m_pendingWrites.cend()) {
                retireWrite(descriptor.id, pending.value());
                m_pendingWrites.erase(pending);
            }
            if (const auto reset = m_pendingResets.constFind(descriptor.id);
                reset != m_pendingResets.cend()) {
                retireWrite(descriptor.id, reset.value());
                m_pendingResets.erase(reset);
            }
            const QVariant external = readValue(descriptor);
            next.acceptedValue = external;
            next.draftValue = external;
            next.dirty = false;
            // Built-in reset providers only have asynchronous completion for
            // capture-history policy updates. Do not mistake an older field
            // write that was just retired for reset work still in flight.
            const bool operationPending =
                descriptor.reset == SettingsSectionReset::HistoryPolicy &&
                isPending(descriptor);
            next.busy = operationPending;
            next.conflicted = false;
            next.phase = operationPending ? SettingsWritePhase::Pending
                                           : SettingsWritePhase::Clean;
            next.error.clear();
            ++next.revision;
            if (operationPending) {
                PendingWrite operation;
                operation.target = external;
                operation.baseline = beforeStates.value(descriptor.id).acceptedValue;
                operation.revision = next.revision;
                operation.fieldError = m_backend.fieldError(descriptor.id);
                operation.configurationError = beforeStatus.lastConfigurationError;
                operation.historyError = beforeStatus.lastHistoryError;
                m_pendingResets.insert(descriptor.id, operation);
            } else {
                m_pendingResets.remove(descriptor.id);
            }
            updateState(descriptor.id, next);
        } else {
            // Reset providers may reject a compound operation after touching
            // one or more values. Retire every older generation in the group,
            // retain the user's existing draft/baseline, and expose a
            // retryable rejected phase instead of silently accepting a partial
            // reset.
            SettingsFieldState next = beforeStates.value(descriptor.id);
            if (const auto pending = m_pendingWrites.constFind(descriptor.id);
                pending != m_pendingWrites.cend()) {
                retireWrite(descriptor.id, pending.value());
                m_pendingWrites.erase(pending);
            }
            if (const auto reset = m_pendingResets.constFind(descriptor.id);
                reset != m_pendingResets.cend()) {
                retireWrite(descriptor.id, reset.value());
                m_pendingResets.erase(reset);
            }
            next.busy = false;
            next.phase = SettingsWritePhase::Rejected;
            next.error = writeError(descriptor);
            ++next.revision;
            updateState(descriptor.id, next);
        }
        refreshField(descriptor.id);
    }
    refreshCommandStates();
    return accepted;
}

bool SettingsRuntimeSession::invoke(const SettingsCommand& command) {
    if (command.kind != SettingsCommandKind::Navigate &&
        command.kind != SettingsCommandKind::CaptureScreenshot &&
        command.kind != SettingsCommandKind::ExecuteQuickAction) {
        return false;
    }
    emit commandRequested(command);
    return true;
}

void SettingsRuntimeSession::refreshField(const QString& fieldId) {
    refreshField(fieldId, std::nullopt);
}

QString SettingsRuntimeSession::backendError(
    const SettingsFieldDescriptor& descriptor) const {
    const QString fieldFailure = m_backend.fieldError(descriptor.id);
    if (!fieldFailure.isEmpty()) {
        return fieldFailure;
    }
    const storage::StorageStatus status = storageStatus();
    const bool historyField = descriptor.reset == SettingsSectionReset::HistoryPolicy ||
                              descriptor.configurationKey.startsWith(
                                  QStringLiteral("capture_history/"));
    if (historyField && !status.lastHistoryError.isEmpty()) {
        return status.lastHistoryError;
    }
    if (!historyField && !status.lastConfigurationError.isEmpty()) {
        return status.lastConfigurationError;
    }
    return {};
}

bool SettingsRuntimeSession::operationErrorChanged(
    const SettingsFieldDescriptor& descriptor, const PendingWrite& operation) const {
    const QString fieldFailure = m_backend.fieldError(descriptor.id);
    if (!fieldFailure.isEmpty() && fieldFailure != operation.fieldError) {
        return true;
    }
    const storage::StorageStatus status = storageStatus();
    const bool historyField = descriptor.reset == SettingsSectionReset::HistoryPolicy ||
                              descriptor.configurationKey.startsWith(
                                  QStringLiteral("capture_history/"));
    if (historyField) {
        return !status.lastHistoryError.isEmpty() &&
               status.lastHistoryError != operation.historyError;
    }
    return !status.lastConfigurationError.isEmpty() &&
           status.lastConfigurationError != operation.configurationError;
}

bool SettingsRuntimeSession::matchesValue(const SettingsFieldDescriptor& descriptor,
                                          const QVariant& first,
                                          const QVariant& second) const {
    return valuesEqual(descriptor, first, second);
}

void SettingsRuntimeSession::retireWrite(const QString& fieldId,
                                         const PendingWrite& write) {
    constexpr int kMaximumRetiredWritesPerField = 8;
    QVector<RetiredWrite>& writes = m_retiredWrites[fieldId];
    for (RetiredWrite& retired : writes) {
        if (retired.revision == write.revision) {
            return;
        }
    }
    if (writes.size() >= kMaximumRetiredWritesPerField) {
        writes.removeFirst();
    }
    writes.push_back({write.target, write.revision, false});
}

void SettingsRuntimeSession::forgetRetiredTarget(
    const SettingsFieldDescriptor& descriptor, const QVariant& target) {
    auto found = m_retiredWrites.find(descriptor.id);
    if (found == m_retiredWrites.end()) {
        return;
    }
    QVector<RetiredWrite>& writes = found.value();
    for (int index = writes.size() - 1; index >= 0; --index) {
        if (matchesValue(descriptor, writes.at(index).target, target)) {
            writes.removeAt(index);
        }
    }
    if (writes.isEmpty()) {
        m_retiredWrites.erase(found);
    }
}

bool SettingsRuntimeSession::suppressRetiredCompletion(
    const SettingsFieldDescriptor& descriptor, const QVariant& external,
    bool backendPending, const PendingWrite* activeWrite) {
    auto found = m_retiredWrites.find(descriptor.id);
    if (found == m_retiredWrites.end()) {
        return false;
    }
    QVector<RetiredWrite>& writes = found.value();

    // An observed retired value is quarantined while it remains the backend's
    // current value. A transition away from it is observable evidence that a
    // later matching update is a new external change, not another notification
    // for the old completion.
    for (int index = writes.size() - 1; index >= 0; --index) {
        if (writes.at(index).completionObserved &&
            !matchesValue(descriptor, writes.at(index).target, external)) {
            writes.removeAt(index);
        }
    }

    const bool activeSettled = activeWrite != nullptr && !backendPending &&
                               matchesValue(descriptor, activeWrite->target, external);
    if (!backendPending && (activeWrite == nullptr || activeSettled)) {
        // No retired request remains in flight. Unobserved targets that do not
        // match the backend can no longer produce a stale completion.
        for (int index = writes.size() - 1; index >= 0; --index) {
            if (!writes.at(index).completionObserved &&
                !matchesValue(descriptor, writes.at(index).target, external)) {
                writes.removeAt(index);
            }
        }
    }

    // A live generation always wins interpretation of the current backend
    // value. If a retired target appears while another request is active, let
    // refreshField classify it as a pending conflict or a failed completion.
    if (activeWrite != nullptr) {
        return false;
    }

    for (RetiredWrite& write : writes) {
        if (!matchesValue(descriptor, external, write.target)) {
            continue;
        }
        write.completionObserved = true;
        return true;
    }
    if (writes.isEmpty()) {
        m_retiredWrites.erase(found);
    }
    return false;
}

void SettingsRuntimeSession::refreshField(const QString& fieldId,
                                           std::optional<quint64> expectedRevision) {
    const auto* descriptor = descriptorFor(fieldId);
    if (descriptor == nullptr) {
        return;
    }
    SettingsFieldState next = state(fieldId);
    if (expectedRevision.has_value() && next.revision != expectedRevision.value()) {
        return;
    }
    const QVariant external = readValue(*descriptor);
    const bool backendPending = isPending(*descriptor);
    const auto pending = m_pendingWrites.constFind(fieldId);
    const bool hasPending = pending != m_pendingWrites.cend();
    const auto reset = m_pendingResets.constFind(fieldId);
    const bool hasPendingReset = reset != m_pendingResets.cend();

    const PendingWrite* activeWrite = hasPending ? &pending.value() : nullptr;
    if (suppressRetiredCompletion(*descriptor, external, backendPending, activeWrite)) {
        // Preserve the current baseline/draft. Repeated synchronized() signals
        // for the same retired backend value must remain no-ops.
        next.busy = hasPending;
        if (hasPending) {
            next.dirty = true;
            next.phase = SettingsWritePhase::Pending;
        }
    } else if (hasPending) {
        const PendingWrite& operation = pending.value();
        if (operation.revision != next.revision) {
            return;
        }
        const bool operationPending = backendPending || m_backend.fieldPending(fieldId);
        const bool operationFailed = operationErrorChanged(*descriptor, operation);
        if (operationFailed) {
            // A new field/storage error is terminal for this generation even
            // when the backend still reports its asynchronous work as pending.
            // Keeping the attempted draft makes the failure retryable.
            if (operationPending) {
                retireWrite(fieldId, operation);
            }
            next.busy = false;
            next.dirty = true;
            next.phase = SettingsWritePhase::Failed;
            next.error = writeError(*descriptor);
            m_pendingWrites.remove(fieldId);
        } else if (operationPending) {
            // A matching backend value is not enough for a policy write: the
            // repository may still be validating/indexing it.
            if (!matchesValue(*descriptor, external, operation.baseline) &&
                !matchesValue(*descriptor, external, operation.target)) {
                next.acceptedValue = external;
                next.conflicted = true;
            }
            next.busy = true;
            next.dirty = true;
            next.phase = SettingsWritePhase::Pending;
        } else if (matchesValue(*descriptor, external, operation.target)) {
            next.acceptedValue = external;
            next.draftValue = external;
            next.dirty = false;
            next.busy = false;
            next.conflicted = false;
            next.phase = SettingsWritePhase::Clean;
            next.error.clear();
            m_pendingWrites.remove(fieldId);
        } else if (matchesValue(*descriptor, external, operation.baseline)) {
            // A completed operation that did not change the backend is a
            // rejected write. Do not replace the attempted draft.
            next.busy = false;
            next.dirty = true;
            next.phase = SettingsWritePhase::Failed;
            next.error = writeError(*descriptor);
            m_pendingWrites.remove(fieldId);
        } else {
            // An external update won the race with this generation. Keep the
            // user's draft, expose the new baseline as a conflict, and never
            // let the stale completion overwrite a newer revision.
            next.busy = false;
            next.dirty = true;
            next.conflicted = true;
            next.phase = SettingsWritePhase::Failed;
            next.error = writeError(*descriptor);
            m_pendingWrites.remove(fieldId);
            next.acceptedValue = external;
        }
    } else if (hasPendingReset) {
        const PendingWrite& operation = reset.value();
        if (operation.revision != next.revision) {
            return;
        }
        const bool operationPending = backendPending || m_backend.fieldPending(fieldId);
        const bool operationFailed = operationErrorChanged(*descriptor, operation);
        if (operationFailed) {
            // A reset has no independent draft buffer. If the backend changed
            // the value before reporting failure, expose that target as a
            // retryable draft against the pre-reset baseline.
            next.acceptedValue = operation.baseline;
            next.draftValue = external;
            next.dirty = !valuesEqual(*descriptor, next.draftValue, next.acceptedValue);
            next.busy = false;
            next.conflicted = false;
            next.phase = SettingsWritePhase::Failed;
            next.error = writeError(*descriptor);
            m_pendingResets.remove(fieldId);
        } else if (operationPending) {
            next.acceptedValue = external;
            next.draftValue = external;
            next.dirty = false;
            next.busy = true;
            next.conflicted = false;
            next.phase = SettingsWritePhase::Pending;
        } else if (matchesValue(*descriptor, external, operation.target)) {
            next.acceptedValue = external;
            next.draftValue = external;
            next.dirty = false;
            next.busy = false;
            next.conflicted = false;
            next.phase = SettingsWritePhase::Clean;
            next.error.clear();
            m_pendingResets.remove(fieldId);
        } else {
            next.acceptedValue = external;
            next.draftValue = operation.target;
            next.dirty = !valuesEqual(*descriptor, next.draftValue, next.acceptedValue);
            next.busy = false;
            next.conflicted = true;
            next.phase = SettingsWritePhase::Failed;
            next.error = writeError(*descriptor);
            m_pendingResets.remove(fieldId);
        }
    } else if (!next.dirty && next.phase != SettingsWritePhase::Failed &&
               next.phase != SettingsWritePhase::Rejected) {
        next.acceptedValue = external;
        next.draftValue = external;
        // History-policy resets update configuration immediately and finish
        // repository work asynchronously. Preserve that in-flight state even
        // though there is no user-authored draft generation. Other reset
        // groups are synchronous configuration mutations and must settle here.
        const bool historyPolicyPending =
            descriptor->reset == SettingsSectionReset::HistoryPolicy && backendPending;
        next.busy = historyPolicyPending;
        next.phase = historyPolicyPending ? SettingsWritePhase::Pending
                                           : SettingsWritePhase::Clean;
        next.conflicted = false;
        next.error.clear();
    } else if (valuesEqual(*descriptor, external, next.draftValue) && !backendPending &&
               next.phase != SettingsWritePhase::Failed &&
               next.phase != SettingsWritePhase::Rejected) {
        // A failed/rejected backend call may have mutated its in-memory value
        // before returning false (or before publishing the failure). Keep the
        // attempted draft and error visible until the caller explicitly
        // retries or discards it instead of silently treating that value as a
        // successful commit.
        next.acceptedValue = external;
        next.dirty = false;
        next.busy = false;
        next.conflicted = false;
        next.phase = SettingsWritePhase::Clean;
        next.error.clear();
    } else if (!valuesEqual(*descriptor, external, next.acceptedValue)) {
        next.conflicted = true;
        // Keep the latest accepted value visible alongside the user's draft;
        // this is the conflict baseline used by retry/discard actions.
        next.acceptedValue = external;
        if (next.phase == SettingsWritePhase::Pending && !backendPending) {
            next.busy = false;
            next.phase = SettingsWritePhase::Failed;
            next.error = writeError(*descriptor);
        }
    }
    const storage::StorageStatus currentStatus = storageStatus();
    if (descriptor->definition == nullptr) {
        next.enabled = false;
    } else if (const auto* actionDefinition =
                   std::get_if<SettingsActionDefinition>(&descriptor->definition->payload)) {
        next.enabled = currentStatus.writeAvailable &&
                       m_backend.actionState(actionDefinition->binding).enabled;
    } else if (isReadOnly(*descriptor)) {
        next.enabled = false;
    } else {
        next.enabled = currentStatus.writeAvailable;
        const bool historyField =
            descriptor->reset == SettingsSectionReset::HistoryPolicy ||
            descriptor->configurationKey.startsWith(QStringLiteral("capture_history/"));
        if (historyField) {
            next.enabled = next.enabled && !currentStatus.historyPolicyUpdating;
        }
        if (const auto* switchDefinition =
                std::get_if<SettingsSwitchDefinition>(&descriptor->definition->payload)) {
            next.enabled = next.enabled && m_backend.switchEnabled(switchDefinition->binding);
        }
    }
    if (next.phase == SettingsWritePhase::Pending && !next.busy && next.dirty) {
        next.phase = SettingsWritePhase::Failed;
    }
    updateState(fieldId, next);
}

void SettingsRuntimeSession::refreshAll() {
    for (const SettingsFieldDescriptor& descriptor : m_registry.fields()) {
        refreshField(descriptor.id);
        refreshOptions(descriptor);
    }
    refreshAuxiliaryInteger(SettingsIntegerBinding::ScreenshotDelaySeconds);
    refreshCommandStates();
    const storage::StorageStatus currentStatus = storageStatus();
    const bool statusChanged = !m_hasStorageStatus ||
                               !sameStorageStatus(m_lastStorageStatus, currentStatus);
    m_lastStorageStatus = currentStatus;
    m_hasStorageStatus = true;
    if (statusChanged) {
        emit storageStateChanged(currentStatus);
    }
    emit refreshed();
}

void SettingsRuntimeSession::refreshAuxiliaryInteger(SettingsIntegerBinding binding) {
    // Some runtime values decorate another field instead of owning a catalog
    // row. Screenshot delay, for example, is edited inside the delayed-capture
    // shortcut. Keep those values reactive without inventing a duplicate
    // settings item or exposing the backend to widgets.
    if (descriptorForInteger(binding) != nullptr) {
        return;
    }
    const int key = static_cast<int>(binding);
    const int value = m_backend.integerValue(binding);
    const auto found = m_auxiliaryIntegerValues.constFind(key);
    if (found != m_auxiliaryIntegerValues.cend() && found.value() == value) {
        return;
    }
    m_auxiliaryIntegerValues.insert(key, value);
    emit auxiliaryIntegerChanged(binding, value);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorFor(
    const QString& fieldId) const {
    return m_registry.field(fieldId);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForSelect(
    SettingsSelectBinding binding) const {
    return m_registry.fieldForSelect(binding);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForSwitch(
    SettingsSwitchBinding binding) const {
    return m_registry.fieldForSwitch(binding);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForInteger(
    SettingsIntegerBinding binding) const {
    return m_registry.fieldForInteger(binding);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForMulti(
    SettingsMultiSelectBinding binding) const {
    return m_registry.fieldForMultiSelect(binding);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForSlider(
    SettingsSliderBinding binding) const {
    return m_registry.fieldForSlider(binding);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForColor(
    SettingsColorBinding binding) const {
    return m_registry.fieldForColor(binding);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForRadio(
    SettingsRadioBinding binding) const {
    return m_registry.fieldForRadio(binding);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForFile(
    SettingsFilePathBinding binding) const {
    return m_registry.fieldForFilePath(binding);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForDirectory(
    SettingsDirectoryPathBinding binding) const {
    return m_registry.fieldForDirectoryPath(binding);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForText(
    SettingsTextBinding binding) const {
    return m_registry.fieldForText(binding);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForShortcut(
    GlobalShortcutAction action) const {
    return m_registry.fieldForShortcut(action);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForLocal(
    SettingsLocalShortcutScope scope, const QString& shortcutId) const {
    return m_registry.fieldForLocalShortcut(scope, shortcutId);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForAction(
    SettingsActionBinding binding) const {
    return m_registry.fieldForAction(binding);
}

const SettingsFieldDescriptor* SettingsRuntimeSession::descriptorForCustom(
    SettingsCustomRenderer renderer) const {
    return m_registry.fieldForCustom(renderer);
}

QVariant SettingsRuntimeSession::readValue(const SettingsFieldDescriptor& descriptor) const {
    if (descriptor.definition == nullptr) {
        return {};
    }
    return std::visit(
        [this](const auto& payload) -> QVariant {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, SettingsSelectDefinition>) {
                return m_backend.selectValue(payload.binding);
            } else if constexpr (std::is_same_v<Payload, SettingsSwitchDefinition>) {
                return m_backend.switchValue(payload.binding);
            } else if constexpr (std::is_same_v<Payload, SettingsIntegerDefinition>) {
                return m_backend.integerValue(payload.binding);
            } else if constexpr (std::is_same_v<Payload, SettingsMultiSelectDefinition>) {
                return m_backend.multiSelectValue(payload.binding);
            } else if constexpr (std::is_same_v<Payload, SettingsSliderDefinition>) {
                return m_backend.sliderValue(payload.binding);
            } else if constexpr (std::is_same_v<Payload, SettingsColorDefinition>) {
                return m_backend.colorValue(payload.binding);
            } else if constexpr (std::is_same_v<Payload, SettingsRadioDefinition>) {
                return m_backend.radioValue(payload.binding);
            } else if constexpr (std::is_same_v<Payload, SettingsFilePathDefinition>) {
                return m_backend.filePathValue(payload.binding);
            } else if constexpr (std::is_same_v<Payload, SettingsDirectoryPathDefinition>) {
                return m_backend.directoryPathValue(payload.binding);
            } else if constexpr (std::is_same_v<Payload, SettingsTextDefinition>) {
                return m_backend.textValue(payload.binding);
            } else if constexpr (std::is_same_v<Payload, SettingsShortcutActionDefinition>) {
                return m_backend.shortcutState(payload.shortcutAction).shortcuts;
            } else if constexpr (std::is_same_v<Payload, SettingsLocalShortcutDefinition>) {
                return stringListVariant(m_backend.localShortcuts(payload.scope,
                                                                    payload.shortcutId));
            } else if constexpr (std::is_same_v<Payload, SettingsActionDefinition>) {
                const SettingsActionState state = m_backend.actionState(payload.binding);
                return QVariantList{state.enabled, state.busy};
            } else if constexpr (std::is_same_v<Payload, SettingsCustomDefinition>) {
                switch (payload.renderer) {
                case SettingsCustomRenderer::DrawingToolbarEditor:
                    return QVariant::fromValue(m_backend.toolbarLayout());
                case SettingsCustomRenderer::TrayMenuOptions:
                    return m_backend.multiSelectValue(SettingsMultiSelectBinding::TrayMenuOptions);
                case SettingsCustomRenderer::StorageStatus:
                    return QVariant::fromValue(m_backend.storageStatus());
                }
                return QVariant();
            } else {
                return QVariant();
            }
        },
        descriptor.definition->payload);
}

bool SettingsRuntimeSession::writeValue(const SettingsFieldDescriptor& descriptor,
                                         const QVariant& value) {
    if (descriptor.definition == nullptr) {
        return false;
    }
    return std::visit(
        [this, &value](const auto& payload) -> bool {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, SettingsSelectDefinition>) {
                return m_backend.applySelectValue(payload.binding, value);
            } else if constexpr (std::is_same_v<Payload, SettingsSwitchDefinition>) {
                return m_backend.applySwitchValue(payload.binding, value.toBool());
            } else if constexpr (std::is_same_v<Payload, SettingsIntegerDefinition>) {
                return m_backend.applyIntegerValue(payload.binding, value.toInt());
            } else if constexpr (std::is_same_v<Payload, SettingsMultiSelectDefinition>) {
                return m_backend.applyMultiSelectValue(payload.binding, value.toList());
            } else if constexpr (std::is_same_v<Payload, SettingsSliderDefinition>) {
                return m_backend.applySliderValue(payload.binding, value.toInt());
            } else if constexpr (std::is_same_v<Payload, SettingsColorDefinition>) {
                return m_backend.applyColorValue(payload.binding, value.value<QColor>());
            } else if constexpr (std::is_same_v<Payload, SettingsRadioDefinition>) {
                return m_backend.applyRadioValue(payload.binding, value);
            } else if constexpr (std::is_same_v<Payload, SettingsFilePathDefinition>) {
                return m_backend.applyFilePathValue(payload.binding, value.toString());
            } else if constexpr (std::is_same_v<Payload, SettingsDirectoryPathDefinition>) {
                return m_backend.applyDirectoryPathValue(payload.binding, value.toString());
            } else if constexpr (std::is_same_v<Payload, SettingsTextDefinition>) {
                return m_backend.applyTextValue(payload.binding, value.toString());
            } else if constexpr (std::is_same_v<Payload, SettingsShortcutActionDefinition>) {
                return m_backend.applyShortcuts(payload.shortcutAction, stringListValue(value));
            } else if constexpr (std::is_same_v<Payload, SettingsLocalShortcutDefinition>) {
                return m_backend.applyLocalShortcuts(payload.scope, payload.shortcutId,
                                                     stringListValue(value));
            } else if constexpr (std::is_same_v<Payload, SettingsCustomDefinition>) {
                switch (payload.renderer) {
                case SettingsCustomRenderer::DrawingToolbarEditor:
                    if (!value.canConvert<storage::ScreenshotToolbarLayout>()) {
                        return false;
                    }
                    return m_backend.applyToolbarLayout(
                        value.value<storage::ScreenshotToolbarLayout>());
                case SettingsCustomRenderer::TrayMenuOptions:
                    return m_backend.applyMultiSelectValue(
                        SettingsMultiSelectBinding::TrayMenuOptions, value.toList());
                case SettingsCustomRenderer::StorageStatus:
                    return false;
                }
                return false;
            } else {
                return false;
            }
        },
        descriptor.definition->payload);
}

bool SettingsRuntimeSession::isPending(const SettingsFieldDescriptor& descriptor) const {
    if (descriptor.definition == nullptr) {
        return false;
    }
    if (m_backend.fieldPending(descriptor.id)) {
        return true;
    }
    const storage::StorageStatus status = storageStatus();
    if (std::holds_alternative<SettingsIntegerDefinition>(descriptor.definition->payload)) {
        const auto& payload = std::get<SettingsIntegerDefinition>(descriptor.definition->payload);
        return status.historyPolicyUpdating &&
               (payload.binding == SettingsIntegerBinding::HistoryRetentionDays ||
                payload.binding == SettingsIntegerBinding::HistoryMaxEntries ||
                payload.binding == SettingsIntegerBinding::HistoryMaxDiskMiB);
    }
    if (std::holds_alternative<SettingsSwitchDefinition>(descriptor.definition->payload)) {
        return status.historyPolicyUpdating &&
               std::get<SettingsSwitchDefinition>(descriptor.definition->payload).binding ==
                   SettingsSwitchBinding::HistoryEnabled;
    }
    return false;
}

QString SettingsRuntimeSession::writeError(
    const SettingsFieldDescriptor& descriptor) const {
    const QString currentError = backendError(descriptor);
    if (!currentError.isEmpty()) {
        return currentError;
    }
    return QStringLiteral("The setting could not be saved");
}

bool SettingsRuntimeSession::valuesEqual(const SettingsFieldDescriptor& descriptor,
                                         const QVariant& first,
                                         const QVariant& second) const {
    if (descriptor.definition == nullptr) {
        return first == second;
    }
    if (std::holds_alternative<SettingsColorDefinition>(descriptor.definition->payload)) {
        return first.value<QColor>() == second.value<QColor>();
    }
    if (std::holds_alternative<SettingsLocalShortcutDefinition>(descriptor.definition->payload) ||
        std::holds_alternative<SettingsShortcutActionDefinition>(descriptor.definition->payload) ||
        std::holds_alternative<SettingsMultiSelectDefinition>(descriptor.definition->payload)) {
        return stringListValue(first) == stringListValue(second);
    }
    if (std::holds_alternative<SettingsCustomDefinition>(descriptor.definition->payload)) {
        const auto& custom = std::get<SettingsCustomDefinition>(descriptor.definition->payload);
        if (custom.renderer == SettingsCustomRenderer::DrawingToolbarEditor) {
            return first.value<storage::ScreenshotToolbarLayout>() ==
                   second.value<storage::ScreenshotToolbarLayout>();
        }
        if (custom.renderer == SettingsCustomRenderer::TrayMenuOptions) {
            return stringListValue(first) == stringListValue(second);
        }
    }
    return first == second;
}

bool SettingsRuntimeSession::isReadOnly(const SettingsFieldDescriptor& descriptor) const {
    if (descriptor.definition == nullptr) {
        return true;
    }
    if (std::holds_alternative<SettingsActionDefinition>(descriptor.definition->payload)) {
        return true;
    }
    if (const auto* custom =
            std::get_if<SettingsCustomDefinition>(&descriptor.definition->payload);
        custom != nullptr && custom->renderer == SettingsCustomRenderer::StorageStatus) {
        return true;
    }
    return false;
}

SettingsOptions SettingsRuntimeSession::buildOptions(
    const SettingsFieldDescriptor& descriptor) const {
    SettingsOptions result;
    if (descriptor.definition == nullptr) {
        result.error = QStringLiteral("Unknown settings field");
        return result;
    }
    if (const auto* select = std::get_if<SettingsSelectDefinition>(&descriptor.definition->payload)) {
        for (const SettingsOptionDefinition& option : select->options) {
            result.values.push_back({option.value, option.label.translated()});
        }
        result.values.append(m_backend.dynamicSelectOptions(select->binding));
    } else if (const auto* multi =
                   std::get_if<SettingsMultiSelectDefinition>(&descriptor.definition->payload)) {
        for (const SettingsOptionDefinition& option : multi->options) {
            result.values.push_back({option.value, option.label.translated()});
        }
    } else if (const auto* radio =
                   std::get_if<SettingsRadioDefinition>(&descriptor.definition->payload)) {
        for (const SettingsRadioOptionDefinition& option : radio->options) {
            result.values.push_back({option.value, option.label.translated()});
        }
    } else if (const auto* custom =
                   std::get_if<SettingsCustomDefinition>(&descriptor.definition->payload);
               custom != nullptr && custom->renderer == SettingsCustomRenderer::TrayMenuOptions) {
        for (const SettingsTrayMenuGroupDefinition& group : m_registry.catalog().trayMenuGroups()) {
            for (const SettingsTrayMenuOptionDefinition& option : group.options) {
                result.values.push_back({option.id, option.label.translated()});
            }
        }
    }
    return result;
}

void SettingsRuntimeSession::refreshOptions(const SettingsFieldDescriptor& descriptor) {
    const SettingsOptions next = buildOptions(descriptor);
    const auto found = m_optionsCache.constFind(descriptor.id);
    if (found == m_optionsCache.cend() || found.value().values != next.values ||
        found.value().loading != next.loading || found.value().error != next.error) {
        m_optionsCache.insert(descriptor.id, next);
        emit optionsChanged(descriptor.id, next);
    }
}

void SettingsRuntimeSession::refreshCommandStates() {
    const SettingsCommandKind kinds[] = {SettingsCommandKind::CaptureScreenshot,
                                         SettingsCommandKind::ExecuteQuickAction,
                                         SettingsCommandKind::Navigate};
    for (SettingsCommandKind kind : kinds) {
        SettingsCommandState next;
        next.enabled = true;
        if (kind == SettingsCommandKind::ExecuteQuickAction) {
            next.enabled = storageStatus().writeAvailable || !hasPendingWrites();
        }
        const int key = static_cast<int>(kind);
        const auto found = m_commandStateCache.constFind(key);
        if (found == m_commandStateCache.cend() || found.value().enabled != next.enabled ||
            found.value().busy != next.busy || found.value().error != next.error) {
            m_commandStateCache.insert(key, next);
            emit commandStateChanged(kind, next);
        }
    }
}

void SettingsRuntimeSession::updateState(const QString& fieldId,
                                         const SettingsFieldState& next) {
    const auto found = m_states.constFind(fieldId);
    if (found != m_states.cend() && found.value() == next) {
        return;
    }
    m_states.insert(fieldId, next);
    emit fieldChanged(fieldId, next);
}

#define SESSION_DELEGATE_SELECT(name, bindingType, descriptorFn) \
    QVariant SettingsRuntimeSession::name(bindingType binding) const { \
        const auto* descriptor = descriptorFn(binding); \
        return descriptor != nullptr ? state(descriptor->id).draftValue : QVariant(); \
    }

SESSION_DELEGATE_SELECT(selectValue, SettingsSelectBinding, descriptorForSelect)

QVector<SettingsRuntimeOption>
SettingsRuntimeSession::dynamicSelectOptions(SettingsSelectBinding binding) const {
    return m_backend.dynamicSelectOptions(binding);
}

bool SettingsRuntimeSession::applySelectValue(SettingsSelectBinding binding,
                                               const QVariant& value) {
    const auto* descriptor = descriptorForSelect(binding);
    return descriptor != nullptr && submitDraft(descriptor->id, value);
}

bool SettingsRuntimeSession::switchValue(SettingsSwitchBinding binding) const {
    const auto* descriptor = descriptorForSwitch(binding);
    return descriptor != nullptr ? state(descriptor->id).draftValue.toBool() : false;
}

bool SettingsRuntimeSession::switchEnabled(SettingsSwitchBinding binding) const {
    return m_backend.switchEnabled(binding);
}

bool SettingsRuntimeSession::applySwitchValue(SettingsSwitchBinding binding, bool value) {
    const auto* descriptor = descriptorForSwitch(binding);
    return descriptor != nullptr && submitDraft(descriptor->id, value);
}

QVariantList SettingsRuntimeSession::multiSelectValue(SettingsMultiSelectBinding binding) const {
    const auto* descriptor = descriptorForMulti(binding);
    if (descriptor != nullptr) {
        return state(descriptor->id).draftValue.toList();
    }
    if (binding == SettingsMultiSelectBinding::TrayMenuOptions) {
        if (const auto* custom = descriptorForCustom(SettingsCustomRenderer::TrayMenuOptions)) {
            return state(custom->id).draftValue.toList();
        }
    }
    return {};
}

bool SettingsRuntimeSession::applyMultiSelectValue(SettingsMultiSelectBinding binding,
                                                   const QVariantList& value) {
    const auto* descriptor = descriptorForMulti(binding);
    if (descriptor == nullptr && binding == SettingsMultiSelectBinding::TrayMenuOptions) {
        descriptor = descriptorForCustom(SettingsCustomRenderer::TrayMenuOptions);
    }
    return descriptor != nullptr && submitDraft(descriptor->id, value);
}

int SettingsRuntimeSession::integerValue(SettingsIntegerBinding binding) const {
    const auto* descriptor = descriptorForInteger(binding);
    if (descriptor != nullptr) {
        return state(descriptor->id).draftValue.toInt();
    }
    const auto cached = m_auxiliaryIntegerValues.constFind(static_cast<int>(binding));
    return cached != m_auxiliaryIntegerValues.cend() ? cached.value()
                                                     : m_backend.integerValue(binding);
}

bool SettingsRuntimeSession::applyIntegerValue(SettingsIntegerBinding binding, int value) {
    const auto* descriptor = descriptorForInteger(binding);
    if (descriptor != nullptr) {
        return submitDraft(descriptor->id, value);
    }
    const bool accepted = m_backend.applyIntegerValue(binding, value);
    refreshAuxiliaryInteger(binding);
    return accepted;
}

int SettingsRuntimeSession::sliderValue(SettingsSliderBinding binding) const {
    const auto* descriptor = descriptorForSlider(binding);
    return descriptor != nullptr ? state(descriptor->id).draftValue.toInt() : 0;
}

bool SettingsRuntimeSession::applySliderValue(SettingsSliderBinding binding, int value) {
    const auto* descriptor = descriptorForSlider(binding);
    return descriptor != nullptr && submitDraft(descriptor->id, value);
}

QColor SettingsRuntimeSession::colorValue(SettingsColorBinding binding) const {
    const auto* descriptor = descriptorForColor(binding);
    return descriptor != nullptr ? state(descriptor->id).draftValue.value<QColor>() : QColor();
}

bool SettingsRuntimeSession::applyColorValue(SettingsColorBinding binding, const QColor& value) {
    const auto* descriptor = descriptorForColor(binding);
    return descriptor != nullptr && submitDraft(descriptor->id, QVariant::fromValue(value));
}

QVariant SettingsRuntimeSession::radioValue(SettingsRadioBinding binding) const {
    const auto* descriptor = descriptorForRadio(binding);
    return descriptor != nullptr ? state(descriptor->id).draftValue : QVariant();
}

bool SettingsRuntimeSession::applyRadioValue(SettingsRadioBinding binding,
                                             const QVariant& value) {
    const auto* descriptor = descriptorForRadio(binding);
    return descriptor != nullptr && submitDraft(descriptor->id, value);
}

QString SettingsRuntimeSession::filePathValue(SettingsFilePathBinding binding) const {
    const auto* descriptor = descriptorForFile(binding);
    return descriptor != nullptr ? state(descriptor->id).draftValue.toString() : QString();
}

bool SettingsRuntimeSession::applyFilePathValue(SettingsFilePathBinding binding,
                                                const QString& value) {
    const auto* descriptor = descriptorForFile(binding);
    return descriptor != nullptr && submitDraft(descriptor->id, value);
}

QString SettingsRuntimeSession::directoryPathValue(SettingsDirectoryPathBinding binding) const {
    const auto* descriptor = descriptorForDirectory(binding);
    return descriptor != nullptr ? state(descriptor->id).draftValue.toString() : QString();
}

bool SettingsRuntimeSession::applyDirectoryPathValue(SettingsDirectoryPathBinding binding,
                                                     const QString& value) {
    const auto* descriptor = descriptorForDirectory(binding);
    return descriptor != nullptr && submitDraft(descriptor->id, value);
}

QString SettingsRuntimeSession::textValue(SettingsTextBinding binding) const {
    const auto* descriptor = descriptorForText(binding);
    return descriptor != nullptr ? state(descriptor->id).draftValue.toString() : QString();
}

bool SettingsRuntimeSession::applyTextValue(SettingsTextBinding binding, const QString& value) {
    const auto* descriptor = descriptorForText(binding);
    return descriptor != nullptr && submitDraft(descriptor->id, value);
}

storage::ScreenshotToolbarLayout SettingsRuntimeSession::toolbarLayout() const {
    if (const auto* descriptor = descriptorForCustom(SettingsCustomRenderer::DrawingToolbarEditor)) {
        const QVariant value = state(descriptor->id).draftValue;
        if (value.canConvert<storage::ScreenshotToolbarLayout>()) {
            return value.value<storage::ScreenshotToolbarLayout>();
        }
    }
    return m_backend.toolbarLayout();
}

bool SettingsRuntimeSession::applyToolbarLayout(const storage::ScreenshotToolbarLayout& layout) {
    if (const auto* descriptor = descriptorForCustom(SettingsCustomRenderer::DrawingToolbarEditor)) {
        return submitDraft(descriptor->id, QVariant::fromValue(layout));
    }
    return m_backend.applyToolbarLayout(layout);
}

GlobalShortcutRegistrationState
SettingsRuntimeSession::shortcutState(GlobalShortcutAction action) const {
    GlobalShortcutRegistrationState result = m_backend.shortcutState(action);
    if (const auto* descriptor = descriptorForShortcut(action)) {
        const SettingsFieldState current = state(descriptor->id);
        if (current.dirty) {
            result.shortcuts = stringListValue(current.draftValue);
            result.status = GlobalShortcutStatus::Failed;
        }
    }
    return result;
}

GlobalShortcutValidationResult
SettingsRuntimeSession::validateShortcut(const QString& shortcut) const {
    return m_backend.validateShortcut(shortcut);
}

bool SettingsRuntimeSession::applyShortcuts(GlobalShortcutAction action,
                                            const QStringList& shortcuts) {
    const auto* descriptor = descriptorForShortcut(action);
    return descriptor != nullptr && submitDraft(descriptor->id, stringListVariant(shortcuts));
}

QStringList SettingsRuntimeSession::localShortcuts(SettingsLocalShortcutScope scope,
                                                   const QString& shortcutId) const {
    const auto* descriptor = descriptorForLocal(scope, shortcutId);
    return descriptor != nullptr ? stringListValue(state(descriptor->id).draftValue)
                                 : QStringList();
}

GlobalShortcutValidationResult SettingsRuntimeSession::validateLocalShortcut(
    SettingsLocalShortcutScope scope, const QString& shortcutId, const QString& shortcut) const {
    return m_backend.validateLocalShortcut(scope, shortcutId, shortcut);
}

bool SettingsRuntimeSession::applyLocalShortcuts(SettingsLocalShortcutScope scope,
                                                 const QString& shortcutId,
                                                 const QStringList& shortcuts) {
    const auto* descriptor = descriptorForLocal(scope, shortcutId);
    return descriptor != nullptr && submitDraft(descriptor->id, stringListVariant(shortcuts));
}

SettingsActionState SettingsRuntimeSession::actionState(SettingsActionBinding binding) const {
    return m_backend.actionState(binding);
}

bool SettingsRuntimeSession::triggerAction(SettingsActionBinding binding) {
    return m_backend.triggerAction(binding);
}

storage::StorageStatus SettingsRuntimeSession::storageStatus() const {
    return m_backend.storageStatus();
}

} // namespace snow_shot::presentation::settings
