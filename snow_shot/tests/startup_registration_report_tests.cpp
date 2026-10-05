#include "../src/app/startupregistrationreport.h"

#include <QApplication>
#include <QScopeGuard>
#include <QStringList>
#include <QWidget>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

QStringList startupWarnings;
void captureStartupWarning(QtMsgType type, const QMessageLogContext&, const QString& message) {
    if (type == QtWarningMsg)
        startupWarnings.append(message);
}

void startupFailuresStayInLogs() {
    QWidget mainWindow;
    const auto initialWidgets = QApplication::topLevelWidgets();
    const auto previousHandler = qInstallMessageHandler(captureStartupWarning);
    const auto restoreHandler =
        qScopeGuard([previousHandler] { qInstallMessageHandler(previousHandler); });
    for (const QString& error :
         {QStringLiteral("Elevated auto-start needs repair. Turn Launch as administrator off and "
                         "on again."),
          QStringLiteral("An interrupted startup change needs administrator authorization. "
                         "Reapply your startup setting.")}) {
        startupWarnings.clear();
        snow_shot::app::reportStartupRegistrationFailure(error);
        // Drain deferred reporting too: the original foreground notification was queued.
        QCoreApplication::processEvents();
        require(startupWarnings == QStringList{error},
                "startup recovery errors must be logged exactly once without quotes");
        require(QApplication::topLevelWidgets() == initialWidgets,
                "startup recovery errors must not create a main window or notification");
        require(!mainWindow.isVisible(),
                "startup recovery errors must leave an existing main window hidden");
    }
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    try {
        startupFailuresStayInLogs();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
