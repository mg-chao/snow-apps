#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_APPLICATIONSEARCHWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_APPLICATIONSEARCHWIDGET_H

#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingssearchindex.h"

#include <QHash>
#include <QString>
#include <QVariant>
#include <QWidget>

class QAbstractItemDelegate;
class QEvent;

namespace snow_shot::presentation::styles {
struct ThemeAliasMetricToken;
struct ThemeColorScheme;
} // namespace snow_shot::presentation::styles
namespace adqt::widgets {
class AdSelect;
}

class ApplicationSearchWidget final : public QWidget {
    Q_OBJECT

  public:
    ApplicationSearchWidget(
        const snow_shot::presentation::settings::SettingsRegistry& registry,
        const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
        QWidget* parent = nullptr);
    ~ApplicationSearchWidget() override;

    void setPlaceholderText(const QString& text);
    [[nodiscard]] QString query() const;
    void clearQuery();
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);
    void rebuildIndex();

  signals:
    void locationActivated(
        const snow_shot::presentation::settings::SettingsLocation& location);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void populateResults(const QString& query);
    void handleSelectedValue(const QVariant& value, const QString& label);

    snow_shot::presentation::settings::SettingsSearchIndex m_index;
    adqt::widgets::AdSelect* m_select = nullptr;
    QAbstractItemDelegate* m_resultDelegate = nullptr;
    QHash<QString, snow_shot::presentation::settings::SettingsLocation> m_locationsByValue;
    bool m_clearingSelection = false;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_APPLICATIONSEARCHWIDGET_H
