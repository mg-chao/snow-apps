// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_CANVASDRAWINGPREFERENCES_H
#define SNOW_SHOT_PRESENTATION_CANVASDRAWINGPREFERENCES_H

#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QDebug>
#include <functional>

namespace snow_shot::presentation {

// Bind once per document runtime; its configuration survives document replacement.
// The context owns the subscription and must not outlive the runtime.
inline void bindCanvasDrawingPreferences(
    SnowCanvasRuntime& runtime, QObject& context,
    std::function<void(const QSet<SnowCanvasTool>&)> quickSelectionChanged = {}) {
    auto& applicationStorage = storage::ApplicationStorage::instance();
    if (!applicationStorage.isInitialized()) {
        return;
    }
    const auto apply = [&runtime, quickSelectionChanged]() {
        const storage::DrawingSettings settings;
        const auto disabledTools = screenshotDrawingTools(settings.quickSelectionDisabledTools());
        if (!runtime.setQuickSelectionDisabledTools(disabledTools)) {
            qWarning("Failed to apply canvas quick-selection preferences");
        }
        if (!runtime.setAutomaticSelectionExcludedTools(
                screenshotDrawingTools(settings.automaticSelectionExcludedTools()))) {
            qWarning("Failed to apply canvas automatic selection exclusions");
        }
        if (!runtime.setAutomaticallySelectNewlyCreatedElements(
                settings.automaticallySelectNewlyCreatedElements())) {
            qWarning("Failed to apply canvas automatic selection preference");
        }
        if (quickSelectionChanged) {
            quickSelectionChanged(disabledTools);
        }
    };
    apply();
    const auto applyColor = [&runtime]() {
        const storage::DrawingSettings settings;
        if (!runtime.setSharedCreationColor(settings.syncToolColors()
                                                ? std::optional<QColor>(settings.sharedToolColor())
                                                : std::nullopt))
            qWarning("Failed to apply canvas shared creation color");
    };
    applyColor();
    QObject::connect(
        &applicationStorage.configuration(), &storage::ConfigurationStore::valueChanged, &context,
        [apply, applyColor](const QString& key, const QJsonValue&) {
            if (key == QStringLiteral("drawing/sync_tool_colors") ||
                key == QStringLiteral("drawing/shared_tool_color"))
                applyColor();
            if (key == QStringLiteral("drawing/quick_selection_disabled_tools") ||
                key == QStringLiteral("drawing/automatic_selection_excluded_tools") ||
                key == QStringLiteral("drawing/automatically_select_newly_created_elements")) {
                apply();
            }
        });
}

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_CANVASDRAWINGPREFERENCES_H
