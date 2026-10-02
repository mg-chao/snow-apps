#pragma once

#include "icon_core_types.h"

#include <QColor>
#include <QMargins>
#include <QObject>
#include <QPointer>
#include <QScopedPointer>
#include <QString>

#include <functional>
#include <optional>

class QWidget;

namespace adqt::widgets {

class AdMessageHandle;
class AdMessagePrivate;

// Window-scoped, non-blocking feedback stack. All methods must be called on the GUI thread.
class AdMessage final : public QObject {
  Q_OBJECT
  Q_DECLARE_PRIVATE(AdMessage)
  Q_DISABLE_COPY_MOVE(AdMessage)

  Q_PROPERTY(QWidget* ownerWindow READ ownerWindow WRITE setOwnerWindow NOTIFY ownerWindowChanged)
  Q_PROPERTY(int topOffset READ topOffset WRITE setTopOffset NOTIFY topOffsetChanged)
  Q_PROPERTY(int defaultDurationMs READ defaultDurationMs WRITE setDefaultDurationMs NOTIFY
                 defaultDurationMsChanged)
  Q_PROPERTY(int maximumCount READ maximumCount WRITE setMaximumCount NOTIFY maximumCountChanged)
  Q_PROPERTY(bool pauseOnHover READ pauseOnHover WRITE setPauseOnHover NOTIFY pauseOnHoverChanged)
  Q_PROPERTY(int count READ count NOTIFY countChanged)

 public:
  enum class Type {
    None,
    Info,
    Success,
    Warning,
    Error,
    Loading,
  };
  Q_ENUM(Type)

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
    std::optional<QColor> contentBg;
    std::optional<int> contentPaddingHorizontal;
    std::optional<int> contentPaddingVertical;
    std::optional<int> borderRadius;
    std::optional<int> iconSize;
    std::optional<int> iconContentGap;
  };

  struct SemanticSlotStyle {
    std::optional<QColor> textColor;
    std::optional<QColor> backgroundColor;
    std::optional<QColor> borderColor;
  };

  struct SemanticStyles {
    SemanticSlotStyle root;
    SemanticSlotStyle icon;
    SemanticSlotStyle content;
  };

  struct StyleContext {
    Type type = Type::None;
    QString key;
    bool hovered = false;
  };

  using SemanticStyleResolver = std::function<SemanticStyles(const StyleContext&)>;
  using Callback = std::function<void()>;

  struct Request {
    Type type = Type::None;
    QString content;
    // Ownership of hosted widgets transfers to the message on a successful open().
    QWidget* contentWidget = nullptr;
    // Opening the same non-empty key updates the existing message and returns its handle.
    QString key;
    // An unset duration uses Config::defaultDurationMs; zero keeps the message open.
    std::optional<int> durationMs;
    std::optional<bool> pauseOnHover;
    std::optional<adqt::icons::IconRef> icon;
    // Like contentWidget, a hosted icon widget is adopted after a successful open().
    QWidget* iconWidget = nullptr;
    ComponentTokens componentTokens;
    SemanticStyles semanticStyles;
    SemanticStyleResolver semanticStyleResolver;
    Callback onClick;
    Callback onClose;
    // Keep the message open but hidden while its full layout cannot fit in the owner.
    bool hideWhenClipped = false;
  };

  struct Config {
    int topOffset = 8;
    int defaultDurationMs = 3000;
    // Zero means unlimited. Positive values evict the oldest active message first.
    int maximumCount = 0;
    bool pauseOnHover = true;
    ComponentTokens componentTokens;
    SemanticStyles semanticStyles;
    SemanticStyleResolver semanticStyleResolver;
  };

  explicit AdMessage(QWidget* ownerWindow, QObject* parent = nullptr);
  ~AdMessage() override;

  QWidget* ownerWindow() const;
  void setOwnerWindow(QWidget* value);

  Config config() const;
  void setConfig(const Config& value);

  int topOffset() const;
  void setTopOffset(int value);

  int defaultDurationMs() const;
  void setDefaultDurationMs(int value);

  int maximumCount() const;
  void setMaximumCount(int value);

  bool pauseOnHover() const;
  void setPauseOnHover(bool value);

  ComponentTokens componentTokens() const;
  void setComponentTokens(const ComponentTokens& value);
  void resetComponentTokens();

  SemanticStyles semanticStyles() const;
  void setSemanticStyles(const SemanticStyles& value);
  void setSemanticStyleResolver(SemanticStyleResolver resolver);

  int count() const;

  AdMessageHandle* open(Request request);
  AdMessageHandle* open(const QString& content, int durationMs = -1);
  AdMessageHandle* info(Request request);
  AdMessageHandle* info(const QString& content, int durationMs = -1);
  AdMessageHandle* success(Request request);
  AdMessageHandle* success(const QString& content, int durationMs = -1);
  AdMessageHandle* warning(Request request);
  AdMessageHandle* warning(const QString& content, int durationMs = -1);
  AdMessageHandle* error(Request request);
  AdMessageHandle* error(const QString& content, int durationMs = -1);
  AdMessageHandle* loading(Request request);
  AdMessageHandle* loading(const QString& content, int durationMs = -1);

 public slots:
  void destroy(const QString& key);
  void destroyAll();

 signals:
  void ownerWindowChanged(QWidget* value);
  void configChanged();
  void topOffsetChanged(int value);
  void defaultDurationMsChanged(int value);
  void maximumCountChanged(int value);
  void pauseOnHoverChanged(bool value);
  void componentTokensChanged();
  void semanticStylesChanged();
  void countChanged(int value);
  void messageOpened(adqt::widgets::AdMessageHandle* handle);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  friend class AdMessageHandle;

  void closeHandle(AdMessageHandle* handle, CloseReason reason);

  QScopedPointer<AdMessagePrivate> d_ptr;
};

class AdMessageHandle final : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AdMessageHandle)

  Q_PROPERTY(QString key READ key CONSTANT)
  Q_PROPERTY(adqt::widgets::AdMessage::Type type READ type NOTIFY typeChanged)
  Q_PROPERTY(QString content READ content NOTIFY contentChanged)
  Q_PROPERTY(bool open READ isOpen NOTIFY openChanged)

 public:
  // Handles are service-owned and are deleted after closed() is emitted.
  ~AdMessageHandle() override;

  QString key() const;
  AdMessage::Type type() const;
  QString content() const;
  bool isOpen() const;
  QWidget* contentWidget() const;
  QWidget* noticeWidget() const;

 public slots:
  void close();

 signals:
  void typeChanged(adqt::widgets::AdMessage::Type value);
  void contentChanged(const QString& value);
  void openChanged(bool value);
  void clicked();
  void closed(adqt::widgets::AdMessage::CloseReason reason);

 private:
  friend class AdMessage;
  friend class AdMessagePrivate;

  explicit AdMessageHandle(AdMessage* owner, const QString& key);

  QPointer<AdMessage> owner_;
  QString key_;
  int typeValue_ = 0;
  QString content_;
  bool open_ = true;
  QPointer<QWidget> contentWidget_;
  QPointer<QWidget> noticeWidget_;
};

class AdMessageService final {
 public:
  // Returns the shared service for ownerWindow's top-level window.
  // Without an owner, selects a visible, non-minimized application window or dialog;
  // returns nullptr when only auxiliary windows (tools, popups, etc.) are visible.
  static AdMessage* instance(QWidget* ownerWindow = nullptr);

  static AdMessage::Config config();
  static void setConfig(const AdMessage::Config& value);

  static AdMessageHandle* open(AdMessage::Request request, QWidget* ownerWindow = nullptr);
  static AdMessageHandle* info(AdMessage::Request request, QWidget* ownerWindow = nullptr);
  static AdMessageHandle* info(const QString& content, int durationMs = -1,
                               QWidget* ownerWindow = nullptr);
  static AdMessageHandle* success(AdMessage::Request request, QWidget* ownerWindow = nullptr);
  static AdMessageHandle* success(const QString& content, int durationMs = -1,
                                  QWidget* ownerWindow = nullptr);
  static AdMessageHandle* warning(AdMessage::Request request, QWidget* ownerWindow = nullptr);
  static AdMessageHandle* warning(const QString& content, int durationMs = -1,
                                  QWidget* ownerWindow = nullptr);
  static AdMessageHandle* error(AdMessage::Request request, QWidget* ownerWindow = nullptr);
  static AdMessageHandle* error(const QString& content, int durationMs = -1,
                                QWidget* ownerWindow = nullptr);
  static AdMessageHandle* loading(AdMessage::Request request, QWidget* ownerWindow = nullptr);
  static AdMessageHandle* loading(const QString& content, int durationMs = -1,
                                  QWidget* ownerWindow = nullptr);

  static void destroy(const QString& key, QWidget* ownerWindow = nullptr);
  static void destroyAll(QWidget* ownerWindow = nullptr);
};

}  // namespace adqt::widgets

Q_DECLARE_METATYPE(adqt::widgets::AdMessage::Type)
Q_DECLARE_METATYPE(adqt::widgets::AdMessage::CloseReason)
