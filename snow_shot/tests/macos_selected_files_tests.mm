#include "../src/platform/macos/selectedfiles_p.h"
#include <QCoreApplication>
#include <QUrl>
#include <cstdlib>
#include <iostream>

using namespace snow_shot::platform;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
struct Descriptor {
    AEDesc value{typeNull, nullptr};
    ~Descriptor() {
        AEDisposeDesc(&value);
    }
};
class FakeTransport final : public macos::FinderEventTransport {
  public:
    QStringList urls;
    OSStatus permission = noErr;
    OSStatus sendError = noErr;
    SInt32 remoteError = noErr;
    mutable int sends = 0;
    mutable int prompts = 0;
    mutable bool running = true;
    bool malformed = false;
    bool terminateAfterPermission = false;
    bool terminateAfterSend = false;
    int cancelOnSend = 0;
    mutable bool cancelled = false;
    quint32 finderProcessId() const override {
        return running ? 42 : 0;
    }
    bool isFinderRunning(quint32 pid) const override {
        return running && pid == 42;
    }
    OSStatus requestPermission(const AEAddressDesc& address) const override {
        ++prompts;
        pid_t pid = 0;
        require(address.descriptorType == typeKernelProcessID &&
                    AEGetDescData(&address, &pid, sizeof(pid)) == noErr && pid == 42,
                "permission must target the captured Finder PID");
        if (terminateAfterPermission)
            running = false;
        return permission;
    }
    OSStatus send(const AppleEvent& event, AppleEvent& reply) const override {
        ++sends;
        if (terminateAfterSend)
            running = false;
        if (cancelOnSend == sends)
            cancelled = true;
        if (sendError != noErr)
            return sendError;
        Descriptor address;
        require(AEGetAttributeDesc(&event, keyAddressAttr, typeKernelProcessID, &address.value) ==
                    noErr,
                "query must address Finder by PID");
        require(AECreateAppleEvent(kCoreEventClass, kAEAnswer, &address.value,
                                   kAutoGenerateReturnID, kAnyTransactionID, &reply) == noErr,
                "create reply");
        if (remoteError != noErr) {
            require(AEPutParamPtr(&reply, keyErrorNumber, typeSInt32, &remoteError,
                                  sizeof(remoteError)) == noErr,
                    "write remote failure");
            return noErr;
        }
        Descriptor object;
        require(AEGetParamDesc(&event, keyDirectObject, typeObjectSpecifier, &object.value) ==
                    noErr,
                "query must use a property specifier");
        DescType code = 0;
        require(AEGetKeyPtr(&object.value, keyAEKeyData, typeType, nullptr, &code, sizeof(code),
                            nullptr) == noErr,
                "query must specify a property code");
        if (code == 'sele') {
            Descriptor list;
            require(AECreateList(nullptr, 0, false, &list.value) == noErr, "create selection list");
            for (qsizetype i = 0; i < urls.size(); ++i) {
                const SInt32 index = static_cast<SInt32>(i);
                require(AEPutPtr(&list.value, 0, typeSInt32, &index, sizeof(index)) == noErr,
                        "append selection");
            }
            if (malformed) {
                const SInt32 value = 1;
                require(AEPutParamPtr(&reply, keyDirectObject, typeSInt32, &value, sizeof(value)) ==
                            noErr,
                        "write malformed selection");
            } else {
                require(AEPutParamDesc(&reply, keyDirectObject, &list.value) == noErr,
                        "write selection");
            }
        } else {
            require(code == 'pURL', "selected items must be read as URLs");
            SInt32 index = -1;
            require(AEGetKeyPtr(&object.value, keyAEContainer, typeSInt32, nullptr, &index,
                                sizeof(index), nullptr) == noErr &&
                        index >= 0 && index < urls.size(),
                    "URL query must retain selected item identity");
            const QByteArray url = urls.at(index).toUtf8();
            require(AEPutParamPtr(&reply, keyDirectObject, typeUTF8Text, url.constData(),
                                  url.size()) == noErr,
                    "write URL");
        }
        return noErr;
    }
};
SelectedFileResult read(const std::shared_ptr<FakeTransport>& transport) {
    const auto backend = macos::createSelectedFileBackend(transport);
    const auto target = backend->captureTarget();
    require(backend->isValidTarget(target) == transport->running, "Finder target validity");
    return backend->selectedFiles(target, [&] { return transport->cancelled; });
}
void parsing() {
    auto transport = std::make_shared<FakeTransport>();
    const QString first = QString::fromUtf8("/tmp/选中 image #1%.png");
    const QString second = QStringLiteral("/tmp/line\nbreak.png");
    transport->urls = {QString::fromUtf8(QUrl::fromLocalFile(first).toEncoded()),
                       QStringLiteral("https://example.com/image.png"),
                       QString::fromUtf8(QUrl::fromLocalFile(second).toEncoded()),
                       QStringLiteral("file://remote/tmp/image.png"),
                       QStringLiteral("file:///tmp/%ZZ"),
                       QStringLiteral("file://localhost/tmp/local.png"),
                       QStringLiteral("file:relative.png"),
                       QStringLiteral("file:///tmp/image.png#fragment")};
    const auto result = read(transport);
    require(result.error == SelectedFileError::None &&
                result.paths == QStringList{first, second, QStringLiteral("/tmp/local.png")},
            "preserve Unicode, escaping, embedded newlines and selection order; skip "
            "nonlocal/invalid URLs");
    require(transport->prompts == 1 && transport->sends == 9,
            "request permission once per invocation");
    transport = std::make_shared<FakeTransport>();
    require(read(transport).paths.isEmpty() && transport->sends == 1,
            "empty selection is a silent success");
    transport->malformed = true;
    require(read(transport).error == SelectedFileError::QueryFailed, "reject malformed selection");
}
void failuresAndCancellation() {
    auto transport = std::make_shared<FakeTransport>();
    transport->permission = errAEEventNotPermitted;
    require(read(transport).error == SelectedFileError::PermissionDenied && transport->sends == 0,
            "denied permission must not query Finder");
    transport->permission = noErr;
    require(read(transport).error == SelectedFileError::None, "retry after approval must succeed");
    transport->sendError = errAETimeout;
    require(read(transport).error == SelectedFileError::Timeout, "surface IPC timeout");
    transport->sendError = noErr;
    transport->remoteError = errAEEventNotPermitted;
    require(read(transport).error == SelectedFileError::PermissionDenied,
            "parse remote permission failure");
    transport->remoteError = errAEEventFailed;
    require(read(transport).error == SelectedFileError::QueryFailed, "parse Finder failure");
    transport = std::make_shared<FakeTransport>();
    transport->running = false;
    require(read(transport).error == SelectedFileError::Unavailable && transport->prompts == 0,
            "missing Finder must not prompt or launch Finder");
    transport = std::make_shared<FakeTransport>();
    transport->terminateAfterPermission = true;
    require(read(transport).error == SelectedFileError::Unavailable && transport->sends == 0,
            "revalidate Finder after permission prompt");
    transport = std::make_shared<FakeTransport>();
    transport->terminateAfterSend = true;
    require(read(transport).error == SelectedFileError::Unavailable,
            "discard selection after Finder exits");
    transport = std::make_shared<FakeTransport>();
    transport->urls = {QStringLiteral("file:///tmp/first.png"),
                       QStringLiteral("file:///tmp/second.png")};
    transport->cancelOnSend = 3;
    const auto partial = read(transport);
    require(partial.paths.isEmpty() && partial.error == SelectedFileError::None &&
                transport->sends == 3,
            "cancellation during URL retrieval must discard already-read paths");
    transport = std::make_shared<FakeTransport>();
    transport->cancelled = true;
    require(read(transport).error == SelectedFileError::None && transport->prompts == 0,
            "cancel before permission request");
    transport->cancelled = false;
    transport->cancelOnSend = 1;
    transport->sendError = errAETimeout;
    const auto result = read(transport);
    require(result.paths.isEmpty() && result.error == SelectedFileError::None,
            "cancellation suppresses a late transport error");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    parsing();
    failuresAndCancellation();
    return 0;
}
