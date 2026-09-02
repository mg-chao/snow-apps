#ifndef SNOW_SHOT_STORAGE_CONFIGURATIONSTORE_H
#define SNOW_SHOT_STORAGE_CONFIGURATIONSTORE_H

#include "snow_shot/storage/storageresult.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

namespace snow_shot::storage {
enum class ConfigurationCompatibility {
    Current,
    RecoveredDefaults,
    FutureVersion,
    Unavailable,
};

class ConfigurationStore final : public QObject {
    Q_OBJECT

  public:
    ConfigurationStore(QString configurationFile, bool readAvailable, bool writeAvailable,
                       int debounceMilliseconds = 1000, QObject* parent = nullptr);

    [[nodiscard]] QJsonValue value(const QString& key) const;
    [[nodiscard]] QMap<QString, QJsonValue> snapshot() const;
    [[nodiscard]] bool isDirty() const;
    [[nodiscard]] bool isWritable() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] ConfigurationCompatibility compatibility() const;

    bool setValue(const QString& key, const QJsonValue& value);
    bool setValues(const QMap<QString, QJsonValue>& values);
    [[nodiscard]] StorageResult flushNow();

  signals:
    void valueChanged(const QString& key, const QJsonValue& value);
    void mutationRejected(const QString& key, const QString& error);
    void errorChanged(const QString& error);

  private:
    void load();
    void scheduleFlush();
    void setLastError(const QString& error);
    void rejectMutation(const QString& key, const QString& error);

    QString m_configurationFile;
    bool m_readAvailable = false;
    bool m_writeAvailable = false;
    mutable QMutex m_mutex;
    QMutex m_ioMutex;
    QMap<QString, QJsonValue> m_values;
    QJsonObject m_document;
    QString m_lastError;
    ConfigurationCompatibility m_compatibility = ConfigurationCompatibility::Current;
    quint64 m_revision = 0;
    bool m_dirty = false;
    QTimer m_flushTimer;
};
} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_CONFIGURATIONSTORE_H
