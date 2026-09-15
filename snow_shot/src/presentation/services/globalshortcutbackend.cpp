#include "globalshortcutbackend_p.h"

namespace snow_shot::presentation {
namespace {

GlobalShortcutValidationResult
validation(const shortcuts::ShortcutBinding& binding, bool supported,
           GlobalShortcutFailureReason reason = GlobalShortcutFailureReason::None) {
    GlobalShortcutValidationResult result;
    result.binding = binding;
    result.shortcut = binding.portableText;
    result.supported = supported;
    result.failureReason = supported ? GlobalShortcutFailureReason::None : reason;
    return result;
}

class UnsupportedGlobalShortcutBackend final : public GlobalShortcutBackend {
  public:
    void setActivationHandler(ActivationHandler) override {}

    GlobalShortcutValidationResult
    validateShortcut(const shortcuts::ShortcutBinding& binding) const override {
        return validation(binding, false, GlobalShortcutFailureReason::UnsupportedPlatform);
    }

    GlobalShortcutBackendResult registerShortcut(int, const shortcuts::ShortcutBinding&) override {
        return {false, GlobalShortcutFailureReason::UnsupportedPlatform, 0};
    }

    void unregisterShortcut(int) override {}
};

} // namespace

std::unique_ptr<GlobalShortcutBackend> createPlatformGlobalShortcutBackend() {
#ifdef Q_OS_WIN
    return createWindowsGlobalShortcutBackend();
#elif defined(Q_OS_MACOS)
    return createMacOSGlobalShortcutBackend();
#else
    return std::make_unique<UnsupportedGlobalShortcutBackend>();
#endif
}

} // namespace snow_shot::presentation
