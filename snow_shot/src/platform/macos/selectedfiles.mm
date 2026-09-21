#include "selectedfiles_p.h"

#include <QUrl>
#include <utility>
#import <AppKit/AppKit.h>

namespace snow_shot::platform {
namespace {
struct Descriptor {
    AEDesc value{typeNull, nullptr};
    ~Descriptor() {
        AEDisposeDesc(&value);
    }
    Descriptor() = default;
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};

SelectedFileResult failure(OSStatus status) {
    if (status == errAEEventNotPermitted) {
        return {{}, SelectedFileError::PermissionDenied};
    }
    if (status == errAETimeout) {
        return {{}, SelectedFileError::Timeout};
    }
    if (status == procNotFound || status == connectionInvalid) {
        return {{}, SelectedFileError::Unavailable};
    }
    return {{}, SelectedFileError::QueryFailed};
}

OSStatus property(DescType code, const AEDesc& container, AEDesc& result) {
    Descriptor record;
    OSStatus status = AECreateList(nullptr, 0, true, &record.value);
    const DescType desiredClass = cProperty;
    const DescType form = formPropertyID;
    if (status == noErr)
        status = AEPutKeyPtr(&record.value, keyAEDesiredClass, typeType, &desiredClass,
                             sizeof(desiredClass));
    if (status == noErr)
        status = AEPutKeyPtr(&record.value, keyAEKeyForm, typeEnumerated, &form, sizeof(form));
    if (status == noErr)
        status = AEPutKeyPtr(&record.value, keyAEKeyData, typeType, &code, sizeof(code));
    if (status == noErr)
        status = AEPutKeyDesc(&record.value, keyAEContainer, &container);
    if (status == noErr)
        status = AECoerceDesc(&record.value, typeObjectSpecifier, &result);
    return status;
}

OSStatus get(const macos::FinderEventTransport& transport, const AEAddressDesc& address,
             const AEDesc& object, AEDesc& result) {
    Descriptor event;
    Descriptor reply;
    OSStatus status = AECreateAppleEvent(kAECoreSuite, kAEGetData, &address, kAutoGenerateReturnID,
                                         kAnyTransactionID, &event.value);
    if (status == noErr)
        status = AEPutParamDesc(&event.value, keyDirectObject, &object);
    if (status == noErr)
        status = transport.send(event.value, reply.value);
    if (status != noErr)
        return status;
    SInt32 remoteError = noErr;
    DescType actualType = typeNull;
    Size actualSize = 0;
    // A successful send can still contain a Finder-side failure.
    const OSStatus errorStatus =
        AEGetParamPtr(&reply.value, keyErrorNumber, typeSInt32, &actualType, &remoteError,
                      sizeof(remoteError), &actualSize);
    if (errorStatus == noErr && remoteError != noErr)
        return remoteError;
    return AEGetParamDesc(&reply.value, keyDirectObject, typeWildCard, &result);
}

class NativeFinderEventTransport final : public macos::FinderEventTransport {
  public:
    quint32 finderProcessId() const override {
        @autoreleasepool {
            for (NSRunningApplication* app in [NSRunningApplication
                     runningApplicationsWithBundleIdentifier:@"com.apple.finder"]) {
                if (!app.terminated)
                    return static_cast<quint32>(app.processIdentifier);
            }
        }
        return 0;
    }
    bool isFinderRunning(quint32 processId) const override {
        @autoreleasepool {
            NSRunningApplication* app = [NSRunningApplication
                runningApplicationWithProcessIdentifier:static_cast<pid_t>(processId)];
            return app && !app.terminated &&
                   [app.bundleIdentifier isEqualToString:@"com.apple.finder"];
        }
    }
    OSStatus requestPermission(const AEAddressDesc& address) const override {
        return AEDeterminePermissionToAutomateTarget(&address, kAECoreSuite, kAEGetData, true);
    }
    OSStatus send(const AppleEvent& event, AppleEvent& reply) const override {
        // Never activate Finder. Bound each IPC wait; cancellation is checked
        // between requests and again before the selection reaches the batch.
        return AESendMessage(&event, &reply, kAEWaitReply | kAECanInteract, 10 * 60);
    }
};

class FinderSelectedFileBackend final : public SelectedFileBackend {
  public:
    explicit FinderSelectedFileBackend(std::shared_ptr<macos::FinderEventTransport> transport)
        : m_transport(std::move(transport)) {}

    SelectedFileTarget captureTarget() const override {
        SelectedFileTarget target;
        target.processId = m_transport->finderProcessId();
        return target;
    }
    bool isValidTarget(const SelectedFileTarget& target) const override {
        return target.processId != 0;
    }
    SelectedFileResult selectedFiles(const SelectedFileTarget& target,
                                     const std::function<bool()>& cancelled) const override {
        @autoreleasepool {
            if (cancelled())
                return {};
            if (!isValidTarget(target) || !m_transport->isFinderRunning(target.processId))
                return {{}, SelectedFileError::Unavailable};
            const pid_t pid = static_cast<pid_t>(target.processId);
            Descriptor address;
            OSStatus status = AECreateDesc(typeKernelProcessID, &pid, sizeof(pid), &address.value);
            if (status == noErr)
                status = m_transport->requestPermission(address.value);
            if (cancelled())
                return {};
            if (status != noErr)
                return failure(status);
            if (!m_transport->isFinderRunning(target.processId))
                return {{}, SelectedFileError::Unavailable};
            Descriptor root;
            Descriptor selection;
            Descriptor items;
            status = property('sele', root.value, selection.value);
            if (status == noErr)
                status = get(*m_transport, address.value, selection.value, items.value);
            if (cancelled())
                return {};
            if (status != noErr)
                return failure(status);
            long count = 0;
            if (items.value.descriptorType != typeAEList ||
                AECountItems(&items.value, &count) != noErr)
                return {{}, SelectedFileError::QueryFailed};
            QStringList paths;
            for (long index = 1; index <= count; ++index) {
                if (cancelled())
                    return {};
                if (!m_transport->isFinderRunning(target.processId))
                    return {{}, SelectedFileError::Unavailable};
                Descriptor item;
                Descriptor urlProperty;
                Descriptor url;
                Descriptor utf8;
                AEKeyword keyword = 0;
                status = AEGetNthDesc(&items.value, index, typeWildCard, &keyword, &item.value);
                if (status == noErr)
                    status = property('pURL', item.value, urlProperty.value);
                if (status == noErr)
                    status = get(*m_transport, address.value, urlProperty.value, url.value);
                // A selected item can disappear while the batch is being read.
                if (status == errAENoSuchObject)
                    continue;
                if (status == noErr)
                    status = AECoerceDesc(&url.value, typeUTF8Text, &utf8.value);
                if (cancelled())
                    return {};
                if (status != noErr)
                    return failure(status);
                const Size size = AEGetDescDataSize(&utf8.value);
                QByteArray bytes(size, '\0');
                if (AEGetDescData(&utf8.value, bytes.data(), size) != noErr)
                    return {{}, SelectedFileError::QueryFailed};
                QUrl fileUrl = QUrl::fromEncoded(bytes, QUrl::StrictMode);
                if (fileUrl.isValid() && fileUrl.isLocalFile() && !fileUrl.hasQuery() &&
                    !fileUrl.hasFragment() && fileUrl.path().startsWith(u'/') &&
                    (fileUrl.host().isEmpty() || fileUrl.host() == QStringLiteral("localhost"))) {
                    fileUrl.setHost({});
                    const QString path = fileUrl.toLocalFile();
                    if (!path.isEmpty())
                        paths.append(path);
                }
            }
            if (cancelled())
                return {};
            if (!m_transport->isFinderRunning(target.processId))
                return {{}, SelectedFileError::Unavailable};
            return {paths};
        }
    }

  private:
    std::shared_ptr<macos::FinderEventTransport> m_transport;
};
} // namespace

std::shared_ptr<SelectedFileBackend> createSelectedFileBackend() {
    return macos::createSelectedFileBackend(std::make_shared<NativeFinderEventTransport>());
}
namespace macos {
std::shared_ptr<SelectedFileBackend>
createSelectedFileBackend(std::shared_ptr<FinderEventTransport> transport) {
    return std::make_shared<FinderSelectedFileBackend>(std::move(transport));
}
} // namespace macos
} // namespace snow_shot::platform
