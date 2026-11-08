#include "wgc_interop.h"

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <winrt/base.h>

namespace {
class TestFactory;

class TestItem final : public IUnknown {
  public:
    explicit TestItem(TestFactory* factory);
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override;
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_references;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --m_references;
        if (!references)
            delete this;
        return references;
    }

  private:
    ~TestItem();
    std::atomic<ULONG> m_references{1};
    TestFactory* m_factory;
};

class TestFactory final : public IGraphicsCaptureItemInterop {
  public:
    explicit TestFactory(int behavior) : m_behavior(behavior) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        *result = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IGraphicsCaptureItemInterop))
            return E_NOINTERFACE;
        *result = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_references;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = --m_references;
        if (!references)
            delete this;
        return references;
    }
    HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID iid,
                                               void** result) override {
        return create(reinterpret_cast<std::uintptr_t>(monitor), iid, result);
    }
    HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID iid, void** result) override {
        return create(reinterpret_cast<std::uintptr_t>(window), iid, result);
    }
    std::atomic<unsigned> liveItems{0};

  private:
    HRESULT create(std::uintptr_t handle, REFIID iid, void** result) {
        *result = nullptr;
        if (handle != 0x10003 ||
            iid != __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem))
            return E_INVALIDARG;
        switch (m_behavior) {
        case 0:
            *result = new TestItem(this);
            return S_OK;
        case 1:
            return E_ACCESSDENIED;
        case 2:
            throw winrt::hresult_error(E_FAIL, L"synthetic WGC server failure");
        case 3:
            throw winrt::hresult_error(static_cast<HRESULT>(0x887A0005),
                                       L"synthetic WGC device loss");
        case 4:
            throw std::runtime_error("synthetic native capture failure");
        case 5:
            throw 42;
        case 6:
            throw std::bad_alloc();
        case 7:
            return S_OK;
        case 8:
            *result = new TestItem(this);
            return E_FAIL;
        case 9:
            *result = new TestItem(this);
            throw winrt::hresult_error(E_FAIL, L"synthetic partial initialization failure");
        default:
            return E_INVALIDARG;
        }
    }
    std::atomic<ULONG> m_references{1};
    int m_behavior;
};

TestItem::TestItem(TestFactory* factory) : m_factory(factory) {
    m_factory->AddRef();
    ++m_factory->liveItems;
}
TestItem::~TestItem() {
    --m_factory->liveItems;
    m_factory->Release();
}
HRESULT TestItem::QueryInterface(REFIID iid, void** result) {
    *result = nullptr;
    if (iid != __uuidof(IUnknown))
        return E_NOINTERFACE;
    *result = this;
    AddRef();
    return S_OK;
}
} // namespace

extern "C" IGraphicsCaptureItemInterop* snow_capture_wgc_test_factory(int behavior) noexcept {
    return new (std::nothrow) TestFactory(behavior);
}

extern "C" unsigned
snow_capture_wgc_test_live_items(IGraphicsCaptureItemInterop* factory) noexcept {
    return static_cast<TestFactory*>(factory)->liveItems;
}
