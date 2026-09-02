#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTDISPLAYSESSION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTDISPLAYSESSION_H

#include "snow_shot/presentation/screenshottypes.h"

#include <QVector>

#include <utility>

class ScreenshotDisplaySession final {
  private:
    struct ScreenshotDisplaySlot {
        CapturedDisplayModel display;
        ScreenshotDisplayPresentationState presentation;
    };

  public:
    void clear() {
        m_slots.clear();
    }

    void reserve(qsizetype size) {
        m_slots.reserve(size);
    }

    void appendDisplay(CapturedDisplayModel display = {},
                       ScreenshotOverlayWindow* overlay = nullptr) {
        ScreenshotDisplaySlot slot;
        slot.display = std::move(display);
        slot.presentation.overlay = overlay;
        m_slots.push_back(slot);
    }

    void removeDisplayAt(qsizetype index) {
        if (index < 0 || index >= m_slots.size()) {
            return;
        }
        m_slots.removeAt(index);
    }

    template <typename RemoveOverlay>
    void removeInactiveDisplaysBeyond(qsizetype targetSize, RemoveOverlay removeOverlay) {
        for (qsizetype index = m_slots.size() - 1; m_slots.size() > targetSize && index >= 0;
             --index) {
            if (m_slots[index].display.active) {
                continue;
            }

            removeOverlay(takeOverlayAt(index));
            removeDisplayAt(index);
        }
    }

    [[nodiscard]] qsizetype size() const {
        return m_slots.size();
    }

    [[nodiscard]] bool isEmpty() const {
        return m_slots.isEmpty();
    }

    [[nodiscard]] bool hasActiveDisplays() const {
        for (const ScreenshotDisplaySlot& slot : m_slots) {
            if (slot.display.active) {
                return true;
            }
        }
        return false;
    }

    template <typename Visitor> void forEachDisplay(Visitor visit) const {
        for (qsizetype index = 0; index < m_slots.size(); ++index) {
            visit(index, m_slots[index].display);
        }
    }

    template <typename Visitor> void forEachMutableDisplay(Visitor visit) {
        for (qsizetype index = 0; index < m_slots.size(); ++index) {
            visit(index, m_slots[index].display);
        }
    }

    template <typename Visitor> void forEachActiveDisplay(Visitor visit) const {
        for (qsizetype index = 0; index < m_slots.size(); ++index) {
            const CapturedDisplayModel& display = m_slots[index].display;
            if (display.active) {
                visit(index, display);
            }
        }
    }

    template <typename Visitor> void forEachMutableActiveDisplay(Visitor visit) {
        for (qsizetype index = 0; index < m_slots.size(); ++index) {
            CapturedDisplayModel& display = m_slots[index].display;
            if (display.active) {
                visit(index, display);
            }
        }
    }

    template <typename Visitor> void forEachDisplayWithOverlay(Visitor visit) const {
        for (qsizetype index = 0; index < m_slots.size(); ++index) {
            const ScreenshotDisplaySlot& slot = m_slots[index];
            visit(index, slot.display, slot.presentation.overlay);
        }
    }

    template <typename Visitor> void forEachMutableDisplayWithOverlay(Visitor visit) {
        for (qsizetype index = 0; index < m_slots.size(); ++index) {
            ScreenshotDisplaySlot& slot = m_slots[index];
            visit(index, slot.display, slot.presentation.overlay);
        }
    }

    template <typename Visitor> void forEachOverlay(Visitor visit) const {
        for (qsizetype index = 0; index < m_slots.size(); ++index) {
            ScreenshotOverlayWindow* overlay = m_slots[index].presentation.overlay;
            if (overlay != nullptr) {
                visit(index, overlay);
            }
        }
    }

    template <typename Visitor> void forEachActiveOverlay(Visitor visit) const {
        for (qsizetype index = 0; index < m_slots.size(); ++index) {
            const ScreenshotDisplaySlot& slot = m_slots[index];
            const CapturedDisplayModel& display = slot.display;
            ScreenshotOverlayWindow* overlay = slot.presentation.overlay;
            if (display.active && overlay != nullptr) {
                visit(index, display, overlay);
            }
        }
    }

    [[nodiscard]] CapturedDisplayModel& displayAt(qsizetype index) {
        return m_slots[index].display;
    }

    [[nodiscard]] const CapturedDisplayModel& displayAt(qsizetype index) const {
        return m_slots[index].display;
    }

    [[nodiscard]] ScreenshotOverlayWindow* overlayAt(qsizetype index) const {
        if (index < 0 || index >= m_slots.size()) {
            return nullptr;
        }
        return m_slots[index].presentation.overlay;
    }

    [[nodiscard]] ScreenshotOverlayWindow* takeOverlayAt(qsizetype index) {
        if (index < 0 || index >= m_slots.size()) {
            return nullptr;
        }
        ScreenshotOverlayWindow* overlay = m_slots[index].presentation.overlay;
        m_slots[index].presentation.overlay = nullptr;
        return overlay;
    }

    template <typename TakeOverlay> void takeEachOverlay(TakeOverlay takeOverlay) {
        for (qsizetype index = 0; index < m_slots.size(); ++index) {
            takeOverlay(index, takeOverlayAt(index));
        }
    }

    template <typename EnsureOverlay>
    [[nodiscard]] ScreenshotOverlayWindow* ensureOverlayAt(qsizetype index,
                                                           EnsureOverlay ensureOverlay) {
        if (index < 0 || index >= m_slots.size()) {
            return nullptr;
        }
        ScreenshotOverlayWindow* overlay = ensureOverlay(m_slots[index].presentation.overlay);
        m_slots[index].presentation.overlay = overlay;
        return overlay;
    }

    [[nodiscard]] ScreenshotOverlayWindow*
    overlayForDisplay(const CapturedDisplayModel* display) const {
        if (display == nullptr || m_slots.isEmpty()) {
            return nullptr;
        }

        for (const ScreenshotDisplaySlot& slot : m_slots) {
            if (&slot.display == display) {
                return slot.presentation.overlay;
            }
        }
        return nullptr;
    }

  private:
    QVector<ScreenshotDisplaySlot> m_slots;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTDISPLAYSESSION_H
