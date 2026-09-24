#pragma once

#include <QTextEdit>
#include <QPointer>

#include "icon_core.h"
#include "input_line_edit.h"
#include "input_policies.h"

class QEnterEvent;
class QHideEvent;
class QLabel;
class QDropEvent;
class QInputMethodEvent;
class QKeyEvent;
class QMimeData;
class QMoveEvent;
class QResizeEvent;
class QScrollBar;
class QShowEvent;
class QToolButton;
class QWidget;

namespace adqt::widgets::detail {
struct InputVisualStyle;
}

namespace adqt::widgets {

class AdTextEdit final : public QTextEdit {
  Q_OBJECT

  Q_PROPERTY(AdLineEdit::ControlSize controlSize READ controlSize WRITE setControlSize NOTIFY
                 controlSizeChanged)
  Q_PROPERTY(AdLineEdit::Variant variant READ variant WRITE setVariant NOTIFY variantChanged)
  Q_PROPERTY(AdLineEdit::Status status READ status WRITE setStatus NOTIFY statusChanged)
  Q_PROPERTY(bool allowClear READ allowClear WRITE setAllowClear NOTIFY allowClearChanged)
  Q_PROPERTY(QString plainText READ plainText WRITE setPlainText NOTIFY plainTextChanged)
  Q_PROPERTY(bool countVisible READ countVisible WRITE setCountVisible NOTIFY countVisibleChanged)
  Q_PROPERTY(int maximumCharacterCount READ maximumCharacterCount WRITE setMaximumCharacterCount
                 NOTIFY maximumCharacterCountChanged)
  Q_PROPERTY(HeightMode heightMode READ heightMode WRITE setHeightMode NOTIFY heightModeChanged)
  Q_PROPERTY(int minimumVisibleRows READ minimumVisibleRows WRITE setMinimumVisibleRows NOTIFY
                 minimumVisibleRowsChanged)
  Q_PROPERTY(int maximumVisibleRows READ maximumVisibleRows WRITE setMaximumVisibleRows NOTIFY
                 maximumVisibleRowsChanged)
  Q_PROPERTY(adqt::icons::IconRef feedbackIconRef READ feedbackIconRef WRITE setFeedbackIconRef
                 NOTIFY feedbackIconRefChanged)
  Q_PROPERTY(adqt::widgets::AdInputTextPolicy* textPolicy READ textPolicy WRITE setTextPolicy NOTIFY
                 textPolicyChanged)

 public:
  using ControlSize = AdLineEdit::ControlSize;
  using Variant = AdLineEdit::Variant;
  using Status = AdLineEdit::Status;
  using FocusSelection = AdLineEdit::FocusSelection;
  enum class HeightMode {
    FixedRows,
    AutoGrow,
    FixedGeometry,
  };
  Q_ENUM(HeightMode)

  explicit AdTextEdit(QWidget* parent = nullptr);
  ~AdTextEdit() override;

  ControlSize controlSize() const;
  void setControlSize(ControlSize value);

  Variant variant() const;
  void setVariant(Variant value);

  Status status() const;
  void setStatus(Status value);

  bool allowClear() const;
  void setAllowClear(bool value);

  void setPlaceholderText(const QString& value);
  QString plainText() const;
  void setPlainText(const QString& value);
  void setReadOnly(bool value);

  int maxLength() const;
  void setMaxLength(int value);

  bool countVisible() const;
  void setCountVisible(bool value);

  int maximumCharacterCount() const;
  void setMaximumCharacterCount(int value);

  HeightMode heightMode() const;
  void setHeightMode(HeightMode value);

  int minimumVisibleRows() const;
  void setMinimumVisibleRows(int value);

  int maximumVisibleRows() const;
  void setMaximumVisibleRows(int value);

  adqt::icons::IconRef feedbackIconRef() const;
  void setFeedbackIconRef(const adqt::icons::IconRef& value);

  AdInputTextPolicy* textPolicy() const;
  void setTextPolicy(AdInputTextPolicy* value);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

  void focusEditor(FocusSelection selection = FocusSelection::Preserve, bool preventScroll = false);
  void blurInput();

 signals:
  void controlSizeChanged(ControlSize value);
  void variantChanged(Variant value);
  void statusChanged(Status value);
  void allowClearChanged(bool value);
  void placeholderTextChanged(const QString& value);
  void plainTextChanged(const QString& value);
  void maxLengthChanged(int value);
  void readOnlyChanged(bool value);
  void countVisibleChanged(bool value);
  void maximumCharacterCountChanged(int value);
  void heightModeChanged(HeightMode value);
  void minimumVisibleRowsChanged(int value);
  void maximumVisibleRowsChanged(int value);
  void feedbackIconRefChanged(const adqt::icons::IconRef& value);
  void textPolicyChanged(AdInputTextPolicy* value);
  void textEdited(const QString& value);
  void cleared();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  bool event(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void inputMethodEvent(QInputMethodEvent* event) override;
  void insertFromMimeData(const QMimeData* source) override;
  void dropEvent(QDropEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void changeEvent(QEvent* event) override;
  void moveEvent(QMoveEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;
  void focusOutEvent(QFocusEvent* event) override;

 private:
  void applyDocumentLayoutMetrics(const adqt::widgets::detail::InputVisualStyle& style);
  void updateCountLabel();
  void updateClearButton();
  void updateFeedbackIcon();
  void updateFeedbackSpinnerState();
  void applyScrollBarStyle();
  void syncOverlayScrollBar();
  void updateOverlayScrollBarGeometry();
  void updateLayoutMetrics();
  void updateHeightConstraints();
  void applyEditorPalette();
  void refreshVisualState(bool geometryChanged);
  void updateInteractionFocusOverlay();
  void syncAccessibleState();
  Status effectiveStatus() const;
  adqt::widgets::detail::InputVisualStyle resolvedStyle() const;
  int effectiveCount(const QString& text) const;
  int effectiveCountMax() const;
  QString normalizedText(const QString& text) const;
  QString countLabelText(const QString& text, int count, int maximum) const;
  int widthAccessoryHint(const detail::InputVisualStyle& style) const;

  ControlSize controlSize_ = ControlSize::Medium;
  Variant variant_ = Variant::Outlined;
  Status status_ = Status::None;
  bool allowClear_ = false;
  int maxLength_ = -1;
  bool countVisible_ = false;
  int maximumCharacterCount_ = -1;
  HeightMode heightMode_ = HeightMode::FixedRows;
  int minimumVisibleRows_ = 2;
  int maximumVisibleRows_ = 6;
  adqt::icons::IconRef feedbackIconRef_;
  QPointer<AdInputTextPolicy> textPolicy_;

  QScrollBar* overlayVerticalScrollBar_ = nullptr;
  QWidget* frameLayer_ = nullptr;
  QToolButton* clearButton_ = nullptr;
  QLabel* feedbackIconLabel_ = nullptr;
  QLabel* countLabel_ = nullptr;

  bool focused_ = false;
  bool feedbackSpinnerSubscribed_ = false;
  bool internalTextUpdate_ = false;
  bool internalDocumentLayoutUpdate_ = false;
  bool userEditInProgress_ = false;
};

}  // namespace adqt::widgets
