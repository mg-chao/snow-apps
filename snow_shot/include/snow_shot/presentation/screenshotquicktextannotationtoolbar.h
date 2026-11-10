#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTQUICKTEXTANNOTATIONTOOLBAR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTQUICKTEXTANNOTATIONTOOLBAR_H

#include "snow_shot/presentation/screenshottextannotation.h"

#include <QObject>
#include <QRect>
#include <array>
#include <memory>
#include <optional>

class QWidget;

class ScreenshotQuickTextAnnotationToolbar final : public QObject {
    Q_OBJECT

  public:
    using Tool = ScreenshotTextAnnotationTool;
    using Style = ScreenshotTextAnnotationStyle;
    struct ToolState final {
        bool active = false;
        std::optional<Style> style;
        bool mixed = false;
        quint32 mixedProperties = 0;
    };
    using States = std::array<ToolState, 4>;

    explicit ScreenshotQuickTextAnnotationToolbar(QWidget* owner);
    ~ScreenshotQuickTextAnnotationToolbar() override;

    // The revision identifies the logical selection, independently of geometry/style changes.
    void updateSelection(const QRect& globalBounds, const States& states, quint64 revision);
    // Reposition the existing selection without querying its annotation styles again.
    void updateSelectionGeometry(const QRect& globalBounds);
    void clearSelection();
    [[nodiscard]] bool isVisible() const;
    [[nodiscard]] bool ownsWindow(const QWidget* widget) const;
    [[nodiscard]] Style styleForTool(Tool tool) const;

  signals:
    void superseded();
    void copyClicked();
    void toolClicked(ScreenshotTextAnnotationTool tool);
    void styleEdited(ScreenshotTextAnnotationTool tool, const ScreenshotTextAnnotationStyle& style,
                     quint32 properties);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Private;
    std::unique_ptr<Private> d;
};

#endif
