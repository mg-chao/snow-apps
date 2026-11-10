// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDACCESSIBILITY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDACCESSIBILITY_H

#include <QObject>
#include <QRect>

class QAccessibleInterface;
class ScreenshotPinnedWindow;
class QWidget;

// A QObject-backed accessible child, independent of the canvas's lifetime and visibility.
class ScreenshotPinnedAccessibility final : public QObject {
  public:
    explicit ScreenshotPinnedAccessibility(ScreenshotPinnedWindow& window);
    ~ScreenshotPinnedAccessibility() override;

    static ScreenshotPinnedAccessibility* forWindow(QObject* object);
    static ScreenshotPinnedAccessibility* forCanvas(QObject* object);
    QWidget* owner() const;
    QRect rect() const;
    bool visible() const;
    bool isDecoration(QObject* object) const;
    QAccessibleInterface* accessibleInterface() const;
    void update(bool languageChanged = false);

  private:
    ScreenshotPinnedWindow& m_window;
    QRect m_lastRect;
    bool m_lastVisible = false;
};

#endif
