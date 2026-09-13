#include "snow_shot/platform/macos/selectedtextcapturebackend.h"

#include <QElapsedTimer>
#include <QRunnable>
#include <QThreadPool>

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <atomic>
#include <mutex>
#include <utility>

namespace snow_shot::platform::macos {
namespace {
using presentation::SelectedTextCaptureResult;
using presentation::SelectedTextStatus;

struct CfRelease {
    void operator()(const void* value) const {
        if (value != nullptr) {
            CFRelease(value);
        }
    }
};
using CfObject = std::unique_ptr<const void, CfRelease>;

SelectedTextCaptureResult axFailure(AXError error) {
    switch (error) {
    case kAXErrorAPIDisabled:
        return {SelectedTextStatus::PermissionDenied, {}};
    case kAXErrorCannotComplete:
        return {SelectedTextStatus::TimedOut, {}};
    case kAXErrorNoValue:
        return {SelectedTextStatus::NoSelection, {}};
    case kAXErrorAttributeUnsupported:
    case kAXErrorNotImplemented:
        return {SelectedTextStatus::Unsupported, {}};
    default:
        return {SelectedTextStatus::Failed, {}};
    }
}

SelectedTextCaptureResult readSelectedText(qint64 processId) {
    @autoreleasepool {
        if (!AXIsProcessTrusted()) {
            return {SelectedTextStatus::PermissionDenied, {}};
        }
        CfObject application(AXUIElementCreateApplication(static_cast<pid_t>(processId)));
        auto app = static_cast<AXUIElementRef>(application.get());
        if (app == nullptr) {
            return {SelectedTextStatus::Failed, {}};
        }
        // Bound each cross-process request, without changing other users of AX in this process.
        constexpr float timeoutSeconds = 0.25F;
        if (const auto error = AXUIElementSetMessagingTimeout(app, timeoutSeconds);
            error != kAXErrorSuccess) {
            return axFailure(error);
        }
        CFTypeRef rawElement = nullptr;
        const auto focusError =
            AXUIElementCopyAttributeValue(app, kAXFocusedUIElementAttribute, &rawElement);
        CfObject focused(rawElement);
        if (focusError != kAXErrorSuccess) {
            return axFailure(focusError);
        }
        if (focused == nullptr || CFGetTypeID(focused.get()) != AXUIElementGetTypeID()) {
            return {SelectedTextStatus::NoSelection, {}};
        }
        auto element = static_cast<AXUIElementRef>(focused.get());
        if (const auto error = AXUIElementSetMessagingTimeout(element, timeoutSeconds);
            error != kAXErrorSuccess) {
            return axFailure(error);
        }
        CFTypeRef rawText = nullptr;
        const auto textError =
            AXUIElementCopyAttributeValue(element, kAXSelectedTextAttribute, &rawText);
        CfObject selectedText(rawText);
        if (textError != kAXErrorSuccess) {
            return axFailure(textError);
        }
        if (selectedText == nullptr || CFGetTypeID(selectedText.get()) != CFStringGetTypeID()) {
            return {SelectedTextStatus::NoSelection, {}};
        }
        const auto text = static_cast<CFStringRef>(selectedText.get());
        const auto length = CFStringGetLength(text);
        // Match the existing service's bounded input policy without reading entire field values.
        constexpr CFIndex maxTextUnits = 1024 * 1024;
        if (length > maxTextUnits) {
            return {SelectedTextStatus::Failed, {}};
        }
        QString result = QString::fromCFString(text);
        return {result.trimmed().isEmpty() ? SelectedTextStatus::NoSelection
                                           : SelectedTextStatus::Selected,
                std::move(result)};
    }
}

struct CaptureRequest {
    std::atomic_bool cancelled = false;
    std::mutex mutex;
    SelectedTextCaptureResult result;
};

class MacOsSelectedTextCaptureBackend final : public presentation::SelectedTextCaptureBackend {
  public:
    explicit MacOsSelectedTextCaptureBackend(SelectedTextAccess access)
        : m_access(std::move(access)) {}
    ~MacOsSelectedTextCaptureBackend() override {
        cancel();
    }

    SelectedTextCaptureResult start() override {
        if (m_request) {
            return {SelectedTextStatus::Busy, {}};
        }
        if (!m_access.accessibilityTrusted || !m_access.foregroundProcessId ||
            !m_access.readSelectedText) {
            return {SelectedTextStatus::Failed, {}};
        }
        if (!m_access.accessibilityTrusted()) {
            return {SelectedTextStatus::PermissionDenied, {}};
        }
        const qint64 processId = m_access.foregroundProcessId();
        if (processId <= 0) {
            return {SelectedTextStatus::NoSelection, {}};
        }
        m_request = std::make_shared<CaptureRequest>();
        m_elapsed.start();
        QThreadPool::globalInstance()->start(
            QRunnable::create([request = m_request, reader = m_access.readSelectedText, processId] {
                if (request->cancelled.load()) {
                    return;
                }
                SelectedTextCaptureResult result;
                try {
                    result = reader(processId);
                } catch (...) {
                    result = {SelectedTextStatus::Failed, {}};
                }
                if (request->cancelled.load()) {
                    return;
                }
                std::lock_guard lock(request->mutex);
                request->result = std::move(result);
            }));
        return {};
    }

    SelectedTextCaptureResult poll() override {
        if (!m_request) {
            return {SelectedTextStatus::NoSelection, {}};
        }
        SelectedTextCaptureResult result;
        {
            std::lock_guard lock(m_request->mutex);
            result = m_request->result;
        }
        if (result.status != SelectedTextStatus::Pending) {
            m_request.reset();
            return result;
        }
        if (m_elapsed.elapsed() >= 1500) {
            cancel();
            return {SelectedTextStatus::TimedOut, {}};
        }
        return {};
    }

    void cancel() override {
        if (m_request) {
            m_request->cancelled.store(true);
            m_request.reset();
        }
    }

  private:
    SelectedTextAccess m_access;
    std::shared_ptr<CaptureRequest> m_request;
    QElapsedTimer m_elapsed;
};
} // namespace

std::unique_ptr<presentation::SelectedTextCaptureBackend> createSelectedTextCaptureBackend() {
    return createSelectedTextCaptureBackend(
        {[] { return AXIsProcessTrusted(); },
         []() -> qint64 {
             return NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
         },
         readSelectedText});
}

std::unique_ptr<presentation::SelectedTextCaptureBackend>
createSelectedTextCaptureBackend(SelectedTextAccess access) {
    return std::make_unique<MacOsSelectedTextCaptureBackend>(std::move(access));
}
} // namespace snow_shot::platform::macos
