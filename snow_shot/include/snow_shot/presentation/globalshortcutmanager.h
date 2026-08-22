#ifndef SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTMANAGER_H
#define SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTMANAGER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

namespace snow_shot::presentation {
enum class GlobalShortcutAction {
    Screenshot,
    ScreenshotDelay,
    ScreenshotFixed,
    ScreenshotOcr,
    ScreenshotTranslation,
    ScreenshotCopy,
    ScreenshotFullScreen,
    ScreenshotFocusedWindow,
    ScreenRecord,
    ScreenRecordCopy,
    OpenCaptureHistory,
    OpenSettings,
    PinClipboardContent,
};

enum class GlobalShortcutStatus {
    Unset,
    Registered,
    PartiallyRegistered,
    Failed,
};

enum class GlobalShortcutFailureReason {
    None,
    InvalidShortcut,
    AlreadyInUse,
    UnsupportedPlatform,
    SystemError,
};

struct GlobalShortcutBindingResult {
    QString shortcut;
    bool registered = false;
    GlobalShortcutFailureReason failureReason = GlobalShortcutFailureReason::None;
    quint32 nativeErrorCode = 0;
};

struct GlobalShortcutRegistrationState {
    GlobalShortcutAction action = GlobalShortcutAction::Screenshot;
    QStringList shortcuts;
    GlobalShortcutStatus status = GlobalShortcutStatus::Unset;
    QVector<GlobalShortcutBindingResult> bindings;
};

struct GlobalShortcutBackendResult {
    bool registered = false;
    GlobalShortcutFailureReason failureReason = GlobalShortcutFailureReason::None;
    quint32 nativeErrorCode = 0;
};

struct GlobalShortcutValidationResult {
    QString shortcut;
    bool supported = false;
    GlobalShortcutFailureReason failureReason = GlobalShortcutFailureReason::InvalidShortcut;
};

class GlobalShortcutBackend {
  public:
    using ActivationHandler = std::function<void(int)>;

    virtual ~GlobalShortcutBackend() = default;

    virtual void setActivationHandler(ActivationHandler handler) = 0;
    [[nodiscard]] virtual GlobalShortcutValidationResult
    validateShortcut(const QString& portableShortcut) const = 0;
    [[nodiscard]] virtual GlobalShortcutBackendResult
    registerShortcut(int registrationId, const QString& portableShortcut) = 0;
    virtual void unregisterShortcut(int registrationId) = 0;
};

class GlobalShortcutManager final : public QObject {
    Q_OBJECT

  public:
    explicit GlobalShortcutManager(QObject* parent = nullptr);
    GlobalShortcutManager(std::unique_ptr<GlobalShortcutBackend> backend,
                          const QString& organization, const QString& application,
                          QObject* parent = nullptr,
                          std::function<bool()> focusedFullscreenDetector = {});
    ~GlobalShortcutManager() override;

    void initialize();
    [[nodiscard]] bool isInitialized() const;
    [[nodiscard]] GlobalShortcutRegistrationState state(GlobalShortcutAction action) const;
    [[nodiscard]] GlobalShortcutValidationResult validateShortcut(const QString& shortcut) const;
    void setShortcuts(GlobalShortcutAction action, const QStringList& shortcuts);
    void setShortcutFunctionsEnabled(bool enabled);
    [[nodiscard]] bool shortcutFunctionsEnabled() const;
    void retryRegistrations();

  signals:
    void activated(snow_shot::presentation::GlobalShortcutAction action);
    void stateChanged(snow_shot::presentation::GlobalShortcutAction action,
                      const snow_shot::presentation::GlobalShortcutRegistrationState& state);

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace snow_shot::presentation

Q_DECLARE_METATYPE(snow_shot::presentation::GlobalShortcutAction)
Q_DECLARE_METATYPE(snow_shot::presentation::GlobalShortcutRegistrationState)

#endif // SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTMANAGER_H
