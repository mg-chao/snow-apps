#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONRESIZEMODALCONTENT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONRESIZEMODALCONTENT_H

#include "snow_shot/presentation/screenshotselectionparams.h"

#include <QPointer>
#include <QWidget>

class QEvent;

namespace adqt::widgets {
class AdButton;
class AdColorPicker;
class AdForm;
class AdFormItem;
class AdInputNumber;
class AdLineEdit;
class AdModal;
class AdSelect;
} // namespace adqt::widgets

class ScreenshotSelectionResizeModalContent final : public QWidget {
    Q_OBJECT

  public:
    enum class CommitResult {
        ApplySelection,
        Invalid,
    };

    ScreenshotSelectionResizeModalContent(const ScreenshotSelectionParams& currentParams,
                                          const QRect& selectionBounds, bool hasPreviousParams,
                                          const ScreenshotSelectionParams& previousParams,
                                          const QVector<ScreenshotSelectionPreset>& presets,
                                          QWidget* parent = nullptr);

    [[nodiscard]] CommitResult commit(ScreenshotSelectionParams* params,
                                      QVector<ScreenshotSelectionPreset>* presets,
                                      bool* presetsChanged);
    [[nodiscard]] QWidget* initialFocusWidget() const;

  signals:
    void presetsUpdated(const QVector<ScreenshotSelectionPreset>& presets);

  private:
    void changeEvent(QEvent* event) override;
    void retranslateUi();
    QWidget* createNormalPage();
    adqt::widgets::AdInputNumber* createIntegerInput(int minimum, int maximum,
                                                     QWidget* parent = nullptr);
    void addNormalColumnSpacer();
    adqt::widgets::AdFormItem* addNormalField(const QString& label, QWidget* control,
                                              const QString& fieldName, bool fullWidth = false);

    void applyParamsToFields(const ScreenshotSelectionParams& params);
    [[nodiscard]] ScreenshotSelectionParams paramsFromFields() const;
    [[nodiscard]] ScreenshotSelectionPreset presetFromFields(const QString& name) const;

    void updateQuickSetOptions();
    void handleQuickSetValue(const QVariant& value);
    void clearQuickSetSelection();
    void openCreatePresetModal();
    void openDeletePresetModal(const QString& presetKey);
    void deletePreset(const QString& presetKey);
    [[nodiscard]] QWidget* modalOwnerWindow() const;
    void updateGeometryRanges();
    void updateAspectRatioLockIcon();
    void handleAspectRatioToggle(bool checked);
    void updateAspectRatioFromFields();
    void syncHeightFromWidth();
    void syncWidthFromHeight();
    bool validateNormalFields();

    QRect m_selectionBounds;
    ScreenshotSelectionParams m_currentParams;
    bool m_hasPreviousParams = false;
    ScreenshotSelectionParams m_previousParams;
    QVector<ScreenshotSelectionPreset> m_presets;
    bool m_presetsChanged = false;
    bool m_syncing = false;
    bool m_applyingQuickSet = false;
    bool m_updatingAspectRatioPeer = false;
    double m_editAspectRatio = 0.0;

    adqt::widgets::AdForm* m_normalForm = nullptr;

    adqt::widgets::AdSelect* m_quickSet = nullptr;
    adqt::widgets::AdInputNumber* m_xInput = nullptr;
    adqt::widgets::AdInputNumber* m_yInput = nullptr;
    adqt::widgets::AdInputNumber* m_widthInput = nullptr;
    adqt::widgets::AdInputNumber* m_heightInput = nullptr;
    adqt::widgets::AdInputNumber* m_radiusInput = nullptr;
    adqt::widgets::AdInputNumber* m_shadowWidthInput = nullptr;
    adqt::widgets::AdColorPicker* m_shadowColorPicker = nullptr;
    adqt::widgets::AdButton* m_lockAspectRatioButton = nullptr;

    QPointer<adqt::widgets::AdModal> m_createPresetModal;
    QPointer<adqt::widgets::AdFormItem> m_createPresetNameItem;
    QPointer<adqt::widgets::AdModal> m_deletePresetModal;
    QString m_deletePresetName;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONRESIZEMODALCONTENT_H
