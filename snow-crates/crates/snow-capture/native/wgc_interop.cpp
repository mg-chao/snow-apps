#include "wgc_interop.h"

#include <new>
#include <winrt/base.h>

namespace {
template <typename Create> HRESULT createItem(void** result, Create create) noexcept {
    if (!result)
        return E_POINTER;
    *result = nullptr;
    try {
        // Windows 10's custom IGraphicsCaptureItemInterop implementation can
        // throw instead of returning an HRESULT. Catch entirely in C++: unwinding
        // through a Rust COM call or callback is not a valid exception boundary.
        winrt::com_ptr<IUnknown> item;
        const HRESULT status = create(item.put_void());
        if (SUCCEEDED(status)) {
            if (!item)
                return E_POINTER;
            *result = item.detach();
        }
        return status;
    } catch (const winrt::hresult_error& error) {
        return error.to_abi();
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
} // namespace

extern "C" HRESULT snow_capture_wgc_create_for_monitor(IGraphicsCaptureItemInterop* factory,
                                                       HMONITOR monitor, REFIID iid,
                                                       void** result) noexcept {
    return createItem(result, [&](void** item) {
        return factory ? factory->CreateForMonitor(monitor, iid, item) : E_POINTER;
    });
}

extern "C" HRESULT snow_capture_wgc_create_for_window(IGraphicsCaptureItemInterop* factory,
                                                      HWND window, REFIID iid,
                                                      void** result) noexcept {
    return createItem(result, [&](void** item) {
        return factory ? factory->CreateForWindow(window, iid, item) : E_POINTER;
    });
}
