#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_SCREENRECORDINGMODAL_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_SCREENRECORDINGMODAL_H

#include "widgets/modal.h"

namespace snow_shot::presentation {

// The recording area owns placement and native stacking while it exists.
// A render job can outlive that area and then use a detached application modal.
inline void configureScreenRecordingModal(adqt::widgets::AdModal& modal, QWidget* owner) {
    modal.setWindowModeDetached(owner == nullptr);
    modal.setOwnerWindow(owner);
    modal.setMode(adqt::widgets::AdModal::Mode::Window);
    modal.setWindowModality(Qt::ApplicationModal);
    modal.setWindowAlwaysOnTop(owner == nullptr);
    modal.setCentered(true);
    modal.setMaskVisible(false);
    modal.setCloseOnMaskClick(false);
}

} // namespace snow_shot::presentation

#endif
