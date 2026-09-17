#pragma once

#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTime>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "icon_core.h"
#include "detail/overlay_popup_controller.h"
#include "input_line_edit.h"
#include "popup_types.h"

class QFrame;
class QEvent;
class QHBoxLayout;
class QHideEvent;
class QLabel;
class QListWidget;
class QMoveEvent;
class QPainter;
class QResizeEvent;
class QScrollArea;
class QShowEvent;
class QTimeEdit;
class QToolButton;
class QVBoxLayout;

namespace adqt::widgets::detail {
class DatePickerCalendarGrid;
class DatePickerLineEdit;
struct DatePickerVisualStyle;
}  // namespace adqt::widgets::detail

namespace adqt::widgets {

class AdDatePickerPanel final : public QWidget {
  Q_OBJECT

  Q_PROPERTY(PickerMode pickerMode READ pickerMode WRITE setPickerMode NOTIFY pickerModeChanged)
  Q_PROPERTY(SelectionMode selectionMode READ selectionMode WRITE setSelectionMode NOTIFY
                 selectionModeChanged)
  Q_PROPERTY(PickerMode panelMode READ panelMode WRITE setPanelMode NOTIFY panelModeChanged)
  Q_PROPERTY(QDate selectedDate READ selectedDate WRITE setSelectedDate NOTIFY selectedDateChanged)
  Q_PROPERTY(QVector<QDate> selectedDates READ selectedDates WRITE setSelectedDates NOTIFY
                 selectedDatesChanged)
  Q_PROPERTY(QDate rangeStartDate READ rangeStartDate WRITE setRangeStartDate NOTIFY rangeChanged)
  Q_PROPERTY(QDate rangeEndDate READ rangeEndDate WRITE setRangeEndDate NOTIFY rangeChanged)
  Q_PROPERTY(QDate viewDate READ viewDate WRITE setViewDate NOTIFY viewDateChanged)
  Q_PROPERTY(QDate minDate READ minDate WRITE setMinDate NOTIFY minDateChanged)
  Q_PROPERTY(QDate maxDate READ maxDate WRITE setMaxDate NOTIFY maxDateChanged)
  Q_PROPERTY(bool showToday READ showToday WRITE setShowToday NOTIFY showTodayChanged)
  Q_PROPERTY(bool showWeek READ showWeek WRITE setShowWeek NOTIFY showWeekChanged)
  Q_PROPERTY(bool needConfirm READ needConfirm WRITE setNeedConfirm NOTIFY needConfirmChanged)
  Q_PROPERTY(bool showTime READ showTime WRITE setShowTime NOTIFY showTimeChanged)
  Q_PROPERTY(bool showNow READ showNow WRITE setShowNow NOTIFY showNowChanged)
  Q_PROPERTY(bool order READ order WRITE setOrder NOTIFY orderChanged)
  Q_PROPERTY(QTime defaultOpenTime READ defaultOpenTime WRITE setDefaultOpenTime NOTIFY
                 defaultOpenTimeChanged)
  Q_PROPERTY(QTime defaultOpenStartTime READ defaultOpenStartTime WRITE setDefaultOpenStartTime
                 NOTIFY defaultOpenTimeRangeChanged)
  Q_PROPERTY(QTime defaultOpenEndTime READ defaultOpenEndTime WRITE setDefaultOpenEndTime NOTIFY
                 defaultOpenTimeRangeChanged)
  Q_PROPERTY(QTime selectedTime READ selectedTime WRITE setSelectedTime NOTIFY selectedTimeChanged)
  Q_PROPERTY(
      QTime rangeStartTime READ rangeStartTime WRITE setRangeStartTime NOTIFY rangeTimeChanged)
  Q_PROPERTY(QTime rangeEndTime READ rangeEndTime WRITE setRangeEndTime NOTIFY rangeTimeChanged)
  Q_PROPERTY(QString timeFormat READ timeFormat WRITE setTimeFormat NOTIFY timeFormatChanged)
  Q_PROPERTY(int hourStep READ hourStep WRITE setHourStep NOTIFY timeStepChanged)
  Q_PROPERTY(int minuteStep READ minuteStep WRITE setMinuteStep NOTIFY timeStepChanged)
  Q_PROPERTY(int secondStep READ secondStep WRITE setSecondStep NOTIFY timeStepChanged)
  Q_PROPERTY(bool hideDisabledOptions READ hideDisabledOptions WRITE setHideDisabledOptions NOTIFY
                 hideDisabledOptionsChanged)
  Q_PROPERTY(bool use12Hours READ use12Hours WRITE setUse12Hours NOTIFY use12HoursChanged)
  Q_PROPERTY(
      bool changeOnScroll READ changeOnScroll WRITE setChangeOnScroll NOTIFY changeOnScrollChanged)
  Q_PROPERTY(bool showHour READ showHour WRITE setShowHour NOTIFY showHourChanged)
  Q_PROPERTY(bool showMinute READ showMinute WRITE setShowMinute NOTIFY showMinuteChanged)
  Q_PROPERTY(bool showSecond READ showSecond WRITE setShowSecond NOTIFY showSecondChanged)
  Q_PROPERTY(bool allowEmptyStart READ allowEmptyStart WRITE setAllowEmptyStart NOTIFY
                 allowEmptyStartChanged)
  Q_PROPERTY(
      bool allowEmptyEnd READ allowEmptyEnd WRITE setAllowEmptyEnd NOTIFY allowEmptyEndChanged)
  Q_PROPERTY(
      bool footerVisible READ footerVisible WRITE setFooterVisible NOTIFY footerVisibleChanged)
  Q_PROPERTY(bool disabled READ disabled WRITE setDisabled NOTIFY disabledChanged)
  Q_PROPERTY(QLocale locale READ locale WRITE setLocale NOTIFY localeChanged)
  Q_PROPERTY(adqt::icons::IconRef prevIconRef READ prevIconRef WRITE setPrevIconRef NOTIFY
                 prevIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef nextIconRef READ nextIconRef WRITE setNextIconRef NOTIFY
                 nextIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef superPrevIconRef READ superPrevIconRef WRITE setSuperPrevIconRef
                 NOTIFY superPrevIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef superNextIconRef READ superNextIconRef WRITE setSuperNextIconRef
                 NOTIFY superNextIconRefChanged)

 public:
  enum class PickerMode {
    Date,
    Week,
    Month,
    Quarter,
    Year,
    Decade,
    Time,
  };
  Q_ENUM(PickerMode)

  enum class SelectionMode {
    Single,
    Multiple,
    Range,
  };
  Q_ENUM(SelectionMode)

  enum class TimeSelectionPart {
    Single,
    Start,
    End,
  };
  Q_ENUM(TimeSelectionPart)

  enum class CellSubType {
    None,
    Hour,
    Minute,
    Second,
    Meridiem,
  };
  Q_ENUM(CellSubType)

  struct ComponentTokens {
    std::optional<int> panelWidth;
    std::optional<int> presetsWidth;
    std::optional<int> presetsMaxWidth;
    std::optional<int> zIndexPopup;
    std::optional<int> timeColumnWidth;
    std::optional<int> timeColumnHeight;
    std::optional<int> timeCellHeight;
    std::optional<int> cellWidth;
    std::optional<int> cellHeight;
    std::optional<int> textHeight;
    std::optional<int> withoutTimeCellHeight;
    std::optional<int> multipleItemHeight;
    std::optional<int> multipleItemHeightSmall;
    std::optional<int> multipleItemHeightLarge;
    std::optional<int> borderRadius;
    std::optional<QColor> panelBackground;
    std::optional<QColor> panelBorderColor;
    std::optional<QColor> cellHoverBackground;
    std::optional<QColor> cellSelectedBackground;
    std::optional<QColor> cellRangeBackground;
    std::optional<QColor> cellRangeHoverBackground;
    std::optional<QColor> cellRangeBorderColor;
    std::optional<QColor> multipleItemBackground;
    std::optional<QColor> multipleItemBorderColor;
    std::optional<QColor> multipleItemTextDisabledColor;
    std::optional<QColor> multipleItemBorderColorDisabled;
    std::optional<QColor> textColor;
    std::optional<QColor> textDisabledColor;
  };

  struct SemanticSlotStyle {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
  };

  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle header;
    SemanticSlotStyle body;
    SemanticSlotStyle content;
    SemanticSlotStyle item;
    SemanticSlotStyle footer;
    SemanticSlotStyle container;
  };

  struct PresetItem {
    using DateProvider = std::function<QDate()>;
    using RangeProvider = std::function<std::pair<QDate, QDate>()>;

    QString label;
    QDate value;
    QDate rangeStartValue;
    QDate rangeEndValue;
    DateProvider valueProvider;
    DateProvider rangeStartValueProvider;
    DateProvider rangeEndValueProvider;
    RangeProvider rangeValueProvider;
  };

  struct DisabledDateContext {
    QDate from;
    PickerMode type = PickerMode::Date;
  };

  struct DisabledTimeContext {
    QDate from;
    TimeSelectionPart part = TimeSelectionPart::Single;
  };

  struct CellRenderInfo {
    QDate date;
    QTime time;
    QString text;
    PickerMode type = PickerMode::Date;
    CellSubType subType = CellSubType::None;
    TimeSelectionPart timePart = TimeSelectionPart::Single;
    int value = -1;
    QRect cellRect;
    QRect contentRect;
    bool inView = true;
    bool today = false;
    bool selected = false;
    bool rangeStart = false;
    bool rangeEnd = false;
    bool inRange = false;
    bool hoverRange = false;
    bool hoverRangeStart = false;
    bool hoverRangeEnd = false;
    bool disabled = false;
  };

  using DatePredicate = std::function<bool(const QDate&)>;
  using DisabledDatePredicate = std::function<bool(const QDate&, const DisabledDateContext&)>;
  using DisabledTimePredicate =
      std::function<bool(const QDate&, const QTime&, const DisabledTimeContext&)>;
  using CellRenderCallback = std::function<void(QPainter&, const CellRenderInfo&)>;

  explicit AdDatePickerPanel(QWidget* parent = nullptr);
  ~AdDatePickerPanel() override;

  PickerMode pickerMode() const;
  void setPickerMode(PickerMode value);

  SelectionMode selectionMode() const;
  void setSelectionMode(SelectionMode value);

  PickerMode panelMode() const;
  void setPanelMode(PickerMode value);

  QDate selectedDate() const;
  void setSelectedDate(const QDate& value);

  QVector<QDate> selectedDates() const;
  void setSelectedDates(const QVector<QDate>& values);

  QDateTime selectedDateTime() const;
  void setSelectedDateTime(const QDateTime& value);

  QTime selectedTime() const;
  void setSelectedTime(const QTime& value);

  QDate rangeStartDate() const;
  void setRangeStartDate(const QDate& value);

  QDate rangeEndDate() const;
  void setRangeEndDate(const QDate& value);

  void setRange(const QDate& start, const QDate& end);
  void setHoverRange(const QDate& start, const QDate& end);
  void clearHoverRange();

  QDateTime rangeStartDateTime() const;
  QDateTime rangeEndDateTime() const;
  void setDateTimeRange(const QDateTime& start, const QDateTime& end);

  QTime rangeStartTime() const;
  void setRangeStartTime(const QTime& value);

  QTime rangeEndTime() const;
  void setRangeEndTime(const QTime& value);

  void setTimeRange(const QTime& start, const QTime& end);

  QDate viewDate() const;
  void setViewDate(const QDate& value);

  QDate minDate() const;
  void setMinDate(const QDate& value);

  QDate maxDate() const;
  void setMaxDate(const QDate& value);

  bool showToday() const;
  void setShowToday(bool value);

  bool showWeek() const;
  void setShowWeek(bool value);

  bool needConfirm() const;
  void setNeedConfirm(bool value);

  bool showTime() const;
  void setShowTime(bool value);

  bool showNow() const;
  void setShowNow(bool value);

  bool order() const;
  void setOrder(bool value);

  QTime defaultOpenTime() const;
  void setDefaultOpenTime(const QTime& value);

  QTime defaultOpenStartTime() const;
  void setDefaultOpenStartTime(const QTime& value);

  QTime defaultOpenEndTime() const;
  void setDefaultOpenEndTime(const QTime& value);

  void setDefaultOpenTimeRange(const QTime& start, const QTime& end);

  QString timeFormat() const;
  void setTimeFormat(const QString& value);
  int hourStep() const;
  void setHourStep(int value);
  int minuteStep() const;
  void setMinuteStep(int value);
  int secondStep() const;
  void setSecondStep(int value);
  void setTimeSteps(int hourStep, int minuteStep, int secondStep);
  bool hideDisabledOptions() const;
  void setHideDisabledOptions(bool value);
  bool use12Hours() const;
  void setUse12Hours(bool value);
  bool changeOnScroll() const;
  void setChangeOnScroll(bool value);
  TimeSelectionPart visibleRangeTimePart() const;
  void setVisibleRangeTimePart(TimeSelectionPart value);
  bool showHour() const;
  void setShowHour(bool value);
  bool showMinute() const;
  void setShowMinute(bool value);
  bool showSecond() const;
  void setShowSecond(bool value);
  void resetShowSecond();
  void setVisibleTimeColumns(bool hour, bool minute, bool second);

  bool allowEmptyStart() const;
  void setAllowEmptyStart(bool value);

  bool allowEmptyEnd() const;
  void setAllowEmptyEnd(bool value);

  void setAllowEmpty(bool start, bool end);

  bool footerVisible() const;
  void setFooterVisible(bool value);

  bool disabled() const;
  void setDisabled(bool value);

  QLocale locale() const;
  void setLocale(const QLocale& value);

  Qt::DayOfWeek firstDayOfWeek() const;
  void setFirstDayOfWeek(Qt::DayOfWeek value);

  adqt::icons::IconRef prevIconRef() const;
  void setPrevIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef nextIconRef() const;
  void setNextIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef superPrevIconRef() const;
  void setSuperPrevIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef superNextIconRef() const;
  void setSuperNextIconRef(const adqt::icons::IconRef& value);

  void setNavigationIconRefs(const adqt::icons::IconRef& superPrev,
                             const adqt::icons::IconRef& prev, const adqt::icons::IconRef& next,
                             const adqt::icons::IconRef& superNext);
  void resetNavigationIconRefs();
  bool hidePreviousNavigation() const;
  void setHidePreviousNavigation(bool value);
  bool hideNextNavigation() const;
  void setHideNextNavigation(bool value);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& styles);
  void resetSemanticStyles();

  QVector<PresetItem> presets() const;
  void setPresets(const QVector<PresetItem>& presets);
  void clearPresets();

  QWidget* extraFooterWidget() const;
  void setExtraFooterWidget(QWidget* widget);
  QWidget* takeExtraFooterWidget();

  DatePredicate disabledDatePredicate() const;
  void setDisabledDatePredicate(DatePredicate predicate);

  DisabledDatePredicate disabledDateContextPredicate() const;
  void setDisabledDateContextPredicate(DisabledDatePredicate predicate);

  DisabledTimePredicate disabledTimePredicate() const;
  void setDisabledTimePredicate(DisabledTimePredicate predicate);

  CellRenderCallback cellRenderCallback() const;
  void setCellRenderCallback(CellRenderCallback callback);
  void clearCellRenderCallback();

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 signals:
  void pickerModeChanged(PickerMode value);
  void selectionModeChanged(SelectionMode value);
  void panelModeChanged(PickerMode value);
  void selectedDateChanged(const QDate& value);
  void selectedDatesChanged(const QVector<QDate>& values);
  void rangeChanged(const QDate& start, const QDate& end);
  void viewDateChanged(const QDate& value);
  void minDateChanged(const QDate& value);
  void maxDateChanged(const QDate& value);
  void showTodayChanged(bool value);
  void showWeekChanged(bool value);
  void needConfirmChanged(bool value);
  void showTimeChanged(bool value);
  void showNowChanged(bool value);
  void orderChanged(bool value);
  void defaultOpenTimeChanged(const QTime& value);
  void defaultOpenTimeRangeChanged(const QTime& start, const QTime& end);
  void selectedTimeChanged(const QTime& value);
  void rangeTimeChanged(const QTime& start, const QTime& end);
  void timeFormatChanged(const QString& value);
  void timeStepChanged(int hourStep, int minuteStep, int secondStep);
  void hideDisabledOptionsChanged(bool value);
  void use12HoursChanged(bool value);
  void changeOnScrollChanged(bool value);
  void showHourChanged(bool value);
  void showMinuteChanged(bool value);
  void showSecondChanged(bool value);
  void allowEmptyStartChanged(bool value);
  void allowEmptyEndChanged(bool value);
  void footerVisibleChanged(bool value);
  void disabledChanged(bool value);
  void localeChanged(const QLocale& value);
  void prevIconRefChanged(const adqt::icons::IconRef& value);
  void nextIconRefChanged(const adqt::icons::IconRef& value);
  void superPrevIconRefChanged(const adqt::icons::IconRef& value);
  void superNextIconRefChanged(const adqt::icons::IconRef& value);
  void componentTokensChanged();
  void semanticStylesChanged();
  void presetsChanged();
  void extraFooterWidgetChanged(QWidget* widget);
  void cellRenderCallbackChanged();
  void dateSelected(const QDate& value);
  void dateActivated(const QDate& value);
  void previewDateChanged(const QDate& value);
  void previewTimeChanged(const QTime& value, TimeSelectionPart part);
  void datesAccepted(const QVector<QDate>& values);
  void rangeAccepted(const QDate& start, const QDate& end);
  void accepted(const QDate& value);
  void dateTimeAccepted(const QDateTime& value);
  void rangeDateTimeAccepted(const QDateTime& start, const QDateTime& end);

 protected:
  void changeEvent(QEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  enum class DisplayMode {
    Date,
    Month,
    Quarter,
    Year,
    Decade,
    Time,
  };

  friend class detail::DatePickerCalendarGrid;
  friend class AdDatePicker;
  friend class AdDateRangePicker;

  void buildUi();
  void refreshStyle();
  void refreshHeader();
  void refreshNavigationButtons();
  void refreshFooter();
  void refreshTimeEditors();
  void refreshPanelBodyVisibility();
  void ensureTimeControlsCreated();
  void ensureTimeColumnsPopulated();
  void rebuildTimeColumns();
  void syncTimeColumnSelections();
  void updateTimeColumnStates();
  void updateTimeColumnRendering();
  void setTimeFromColumnRows(TimeSelectionPart part, int hour, int minute, int second, bool pm);
  bool handleTimeColumnWheel(QObject* watched, QEvent* event);
  bool handleTimeColumnPreview(QObject* watched, QEvent* event);
  void ensurePresetsUi();
  void refreshPresetsStyle(const detail::DatePickerVisualStyle& style);
  void rebuildPresets();
  void syncGridState();
  void syncTimeEditors();
  void setDisplayMode(DisplayMode value);
  void navigate(int months, int years);
  void switchHeaderView();
  void selectDateFromGrid(const QDate& value);
  void applyPreset(const PresetItem& preset);
  void acceptCurrentSelection();
  QDate effectiveDateForTimePart(TimeSelectionPart part) const;
  bool canAcceptRange(const QDate& start, const QDate& end) const;
  QVector<QDate> normalizedDates(const QVector<QDate>& values) const;
  void syncSelectedDateKeys();
  QDate normalizedViewDate(const QDate& value) const;
  QDate viewRangeStart(const QDate& value) const;
  QDate viewRangeEnd(const QDate& value) const;
  bool viewDateCanDisplay(const QDate& value) const;
  bool isSelectableForMode(PickerMode mode, const QDate& value, const QDate& from = QDate()) const;
  QDate rangeSelectionContextFrom() const;
  DisplayMode defaultDisplayModeForPicker(PickerMode value) const;
  DisplayMode displayModeForPanelMode(PickerMode value) const;
  PickerMode panelModeForDisplayMode(DisplayMode value) const;
  bool effectiveShowTime() const;
  bool timeControlsVisible() const;
  int timePanelColumnCount() const;
  void refreshTimePanelGeometry();
  bool effectiveNeedConfirm() const;
  QString effectiveTimeFormat() const;
  bool effectiveShowHourColumn() const;
  bool effectiveShowMinuteColumn() const;
  bool effectiveShowSecondColumn() const;
  bool isTimeSelectable(const QDate& date, const QTime& time, TimeSelectionPart part,
                        const QDate& from = QDate()) const;
  void invalidateResolvedStyle() const;
  const detail::DatePickerVisualStyle& resolvedStyle() const;

  PickerMode pickerMode_ = PickerMode::Date;
  SelectionMode selectionMode_ = SelectionMode::Single;
  DisplayMode displayMode_ = DisplayMode::Date;
  QDate selectedDate_;
  QVector<QDate> selectedDates_;
  QSet<qint64> selectedDateKeys_;
  QTime selectedTime_ = QTime(0, 0, 0);
  QDate rangeStartDate_;
  QDate rangeEndDate_;
  QDate hoverRangeStartDate_;
  QDate hoverRangeEndDate_;
  bool hoverRangeActive_ = false;
  QTime rangeStartTime_ = QTime(0, 0, 0);
  QTime rangeEndTime_ = QTime(0, 0, 0);
  QDate viewDate_;
  QDate minDate_;
  QDate maxDate_;
  Qt::DayOfWeek firstDayOfWeek_ = Qt::Monday;
  bool showToday_ = true;
  bool showWeek_ = false;
  bool needConfirm_ = false;
  bool showTime_ = false;
  bool showNow_ = true;
  bool order_ = true;
  QTime defaultOpenTime_ = QTime(0, 0, 0);
  QTime defaultOpenStartTime_ = QTime(0, 0, 0);
  QTime defaultOpenEndTime_ = QTime(0, 0, 0);
  QString timeFormat_;
  int hourStep_ = 1;
  int minuteStep_ = 1;
  int secondStep_ = 1;
  bool hideDisabledOptions_ = false;
  bool use12Hours_ = false;
  bool changeOnScroll_ = false;
  TimeSelectionPart visibleRangeTimePart_ = TimeSelectionPart::Start;
  bool showHour_ = true;
  bool showMinute_ = true;
  bool showSecond_ = true;
  bool showSecondExplicit_ = false;
  bool timeColumnsPopulated_ = false;
  bool allowEmptyStart_ = false;
  bool allowEmptyEnd_ = false;
  bool footerVisible_ = true;
  bool disabled_ = false;
  QLocale locale_;
  bool localeExplicit_ = false;
  bool applyingGlobalLocale_ = false;
  adqt::icons::IconRef prevIconRef_;
  adqt::icons::IconRef nextIconRef_;
  adqt::icons::IconRef superPrevIconRef_;
  adqt::icons::IconRef superNextIconRef_;
  bool hidePreviousNavigation_ = false;
  bool hideNextNavigation_ = false;
  ComponentTokens componentTokens_;
  SemanticStyles semanticStyles_;
  mutable std::unique_ptr<detail::DatePickerVisualStyle> resolvedStyle_;
  QVector<PresetItem> presets_;
  DatePredicate disabledDatePredicate_;
  DisabledDatePredicate disabledDateContextPredicate_;
  DisabledTimePredicate disabledTimePredicate_;
  CellRenderCallback cellRenderCallback_;

  QVBoxLayout* rootLayout_ = nullptr;
  QWidget* panelLayoutWidget_ = nullptr;
  QHBoxLayout* panelLayout_ = nullptr;
  QWidget* panelFrame_ = nullptr;
  QVBoxLayout* panelFrameLayout_ = nullptr;
  QWidget* header_ = nullptr;
  QHBoxLayout* headerLayout_ = nullptr;
  QToolButton* superPrevButton_ = nullptr;
  QToolButton* prevButton_ = nullptr;
  QWidget* viewHost_ = nullptr;
  QHBoxLayout* viewHostLayout_ = nullptr;
  QToolButton* viewButton_ = nullptr;
  QToolButton* monthViewButton_ = nullptr;
  QToolButton* yearViewButton_ = nullptr;
  QToolButton* nextButton_ = nullptr;
  QToolButton* superNextButton_ = nullptr;
  QWidget* timeHeader_ = nullptr;
  QLabel* timeHeaderLabel_ = nullptr;
  QWidget* contentWidget_ = nullptr;
  QHBoxLayout* contentLayout_ = nullptr;
  detail::DatePickerCalendarGrid* grid_ = nullptr;
  QWidget* footer_ = nullptr;
  QVBoxLayout* footerOuterLayout_ = nullptr;
  QWidget* extraFooterHost_ = nullptr;
  QWidget* extraFooterWidget_ = nullptr;
  QWidget* presetsWidget_ = nullptr;
  QScrollArea* presetsScrollArea_ = nullptr;
  QWidget* presetsListWidget_ = nullptr;
  QVBoxLayout* presetsLayout_ = nullptr;
  QWidget* timeWidget_ = nullptr;
  QHBoxLayout* timeLayout_ = nullptr;
  QLabel* singleTimeLabel_ = nullptr;
  QLabel* rangeStartTimeLabel_ = nullptr;
  QLabel* rangeEndTimeLabel_ = nullptr;
  QTimeEdit* selectedTimeEdit_ = nullptr;
  QTimeEdit* rangeStartTimeEdit_ = nullptr;
  QTimeEdit* rangeEndTimeEdit_ = nullptr;
  QListWidget* selectedHourList_ = nullptr;
  QListWidget* selectedMinuteList_ = nullptr;
  QListWidget* selectedSecondList_ = nullptr;
  QListWidget* selectedMeridiemList_ = nullptr;
  QListWidget* rangeStartHourList_ = nullptr;
  QListWidget* rangeStartMinuteList_ = nullptr;
  QListWidget* rangeStartSecondList_ = nullptr;
  QListWidget* rangeStartMeridiemList_ = nullptr;
  QListWidget* rangeEndHourList_ = nullptr;
  QListWidget* rangeEndMinuteList_ = nullptr;
  QListWidget* rangeEndSecondList_ = nullptr;
  QListWidget* rangeEndMeridiemList_ = nullptr;
  QWidget* footerActionsWidget_ = nullptr;
  QHBoxLayout* footerLayout_ = nullptr;
  QToolButton* todayButton_ = nullptr;
  QToolButton* okButton_ = nullptr;
};

class AdDatePicker final : public QWidget, private detail::OverlayPopupControllerDelegate {
  Q_OBJECT

  Q_PROPERTY(QDate date READ date WRITE setDate NOTIFY dateChanged)
  Q_PROPERTY(QDateTime dateTime READ dateTime WRITE setDateTime NOTIFY dateTimeChanged)
  Q_PROPERTY(QTime time READ time WRITE setTime NOTIFY timeChanged)
  Q_PROPERTY(QVector<QDate> selectedDates READ selectedDates WRITE setSelectedDates NOTIFY
                 selectedDatesChanged)
  Q_PROPERTY(bool multiple READ multiple WRITE setMultiple NOTIFY multipleChanged)
  Q_PROPERTY(bool order READ order WRITE setOrder NOTIFY orderChanged)
  Q_PROPERTY(int maxTagCount READ maxTagCount WRITE setMaxTagCount NOTIFY maxTagCountChanged)
  Q_PROPERTY(bool responsiveMaxTagCount READ responsiveMaxTagCount WRITE setResponsiveMaxTagCount
                 NOTIFY responsiveMaxTagCountChanged)
  Q_PROPERTY(AdDatePickerPanel::PickerMode pickerMode READ pickerMode WRITE setPickerMode NOTIFY
                 pickerModeChanged)
  Q_PROPERTY(Size size READ size WRITE setSize NOTIFY sizeChanged)
  Q_PROPERTY(Variant variant READ variant WRITE setVariant NOTIFY variantChanged)
  Q_PROPERTY(Status status READ status WRITE setStatus NOTIFY statusChanged)
  Q_PROPERTY(bool allowClear READ allowClear WRITE setAllowClear NOTIFY allowClearChanged)
  Q_PROPERTY(
      bool inputReadOnly READ inputReadOnly WRITE setInputReadOnly NOTIFY inputReadOnlyChanged)
  Q_PROPERTY(QString id READ id WRITE setId NOTIFY idChanged)
  Q_PROPERTY(
      PreviewValue previewValue READ previewValue WRITE setPreviewValue NOTIFY previewValueChanged)
  Q_PROPERTY(bool popupVisible READ popupVisible WRITE setPopupVisible NOTIFY popupVisibleChanged)
  Q_PROPERTY(bool defaultOpen READ defaultOpen WRITE setDefaultOpen NOTIFY defaultOpenChanged)
  Q_PROPERTY(QDate defaultPickerValue READ defaultPickerValue WRITE setDefaultPickerValue NOTIFY
                 defaultPickerValueChanged)
  Q_PROPERTY(QDate pickerValue READ pickerValue WRITE setPickerValue NOTIFY pickerValueChanged)
  Q_PROPERTY(AdDatePickerPanel::PickerMode panelMode READ panelMode WRITE setPanelMode NOTIFY
                 panelModeChanged)
  Q_PROPERTY(bool showToday READ showToday WRITE setShowToday NOTIFY showTodayChanged)
  Q_PROPERTY(bool showWeek READ showWeek WRITE setShowWeek NOTIFY showWeekChanged)
  Q_PROPERTY(bool needConfirm READ needConfirm WRITE setNeedConfirm NOTIFY needConfirmChanged)
  Q_PROPERTY(bool showTime READ showTime WRITE setShowTime NOTIFY showTimeChanged)
  Q_PROPERTY(bool showNow READ showNow WRITE setShowNow NOTIFY showNowChanged)
  Q_PROPERTY(QTime defaultOpenTime READ defaultOpenTime WRITE setDefaultOpenTime NOTIFY
                 defaultOpenTimeChanged)
  Q_PROPERTY(QString timeFormat READ timeFormat WRITE setTimeFormat NOTIFY timeFormatChanged)
  Q_PROPERTY(int hourStep READ hourStep WRITE setHourStep NOTIFY timeStepChanged)
  Q_PROPERTY(int minuteStep READ minuteStep WRITE setMinuteStep NOTIFY timeStepChanged)
  Q_PROPERTY(int secondStep READ secondStep WRITE setSecondStep NOTIFY timeStepChanged)
  Q_PROPERTY(bool hideDisabledOptions READ hideDisabledOptions WRITE setHideDisabledOptions NOTIFY
                 hideDisabledOptionsChanged)
  Q_PROPERTY(bool use12Hours READ use12Hours WRITE setUse12Hours NOTIFY use12HoursChanged)
  Q_PROPERTY(
      bool changeOnScroll READ changeOnScroll WRITE setChangeOnScroll NOTIFY changeOnScrollChanged)
  Q_PROPERTY(bool showHour READ showHour WRITE setShowHour NOTIFY showHourChanged)
  Q_PROPERTY(bool showMinute READ showMinute WRITE setShowMinute NOTIFY showMinuteChanged)
  Q_PROPERTY(bool showSecond READ showSecond WRITE setShowSecond NOTIFY showSecondChanged)
  Q_PROPERTY(bool disabled READ disabled WRITE setDisabled NOTIFY disabledChanged)
  Q_PROPERTY(QDate minDate READ minDate WRITE setMinDate NOTIFY minDateChanged)
  Q_PROPERTY(QDate maxDate READ maxDate WRITE setMaxDate NOTIFY maxDateChanged)
  Q_PROPERTY(
      QString displayFormat READ displayFormat WRITE setDisplayFormat NOTIFY displayFormatChanged)
  Q_PROPERTY(QStringList displayFormats READ displayFormats WRITE setDisplayFormats NOTIFY
                 displayFormatsChanged)
  Q_PROPERTY(bool maskFormat READ maskFormat WRITE setMaskFormat NOTIFY maskFormatChanged)
  Q_PROPERTY(bool preserveInvalidOnBlur READ preserveInvalidOnBlur WRITE setPreserveInvalidOnBlur
                 NOTIFY preserveInvalidOnBlurChanged)
  Q_PROPERTY(QLocale locale READ locale WRITE setLocale NOTIFY localeChanged)
  Q_PROPERTY(QString placeholder READ placeholder WRITE setPlaceholder NOTIFY placeholderChanged)
  Q_PROPERTY(QString prefixText READ prefixText WRITE setPrefixText NOTIFY prefixTextChanged)
  Q_PROPERTY(QString suffixText READ suffixText WRITE setSuffixText NOTIFY suffixTextChanged)
  Q_PROPERTY(adqt::icons::IconRef prefixIconRef READ prefixIconRef WRITE setPrefixIconRef NOTIFY
                 prefixIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef suffixIconRef READ suffixIconRef WRITE setSuffixIconRef NOTIFY
                 suffixIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef feedbackIconRef READ feedbackIconRef WRITE setFeedbackIconRef
                 NOTIFY feedbackIconRefChanged)
  Q_PROPERTY(bool suffixIconVisible READ suffixIconVisible WRITE setSuffixIconVisible NOTIFY
                 suffixIconVisibleChanged)
  Q_PROPERTY(adqt::icons::IconRef clearIconRef READ clearIconRef WRITE setClearIconRef NOTIFY
                 clearIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef prevIconRef READ prevIconRef WRITE setPrevIconRef NOTIFY
                 prevIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef nextIconRef READ nextIconRef WRITE setNextIconRef NOTIFY
                 nextIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef superPrevIconRef READ superPrevIconRef WRITE setSuperPrevIconRef
                 NOTIFY superPrevIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef superNextIconRef READ superNextIconRef WRITE setSuperNextIconRef
                 NOTIFY superNextIconRefChanged)
  Q_PROPERTY(Placement placement READ placement WRITE setPlacement NOTIFY placementChanged)
  Q_PROPERTY(adqt::widgets::AdPopupLayerMode popupLayerMode READ popupLayerMode WRITE
                 setPopupLayerMode NOTIFY popupLayerModeChanged)

 public:
  enum class Size {
    Large,
    Middle,
    Small,
  };
  Q_ENUM(Size)

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

  enum class Placement {
    BottomLeft,
    BottomRight,
    TopLeft,
    TopRight,
  };
  Q_ENUM(Placement)

  enum class PreviewValue {
    Disabled,
    Hover,
  };
  Q_ENUM(PreviewValue)

  enum class PanelComponentRole {
    Single,
    RangeStart,
    RangeEnd,
  };
  Q_ENUM(PanelComponentRole)

  using PickerMode = AdDatePickerPanel::PickerMode;
  using ComponentTokens = AdDatePickerPanel::ComponentTokens;
  using SemanticSlotStyle = AdDatePickerPanel::SemanticSlotStyle;
  using PanelSemanticStyles = AdDatePickerPanel::SemanticStyles;
  using PresetItem = AdDatePickerPanel::PresetItem;
  using DatePredicate = AdDatePickerPanel::DatePredicate;
  using DisabledDateContext = AdDatePickerPanel::DisabledDateContext;
  using DisabledDatePredicate = AdDatePickerPanel::DisabledDatePredicate;
  using DisabledTimeContext = AdDatePickerPanel::DisabledTimeContext;
  using DisabledTimePredicate = AdDatePickerPanel::DisabledTimePredicate;
  using TimeSelectionPart = AdDatePickerPanel::TimeSelectionPart;
  using CellRenderInfo = AdDatePickerPanel::CellRenderInfo;
  using CellRenderCallback = AdDatePickerPanel::CellRenderCallback;
  using DisplayTextCallback = std::function<QString(const QDate&, const QTime&)>;
  using PopupContentWrapperFactory = std::function<QWidget*(QWidget*, QWidget*)>;
  using PopupLayerMode = AdPopupLayerMode;

  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle prefix;
    SemanticSlotStyle input;
    SemanticSlotStyle suffix;
    PanelSemanticStyles popup;
  };

  struct StyleContext {
    PickerMode pickerMode = PickerMode::Date;
    Size size = Size::Middle;
    Variant variant = Variant::Outlined;
    Status status = Status::None;
    bool disabled = false;
    bool popupVisible = false;
    bool multiple = false;
    bool showTime = false;
    bool needConfirm = false;
    QDate date;
    QVector<QDate> selectedDates;
  };

  using SemanticStyleResolver = std::function<SemanticStyles(const StyleContext&)>;

  struct PanelComponentContext {
    AdDatePickerPanel* originPanel = nullptr;
    PanelComponentRole role = PanelComponentRole::Single;
    PickerMode pickerMode = PickerMode::Date;
    PickerMode panelMode = PickerMode::Date;
    QDate selectedDate;
    QVector<QDate> selectedDates;
    QDate rangeStartDate;
    QDate rangeEndDate;
    QDate viewDate;
    bool range = false;
    bool multiple = false;
    bool disabled = false;
    std::function<void(const QDate&)> selectDate;
    std::function<void(const QDate&)> previewDate;
    std::function<void(const QDate&)> setViewDate;
    std::function<void(PickerMode)> setPanelMode;
    std::function<void()> acceptSelection;
  };

  using PanelComponentFactory =
      std::function<QWidget*(const PanelComponentContext&, QWidget* parent)>;

  explicit AdDatePicker(QWidget* parent = nullptr);
  ~AdDatePicker() override;

  QDate date() const;
  void setDate(const QDate& value);

  QDateTime dateTime() const;
  void setDateTime(const QDateTime& value);

  QTime time() const;
  void setTime(const QTime& value);

  QVector<QDate> selectedDates() const;
  void setSelectedDates(const QVector<QDate>& values);

  bool multiple() const;
  void setMultiple(bool value);

  bool order() const;
  void setOrder(bool value);

  int maxTagCount() const;
  void setMaxTagCount(int value);

  bool responsiveMaxTagCount() const;
  void setResponsiveMaxTagCount(bool value);

  PickerMode pickerMode() const;
  void setPickerMode(PickerMode value);

  Size size() const;
  void setSize(Size value);

  Variant variant() const;
  void setVariant(Variant value);

  Status status() const;
  void setStatus(Status value);

  bool allowClear() const;
  void setAllowClear(bool value);

  bool inputReadOnly() const;
  void setInputReadOnly(bool value);

  QString id() const;
  void setId(const QString& value);

  PreviewValue previewValue() const;
  void setPreviewValue(PreviewValue value);

  bool popupVisible() const;
  void setPopupVisible(bool value);
  void showPopup();
  void hidePopup();

  bool defaultOpen() const;
  void setDefaultOpen(bool value);

  QDate defaultPickerValue() const;
  void setDefaultPickerValue(const QDate& value);

  QDate pickerValue() const;
  void setPickerValue(const QDate& value);

  PickerMode panelMode() const;
  void setPanelMode(PickerMode value);

  bool showToday() const;
  void setShowToday(bool value);

  bool showWeek() const;
  void setShowWeek(bool value);

  bool needConfirm() const;
  void setNeedConfirm(bool value);

  bool showTime() const;
  void setShowTime(bool value);

  bool showNow() const;
  void setShowNow(bool value);

  QTime defaultOpenTime() const;
  void setDefaultOpenTime(const QTime& value);

  QString timeFormat() const;
  void setTimeFormat(const QString& value);
  int hourStep() const;
  void setHourStep(int value);
  int minuteStep() const;
  void setMinuteStep(int value);
  int secondStep() const;
  void setSecondStep(int value);
  void setTimeSteps(int hourStep, int minuteStep, int secondStep);
  bool hideDisabledOptions() const;
  void setHideDisabledOptions(bool value);
  bool use12Hours() const;
  void setUse12Hours(bool value);
  bool changeOnScroll() const;
  void setChangeOnScroll(bool value);
  bool showHour() const;
  void setShowHour(bool value);
  bool showMinute() const;
  void setShowMinute(bool value);
  bool showSecond() const;
  void setShowSecond(bool value);
  void resetShowSecond();
  void setVisibleTimeColumns(bool hour, bool minute, bool second);

  bool disabled() const;
  void setDisabled(bool value);

  QDate minDate() const;
  void setMinDate(const QDate& value);

  QDate maxDate() const;
  void setMaxDate(const QDate& value);

  QString displayFormat() const;
  void setDisplayFormat(const QString& value);

  QStringList displayFormats() const;
  void setDisplayFormats(const QStringList& values);

  bool maskFormat() const;
  void setMaskFormat(bool value);

  bool preserveInvalidOnBlur() const;
  void setPreserveInvalidOnBlur(bool value);

  QLocale locale() const;
  void setLocale(const QLocale& value);

  QString placeholder() const;
  void setPlaceholder(const QString& value);

  QString prefixText() const;
  void setPrefixText(const QString& value);

  QString suffixText() const;
  void setSuffixText(const QString& value);

  adqt::icons::IconRef prefixIconRef() const;
  void setPrefixIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef suffixIconRef() const;
  void setSuffixIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef feedbackIconRef() const;
  void setFeedbackIconRef(const adqt::icons::IconRef& value);

  bool suffixIconVisible() const;
  void setSuffixIconVisible(bool value);

  adqt::icons::IconRef clearIconRef() const;
  void setClearIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef prevIconRef() const;
  void setPrevIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef nextIconRef() const;
  void setNextIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef superPrevIconRef() const;
  void setSuperPrevIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef superNextIconRef() const;
  void setSuperNextIconRef(const adqt::icons::IconRef& value);

  Placement placement() const;
  void setPlacement(Placement value);

  PopupLayerMode popupLayerMode() const override;
  void setPopupLayerMode(PopupLayerMode value);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& styles);
  void setSemanticStyleResolver(SemanticStyleResolver resolver);
  void clearSemanticStyleResolver();

  QVector<PresetItem> presets() const;
  void setPresets(const QVector<PresetItem>& presets);
  void clearPresets();

  QWidget* extraFooterWidget() const;
  void setExtraFooterWidget(QWidget* widget);
  QWidget* takeExtraFooterWidget();

  DatePredicate disabledDatePredicate() const;
  void setDisabledDatePredicate(DatePredicate predicate);

  DisabledDatePredicate disabledDateContextPredicate() const;
  void setDisabledDateContextPredicate(DisabledDatePredicate predicate);

  DisabledTimePredicate disabledTimePredicate() const;
  void setDisabledTimePredicate(DisabledTimePredicate predicate);

  DisplayTextCallback displayTextCallback() const;
  void setDisplayTextCallback(DisplayTextCallback callback);
  void clearDisplayTextCallback();

  CellRenderCallback cellRenderCallback() const;
  void setCellRenderCallback(CellRenderCallback callback);
  void clearCellRenderCallback();

  PopupContentWrapperFactory popupContentWrapperFactory() const;
  void setPopupContentWrapperFactory(PopupContentWrapperFactory factory);
  void clearPopupContentWrapperFactory();

  PanelComponentFactory panelComponentFactory() const;
  void setPanelComponentFactory(PanelComponentFactory factory);
  void clearPanelComponentFactory();

  void focus();
  void blur();

  AdLineEdit* lineEdit() const;
  AdDatePickerPanel* panel() const;

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 signals:
  void dateChanged(const QDate& value);
  void dateTimeChanged(const QDateTime& value);
  void timeChanged(const QTime& value);
  void selectedDatesChanged(const QVector<QDate>& values);
  void multipleChanged(bool value);
  void orderChanged(bool value);
  void maxTagCountChanged(int value);
  void responsiveMaxTagCountChanged(bool value);
  void pickerModeChanged(PickerMode value);
  void sizeChanged(Size value);
  void variantChanged(Variant value);
  void statusChanged(Status value);
  void allowClearChanged(bool value);
  void inputReadOnlyChanged(bool value);
  void idChanged(const QString& value);
  void previewValueChanged(PreviewValue value);
  void popupVisibleChanged(bool value);
  void defaultOpenChanged(bool value);
  void defaultPickerValueChanged(const QDate& value);
  void pickerValueChanged(const QDate& value);
  void panelModeChanged(PickerMode value);
  void showTodayChanged(bool value);
  void showWeekChanged(bool value);
  void needConfirmChanged(bool value);
  void showTimeChanged(bool value);
  void showNowChanged(bool value);
  void defaultOpenTimeChanged(const QTime& value);
  void timeFormatChanged(const QString& value);
  void timeStepChanged(int hourStep, int minuteStep, int secondStep);
  void hideDisabledOptionsChanged(bool value);
  void use12HoursChanged(bool value);
  void changeOnScrollChanged(bool value);
  void showHourChanged(bool value);
  void showMinuteChanged(bool value);
  void showSecondChanged(bool value);
  void disabledChanged(bool value);
  void minDateChanged(const QDate& value);
  void maxDateChanged(const QDate& value);
  void displayFormatChanged(const QString& value);
  void displayFormatsChanged(const QStringList& values);
  void maskFormatChanged(bool value);
  void preserveInvalidOnBlurChanged(bool value);
  void localeChanged(const QLocale& value);
  void placeholderChanged(const QString& value);
  void prefixTextChanged(const QString& value);
  void suffixTextChanged(const QString& value);
  void prefixIconRefChanged(const adqt::icons::IconRef& value);
  void suffixIconRefChanged(const adqt::icons::IconRef& value);
  void feedbackIconRefChanged(const adqt::icons::IconRef& value);
  void suffixIconVisibleChanged(bool value);
  void clearIconRefChanged(const adqt::icons::IconRef& value);
  void prevIconRefChanged(const adqt::icons::IconRef& value);
  void nextIconRefChanged(const adqt::icons::IconRef& value);
  void superPrevIconRefChanged(const adqt::icons::IconRef& value);
  void superNextIconRefChanged(const adqt::icons::IconRef& value);
  void placementChanged(Placement value);
  void popupLayerModeChanged(PopupLayerMode value);
  void componentTokensChanged();
  void semanticStylesChanged();
  void presetsChanged();
  void extraFooterWidgetChanged(QWidget* widget);
  void displayTextCallbackChanged();
  void cellRenderCallbackChanged();
  void popupContentWrapperFactoryChanged();
  void panelComponentFactoryChanged();
  void panelChanged(const QDate& viewDate, PickerMode mode);
  void focused();
  void blurred();
  void cleared();
  void accepted(const QDate& value);
  void acceptedDateTime(const QDateTime& value);
  void datesAccepted(const QVector<QDate>& values);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void changeEvent(QEvent* event) override;
  void moveEvent(QMoveEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  void buildUi();
  void ensurePopup();
  void destroyPopup();
  void applyPopupLayerMode();
  void syncPopupGeometry();
  void schedulePopupFocusOutClose();
  void setPopupVisibleInternal(bool value, bool emitSignal);
  void syncLineEdit();
  void syncLineEditStyle();
  void syncLineEditMask();
  void syncInputIds();
  SemanticStyles effectiveSemanticStyles() const;
  PanelComponentContext makePanelComponentContext(AdDatePickerPanel* panel,
                                                  PanelComponentRole role);
  QWidget* createPanelComponentWidget(QWidget* parent, AdDatePickerPanel* panel,
                                      PanelComponentRole role);
  void selectPanelComponentDate(const QDate& value);
  void acceptPanelComponentSelection();
  bool syncPopupPanelSelectionText();
  void handlePopupSelectedTimeChanged(const QTime& value);
  void handlePreviewDateChanged(const QDate& value);
  void handlePreviewTimeChanged(const QTime& value, TimeSelectionPart part);
  void clearPreviewText();
  void syncPanelState();
  void commitInputText();
  void clearDateInternal(bool emitSignals);
  QVector<QDate> normalizedDates(const QVector<QDate>& values) const;
  QStringList effectiveMultipleDisplayTexts(const QVector<QDate>& values) const;
  QString effectiveDisplayText(const QDate& value) const;
  QString effectivePlaceholder() const;
  QString defaultDisplayFormat() const;
  QStringList effectiveParseFormats() const;
  bool effectiveTextIncludesTime() const;
  bool effectiveUse12Hours() const;
  QString effectiveTimeFormat() const;
  bool effectiveShowSecondColumn() const;
  Qt::DayOfWeek effectiveFirstDayOfWeek() const;
  bool effectiveShowWeek() const;
  PickerMode normalizedPanelMode(PickerMode value) const;
  PickerMode effectivePanelMode() const;
  QDateTime parseText(const QString& text, bool* ok) const;
  QDateTime parseMaskedText(const QString& text, bool* ok) const;
  bool isDateSelectable(const QDate& value, const QDate& from = QDate()) const;
  bool isDateTimeSelectable(const QDateTime& value) const;

  void handleControllerPopupVisibleChanged(bool value);

  QObject* popupOwnerObject() const override;
  QWidget* popupAnchorWidget() const override;
  QWidget* popupScopeWindow() const override;
  QWidget* popupSurfaceWidget() const override;
  QWidget* popupEnsureSurface() override;
  void popupPrepareToShow() override;
  bool popupHasContent() const override;
  detail::OverlayPopupPlacement popupPlacement() const override;
  bool popupAutoAdjustOverflow() const override;
  bool popupArrowVisible() const override;
  bool popupArrowPointAtCenter() const override;
  int popupOffset() const override;
  int popupArrowOffsetHorizontal() const override;
  int popupArrowOffsetVertical() const override;
  void popupApplyResolvedPlacement(detail::OverlayPopupPlacement placement,
                                   qreal arrowCenterCoord) override;

  QDate date_;
  QTime time_ = QTime(0, 0, 0);
  QVector<QDate> selectedDates_;
  PickerMode pickerMode_ = PickerMode::Date;
  Size size_ = Size::Middle;
  Variant variant_ = Variant::Outlined;
  Status status_ = Status::None;
  bool allowClear_ = true;
  bool inputReadOnly_ = false;
  PreviewValue previewValue_ = PreviewValue::Hover;
  bool popupVisible_ = false;
  bool defaultOpen_ = false;
  bool defaultOpenApplied_ = false;
  bool showToday_ = true;
  bool showWeek_ = false;
  bool showWeekExplicit_ = false;
  bool needConfirm_ = false;
  bool showTime_ = false;
  bool showNow_ = true;
  QTime defaultOpenTime_ = QTime(0, 0, 0);
  bool multiple_ = false;
  bool order_ = true;
  int maxTagCount_ = -1;
  bool responsiveMaxTagCount_ = false;
  QString timeFormat_;
  int hourStep_ = 1;
  int minuteStep_ = 1;
  int secondStep_ = 1;
  bool hideDisabledOptions_ = false;
  bool use12Hours_ = false;
  bool changeOnScroll_ = false;
  bool showHour_ = true;
  bool showMinute_ = true;
  bool showSecond_ = true;
  bool showSecondExplicit_ = false;
  QDate minDate_;
  QDate maxDate_;
  QDate defaultPickerValue_;
  QDate pickerValue_;
  PickerMode panelMode_ = PickerMode::Date;
  bool panelModeExplicit_ = false;
  QString displayFormat_;
  QStringList displayFormats_;
  bool maskFormat_ = false;
  bool preserveInvalidOnBlur_ = false;
  QLocale locale_;
  bool localeExplicit_ = false;
  bool applyingGlobalLocale_ = false;
  QString id_;
  QString placeholder_;
  QString prefixText_;
  QString suffixText_;
  adqt::icons::IconRef prefixIconRef_;
  adqt::icons::IconRef suffixIconRef_;
  adqt::icons::IconRef feedbackIconRef_;
  bool suffixIconVisible_ = true;
  adqt::icons::IconRef clearIconRef_;
  adqt::icons::IconRef prevIconRef_;
  adqt::icons::IconRef nextIconRef_;
  adqt::icons::IconRef superPrevIconRef_;
  adqt::icons::IconRef superNextIconRef_;
  Placement placement_ = Placement::BottomLeft;
  PopupLayerMode popupLayerMode_ = PopupLayerMode::InWindow;
  ComponentTokens componentTokens_;
  SemanticStyles semanticStyles_;
  SemanticStyleResolver semanticStyleResolver_;
  QVector<PresetItem> presets_;
  DatePredicate disabledDatePredicate_;
  DisabledDatePredicate disabledDateContextPredicate_;
  DisabledTimePredicate disabledTimePredicate_;
  DisplayTextCallback displayTextCallback_;
  CellRenderCallback cellRenderCallback_;
  bool panelDisabledDatePredicateDirty_ = true;
  bool panelDisabledDateContextPredicateDirty_ = true;
  bool panelDisabledTimePredicateDirty_ = true;
  bool panelCellRenderCallbackDirty_ = true;
  PopupContentWrapperFactory popupContentWrapperFactory_;
  PanelComponentFactory panelComponentFactory_;
  QPointer<QWidget> extraFooterWidget_;
  bool suppressPopupHideClose_ = false;
  bool syncingPopupPanel_ = false;
  bool syncingText_ = false;
  bool suppressInputCommitOnFocusOut_ = false;
  QDate previewDate_;
  QTime previewTime_;
  bool previewTimeActive_ = false;

  QHBoxLayout* rootLayout_ = nullptr;
  detail::DatePickerLineEdit* lineEdit_ = nullptr;
  detail::OverlayPopupController* popupController_ = nullptr;
  QWidget* popup_ = nullptr;
  QWidget* popupBodyHost_ = nullptr;
  QVBoxLayout* popupLayout_ = nullptr;
  QWidget* popupContentWidget_ = nullptr;
  AdDatePickerPanel* popupPanel_ = nullptr;
};

class AdDateRangePicker final : public QWidget, private detail::OverlayPopupControllerDelegate {
  Q_OBJECT

  Q_PROPERTY(QDate startDate READ startDate WRITE setStartDate NOTIFY rangeChanged)
  Q_PROPERTY(QDate endDate READ endDate WRITE setEndDate NOTIFY rangeChanged)
  Q_PROPERTY(
      QDateTime startDateTime READ startDateTime WRITE setStartDateTime NOTIFY dateTimeRangeChanged)
  Q_PROPERTY(
      QDateTime endDateTime READ endDateTime WRITE setEndDateTime NOTIFY dateTimeRangeChanged)
  Q_PROPERTY(QTime startTime READ startTime WRITE setStartTime NOTIFY timeRangeChanged)
  Q_PROPERTY(QTime endTime READ endTime WRITE setEndTime NOTIFY timeRangeChanged)
  Q_PROPERTY(AdDatePickerPanel::PickerMode pickerMode READ pickerMode WRITE setPickerMode NOTIFY
                 pickerModeChanged)
  Q_PROPERTY(AdDatePicker::Size size READ size WRITE setSize NOTIFY sizeChanged)
  Q_PROPERTY(AdDatePicker::Variant variant READ variant WRITE setVariant NOTIFY variantChanged)
  Q_PROPERTY(AdDatePicker::Status status READ status WRITE setStatus NOTIFY statusChanged)
  Q_PROPERTY(bool allowClear READ allowClear WRITE setAllowClear NOTIFY allowClearChanged)
  Q_PROPERTY(
      bool inputReadOnly READ inputReadOnly WRITE setInputReadOnly NOTIFY inputReadOnlyChanged)
  Q_PROPERTY(QString id READ id WRITE setId NOTIFY idChanged)
  Q_PROPERTY(QString startId READ startId WRITE setStartId NOTIFY startIdChanged)
  Q_PROPERTY(QString endId READ endId WRITE setEndId NOTIFY endIdChanged)
  Q_PROPERTY(AdDatePicker::PreviewValue previewValue READ previewValue WRITE setPreviewValue NOTIFY
                 previewValueChanged)
  Q_PROPERTY(bool popupVisible READ popupVisible WRITE setPopupVisible NOTIFY popupVisibleChanged)
  Q_PROPERTY(bool defaultOpen READ defaultOpen WRITE setDefaultOpen NOTIFY defaultOpenChanged)
  Q_PROPERTY(QDate defaultPickerValue READ defaultPickerValue WRITE setDefaultPickerValue NOTIFY
                 defaultPickerValueChanged)
  Q_PROPERTY(QDate pickerValue READ pickerValue WRITE setPickerValue NOTIFY pickerValueChanged)
  Q_PROPERTY(AdDatePickerPanel::PickerMode panelMode READ panelMode WRITE setPanelMode NOTIFY
                 panelModeChanged)
  Q_PROPERTY(bool order READ order WRITE setOrder NOTIFY orderChanged)
  Q_PROPERTY(bool needConfirm READ needConfirm WRITE setNeedConfirm NOTIFY needConfirmChanged)
  Q_PROPERTY(bool showTime READ showTime WRITE setShowTime NOTIFY showTimeChanged)
  Q_PROPERTY(bool showToday READ showToday WRITE setShowToday NOTIFY showTodayChanged)
  Q_PROPERTY(bool showNow READ showNow WRITE setShowNow NOTIFY showNowChanged)
  Q_PROPERTY(QTime defaultOpenStartTime READ defaultOpenStartTime WRITE setDefaultOpenStartTime
                 NOTIFY defaultOpenTimeRangeChanged)
  Q_PROPERTY(QTime defaultOpenEndTime READ defaultOpenEndTime WRITE setDefaultOpenEndTime NOTIFY
                 defaultOpenTimeRangeChanged)
  Q_PROPERTY(QString timeFormat READ timeFormat WRITE setTimeFormat NOTIFY timeFormatChanged)
  Q_PROPERTY(int hourStep READ hourStep WRITE setHourStep NOTIFY timeStepChanged)
  Q_PROPERTY(int minuteStep READ minuteStep WRITE setMinuteStep NOTIFY timeStepChanged)
  Q_PROPERTY(int secondStep READ secondStep WRITE setSecondStep NOTIFY timeStepChanged)
  Q_PROPERTY(bool hideDisabledOptions READ hideDisabledOptions WRITE setHideDisabledOptions NOTIFY
                 hideDisabledOptionsChanged)
  Q_PROPERTY(bool use12Hours READ use12Hours WRITE setUse12Hours NOTIFY use12HoursChanged)
  Q_PROPERTY(
      bool changeOnScroll READ changeOnScroll WRITE setChangeOnScroll NOTIFY changeOnScrollChanged)
  Q_PROPERTY(bool showHour READ showHour WRITE setShowHour NOTIFY showHourChanged)
  Q_PROPERTY(bool showMinute READ showMinute WRITE setShowMinute NOTIFY showMinuteChanged)
  Q_PROPERTY(bool showSecond READ showSecond WRITE setShowSecond NOTIFY showSecondChanged)
  Q_PROPERTY(bool allowEmptyStart READ allowEmptyStart WRITE setAllowEmptyStart NOTIFY
                 allowEmptyStartChanged)
  Q_PROPERTY(
      bool allowEmptyEnd READ allowEmptyEnd WRITE setAllowEmptyEnd NOTIFY allowEmptyEndChanged)
  Q_PROPERTY(
      bool startDisabled READ startDisabled WRITE setStartDisabled NOTIFY startDisabledChanged)
  Q_PROPERTY(bool endDisabled READ endDisabled WRITE setEndDisabled NOTIFY endDisabledChanged)
  Q_PROPERTY(bool disabled READ disabled WRITE setDisabled NOTIFY disabledChanged)
  Q_PROPERTY(QDate minDate READ minDate WRITE setMinDate NOTIFY minDateChanged)
  Q_PROPERTY(QDate maxDate READ maxDate WRITE setMaxDate NOTIFY maxDateChanged)
  Q_PROPERTY(
      QString displayFormat READ displayFormat WRITE setDisplayFormat NOTIFY displayFormatChanged)
  Q_PROPERTY(QStringList displayFormats READ displayFormats WRITE setDisplayFormats NOTIFY
                 displayFormatsChanged)
  Q_PROPERTY(bool maskFormat READ maskFormat WRITE setMaskFormat NOTIFY maskFormatChanged)
  Q_PROPERTY(bool preserveInvalidOnBlur READ preserveInvalidOnBlur WRITE setPreserveInvalidOnBlur
                 NOTIFY preserveInvalidOnBlurChanged)
  Q_PROPERTY(QLocale locale READ locale WRITE setLocale NOTIFY localeChanged)
  Q_PROPERTY(QString placeholder READ placeholder WRITE setPlaceholder NOTIFY placeholderChanged)
  Q_PROPERTY(QString startPlaceholder READ startPlaceholder WRITE setStartPlaceholder NOTIFY
                 startPlaceholderChanged)
  Q_PROPERTY(QString endPlaceholder READ endPlaceholder WRITE setEndPlaceholder NOTIFY
                 endPlaceholderChanged)
  Q_PROPERTY(QString separator READ separator WRITE setSeparator NOTIFY separatorChanged)
  Q_PROPERTY(QString prefixText READ prefixText WRITE setPrefixText NOTIFY prefixTextChanged)
  Q_PROPERTY(QString suffixText READ suffixText WRITE setSuffixText NOTIFY suffixTextChanged)
  Q_PROPERTY(adqt::icons::IconRef prefixIconRef READ prefixIconRef WRITE setPrefixIconRef NOTIFY
                 prefixIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef suffixIconRef READ suffixIconRef WRITE setSuffixIconRef NOTIFY
                 suffixIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef feedbackIconRef READ feedbackIconRef WRITE setFeedbackIconRef
                 NOTIFY feedbackIconRefChanged)
  Q_PROPERTY(bool suffixIconVisible READ suffixIconVisible WRITE setSuffixIconVisible NOTIFY
                 suffixIconVisibleChanged)
  Q_PROPERTY(adqt::icons::IconRef clearIconRef READ clearIconRef WRITE setClearIconRef NOTIFY
                 clearIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef prevIconRef READ prevIconRef WRITE setPrevIconRef NOTIFY
                 prevIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef nextIconRef READ nextIconRef WRITE setNextIconRef NOTIFY
                 nextIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef superPrevIconRef READ superPrevIconRef WRITE setSuperPrevIconRef
                 NOTIFY superPrevIconRefChanged)
  Q_PROPERTY(adqt::icons::IconRef superNextIconRef READ superNextIconRef WRITE setSuperNextIconRef
                 NOTIFY superNextIconRefChanged)
  Q_PROPERTY(
      AdDatePicker::Placement placement READ placement WRITE setPlacement NOTIFY placementChanged)
  Q_PROPERTY(adqt::widgets::AdPopupLayerMode popupLayerMode READ popupLayerMode WRITE
                 setPopupLayerMode NOTIFY popupLayerModeChanged)

 public:
  enum class RangePart {
    Start,
    End,
  };
  Q_ENUM(RangePart)

  using PickerMode = AdDatePickerPanel::PickerMode;
  using Size = AdDatePicker::Size;
  using Variant = AdDatePicker::Variant;
  using Status = AdDatePicker::Status;
  using Placement = AdDatePicker::Placement;
  using PreviewValue = AdDatePicker::PreviewValue;
  using ComponentTokens = AdDatePickerPanel::ComponentTokens;
  using SemanticSlotStyle = AdDatePicker::SemanticSlotStyle;
  using PanelSemanticStyles = AdDatePicker::PanelSemanticStyles;
  using SemanticStyles = AdDatePicker::SemanticStyles;
  using PresetItem = AdDatePickerPanel::PresetItem;
  using DatePredicate = AdDatePickerPanel::DatePredicate;
  using DisabledDateContext = AdDatePickerPanel::DisabledDateContext;
  using DisabledDatePredicate = AdDatePickerPanel::DisabledDatePredicate;
  using DisabledTimeContext = AdDatePickerPanel::DisabledTimeContext;
  using DisabledTimePredicate = AdDatePickerPanel::DisabledTimePredicate;
  using TimeSelectionPart = AdDatePickerPanel::TimeSelectionPart;
  using CellRenderInfo = AdDatePickerPanel::CellRenderInfo;
  using CellRenderCallback = AdDatePickerPanel::CellRenderCallback;
  using DisplayTextCallback = AdDatePicker::DisplayTextCallback;
  using PopupContentWrapperFactory = AdDatePicker::PopupContentWrapperFactory;
  using PanelComponentRole = AdDatePicker::PanelComponentRole;
  using PanelComponentContext = AdDatePicker::PanelComponentContext;
  using PanelComponentFactory = AdDatePicker::PanelComponentFactory;
  using PopupLayerMode = AdPopupLayerMode;

  struct StyleContext {
    PickerMode pickerMode = PickerMode::Date;
    Size size = Size::Middle;
    Variant variant = Variant::Outlined;
    Status status = Status::None;
    bool disabled = false;
    bool popupVisible = false;
    bool showTime = false;
    bool needConfirm = false;
    RangePart activeRange = RangePart::Start;
    QDate startDate;
    QDate endDate;
  };

  using SemanticStyleResolver = std::function<SemanticStyles(const StyleContext&)>;

  explicit AdDateRangePicker(QWidget* parent = nullptr);
  ~AdDateRangePicker() override;

  QDate startDate() const;
  void setStartDate(const QDate& value);

  QDate endDate() const;
  void setEndDate(const QDate& value);

  void setRange(const QDate& start, const QDate& end);
  void clear();

  QDateTime startDateTime() const;
  void setStartDateTime(const QDateTime& value);

  QDateTime endDateTime() const;
  void setEndDateTime(const QDateTime& value);

  void setDateTimeRange(const QDateTime& start, const QDateTime& end);

  QTime startTime() const;
  void setStartTime(const QTime& value);

  QTime endTime() const;
  void setEndTime(const QTime& value);

  void setTimeRange(const QTime& start, const QTime& end);

  PickerMode pickerMode() const;
  void setPickerMode(PickerMode value);

  Size size() const;
  void setSize(Size value);

  Variant variant() const;
  void setVariant(Variant value);

  Status status() const;
  void setStatus(Status value);

  bool allowClear() const;
  void setAllowClear(bool value);

  bool inputReadOnly() const;
  void setInputReadOnly(bool value);

  QString id() const;
  void setId(const QString& value);

  QString startId() const;
  void setStartId(const QString& value);

  QString endId() const;
  void setEndId(const QString& value);

  void setRangeIds(const QString& start, const QString& end);

  PreviewValue previewValue() const;
  void setPreviewValue(PreviewValue value);

  bool popupVisible() const;
  void setPopupVisible(bool value);
  void showPopup();
  void hidePopup();

  bool defaultOpen() const;
  void setDefaultOpen(bool value);

  QDate defaultPickerValue() const;
  void setDefaultPickerValue(const QDate& value);

  QDate pickerValue() const;
  void setPickerValue(const QDate& value);

  PickerMode panelMode() const;
  void setPanelMode(PickerMode value);

  bool order() const;
  void setOrder(bool value);

  bool needConfirm() const;
  void setNeedConfirm(bool value);

  bool showTime() const;
  void setShowTime(bool value);

  bool showToday() const;
  void setShowToday(bool value);

  bool showNow() const;
  void setShowNow(bool value);

  QTime defaultOpenStartTime() const;
  void setDefaultOpenStartTime(const QTime& value);

  QTime defaultOpenEndTime() const;
  void setDefaultOpenEndTime(const QTime& value);

  void setDefaultOpenTimeRange(const QTime& start, const QTime& end);

  QString timeFormat() const;
  void setTimeFormat(const QString& value);
  int hourStep() const;
  void setHourStep(int value);
  int minuteStep() const;
  void setMinuteStep(int value);
  int secondStep() const;
  void setSecondStep(int value);
  void setTimeSteps(int hourStep, int minuteStep, int secondStep);
  bool hideDisabledOptions() const;
  void setHideDisabledOptions(bool value);
  bool use12Hours() const;
  void setUse12Hours(bool value);
  bool changeOnScroll() const;
  void setChangeOnScroll(bool value);
  bool showHour() const;
  void setShowHour(bool value);
  bool showMinute() const;
  void setShowMinute(bool value);
  bool showSecond() const;
  void setShowSecond(bool value);
  void resetShowSecond();
  void setVisibleTimeColumns(bool hour, bool minute, bool second);

  bool allowEmptyStart() const;
  void setAllowEmptyStart(bool value);

  bool allowEmptyEnd() const;
  void setAllowEmptyEnd(bool value);

  void setAllowEmpty(bool start, bool end);

  bool startDisabled() const;
  void setStartDisabled(bool value);

  bool endDisabled() const;
  void setEndDisabled(bool value);

  void setDisabledRange(bool start, bool end);

  bool disabled() const;
  void setDisabled(bool value);

  QDate minDate() const;
  void setMinDate(const QDate& value);

  QDate maxDate() const;
  void setMaxDate(const QDate& value);

  QString displayFormat() const;
  void setDisplayFormat(const QString& value);

  QStringList displayFormats() const;
  void setDisplayFormats(const QStringList& values);

  bool maskFormat() const;
  void setMaskFormat(bool value);

  bool preserveInvalidOnBlur() const;
  void setPreserveInvalidOnBlur(bool value);

  QLocale locale() const;
  void setLocale(const QLocale& value);

  QString placeholder() const;
  void setPlaceholder(const QString& value);

  QString startPlaceholder() const;
  void setStartPlaceholder(const QString& value);

  QString endPlaceholder() const;
  void setEndPlaceholder(const QString& value);

  void setRangePlaceholders(const QString& start, const QString& end);

  QString separator() const;
  void setSeparator(const QString& value);

  QString prefixText() const;
  void setPrefixText(const QString& value);

  QString suffixText() const;
  void setSuffixText(const QString& value);

  adqt::icons::IconRef prefixIconRef() const;
  void setPrefixIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef suffixIconRef() const;
  void setSuffixIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef feedbackIconRef() const;
  void setFeedbackIconRef(const adqt::icons::IconRef& value);

  bool suffixIconVisible() const;
  void setSuffixIconVisible(bool value);

  adqt::icons::IconRef clearIconRef() const;
  void setClearIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef prevIconRef() const;
  void setPrevIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef nextIconRef() const;
  void setNextIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef superPrevIconRef() const;
  void setSuperPrevIconRef(const adqt::icons::IconRef& value);

  adqt::icons::IconRef superNextIconRef() const;
  void setSuperNextIconRef(const adqt::icons::IconRef& value);

  Placement placement() const;
  void setPlacement(Placement value);

  PopupLayerMode popupLayerMode() const override;
  void setPopupLayerMode(PopupLayerMode value);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& tokens);
  void resetComponentTokens();

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& styles);
  void setSemanticStyleResolver(SemanticStyleResolver resolver);
  void clearSemanticStyleResolver();

  QVector<PresetItem> presets() const;
  void setPresets(const QVector<PresetItem>& presets);
  void clearPresets();

  QWidget* extraFooterWidget() const;
  void setExtraFooterWidget(QWidget* widget);
  QWidget* takeExtraFooterWidget();

  DatePredicate disabledDatePredicate() const;
  void setDisabledDatePredicate(DatePredicate predicate);

  DisabledDatePredicate disabledDateContextPredicate() const;
  void setDisabledDateContextPredicate(DisabledDatePredicate predicate);

  DisabledTimePredicate disabledTimePredicate() const;
  void setDisabledTimePredicate(DisabledTimePredicate predicate);

  DisplayTextCallback displayTextCallback() const;
  void setDisplayTextCallback(DisplayTextCallback callback);
  void clearDisplayTextCallback();

  CellRenderCallback cellRenderCallback() const;
  void setCellRenderCallback(CellRenderCallback callback);
  void clearCellRenderCallback();

  PopupContentWrapperFactory popupContentWrapperFactory() const;
  void setPopupContentWrapperFactory(PopupContentWrapperFactory factory);
  void clearPopupContentWrapperFactory();

  PanelComponentFactory panelComponentFactory() const;
  void setPanelComponentFactory(PanelComponentFactory factory);
  void clearPanelComponentFactory();

  void focus(RangePart range = RangePart::Start);
  void blur();

  AdLineEdit* lineEdit() const;
  AdDatePickerPanel* panel() const;
  AdDatePickerPanel* endPanel() const;

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 signals:
  void rangeChanged(const QDate& start, const QDate& end);
  void dateTimeRangeChanged(const QDateTime& start, const QDateTime& end);
  void timeRangeChanged(const QTime& start, const QTime& end);
  void pickerModeChanged(PickerMode value);
  void sizeChanged(Size value);
  void variantChanged(Variant value);
  void statusChanged(Status value);
  void allowClearChanged(bool value);
  void inputReadOnlyChanged(bool value);
  void idChanged(const QString& value);
  void startIdChanged(const QString& value);
  void endIdChanged(const QString& value);
  void rangeIdsChanged(const QString& start, const QString& end);
  void previewValueChanged(PreviewValue value);
  void popupVisibleChanged(bool value);
  void defaultOpenChanged(bool value);
  void defaultPickerValueChanged(const QDate& value);
  void pickerValueChanged(const QDate& value);
  void panelModeChanged(PickerMode value);
  void orderChanged(bool value);
  void needConfirmChanged(bool value);
  void showTimeChanged(bool value);
  void showTodayChanged(bool value);
  void showNowChanged(bool value);
  void defaultOpenTimeRangeChanged(const QTime& start, const QTime& end);
  void timeFormatChanged(const QString& value);
  void timeStepChanged(int hourStep, int minuteStep, int secondStep);
  void hideDisabledOptionsChanged(bool value);
  void use12HoursChanged(bool value);
  void changeOnScrollChanged(bool value);
  void showHourChanged(bool value);
  void showMinuteChanged(bool value);
  void showSecondChanged(bool value);
  void allowEmptyStartChanged(bool value);
  void allowEmptyEndChanged(bool value);
  void startDisabledChanged(bool value);
  void endDisabledChanged(bool value);
  void disabledChanged(bool value);
  void minDateChanged(const QDate& value);
  void maxDateChanged(const QDate& value);
  void displayFormatChanged(const QString& value);
  void displayFormatsChanged(const QStringList& values);
  void maskFormatChanged(bool value);
  void preserveInvalidOnBlurChanged(bool value);
  void localeChanged(const QLocale& value);
  void placeholderChanged(const QString& value);
  void startPlaceholderChanged(const QString& value);
  void endPlaceholderChanged(const QString& value);
  void rangePlaceholdersChanged(const QString& start, const QString& end);
  void separatorChanged(const QString& value);
  void prefixTextChanged(const QString& value);
  void suffixTextChanged(const QString& value);
  void prefixIconRefChanged(const adqt::icons::IconRef& value);
  void suffixIconRefChanged(const adqt::icons::IconRef& value);
  void feedbackIconRefChanged(const adqt::icons::IconRef& value);
  void suffixIconVisibleChanged(bool value);
  void clearIconRefChanged(const adqt::icons::IconRef& value);
  void prevIconRefChanged(const adqt::icons::IconRef& value);
  void nextIconRefChanged(const adqt::icons::IconRef& value);
  void superPrevIconRefChanged(const adqt::icons::IconRef& value);
  void superNextIconRefChanged(const adqt::icons::IconRef& value);
  void placementChanged(Placement value);
  void popupLayerModeChanged(PopupLayerMode value);
  void componentTokensChanged();
  void semanticStylesChanged();
  void presetsChanged();
  void extraFooterWidgetChanged(QWidget* widget);
  void displayTextCallbackChanged();
  void cellRenderCallbackChanged();
  void popupContentWrapperFactoryChanged();
  void panelComponentFactoryChanged();
  void calendarChanged(const QDate& start, const QDate& end, RangePart range);
  void panelChanged(const QDate& primaryViewDate, const QDate& secondaryViewDate, PickerMode mode);
  void focused(RangePart range);
  void blurred(RangePart range);
  void cleared();
  void accepted(const QDate& start, const QDate& end);
  void acceptedDateTimeRange(const QDateTime& start, const QDateTime& end);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void changeEvent(QEvent* event) override;
  void moveEvent(QMoveEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  void buildUi();
  void ensurePopup();
  void destroyPopup();
  void applyPopupLayerMode();
  void syncPopupGeometry();
  void syncPopupActiveAlignment();
  void syncPopupArrowPosition();
  int popupPanelContainerWidth() const;
  int popupPanelAlignmentOffset(detail::OverlayPopupPlacement placement) const;
  void setPopupVisibleInternal(bool value, bool emitSignal);
  void syncLineEdit();
  void syncLineEditRangeDisplay(const QDate& start, const QDate& end, const QTime& startTime,
                                const QTime& endTime);
  void syncLineEditStyle();
  void syncLineEditMask();
  void syncInputIds();
  SemanticStyles effectiveSemanticStyles() const;
  PanelComponentContext makePanelComponentContext(AdDatePickerPanel* panel,
                                                  PanelComponentRole role);
  QWidget* createPanelComponentWidget(QWidget* parent, AdDatePickerPanel* panel,
                                      PanelComponentRole role);
  void selectPanelComponentDate(PanelComponentRole role, const QDate& value);
  void acceptPanelComponentSelection();
  void handlePreviewDateChanged(const QDate& value);
  void handlePreviewRangeChanged(const QDate& start, const QDate& end);
  void handlePreviewTimeChanged(const QTime& value, TimeSelectionPart part);
  void clearPreviewText();
  void ensureRangePresetsUi();
  void rebuildRangePresets();
  void refreshRangePresets();
  bool effectiveShowNowAction() const;
  void handlePopupNow();
  void refreshRangeFooter();
  void applyRangePreset(const PresetItem& preset);
  void syncPanelState();
  void syncPopupPanelViewsFromPrimary(const QDate& primaryViewDate);
  void syncPopupPanelViewsFromSecondary(const QDate& secondaryViewDate);
  void handlePopupRangeChanged(AdDatePickerPanel* sourcePanel, const QDate& start,
                               const QDate& end);
  void handlePopupRangeTimeChanged(AdDatePickerPanel* sourcePanel, const QTime& start,
                                   const QTime& end);
  void handlePopupRangeAccepted(const QDate& start, const QDate& end);
  void syncPopupCalendarFromCommitted();
  QDate popupCalendarStartDate() const;
  QDate popupCalendarEndDate() const;
  QTime popupCalendarStartTime() const;
  QTime popupCalendarEndTime() const;
  void setActiveRangePart(RangePart range, bool record);
  std::optional<RangePart> nextActiveRangePart(const QDate& start, const QDate& end) const;
  bool canConfirmActiveRangePart(const QDate& start, const QDate& end) const;
  bool confirmActiveRangePart(bool closePopup, bool emitAccepted);
  bool popupNeedsExplicitSubmit() const;
  bool commitPopupCalendarRange(bool closePopup, bool emitAccepted);
  void commitInputText();
  bool canAcceptRange(const QDate& start, const QDate& end) const;
  std::optional<QTime> validTimeForRangePart(const QDate& date, const QTime& preferred,
                                             RangePart range, const QDate& from) const;
  QDate adjustedPrimaryPanelViewDate(const QDate& primaryViewDate) const;
  QDate secondaryPanelViewDate(const QDate& primaryViewDate) const;
  QDate primaryPanelViewDate(const QDate& secondaryViewDate) const;
  QString effectiveDisplayText(const QDate& value) const;
  QString effectiveDisplayText(const QDate& value, const QTime& time) const;
  QString effectiveRangeText() const;
  QString effectiveRangeText(const QDate& start, const QDate& end, const QTime& startTime,
                             const QTime& endTime) const;
  QString effectivePlaceholder() const;
  QString effectiveStartPlaceholder() const;
  QString effectiveEndPlaceholder() const;
  QString effectiveSeparator() const;
  QString defaultDisplayFormat() const;
  QStringList effectiveParseFormats() const;
  bool effectiveTextIncludesTime() const;
  bool effectiveUse12Hours() const;
  QString effectiveTimeFormat() const;
  bool effectiveShowSecondColumn() const;
  Qt::DayOfWeek effectiveFirstDayOfWeek() const;
  RangePart activeRangePart() const;
  void moveCursorToRangePart(RangePart range);
  PickerMode normalizedPanelMode(PickerMode value) const;
  PickerMode effectivePanelMode() const;
  QDateTime parseMaskedText(const QString& text, const QTime& fallbackTime, bool* ok) const;
  bool effectiveInputDisabled() const;
  bool canAcceptRangeForInteraction(const QDate& start, const QDate& end) const;
  bool respectsEndpointDisabledOrder(const QDate& start, const QDate& end) const;
  bool isDisabledEndpointCrossingCandidate(const QDate& value) const;
  void applyEndpointDisabledToRange(QDate* start, QDate* end) const;
  void mergeEndpointDisabledPopupRange(QDate* start, QDate* end) const;
  bool isDateSelectable(const QDate& value, const QDate& from = QDate()) const;
  bool isDateTimeSelectable(const QDateTime& value, TimeSelectionPart part,
                            const QDate& from = QDate()) const;
  void clearRangeInternal(bool emitSignals, bool respectEndpointDisabled = false);

  void handleControllerPopupVisibleChanged(bool value);

  QObject* popupOwnerObject() const override;
  QWidget* popupAnchorWidget() const override;
  QWidget* popupScopeWindow() const override;
  QWidget* popupSurfaceWidget() const override;
  QWidget* popupEnsureSurface() override;
  void popupPrepareToShow() override;
  bool popupHasContent() const override;
  detail::OverlayPopupPlacement popupPlacement() const override;
  bool popupAutoAdjustOverflow() const override;
  bool popupArrowVisible() const override;
  bool popupArrowPointAtCenter() const override;
  int popupOffset() const override;
  int popupArrowOffsetHorizontal() const override;
  int popupArrowOffsetVertical() const override;
  void popupApplyResolvedPlacement(detail::OverlayPopupPlacement placement,
                                   qreal arrowCenterCoord) override;

  QDate startDate_;
  QDate endDate_;
  QTime startTime_ = QTime(0, 0, 0);
  QTime endTime_ = QTime(0, 0, 0);
  PickerMode pickerMode_ = PickerMode::Date;
  Size size_ = Size::Middle;
  Variant variant_ = Variant::Outlined;
  Status status_ = Status::None;
  bool allowClear_ = true;
  bool inputReadOnly_ = false;
  PreviewValue previewValue_ = PreviewValue::Hover;
  bool popupVisible_ = false;
  bool defaultOpen_ = false;
  bool defaultOpenApplied_ = false;
  bool order_ = true;
  bool needConfirm_ = false;
  bool showTime_ = false;
  bool showToday_ = false;
  bool showTodayExplicit_ = false;
  bool showNow_ = false;
  bool showNowExplicit_ = false;
  QTime defaultOpenStartTime_ = QTime(0, 0, 0);
  QTime defaultOpenEndTime_ = QTime(0, 0, 0);
  QString timeFormat_;
  int hourStep_ = 1;
  int minuteStep_ = 1;
  int secondStep_ = 1;
  bool hideDisabledOptions_ = false;
  bool use12Hours_ = false;
  bool changeOnScroll_ = false;
  bool showHour_ = true;
  bool showMinute_ = true;
  bool showSecond_ = true;
  bool showSecondExplicit_ = false;
  bool allowEmptyStart_ = false;
  bool allowEmptyEnd_ = false;
  bool startDisabled_ = false;
  bool endDisabled_ = false;
  QDate minDate_;
  QDate maxDate_;
  QDate defaultPickerValue_;
  QDate pickerValue_;
  PickerMode panelMode_ = PickerMode::Date;
  bool panelModeExplicit_ = false;
  QString displayFormat_;
  QStringList displayFormats_;
  bool maskFormat_ = false;
  bool preserveInvalidOnBlur_ = false;
  QLocale locale_;
  bool localeExplicit_ = false;
  bool applyingGlobalLocale_ = false;
  QString id_;
  QString startId_;
  QString endId_;
  QString placeholder_;
  QString startPlaceholder_;
  QString endPlaceholder_;
  QString separator_;
  QString prefixText_;
  QString suffixText_;
  adqt::icons::IconRef prefixIconRef_;
  adqt::icons::IconRef suffixIconRef_;
  adqt::icons::IconRef feedbackIconRef_;
  bool suffixIconVisible_ = true;
  adqt::icons::IconRef clearIconRef_;
  adqt::icons::IconRef prevIconRef_;
  adqt::icons::IconRef nextIconRef_;
  adqt::icons::IconRef superPrevIconRef_;
  adqt::icons::IconRef superNextIconRef_;
  Placement placement_ = Placement::BottomLeft;
  PopupLayerMode popupLayerMode_ = PopupLayerMode::InWindow;
  ComponentTokens componentTokens_;
  SemanticStyles semanticStyles_;
  SemanticStyleResolver semanticStyleResolver_;
  QVector<PresetItem> presets_;
  DatePredicate disabledDatePredicate_;
  DisabledDatePredicate disabledDateContextPredicate_;
  DisabledTimePredicate disabledTimePredicate_;
  DisplayTextCallback displayTextCallback_;
  CellRenderCallback cellRenderCallback_;
  bool panelDisabledDatePredicateDirty_ = true;
  bool panelDisabledDateContextPredicateDirty_ = true;
  bool panelDisabledTimePredicateDirty_ = true;
  bool panelCellRenderCallbackDirty_ = true;
  PopupContentWrapperFactory popupContentWrapperFactory_;
  PanelComponentFactory panelComponentFactory_;
  QPointer<QWidget> extraFooterWidget_;
  bool suppressPopupHideClose_ = false;
  bool suppressPopupCloseSubmit_ = false;
  bool syncingText_ = false;
  RangePart lastFocusedRangePart_ = RangePart::Start;
  QVector<RangePart> activeRangeHistory_;
  bool popupCalendarActive_ = false;
  QDate popupCalendarStartDate_;
  QDate popupCalendarEndDate_;
  QTime popupCalendarStartTime_ = QTime(0, 0, 0);
  QTime popupCalendarEndTime_ = QTime(0, 0, 0);
  bool previewRangeActive_ = false;
  bool previewTimeActive_ = false;
  TimeSelectionPart previewTimePart_ = TimeSelectionPart::Single;

  QHBoxLayout* rootLayout_ = nullptr;
  detail::DatePickerLineEdit* lineEdit_ = nullptr;
  detail::OverlayPopupController* popupController_ = nullptr;
  QWidget* popup_ = nullptr;
  QWidget* popupBodyHost_ = nullptr;
  QVBoxLayout* popupLayout_ = nullptr;
  QWidget* popupContentWidget_ = nullptr;
  QWidget* popupPanelsWidget_ = nullptr;
  QWidget* popupPresetsWidget_ = nullptr;
  QScrollArea* popupPresetsScrollArea_ = nullptr;
  QWidget* popupPresetsListWidget_ = nullptr;
  QVBoxLayout* popupPresetsLayout_ = nullptr;
  QWidget* popupMainWidget_ = nullptr;
  QVBoxLayout* popupMainLayout_ = nullptr;
  QWidget* popupPanelRowWidget_ = nullptr;
  QHBoxLayout* popupPanelRowLayout_ = nullptr;
  QWidget* popupFooter_ = nullptr;
  QVBoxLayout* popupFooterOuterLayout_ = nullptr;
  QWidget* popupExtraFooterHost_ = nullptr;
  QWidget* popupFooterActionsWidget_ = nullptr;
  QHBoxLayout* popupFooterLayout_ = nullptr;
  QToolButton* popupNowButton_ = nullptr;
  QToolButton* popupOkButton_ = nullptr;
  QWidget* popupPrimaryContentWidget_ = nullptr;
  QWidget* popupSecondaryContentWidget_ = nullptr;
  QHBoxLayout* popupPanelsLayout_ = nullptr;
  AdDatePickerPanel* popupPanel_ = nullptr;
  AdDatePickerPanel* popupEndPanel_ = nullptr;
  bool syncingPopupPanels_ = false;
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdDatePickerPanel::PickerMode)
Q_DECLARE_METATYPE(adqt::widgets::AdDatePickerPanel::SelectionMode)
Q_DECLARE_METATYPE(adqt::widgets::AdDatePicker::Size)
Q_DECLARE_METATYPE(adqt::widgets::AdDatePicker::Variant)
Q_DECLARE_METATYPE(adqt::widgets::AdDatePicker::Status)
Q_DECLARE_METATYPE(adqt::widgets::AdDatePicker::PreviewValue)
Q_DECLARE_METATYPE(adqt::widgets::AdDatePicker::PanelComponentRole)
Q_DECLARE_METATYPE(adqt::widgets::AdDateRangePicker::RangePart)
Q_DECLARE_METATYPE(adqt::widgets::AdDatePicker::Placement)
Q_DECLARE_METATYPE(adqt::widgets::AdDatePickerPanel::PresetItem)
Q_DECLARE_METATYPE(QVector<adqt::widgets::AdDatePickerPanel::PresetItem>)
Q_DECLARE_METATYPE(adqt::widgets::AdDatePickerPanel::DisabledDateContext)
Q_DECLARE_METATYPE(adqt::widgets::AdDatePickerPanel::TimeSelectionPart)
Q_DECLARE_METATYPE(adqt::widgets::AdDatePickerPanel::DisabledTimeContext)
