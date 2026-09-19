#pragma once

#include <QWidget>
#include <functional>

class QLabel;
class QAbstractButton;

// Injectable native actions keep permission transitions testable without changing TCC.
struct SmartSelectionPermissionActions {
    std::function<bool(bool prompt)> check;
    std::function<void()> openSettings;
};

class SmartSelectionPermissionWidget final : public QWidget {
  public:
    explicit SmartSelectionPermissionWidget(QWidget* parent = nullptr,
                                            SmartSelectionPermissionActions actions = {});
    void refresh();

  protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void retranslate();
    SmartSelectionPermissionActions m_actions;
    QLabel* m_status;
    QAbstractButton* m_open;
    QAbstractButton* m_request;
    QAbstractButton* m_retry;
    bool m_granted = false;
};
