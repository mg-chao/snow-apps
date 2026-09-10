#pragma once

#include "snow_shot/update/updatecontract.h"
#include <functional>

namespace snow_shot::update {
struct TransactionHooks {
    std::function<bool()> probe;
    std::function<void(const QString&)> checkpoint;
};

QString installationRoot(const QString& executableDirectory);
QJsonObject installationRecord(const QString& root);
void validateInstallationRoot(const QString& root);
void pruneUpdateWork(const QString& root);
void validateTargetPath(const QString& root, const QString& relative);
bool transactionPending(const QString& root);
void recoverTransaction(const QString& root);
void applyTransaction(const QString& root, const QString& archive, const UpdateRelease& release,
                      const TransactionHooks& hooks = {});
void uninstallOwnedFiles(const QString& root);
void auditRelease(const QString& directory, const UpdateRelease& release);
} // namespace snow_shot::update
