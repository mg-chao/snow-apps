#pragma once

#include "snow_shot/update/updatecontract.h"

namespace snow_shot::update {

// On-disk installation state queries shared by the application and the
// helper process; the apply/recover machinery itself lives in the helper.

struct InstallationIdentity {
    QString variant;
    QString version;
};

QString installationRoot(const QString& executableDirectory);
InstallationIdentity installationRecord(const QString& root);
bool transactionPending(const QString& root);

} // namespace snow_shot::update
