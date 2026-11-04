// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtGlobal>

#include <algorithm>
#include <list>
#include <memory>

// GUI-thread retention only. Commands own immutable results independently, so eviction never
// cancels work or invalidates a consumer. Pending jobs must not enter this cache: their source
// donors and scratch storage can be much larger than the eventual output.
class ScreenshotExportCache final {
  public:
    explicit ScreenshotExportCache(qint64 maximumBytes) : m_maximumBytes(maximumBytes) {}

    static ScreenshotExportCache& pinnedPixels() {
        static ScreenshotExportCache cache(64 * 1024 * 1024);
        return cache;
    }

    static ScreenshotExportCache& encodedBytes() {
        static ScreenshotExportCache cache(64 * 1024 * 1024);
        return cache;
    }

    static ScreenshotExportCache& encodedFiles() {
        static ScreenshotExportCache cache(256 * 1024 * 1024);
        return cache;
    }

    void retain(std::shared_ptr<void> value, qint64 bytes) {
        if (!value)
            return;
        remove(value.get());
        if (bytes <= 0 || bytes > m_maximumBytes)
            return;
        while (!m_entries.empty() && m_bytes > m_maximumBytes - bytes) {
            m_bytes -= m_entries.front().bytes;
            m_entries.pop_front();
        }
        m_entries.push_back({std::move(value), bytes});
        m_bytes += bytes;
    }

    void touch(const void* value) {
        const auto found = find(value);
        if (found != m_entries.end())
            m_entries.splice(m_entries.end(), m_entries, found);
    }

    void remove(const void* value) {
        const auto found = find(value);
        if (found != m_entries.end()) {
            m_bytes -= found->bytes;
            m_entries.erase(found);
        }
    }

    void clear() {
        m_entries.clear();
        m_bytes = 0;
    }

    [[nodiscard]] qint64 retainedBytes() const {
        return m_bytes;
    }

  private:
    struct Entry {
        std::shared_ptr<void> value;
        qint64 bytes;
    };
    std::list<Entry>::iterator find(const void* value) {
        return std::find_if(m_entries.begin(), m_entries.end(),
                            [value](const Entry& entry) { return entry.value.get() == value; });
    }
    const qint64 m_maximumBytes;
    qint64 m_bytes = 0;
    std::list<Entry> m_entries;
};
