#pragma once

// Load Qt with the real host processor before selecting the application's ARM64
// manifest branch. This fixture checks asset identities without emulating ARM.
#include <QtCore/QtCore>
#include <QtNetwork/QtNetwork>

#if !defined(Q_OS_WIN) || !defined(Q_PROCESSOR_X86_64)
#error This OCR manifest fixture requires an x64 Windows host.
#endif

#define Q_PROCESSOR_ARM_64
