#ifndef SNOW_SHOT_APP_OPERATIONADMISSION_H
#define SNOW_SHOT_APP_OPERATIONADMISSION_H

#include <cstdint>

namespace snow_shot::app {
enum class ApplicationOperation { Restart, UpdateRequest, UpdateHandoff, Relocation, Store, Trim };
enum class OperationBlocker : std::uint32_t {
    None = 0,
    Suspended = 1u << 0,
    Storage = 1u << 1,
    Updater = 1u << 2,
    Capture = 1u << 3,
    DirectCapture = 1u << 4,
    Editors = 1u << 5,
    Ocr = 1u << 6,
    Mcp = 1u << 7,
    Diagnostics = 1u << 8,
    Export = 1u << 9,
    Interaction = 1u << 10,
    Administrator = 1u << 11
};
struct OperationFacts {
    bool suspended = false;
    bool storageChanging = false;
    bool updaterUnavailableOrApplying = false;
    bool updaterBusy = false;
    bool captureBusy = false;
    bool captureInteracting = false;
    bool directCaptureBusy = false;
    bool unsavedEditors = false;
    bool ocrBusy = false;
    bool mcpBusy = false;
    bool diagnosticsBusy = false;
    bool exports = false;
    bool globalCanvas = false;
    bool pinOrToolbarInteraction = false;
    bool foreground = false;
    bool administratorPending = false;
};
struct OperationAdmission {
    std::uint32_t blockers = 0;
    [[nodiscard]] bool allowed() const {
        return blockers == 0;
    }
    [[nodiscard]] bool contains(OperationBlocker blocker) const {
        return (blockers & static_cast<std::uint32_t>(blocker)) != 0;
    }
};
// Inspection is a snapshot, never a reservation. Flush/checkpoint and final rechecks belong to
// the transaction owner. Different operations deliberately protect different resource sets.
[[nodiscard]] inline OperationAdmission inspectOperation(ApplicationOperation operation,
                                                         const OperationFacts& facts) {
    OperationAdmission result;
    const auto block = [&](bool condition, OperationBlocker reason) {
        if (condition)
            result.blockers |= static_cast<std::uint32_t>(reason);
    };
    const bool restart =
        operation == ApplicationOperation::Restart || operation == ApplicationOperation::Relocation;
    const bool trim = operation == ApplicationOperation::Trim;
    const bool store = operation == ApplicationOperation::Store;
    const bool relocation = operation == ApplicationOperation::Relocation;
    const bool handoff = operation == ApplicationOperation::UpdateHandoff;
    // Final handoff and cooperative Store admission run before acquiring their own pause.
    // Any existing pause therefore belongs to a competing transition, including preparation.
    block((restart || trim || handoff || store) && facts.suspended, OperationBlocker::Suspended);
    block((restart || trim || store || handoff) && facts.storageChanging,
          OperationBlocker::Storage);
    block((restart && facts.updaterUnavailableOrApplying) || (trim && facts.updaterBusy),
          OperationBlocker::Updater);
    block((trim || store) ? facts.captureInteracting : facts.captureBusy,
          OperationBlocker::Capture);
    block(facts.directCaptureBusy, OperationBlocker::DirectCapture);
    block((restart || handoff) && facts.unsavedEditors, OperationBlocker::Editors);
    block((relocation || trim || store) && facts.ocrBusy, OperationBlocker::Ocr);
    block((relocation || trim || store || handoff) && facts.mcpBusy, OperationBlocker::Mcp);
    block((relocation || trim || store) && facts.diagnosticsBusy, OperationBlocker::Diagnostics);
    block((relocation || store) && facts.exports, OperationBlocker::Export);
    block(((trim || store) && facts.pinOrToolbarInteraction) || (store && facts.globalCanvas) ||
              (trim && facts.foreground),
          OperationBlocker::Interaction);
    block(operation == ApplicationOperation::UpdateRequest && facts.administratorPending,
          OperationBlocker::Administrator);
    return result;
}
} // namespace snow_shot::app
#endif
