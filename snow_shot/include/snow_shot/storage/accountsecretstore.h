#ifndef SNOW_SHOT_STORAGE_ACCOUNTSECRETSTORE_H
#define SNOW_SHOT_STORAGE_ACCOUNTSECRETSTORE_H

#include <QByteArray>
#include <QString>

namespace snow_shot::storage {
// Secrets are kept outside ConfigurationStore and its export/MCP interfaces.
class AccountSecretStore {
  public:
    explicit AccountSecretStore(QString identity, QString directory = {});
    [[nodiscard]] QByteArray read() const;
    [[nodiscard]] bool write(const QByteArray& value) const;
    [[nodiscard]] bool remove() const;

  private:
    QString m_identity;
    QString m_directory;
};
} // namespace snow_shot::storage

#endif
