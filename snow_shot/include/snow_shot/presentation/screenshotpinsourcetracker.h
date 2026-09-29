#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINSOURCETRACKER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINSOURCETRACKER_H

#include "snow_shot/storage/pinnedsourceidentity.h"
#include <QClipboard>
#include <QObject>
#include <QUuid>

// GUI-thread clipboard identities require no access to the clipboard payload.
class ScreenshotPinSourceTracker final : public QObject {
  public:
    explicit ScreenshotPinSourceTracker(QClipboard* clipboard) {
        if (clipboard) {
            connect(clipboard, &QClipboard::dataChanged, this, [this] { ++m_generation; });
        }
    }

    [[nodiscard]] snow_shot::storage::PinnedSourceIdentity clipboardIdentity() const {
        return {QStringLiteral("clipboard:%1:%2").arg(m_session).arg(m_generation)};
    }

  private:
    const QString m_session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    quint64 m_generation = 0;
};

#endif
