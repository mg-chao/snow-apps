#pragma once

#include "icon_core_types.h"

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QScopedPointer>
#include <QString>

#include <functional>
#include <optional>

class QWidget;

namespace adqt::widgets {

class AdNotificationHandle;
class AdNotificationPrivate;

// Window-scoped notification stack. All methods must be called on the GUI thread.
class AdNotification final : public QObject {
  Q_OBJECT
  Q_DECLARE_PRIVATE(AdNotification)
  Q_DISABLE_COPY_MOVE(AdNotification)

  Q_PROPERTY(QWidget* ownerWindow READ ownerWindow WRITE setOwnerWindow NOTIFY ownerWindowChanged)
  Q_PROPERTY(int topOffset READ topOffset WRITE setTopOffset NOTIFY topOffsetChanged)
  Q_PROPERTY(int bottomOffset READ bottomOffset WRITE setBottomOffset NOTIFY bottomOffsetChanged)
  Q_PROPERTY(int defaultDurationMs READ defaultDurationMs WRITE setDefaultDurationMs NOTIFY
                 defaultDurationMsChanged)
  Q_PROPERTY(int maximumCount READ maximumCount WRITE setMaximumCount NOTIFY maximumCountChanged)
  Q_PROPERTY(bool pauseOnHover READ pauseOnHover WRITE setPauseOnHover NOTIFY pauseOnHoverChanged)
  Q_PROPERTY(bool showProgress READ showProgress WRITE setShowProgress NOTIFY showProgressChanged)
  Q_PROPERTY(bool stackEnabled READ stackEnabled WRITE setStackEnabled NOTIFY stackEnabledChanged)
  Q_PROPERTY(int count READ count NOTIFY countChanged)

 public:
  enum class Type { None, Info, Success, Warning, Error };
  Q_ENUM(Type)

  enum class Placement { Top, TopLeft, TopRight, Bottom, BottomLeft, BottomRight };
  Q_ENUM(Placement)

  enum class AccessibilityRole { Alert, Status };
  Q_ENUM(AccessibilityRole)

  enum class CloseReason {
    Timeout,
    Manual,
    Destroyed,
    MaxCount,
    ScopeHidden,
    OwnerDestroyed,
  };
  Q_ENUM(CloseReason)

  struct ComponentTokens {
    std::optional<int> zIndexPopup;
    std::optional<int> width;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> successBackgroundColor;
    std::optional<QColor> infoBackgroundColor;
    std::optional<QColor> warningBackgroundColor;
    std::optional<QColor> errorBackgroundColor;
    std::optional<QColor> progressColor;
    std::optional<int> paddingHorizontal;
    std::optional<int> paddingVertical;
    std::optional<int> borderRadius;
    std::optional<int> iconSize;
    std::optional<int> iconContentGap;
    std::optional<int> titleDescriptionGap;
    std::optional<int> marginBottom;
    std::optional<int> edgeMargin;
    std::optional<int> progressHeight;
    std::optional<int> stackOffset;
    std::optional<int> stackGap;
  };

  struct SemanticSlotStyle {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
  };

  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle title;
    SemanticSlotStyle description;
    SemanticSlotStyle actions;
    SemanticSlotStyle icon;
  };

  struct StyleContext {
    Type type = Type::None;
    Placement placement = Placement::TopRight;
    QString key;
    bool hovered = false;
  };

  using SemanticStyleResolver = std::function<SemanticStyles(const StyleContext&)>;
  using Callback = std::function<void()>;

  struct Request {
    Type type = Type::None;
    QString title;
    QString description;
    // Hosted widgets are reparented to the notice only after open() succeeds.
    // Replacing keyed content schedules the previously hosted widget for deletion.
    QWidget* titleWidget = nullptr;
    QWidget* descriptionWidget = nullptr;
    QWidget* actionsWidget = nullptr;
    QString key;
    // An unset duration uses Config::defaultDurationMs; zero keeps the notice open.
    std::optional<int> durationMs;
    std::optional<bool> pauseOnHover;
    std::optional<bool> showProgress;
    std::optional<bool> closable;
    std::optional<Placement> placement;
    AccessibilityRole accessibilityRole = AccessibilityRole::Alert;
    std::optional<adqt::icons::IconRef> icon;
    QWidget* iconWidget = nullptr;
    std::optional<adqt::icons::IconRef> closeIcon;
    // Adopted as non-interactive visual content inside the native close button.
    QWidget* closeIconWidget = nullptr;
    ComponentTokens componentTokens;
    SemanticStyles semanticStyles;
    SemanticStyleResolver semanticStyleResolver;
    Callback onClick;
    Callback onClose;
    Callback onCloseButton;
  };

  struct Config {
    int topOffset = 24;
    int bottomOffset = 24;
    int defaultDurationMs = 4500;
    Placement defaultPlacement = Placement::TopRight;
    // Zero means unlimited. Positive values evict the oldest active notice first.
    int maximumCount = 0;
    bool pauseOnHover = true;
    bool showProgress = false;
    bool closable = true;
    bool stackEnabled = true;
    int stackThreshold = 3;
    ComponentTokens componentTokens;
    SemanticStyles semanticStyles;
    SemanticStyleResolver semanticStyleResolver;
  };

  explicit AdNotification(QWidget* ownerWindow, QObject* parent = nullptr);
  ~AdNotification() override;

  QWidget* ownerWindow() const;
  void setOwnerWindow(QWidget* value);
  Config config() const;
  void setConfig(const Config& value);
  int topOffset() const;
  void setTopOffset(int value);
  int bottomOffset() const;
  void setBottomOffset(int value);
  int defaultDurationMs() const;
  void setDefaultDurationMs(int value);
  int maximumCount() const;
  void setMaximumCount(int value);
  bool pauseOnHover() const;
  void setPauseOnHover(bool value);
  bool showProgress() const;
  void setShowProgress(bool value);
  bool stackEnabled() const;
  void setStackEnabled(bool value);
  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& value);
  void resetComponentTokens();
  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& value);
  void setSemanticStyleResolver(SemanticStyleResolver resolver);
  int count() const;

  AdNotificationHandle* open(Request request);
  AdNotificationHandle* open(const QString& title, const QString& description = QString(),
                             int durationMs = -1);
  AdNotificationHandle* info(Request request);
  AdNotificationHandle* info(const QString& title, const QString& description = QString(),
                             int durationMs = -1);
  AdNotificationHandle* success(Request request);
  AdNotificationHandle* success(const QString& title, const QString& description = QString(),
                                int durationMs = -1);
  AdNotificationHandle* warning(Request request);
  AdNotificationHandle* warning(const QString& title, const QString& description = QString(),
                                int durationMs = -1);
  AdNotificationHandle* error(Request request);
  AdNotificationHandle* error(const QString& title, const QString& description = QString(),
                              int durationMs = -1);

 public slots:
  void destroy(const QString& key);
  void destroyAll();

 signals:
  void ownerWindowChanged(QWidget* value);
  void configChanged();
  void topOffsetChanged(int value);
  void bottomOffsetChanged(int value);
  void defaultDurationMsChanged(int value);
  void maximumCountChanged(int value);
  void pauseOnHoverChanged(bool value);
  void showProgressChanged(bool value);
  void stackEnabledChanged(bool value);
  void componentTokensChanged();
  void semanticStylesChanged();
  void countChanged(int value);
  void notificationOpened(adqt::widgets::AdNotificationHandle* handle);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  friend class AdNotificationHandle;
  void closeHandle(AdNotificationHandle* handle, CloseReason reason);
  QScopedPointer<AdNotificationPrivate> d_ptr;
};

class AdNotificationHandle final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AdNotificationHandle)

  Q_PROPERTY(QString key READ key CONSTANT)
  Q_PROPERTY(adqt::widgets::AdNotification::Type type READ type NOTIFY typeChanged)
  Q_PROPERTY(QString title READ title NOTIFY titleChanged)
  Q_PROPERTY(QString description READ description NOTIFY descriptionChanged)
  Q_PROPERTY(bool open READ isOpen NOTIFY openChanged)

 public:
  // Handles are service-owned and are deleted after closed() is emitted.
  ~AdNotificationHandle() override;
  QString key() const;
  AdNotification::Type type() const;
  QString title() const;
  QString description() const;
  bool isOpen() const;
  QWidget* noticeWidget() const;

 public slots:
  void close();

 signals:
  void typeChanged(adqt::widgets::AdNotification::Type value);
  void titleChanged(const QString& value);
  void descriptionChanged(const QString& value);
  void openChanged(bool value);
  void clicked();
  void closed(adqt::widgets::AdNotification::CloseReason reason);

 private:
  friend class AdNotification;
  friend class AdNotificationPrivate;
  explicit AdNotificationHandle(AdNotification* owner, const QString& key);
  QPointer<AdNotification> owner_;
  QString key_;
  int typeValue_ = 0;
  QString title_;
  QString description_;
  bool open_ = true;
  QPointer<QWidget> noticeWidget_;
};

class AdNotificationService final {
 public:
  // Returns the shared service for ownerWindow's top-level window.
  // Without an owner, selects a visible, non-minimized application window or dialog;
  // returns nullptr when only auxiliary windows (tools, popups, etc.) are visible.
  static AdNotification* instance(QWidget* ownerWindow = nullptr);
  static AdNotification::Config config();
  static void setConfig(const AdNotification::Config& value);
  static AdNotificationHandle* open(AdNotification::Request request,
                                    QWidget* ownerWindow = nullptr);
  static AdNotificationHandle* open(const QString& title, const QString& description = QString(),
                                    int durationMs = -1, QWidget* ownerWindow = nullptr);
  static AdNotificationHandle* info(AdNotification::Request request,
                                    QWidget* ownerWindow = nullptr);
  static AdNotificationHandle* info(const QString& title, const QString& description = QString(),
                                    int durationMs = -1, QWidget* ownerWindow = nullptr);
  static AdNotificationHandle* success(AdNotification::Request request,
                                       QWidget* ownerWindow = nullptr);
  static AdNotificationHandle* success(const QString& title, const QString& description = QString(),
                                       int durationMs = -1, QWidget* ownerWindow = nullptr);
  static AdNotificationHandle* warning(AdNotification::Request request,
                                       QWidget* ownerWindow = nullptr);
  static AdNotificationHandle* warning(const QString& title, const QString& description = QString(),
                                       int durationMs = -1, QWidget* ownerWindow = nullptr);
  static AdNotificationHandle* error(AdNotification::Request request,
                                     QWidget* ownerWindow = nullptr);
  static AdNotificationHandle* error(const QString& title, const QString& description = QString(),
                                     int durationMs = -1, QWidget* ownerWindow = nullptr);
  static void destroy(const QString& key, QWidget* ownerWindow = nullptr);
  static void destroyAll(QWidget* ownerWindow = nullptr);
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdNotification::Type)
Q_DECLARE_METATYPE(adqt::widgets::AdNotification::Placement)
Q_DECLARE_METATYPE(adqt::widgets::AdNotification::AccessibilityRole)
Q_DECLARE_METATYPE(adqt::widgets::AdNotification::CloseReason)
