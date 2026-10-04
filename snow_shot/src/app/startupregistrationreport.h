#ifndef SNOW_SHOT_APP_STARTUPREGISTRATIONREPORT_H
#define SNOW_SHOT_APP_STARTUPREGISTRATIONREPORT_H

#include <QDebug>
#include <QString>

namespace snow_shot::app {

// Automatic startup reconciliation must not bring the application to the foreground.
inline void reportStartupRegistrationFailure(const QString& error) {
    qWarning().noquote() << error;
}

} // namespace snow_shot::app

#endif // SNOW_SHOT_APP_STARTUPREGISTRATIONREPORT_H
