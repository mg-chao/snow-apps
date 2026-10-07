#include "../src/platform/windows/nativeprintdialog.h"

#include <QApplication>
#include <QColorSpace>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <shellapi.h>
#include <winrt/base.h>

#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {
using Service = ScreenshotPrintService;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flush() {
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
}
void processUntil(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), "timed out waiting for asynchronous native print preparation");
}

enum class Outcome {
    ActivateFailure,
    MissingTarget,
    EnterFailure,
    RejectedImage,
    DropFailure,
    RetainedDropFailure,
    Cancel,
    RetainedCancel,
    ImmediateRelease,
    DeferredRelease,
    WorkerRelease,
    WindowHide,
    DestroyOwner
};
Outcome outcome = Outcome::DeferredRelease;
QWidget* expectedOwner = nullptr;
QImage expectedImage;
QString snapshotPath;
winrt::com_ptr<IDataObject> retainedData;
int activations = 0;
int enters = 0;
int drops = 0;
int leaves = 0;

class WizardTarget : public winrt::implements<WizardTarget, IDropTarget> {
  public:
    HRESULT __stdcall DragEnter(IDataObject* data, DWORD keys, POINTL,
                                DWORD* effect) noexcept override {
        ++enters;
        require(keys == MK_LBUTTON && *effect == DROPEFFECT_COPY,
                "the wizard must receive a copy-only image handoff");
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        require(SUCCEEDED(data->GetData(&format, &medium)),
                "the wizard must receive a Shell file data object");
        const auto files = reinterpret_cast<HDROP>(medium.hGlobal);
        require(DragQueryFileW(files, 0xffffffff, nullptr, 0) == 1,
                "each print request must contain exactly one snapshot");
        const UINT length = DragQueryFileW(files, 0, nullptr, 0);
        std::vector<wchar_t> path(static_cast<size_t>(length) + 1);
        DragQueryFileW(files, 0, path.data(), length + 1);
        snapshotPath = QString::fromWCharArray(path.data());
        ReleaseStgMedium(&medium);
        const QImage snapshot(snapshotPath);
        require(QFileInfo(snapshotPath).suffix() == QStringLiteral("png") &&
                    snapshot.size() == expectedImage.size() &&
                    snapshot.pixelColor(0, 0) == expectedImage.pixelColor(0, 0) &&
                    snapshot.pixelColor(1, 0) == expectedImage.pixelColor(1, 0) &&
                    snapshot.colorSpace() == expectedImage.colorSpace(),
                "the lossless snapshot must preserve full-size pixels and sRGB color");
        if (outcome == Outcome::RejectedImage)
            *effect = DROPEFFECT_NONE;
        return outcome == Outcome::EnterFailure ? E_FAIL : S_OK;
    }
    HRESULT __stdcall DragOver(DWORD, POINTL, DWORD*) noexcept override {
        return E_NOTIMPL;
    }
    HRESULT __stdcall DragLeave() noexcept override {
        ++leaves;
        return S_OK;
    }
    HRESULT __stdcall Drop(IDataObject* data, DWORD keys, POINTL, DWORD* effect) noexcept override {
        ++drops;
        require(keys == MK_LBUTTON && *effect == DROPEFFECT_COPY,
                "the wizard must own print layout and copies without moving the source");
        if (outcome == Outcome::RetainedDropFailure || outcome == Outcome::RetainedCancel)
            retainedData.copy_from(data);
        if (outcome == Outcome::DropFailure || outcome == Outcome::RetainedDropFailure)
            return E_FAIL;
        if (outcome == Outcome::Cancel || outcome == Outcome::RetainedCancel)
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (outcome != Outcome::ImmediateRelease)
            retainedData.copy_from(data);
        if (outcome == Outcome::DestroyOwner) {
            delete expectedOwner;
            expectedOwner = nullptr;
        }
        return S_OK;
    }
};

HRESULT WINAPI activateWizard(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid,
                              LPVOID* target) {
    require(QThread::currentThread() == qApp->thread(),
            "native wizard activation must remain on the initiating GUI apartment");
    ++activations;
    constexpr CLSID expected{
        0x60fd46de, 0xf830, 0x4894, {0xa6, 0x28, 0x6f, 0xa8, 0x1b, 0xc0, 0x19, 0x0d}};
    require(clsid == expected && iid == __uuidof(IDropTarget) && !outer &&
                context == CLSCTX_INPROC_SERVER,
            "legacy printing must activate the documented Windows Photo Printing Wizard");
    *target = nullptr;
    if (outcome == Outcome::ActivateFailure)
        return E_ACCESSDENIED;
    if (outcome != Outcome::MissingTarget)
        *target = winrt::make<WizardTarget>().detach();
    return S_OK;
}

void showAndHideMockWizard() {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"NativeHWNDHost";
    require(RegisterClassW(&windowClass) != 0, "the headless wizard host must register");
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 100,
                                        100, nullptr, nullptr, windowClass.hInstance, nullptr);
    require(window != nullptr, "the headless wizard host must initialize");
    NotifyWinEvent(EVENT_OBJECT_SHOW, window, OBJID_WINDOW, CHILDID_SELF);
    flush();
    NotifyWinEvent(EVENT_OBJECT_HIDE, window, OBJID_WINDOW, CHILDID_SELF);
    flush();
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
}

void destroyedOwnerDuringPreparation() {
    activations = 0;
    auto* owner = new QWidget;
    int completions = 0;
    QImage image(8, 4, QImage::Format_RGB32);
    image.fill(Qt::white);
    screenshotLegacyWindowsPrintBackend({&activateWizard})(
        owner, image, [&](Service::Result result) {
            require(result.status == Service::Status::Cancelled,
                    "destroying the owner before PNG preparation finishes must cancel printing");
            ++completions;
        });
    require(activations == 0 && completions == 0,
            "PNG preparation must defer all native COM and wizard interaction");
    delete owner;
    processUntil([&] { return completions == 1; });
    require(activations == 0, "cancelled preparation must never activate the native wizard");
}

void photoWizardOutcomesAndSnapshotLifetime() {
    const auto backend = screenshotLegacyWindowsPrintBackend({&activateWizard});
    for (auto next : {Outcome::ActivateFailure, Outcome::MissingTarget, Outcome::EnterFailure,
                      Outcome::RejectedImage, Outcome::DropFailure, Outcome::RetainedDropFailure,
                      Outcome::Cancel, Outcome::RetainedCancel, Outcome::ImmediateRelease,
                      Outcome::DeferredRelease, Outcome::WorkerRelease, Outcome::WindowHide,
                      Outcome::DestroyOwner}) {
        outcome = next;
        activations = enters = drops = leaves = 0;
        snapshotPath.clear();
        expectedOwner = new QWidget;
        expectedOwner->setWindowFlag(Qt::WindowStaysOnTopHint);
        const auto flags = expectedOwner->windowFlags();
        const auto handle = expectedOwner->winId();
        QImage image(8, 4, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        image.setPixelColor(1, 0, QColor(255, 0, 0, 128));
        image.setDevicePixelRatio(2);
        expectedImage = Service::opaqueImage(image);
        QObject receiver;
        Service service(backend);
        int completions = 0;
        Service::Result final;
        require(service.printImage(&receiver, expectedOwner, image,
                                   [&](Service::Result result) {
                                       require(QThread::currentThread() == qApp->thread(),
                                               "wizard completion must return to the GUI thread");
                                       final = std::move(result);
                                       ++completions;
                                   }),
                "each legacy image request must start");
        image.fill(Qt::black);
        processUntil([&] { return activations == 1 && (retainedData || !service.busy()); });
        if (retainedData) {
            require(QFileInfo::exists(snapshotPath),
                    "the PNG must remain available after the asynchronous handoff");
            if (next != Outcome::DestroyOwner) {
                require(completions == 0 && service.busy(),
                        "the request must remain pending while the wizard retains the snapshot");
                require(!service.printImage(&receiver, expectedOwner, expectedImage, [](auto) {}),
                        "an active wizard must prevent duplicate print dialogs");
            }
            if (next == Outcome::WindowHide) {
                showAndHideMockWizard();
                require(completions == 1 && !service.busy() &&
                            final.status == Service::Status::HandedOff &&
                            QFileInfo::exists(snapshotPath),
                        "closing the dialog must release interaction while retained Shell data "
                        "keeps the snapshot available for native printing");
            }
            if (next == Outcome::WorkerRelease) {
                std::thread worker([data = std::move(retainedData)]() mutable { data = nullptr; });
                worker.join();
            } else {
                retainedData = nullptr;
            }
            flush();
        }
        const bool cancelled = next == Outcome::Cancel || next == Outcome::RetainedCancel ||
                               next == Outcome::DestroyOwner;
        const bool handedOff = next == Outcome::ImmediateRelease ||
                               next == Outcome::DeferredRelease || next == Outcome::WorkerRelease ||
                               next == Outcome::WindowHide;
        if (completions != 1 || service.busy() || activations != 1)
            std::cerr << "outcome=" << static_cast<int>(next) << " completions=" << completions
                      << " busy=" << service.busy() << " activations=" << activations
                      << " enters=" << enters << " drops=" << drops
                      << " error=" << final.error.toStdString() << '\n';
        require(completions == 1 && !service.busy() && activations == 1,
                "every wizard outcome must finish once and release the request");
        require(final.status == (cancelled   ? Service::Status::Cancelled
                                 : handedOff ? Service::Status::HandedOff
                                             : Service::Status::Failed) &&
                    final.error.isEmpty() == (cancelled || handedOff),
                "handoff must not be misreported as submission or suppress activation failures");
        require(
            enters ==
                    (next == Outcome::ActivateFailure || next == Outcome::MissingTarget ? 0 : 1) &&
                drops == (next == Outcome::ActivateFailure || next == Outcome::MissingTarget ||
                                  next == Outcome::EnterFailure || next == Outcome::RejectedImage
                              ? 0
                              : 1) &&
                leaves == (next == Outcome::RejectedImage ? 1 : 0),
            "a rejected or failed initialization must not reach Drop or open a second dialog");
        if (!snapshotPath.isEmpty())
            require(!QFileInfo::exists(snapshotPath) &&
                        !QDir(QFileInfo(snapshotPath).absolutePath()).exists(),
                    "closing the wizard must remove its temporary PNG and directory");
        if (expectedOwner) {
            require(expectedOwner->windowFlags() == flags &&
                        expectedOwner->internalWinId() == handle,
                    "photo printing must preserve topmost flags and the native owner");
            delete expectedOwner;
            expectedOwner = nullptr;
        }
        flush();
        require(completions == 1, "late data release must not complete a request twice");
    }
}

QString retainSnapshotUntilShutdown() {
    outcome = Outcome::DeferredRelease;
    expectedImage = QImage(8, 4, QImage::Format_RGB32);
    expectedImage.fill(Qt::white);
    expectedImage.setColorSpace(QColorSpace(QColorSpace::SRgb));
    QWidget owner;
    int completions = 0;
    activations = 0;
    screenshotLegacyWindowsPrintBackend({&activateWizard})(
        &owner, expectedImage, [&](Service::Result result) {
            require(result.status == Service::Status::HandedOff,
                    "closing the shutdown fixture must release its print request");
            ++completions;
        });
    processUntil([&] { return bool(retainedData); });
    showAndHideMockWizard();
    require(completions == 1 && retainedData && QFileInfo::exists(snapshotPath),
            "the shutdown fixture must retain native snapshot references after closure");
    return snapshotPath;
}

HWND nativeOwner = nullptr;
HWND nativeWizard = nullptr;
bool nativeDialogSeen = false;
bool destroyNativeOwner = false;
QWidget* nativeOwnerWidget = nullptr;

BOOL CALLBACK cancelNativeWizard(HWND window, LPARAM) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !IsWindowVisible(window))
        return TRUE;
    wchar_t className[32]{};
    GetClassNameW(window, className, 32);
    if (QString::fromWCharArray(className) != QStringLiteral("NativeHWNDHost"))
        return TRUE;
    if (!(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST))
        return TRUE;
    require((GetWindowLongPtrW(nativeOwner, GWL_EXSTYLE) & WS_EX_TOPMOST),
            "the real wizard must appear above its topmost owner without lowering it");
    nativeDialogSeen = true;
    nativeWizard = window;
    if (destroyNativeOwner) {
        delete nativeOwnerWidget;
        nativeOwnerWidget = nullptr;
    } else {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return FALSE;
}

// Opt-in desktop probe: opens the real wizard and closes it without pressing Print.
void nativePhotoWizardCancellation() {
    for (bool destroyOwner : {false, true}) {
        nativeOwnerWidget = new QWidget;
        nativeOwnerWidget->setWindowFlag(Qt::WindowStaysOnTopHint);
        nativeOwnerWidget->resize(200, 100);
        nativeOwnerWidget->show();
        nativeOwner = reinterpret_cast<HWND>(nativeOwnerWidget->winId());
        nativeDialogSeen = false;
        destroyNativeOwner = destroyOwner;
        QImage image(800, 400, QImage::Format_RGB32);
        image.fill(Qt::white);
        QEventLoop loop;
        int completions = 0;
        screenshotLegacyWindowsPrintBackend()(
            nativeOwnerWidget, image, [&](Service::Result result) {
                if (result.status !=
                    (destroyOwner ? Service::Status::Cancelled : Service::Status::HandedOff))
                    std::cerr << "native status=" << static_cast<int>(result.status)
                              << " error=" << result.error.toStdString() << '\n';
                require(result.status == (destroyOwner ? Service::Status::Cancelled
                                                       : Service::Status::HandedOff),
                        "the real wizard must close without claiming a printer job was submitted");
                ++completions;
                loop.quit();
            });
        QTimer cancel;
        QObject::connect(&cancel, &QTimer::timeout, &loop, [&] {
            if (!nativeDialogSeen)
                EnumWindows(&cancelNativeWizard, 0);
        });
        cancel.start(25);
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        if (!nativeDialogSeen || completions != 1)
            std::cerr << "native destroyOwner=" << destroyOwner << " seen=" << nativeDialogSeen
                      << " completions=" << completions << " window=" << IsWindow(nativeWizard)
                      << " visible=" << IsWindowVisible(nativeWizard) << '\n';
        require(nativeDialogSeen && completions == 1,
                "the real wizard must become visible and complete once after cancellation");
        delete nativeOwnerWidget;
        nativeOwnerWidget = nullptr;
    }
    std::cout << "Windows Photo Printing Wizard opened and cancelled; owner destruction verified\n";
}
} // namespace

int main(int argc, char** argv) {
    QString shutdownSnapshot;
    {
        QApplication app(argc, argv);
        app.setQuitOnLastWindowClosed(false);
        if (app.arguments().contains(QStringLiteral("--native-cancel-only"))) {
            nativePhotoWizardCancellation();
            return 0;
        }
        photoWizardOutcomesAndSnapshotLifetime();
        destroyedOwnerDuringPreparation();
        shutdownSnapshot = retainSnapshotUntilShutdown();
    }
    require(!QFileInfo::exists(shutdownSnapshot) &&
                !QDir(QFileInfo(shutdownSnapshot).absolutePath()).exists(),
            "application shutdown must clean snapshots even when Windows retains COM references");
    retainedData = nullptr;
    return 0;
}
