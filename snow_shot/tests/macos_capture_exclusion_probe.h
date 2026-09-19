#pragma once
#include <QWidget>
#include <functional>

// Retains the native window so tests can verify restoration after QWidget destruction.
std::function<bool(bool)> macosCaptureSharingProbe(QWidget* widget);
