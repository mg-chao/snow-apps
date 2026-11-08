#pragma once

#include <Windows.h>
#include <windows.graphics.capture.interop.h>

extern "C" HRESULT snow_capture_wgc_create_for_monitor(IGraphicsCaptureItemInterop* factory,
                                                       HMONITOR monitor, REFIID iid,
                                                       void** result) noexcept;
extern "C" HRESULT snow_capture_wgc_create_for_window(IGraphicsCaptureItemInterop* factory,
                                                      HWND window, REFIID iid,
                                                      void** result) noexcept;
