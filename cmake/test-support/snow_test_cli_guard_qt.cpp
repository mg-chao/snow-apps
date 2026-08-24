// Qt-aware CLI dialog guard for test executables.
//
// Qt delivers fatal messages (Q_ASSERT, Q_ASSERT_X, qFatal) to the installed
// message handler first and only afterwards runs its own fatal handling,
// which on MSVC debug builds raises the modal "Debug Error! ... Press Retry
// to debug the application" dialog. Static-runtime Qt kits carry their own
// CRT instance, so that dialog cannot be reconfigured from the test process;
// printing the message here and terminating immediately keeps every fatal
// report a plain stderr diagnostic plus a failure exit code, in every Qt
// build flavor.

#include "snow_test_cli_guard.h"

#include <QMessageLogContext>
#include <QString>
#include <QtGlobal>

#include <cstdio>
#include <cstdlib>

namespace {

const char* messageTypeName(QtMsgType type) {
    switch (type) {
    case QtDebugMsg:
        return "debug";
    case QtInfoMsg:
        return "info";
    case QtWarningMsg:
        return "warning";
    case QtCriticalMsg:
        return "critical";
    case QtFatalMsg:
        return "fatal";
    }
    return "unknown";
}

void cliFriendlyMessageHandler(QtMsgType type, const QMessageLogContext& context,
                               const QString& message) {
    if (context.file != nullptr && context.file[0] != '\0') {
        std::fprintf(stderr, "[qt.%s] %s (%s:%d, %s)\n", messageTypeName(type), qPrintable(message),
                     context.file, context.line,
                     context.category != nullptr ? context.category : "default");
    } else {
        std::fprintf(stderr, "[qt.%s] %s\n", messageTypeName(type), qPrintable(message));
    }
    std::fflush(stderr);
    if (type == QtFatalMsg) {
        std::fputs("[qt.fatal] terminating the test process immediately\n", stderr);
        std::fflush(stderr);
        // Skip Qt's fatal handling (debugger dialogs) and static destructors,
        // which can deadlock or trip secondary asserts after a fatal report.
        std::_Exit(EXIT_FAILURE);
    }
}

struct QtCliGuardInstaller {
    QtCliGuardInstaller() {
        snow_test_cli::install_native_guards();
        qInstallMessageHandler(cliFriendlyMessageHandler);
    }
};

const QtCliGuardInstaller qtCliGuardInstaller = {};

} // namespace
