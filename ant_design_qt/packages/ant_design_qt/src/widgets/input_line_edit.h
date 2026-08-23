#pragma once

#include <QLineEdit>
#include <QPointer>

#include "control_scale.h"
#include "icon_core.h"
#include "input_policies.h"

class QEnterEvent;
class QLabel;
class QMouseEvent;
class QMoveEvent;
class QResizeEvent;
class QShowEvent;
class QHideEvent;
class QToolButton;

namespace adqt::widgets::detail {
struct InputVisualStyle;
}

namespace adqt::widgets {

class AdLineEdit : public QLineEdit, public AdControlScaleParticipant {
  Q_OBJECT

  Q_PROPERTY(
      ControlSize controlSize READ controlSize WRITE setControlSize NOTIFY controlSizeChanged)
  Q_PROPERTY(Variant variant READ variant WRITE setVariant NOTIFY variantChanged)
  Q_PROPERTY(Status status READ status WRITE setStatus NOTIFY statusChanged)
  Q_PROPERTY(bool allowClear READ allowClear WRITE setAllowClear NOTIFY allowClearChanged)
  Q_PROPERTY(QString prefixText READ prefixText WRITE setPrefixText NOTIFY prefixTextChanged)
  Q_PROPERTY(QString suffixText READ suffixText WRITE setSuffixText NOTIFY suffixTextChanged)
  Q_PROPERTY(bool countVisible READ countVisible WRITE setCountVisible NOTIFY countVisibleChanged)
  Q_PROPERTY(int maximumCharacterCount READ maximumCharacterCount WRITE setMaximumCharacterCount
                 NOTIFY maximumCharacterCountChanged)
  Q_PROPERTY(bool joinedLeft READ joinedLeft WRITE setJoinedLeft NOTIFY joinedLeftChanged)
  Q_PROPERTY(bool joinedRight READ joinedRight WRITE setJoinedRight NOTIFY joinedRightChanged)
  Q_PROPERTY(adqt::icons::IconRef prefixIconRef READ prefixIconRef WRITE setPrefixIconRef NOTIFY
                 prefixIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef suffixIconRef READ suffixIconRef WRITE setSuffixIconRef NOTIFY
                 suffixIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef feedbackIconRef READ feedbackIconRef WRITE setFeedbackIconRef
                 NOTIFY feedbackIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef clearIconRef READ clearIconRef WRITE setClearIconRef NOTIFY
                 clearIconRefChanged)
  Q_PROPERTY(adqt::widgets::AdInputTextPolicy* textPolicy READ textPolicy WRITE setTextPolicy NOTIFY
                 textPolicyChanged)

 public:
  enum class ControlSize {
    Large,
    Medium,
    Small,
  };
  Q_ENUM(ControlSize)

  enum class Variant {
    Outlined,
    Filled,
    Borderless,
    Underlined,
  };
  Q_ENUM(Variant)

  enum class Status {
    None,
    Error,
    Warning,
  };
  Q_ENUM(Status)

  enum class FocusSelection {
    Preserve,
    Start,
    End,
    SelectAll,
  };
  Q_ENUM(FocusSelection)

  explicit AdLineEdit(QWidget* parent = nullptr);
  ~AdLineEdit() override;

  ControlSize controlSize() const;
  void setControlSize(ControlSize value);

  Variant variant() const;
  void setVariant(Variant value);

  Status status() const;
  void setStatus(Status value);

  bool allowClear() const;
  void setAllowClear(bool value);

  void setPlaceholderText(const QString& value);
  void setText(const QString& value);
  void clear();
  void setMaxLength(int value);
  void setReadOnly(bool value);
  void setEchoMode(QLineEdit::EchoMode value);

  QString prefixText() const;
  void setPrefixText(const QString& value);

  QString suffixText() const;
  void setSuffixText(const QString& value);

  bool countVisible() const;
  void setCountVisible(bool value);

  int maximumCharacterCount() const;
  void setMaximumCharacterCount(int value);

  bool joinedLeft() const;
  void setJoinedLeft(bool value);

  bool joinedRight() const;
  void setJoinedRight(bool value);

  adqt::icons::IconRef prefixIconRef() const;
  void setPrefixIconRef(const adqt::icons::IconRef& token);

  adqt::icons::IconRef suffixIconRef() const;
  void setSuffixIconRef(const adqt::icons::IconRef& token);

  adqt::icons::IconRef feedbackIconRef() const;
  void setFeedbackIconRef(const adqt::icons::IconRef& token);

  adqt::icons::IconRef clearIconRef() const;
  void setClearIconRef(const adqt::icons::IconRef& token);

  AdInputTextPolicy* textPolicy() const;
  void setTextPolicy(AdInputTextPolicy* value);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  void prepareControlScale(const AdControlScaleContext& context) override;
  void commitControlScale(const AdControlScaleContext& context) override;

  void focusEditor(FocusSelection selection = FocusSelection::Preserve, bool preventScroll = false);
  void blurInput();

 signals:
  void controlSizeChanged(ControlSize value);
  void variantChanged(Variant value);
  void statusChanged(Status value);
  void allowClearChanged(bool value);
  void placeholderTextChanged(const QString& value);
  void maxLengthChanged(int value);
  void readOnlyChanged(bool value);
  void prefixTextChanged(const QString& value);
  void suffixTextChanged(const QString& value);
  void countVisibleChanged(bool value);
  void maximumCharacterCountChanged(int value);
  void joinedLeftChanged(bool value);
  void joinedRightChanged(bool value);
  void echoModeChanged(QLineEdit::EchoMode value);
  void prefixIconRefChanged(const adqt::icons::IconRef& token);
  void suffixIconRefChanged(const adqt::icons::IconRef& token);
  void feedbackIconRefChanged(const adqt::icons::IconRef& token);
  void clearIconRefChanged(const adqt::icons::IconRef& token);
  void textPolicyChanged(AdInputTextPolicy* value);
  void cleared();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  bool event(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void changeEvent(QEvent* event) override;
  void moveEvent(QMoveEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;
  void focusOutEvent(QFocusEvent* event) override;

  void setTrailingActionLeading(bool value);
  void setTrailingActionVisible(bool value);
  void setTrailingActionIconRef(const adqt::icons::IconRef& token);
  void setTrailingActionAccessibleName(const QString& value);
  void setClearOverlaysTrailingAction(bool value);
  QToolButton* trailingActionButton() const;

 private:
  bool hasClearValue() const;
  bool clearButtonWantsVisible() const;
  bool clearButtonReservesWidth() const;
  void updateAccessoryVisibility();
  void updateAccessoryGeometry();
  void updateTextMargins();
  void updateCountLabel();
  void updateClearButton();
  void updatePrefixVisual();
  void updateSuffixVisual();
  void updateFeedbackSpinnerState();
  void applyEditorPalette();
  void refreshVisualState(bool geometryChanged);
  void updateInteractionFocusOverlay();
  void syncAccessibleState();
  Status effectiveStatus() const;
  adqt::widgets::detail::InputVisualStyle resolvedStyle() const;
  void updateCursorForRole();
  void updateJoinedZOrder();
  int effectiveCount(const QString& value) const;
  int effectiveCountMax() const;
  QString normalizedText(const QString& value) const;
  QString countLabelText(const QString& value, int count, int maximum) const;
  int accessoryWidthHint(const detail::InputVisualStyle& style) const;

  ControlSize controlSize_ = ControlSize::Medium;
  Variant variant_ = Variant::Outlined;
  Status status_ = Status::None;
  bool allowClear_ = false;
  QString prefixText_;
  QString suffixText_;
  bool countVisible_ = false;
  int maxLength_ = -1;
  int maximumCharacterCount_ = -1;
  bool joinedLeft_ = false;
  bool joinedRight_ = false;
  QPointer<AdInputTextPolicy> textPolicy_;
  adqt::icons::IconRef prefixIconRef_;
  adqt::icons::IconRef suffixIconRef_;
  adqt::icons::IconRef feedbackIconRef_;
  adqt::icons::IconRef clearIconRef_;
  bool trailingActionVisible_ = false;
  bool suffixActionLeading_ = false;
  bool clearOverlaysTrailingAction_ = false;
  adqt::icons::IconRef trailingActionIconRef_;
  QString trailingActionAccessibleName_;

  QLabel* prefixIconLabel_ = nullptr;
  QLabel* prefixLabel_ = nullptr;
  QLabel* suffixLabel_ = nullptr;
  QLabel* suffixIconLabel_ = nullptr;
  QLabel* feedbackIconLabel_ = nullptr;
  QToolButton* clearButton_ = nullptr;
  QToolButton* suffixActionButton_ = nullptr;
  QLabel* countLabel_ = nullptr;

  AdControlScaleContext controlScale_;
  QFont referenceFont_;
  bool hovered_ = false;
  bool focused_ = false;
  bool feedbackSpinnerSubscribed_ = false;
  bool internalTextUpdate_ = false;
  bool referenceFontCaptured_ = false;
};

}  // namespace adqt::widgets
