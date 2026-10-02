#ifndef SNOW_SHOT_STORAGE_FLOATINGTOOLBARSETTINGS_H
#define SNOW_SHOT_STORAGE_FLOATINGTOOLBARSETTINGS_H

#include "snow_shot/presentation/editionfeatures.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QJsonObject>

namespace snow_shot::storage {

inline ScreenshotToolbarLayout defaultFloatingToolbarLayout() {
    ScreenshotToolbarLayout layout{
        {{QStringLiteral("screenshot")},
         {QStringLiteral("pin-to-screen")},
         {QStringLiteral("screenshot-delay")},
         {QStringLiteral("latex-recognition"), QStringLiteral("barcode-recognition"),
          QStringLiteral("table-recognition"), QStringLiteral("text-translation"),
          QStringLiteral("record-screen"), QStringLiteral("scrolling-screenshot"),
          QStringLiteral("text-recognition")}},
        {QStringLiteral("save-as-file"), QStringLiteral("convert-to-markdown"),
         QStringLiteral("convert-to-html")}};
    const auto unavailable = [](const QString& id) {
        return !presentation::editionActionToolAvailable(id);
    };
    for (auto& position : layout.positions)
        position.removeIf(unavailable);
    layout.positions.removeIf([](const QStringList& position) { return position.isEmpty(); });
    layout.hidden.removeIf(unavailable);
    return layout;
}

inline QStringList floatingToolbarItemIds() {
    const auto layout = defaultFloatingToolbarLayout();
    QStringList ids;
    for (const auto& position : layout.positions)
        ids.append(position);
    ids.append(layout.hidden);
    return ids;
}

class FloatingToolbarSettings final {
  public:
    [[nodiscard]] bool enabled() const;
    bool setEnabled(bool value) const;
    [[nodiscard]] bool toolbarMode() const;
    bool setToolbarMode(bool value) const;
    [[nodiscard]] bool hideInFullscreen() const;
    bool setHideInFullscreen(bool value) const;
    [[nodiscard]] bool hideDuringCapture() const;
    bool setHideDuringCapture(bool value) const;
    [[nodiscard]] int opacity() const;
    bool setOpacity(int value) const;
    [[nodiscard]] QJsonObject placement() const;
    bool setPlacement(const QJsonObject& value) const;
};

} // namespace snow_shot::storage
#endif
