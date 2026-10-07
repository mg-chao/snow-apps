#pragma once

#include "snow_draw_engine.h"

class ScopedRuntimeHandle final {
  public:
    ScopedRuntimeHandle() = default;
    explicit ScopedRuntimeHandle(SnowRuntime handle) : m_handle(handle) {}
    ~ScopedRuntimeHandle() {
        reset();
    }

    ScopedRuntimeHandle(const ScopedRuntimeHandle&) = delete;
    ScopedRuntimeHandle& operator=(const ScopedRuntimeHandle&) = delete;

    ScopedRuntimeHandle(ScopedRuntimeHandle&& other) noexcept : m_handle(other.release()) {}

    ScopedRuntimeHandle& operator=(ScopedRuntimeHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    SnowRuntime get() const {
        return m_handle;
    }

    SnowRuntime* outParam() {
        reset();
        return &m_handle;
    }

    SnowRuntime release() {
        SnowRuntime handle = m_handle;
        m_handle = nullptr;
        return handle;
    }

    void reset(SnowRuntime handle = nullptr) {
        if (m_handle != nullptr) {
            snow_runtime_destroy(m_handle);
        }
        m_handle = handle;
    }

  private:
    SnowRuntime m_handle = nullptr;
};

class ScopedChangedViewportList final {
  public:
    ScopedChangedViewportList() = default;
    explicit ScopedChangedViewportList(SnowChangedViewportList handle) : m_handle(handle) {}
    ~ScopedChangedViewportList() {
        reset();
    }

    ScopedChangedViewportList(const ScopedChangedViewportList&) = delete;
    ScopedChangedViewportList& operator=(const ScopedChangedViewportList&) = delete;

    ScopedChangedViewportList(ScopedChangedViewportList&& other) noexcept
        : m_handle(other.release()) {}

    ScopedChangedViewportList& operator=(ScopedChangedViewportList&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    SnowChangedViewportList get() const {
        return m_handle;
    }

    SnowChangedViewportList* outParam() {
        reset();
        return &m_handle;
    }

    SnowChangedViewportList release() {
        SnowChangedViewportList handle = m_handle;
        m_handle = nullptr;
        return handle;
    }

    void reset(SnowChangedViewportList handle = nullptr) {
        if (m_handle != nullptr) {
            snow_changed_viewports_destroy(m_handle);
        }
        m_handle = handle;
    }

  private:
    SnowChangedViewportList m_handle = nullptr;
};

// Balances a presentation scope even when a caller exits after a failed command.
class ScopedPresentationUpdate final {
  public:
    explicit ScopedPresentationUpdate(SnowRuntime runtime)
        : m_runtime(snow_runtime_begin_presentation_update(runtime) == SNOW_OK ? runtime
                                                                               : nullptr) {}
    ~ScopedPresentationUpdate() {
        ScopedChangedViewportList ignored;
        finish(ignored);
    }

    ScopedPresentationUpdate(const ScopedPresentationUpdate&) = delete;
    ScopedPresentationUpdate& operator=(const ScopedPresentationUpdate&) = delete;

    bool isActive() const {
        return m_runtime != nullptr;
    }

    bool finish(ScopedChangedViewportList& changedViewports) {
        if (m_runtime == nullptr) {
            return false;
        }
        SnowRuntime runtime = m_runtime;
        m_runtime = nullptr;
        return snow_runtime_end_presentation_update_ex(runtime, changedViewports.outParam()) ==
               SNOW_OK;
    }

  private:
    SnowRuntime m_runtime = nullptr;
};

class ScopedPatchHandle final {
  public:
    ScopedPatchHandle() = default;
    explicit ScopedPatchHandle(SnowPatchHandle handle) : m_handle(handle) {}
    ~ScopedPatchHandle() {
        reset();
    }

    ScopedPatchHandle(const ScopedPatchHandle&) = delete;
    ScopedPatchHandle& operator=(const ScopedPatchHandle&) = delete;

    ScopedPatchHandle(ScopedPatchHandle&& other) noexcept : m_handle(other.release()) {}

    ScopedPatchHandle& operator=(ScopedPatchHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    SnowPatchHandle get() const {
        return m_handle;
    }

    SnowPatchHandle* outParam() {
        reset();
        return &m_handle;
    }

    SnowPatchHandle release() {
        SnowPatchHandle handle = m_handle;
        m_handle = nullptr;
        return handle;
    }

    void reset(SnowPatchHandle handle = nullptr) {
        if (m_handle != nullptr) {
            snow_patch_destroy(m_handle);
        }
        m_handle = handle;
    }

  private:
    SnowPatchHandle m_handle = nullptr;
};
