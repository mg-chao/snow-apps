#pragma once

#include <QColor>
#include <QPixmap>
#include <QPointer>
#include <QRectF>
#include <QWidget>

namespace snow_shot::presentation {
class MainWindowSkinController;

class MainWindowSkinWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit MainWindowSkinWidget(QWidget* parent = nullptr,
                                  MainWindowSkinController* controller = nullptr);
    ~MainWindowSkinWidget() override;
    [[nodiscard]] bool skinActive() const;
    [[nodiscard]] qreal maskOpacity() const;
    void setBaseColor(const QColor& color);

  signals:
    void skinAppearanceChanged();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void syncFrame();
    QPointer<MainWindowSkinController> m_controller;
    QColor m_baseColor = Qt::white;
    QPixmap m_frame;
    QRectF m_placement;
    bool m_attached = false;
};
} // namespace snow_shot::presentation
