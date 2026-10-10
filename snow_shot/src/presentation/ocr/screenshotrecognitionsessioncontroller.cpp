#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include <QJsonArray>
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "snow_shot/presentation/components/screenshottranslationsettingsdialog.h"
#endif
#include "snow_shot/presentation/screenshotocrlayout.h"

#include "snow_shot/presentation/editionfeatures.h"

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrvisuals.h"
#include "snow_shot/presentation/screenshotocrtexteditingsession.h"
#include "snow_shot/presentation/screenshotocrtexttransform.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
#include "snow_shot/presentation/screenshotimageconversioncontroller.h"
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
#include "snow_shot/presentation/screenshottabledocument.h"
#endif
#include "snow_shot/presentation/screenshottableeditor.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QThreadPool>
#include <QLocale>
#include <QMimeData>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "widgets/alert.h"
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "widgets/button.h"
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "widgets/form.h"
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "widgets/modal.h"
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "widgets/select.h"
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "widgets/switch.h"
#endif

#include <algorithm>
#include <utility>

namespace {

std::shared_ptr<ScreenshotOcrPresentation>
copyTextPresentation(const std::shared_ptr<ScreenshotOcrPresentation>& presentation) {
    if (presentation == nullptr) {
        return {};
    }
    auto copy = std::make_shared<ScreenshotOcrPresentation>();
    copy->selection = presentation->selection;
    copy->lines = presentation->lines;
    copy->prepareForRendering();
    return copy;
}

#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
bool translationMatchesSource(const ScreenshotOcrPresentation& translation,
                              const ScreenshotOcrPresentation& source) {
    if (translation.selection != source.selection) {
        return false;
    }
    const auto matchesLayout = [&translation](const QVector<ScreenshotOcrLine>& lines) {
        if (translation.lines.size() != lines.size()) {
            return false;
        }
        for (int index = 0; index < lines.size(); ++index) {
            const auto& translated = translation.lines[index];
            const auto& original = lines[index];
            if (translated.quad != original.quad || translated.direction != original.direction ||
                translated.paragraph != original.paragraph ||
                translated.sourceLineQuads != original.sourceLineQuads) {
                return false;
            }
        }
        return true;
    };
    // A captured pin retains the layout used at translation time, regardless of current settings.
    return matchesLayout(source.lines) || matchesLayout(snow_shot::presentation::mergeOcrLayout(
                                              source.lines, source.selection.topLeft()));
}

#endif

#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
using snow_shot::translation::TranslationJob;
using snow_shot::translation::TranslationService;
#endif

bool backgroundFillEnabled() {
    return snow_shot::storage::ApplicationStorage::instance()
               .configuration()
               .value(QStringLiteral("text_recognition/fill_style"))
               .toString() == QStringLiteral("background_fill");
}

} // namespace

ScreenshotRecognitionSessionController::ScreenshotRecognitionSessionController(
    ScreenshotOcrRecognitionPort* recognition, ScreenshotQrRecognitionPort* qrRecognition,
    SnowShotApiClient* tableRecognition, ScreenshotRecognitionSessionActions actions,
    QObject* parent)
    : QObject(parent), m_actions(std::move(actions)) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableModelSelection = snow_shot::storage::ScreenshotRecognitionModelSettings().tableModel();
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    m_latexModelSelection = snow_shot::storage::ScreenshotRecognitionModelSettings().latexModel();
#endif
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    m_conversion = new ScreenshotImageConversionController(this);
    connect(m_conversion, &ScreenshotImageConversionController::changed, this, [this]() {
        updateConversionState();
        updateConversionMessage();
    });
    connect(m_conversion, &ScreenshotImageConversionController::resultsChanged, this,
            &ScreenshotRecognitionSessionController::recognitionResultsChanged);
#endif
    setProviders(recognition, qrRecognition, tableRecognition);
    connect(&snow_shot::storage::ApplicationStorage::instance().configuration(),
            &snow_shot::storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue&) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
                if (!m_savingRecognitionModel &&
                    (key == QStringLiteral("screenshot_table/model") ||
                     key == QStringLiteral("screenshot_latex/model"))) {
                    const Mode changed =
                        key == QStringLiteral("screenshot_table/model") ? Mode::Table : Mode::Latex;
                    const auto settings = snow_shot::storage::ScreenshotRecognitionModelSettings();
                    QString selected;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
                    if (changed == Mode::Table)
                        selected = settings.tableModel();
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
                    if (changed == Mode::Latex)
                        selected = settings.latexModel();
#endif
                    if (m_active && m_mode == changed) {
                        setRecognitionModel(selected);
                    } else {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
                        if (changed == Mode::Table)
                            m_tableModelSelection = selected;
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
                        if (changed == Mode::Latex)
                            m_latexModelSelection = selected;
#endif
                        updateRecognitionModels();
                    }
                    return;
                }
#endif
                if (key == QStringLiteral("text_recognition/show_original_image_preview")) {
                    updateOriginalImagePreview();
                    return;
                }
                if (key == QStringLiteral("text_recognition/fill_style")) {
                    if (m_active && m_mode == Mode::Text) {
                        setPendingTextRecognitionRendering(true);
                        if (m_presentation != nullptr) {
                            applyPresentation(m_presentation);
                            startTextRender();
                        }
                    }
                    return;
                }
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
                if (key != QStringLiteral("screenshot_translation/layout_processing")) {
                    return;
                }
                const auto it = m_textCache.constFind(m_translationKey);
                if (it != m_textCache.cend() && it->hasTranslationConfiguration &&
                    it->translationConfiguration !=
                        snow_shot::storage::ScreenshotTranslationSettings().configuration()) {
                    invalidateCurrentTranslation(m_translating);
                }
#endif
            });
}

ScreenshotRecognitionSessionController::~ScreenshotRecognitionSessionController() {
    invalidate();
}

void ScreenshotRecognitionSessionController::setProviders(
    ScreenshotOcrRecognitionPort* recognition, ScreenshotQrRecognitionPort* qrRecognition,
    SnowShotApiClient* tableRecognition) {
    if (m_recognition == nullptr && recognition != nullptr) {
        m_recognition = recognition;
        connect(recognition, &QObject::destroyed, this,
                [this]() { handleRecognitionProviderDestroyed(Mode::Text); });
    }
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    if (snow_shot::app::edition::qrRecognition && m_qrRecognition == nullptr &&
        qrRecognition != nullptr) {
        m_qrRecognition = qrRecognition;
        connect(qrRecognition, &QObject::destroyed, this,
                [this]() { handleRecognitionProviderDestroyed(Mode::Qr); });
    }
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_LATEX_RECOGNITION || SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (m_tableRecognition == nullptr && tableRecognition != nullptr) {
        m_tableRecognition = tableRecognition;
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
        m_conversion->setProvider(tableRecognition);
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        m_translationService = &TranslationService::forClient(
            *tableRecognition, snow_shot::storage::ApplicationStorage::instance().configuration(),
            snow_shot::presentation::LanguageManager::instance().currentLocale());
        connect(m_translationService, &TranslationService::preferencesChanged, this,
                [this](TranslationService::ChangeReason reason) {
                    if (reason != TranslationService::ChangeReason::UserPreferences)
                        return;
                    QTimer::singleShot(0, this, [this] {
                        const auto entry = m_textCache.constFind(m_translationKey);
                        if (entry != m_textCache.cend() && entry->hasTranslationConfiguration &&
                            entry->translationConfiguration !=
                                snow_shot::storage::ScreenshotTranslationSettings().configuration())
                            invalidateCurrentTranslation(m_translating);
                        updateTextState();
                    });
                });
        connect(m_translationService, &TranslationService::modelInvalidated, this,
                [this](const QString& id) {
                    const auto active = m_textCache.constFind(m_translationKey);
                    const bool affected = active != m_textCache.cend() &&
                                          active->translationConfiguration.modelId == id;
                    if (affected)
                        invalidateCurrentTranslation(false);
                    for (auto& entry : m_textCache) {
                        if (entry.translationConfiguration.modelId != id)
                            continue;
                        if (entry.translationJob != nullptr) {
                            entry.translationJob->cancel();
                            entry.translationJob->deleteLater();
                            entry.translationJob = nullptr;
                        }
                        entry.hasTranslationConfiguration = false;
                        entry.hasSuccessfulTranslation = false;
                        entry.translationStatus = TextCacheEntry::TranslationStatus::Absent;
                        entry.translationText.clear();
                        entry.successfulTranslation.clear();
                        entry.overlayTranslation = {};
                    }
                    if (affected && m_translating)
                        showStatus(TranslationService::modelConfigurationChangedText(), true);
                    emit recognitionResultsChanged();
                });
#endif

        connect(tableRecognition, &QObject::destroyed, this, [this]() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
            const bool awaitingModels = m_active &&
                                        (m_mode == Mode::Table || m_mode == Mode::Latex) &&
                                        m_recognitionAwaitingModel;
            m_recognitionModelsToken = 0;
            m_recognitionModelsLoading = false;
            ++m_recognitionModelsGeneration;
#endif
            handleRecognitionProviderDestroyed(Mode::Table);
            handleRecognitionProviderDestroyed(Mode::Latex);
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
            if (awaitingModels)
                failModelRecognition(m_mode == Mode::Table ? tr("Table recognition failed")
                                                           : tr("LaTeX recognition failed"));
#endif
        });
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
        connect(tableRecognition, &SnowShotApiClient::chatModelsChanged, this,
                [this] { updateRecognitionModels(); });
        connect(tableRecognition, &SnowShotApiClient::baseUrlChanged, this, [this] {
            const bool affected =
                m_active && (m_mode == Mode::Table || m_mode == Mode::Latex) &&
                !recognitionModelSelection(m_mode).startsWith(QStringLiteral("custom:"));
            if (affected) {
                cancelModelRecognition(m_mode);
                m_recognitionEffectiveModel.clear();
                clearContent();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
                m_tableSession.reset();
#endif
            }
            if (m_recognitionModelsToken)
                m_tableRecognition->cancel(m_recognitionModelsToken);
            m_recognitionModelsToken = 0;
            m_recognitionModelsLoading = false;
            ++m_recognitionModelsGeneration;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
            for (auto it = m_tableEntries.begin(); it != m_tableEntries.end();) {
                if (it->model.startsWith(QStringLiteral("custom:"))) {
                    ++it;
                    continue;
                }
                m_tableCache.remove(it.key());
                m_tableResults.remove(it.key());
                m_tableEntryTargets.remove(it.key());
                it = m_tableEntries.erase(it);
            }
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
            for (auto it = m_latexEntries.begin(); it != m_latexEntries.end();) {
                if (it->model.startsWith(QStringLiteral("custom:"))) {
                    ++it;
                    continue;
                }
                m_latexCache.remove(it.key());
                m_latexResults.remove(it.key());
                m_latexEntryTargets.remove(it.key());
                it = m_latexEntries.erase(it);
            }
#endif
            if (affected)
                failModelRecognition(
                    tr("Recognition service changed. Retry to use the updated settings."));
            updateRecognitionModels();
            emit recognitionResultsChanged();
        });
        connect(tableRecognition, &SnowShotApiClient::customModelInvalidated, this,
                [this](const QString& model, bool, bool vision) {
                    if (!vision)
                        return;
                    const bool affected = m_active && recognitionModelSelection(m_mode) == model;
                    if (affected) {
                        cancelModelRecognition(m_mode);
                        clearContent();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
                        m_tableSession.reset();
#endif
                    }
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
                    for (auto it = m_tableEntries.begin(); it != m_tableEntries.end();) {
                        if (it->model != model) {
                            ++it;
                            continue;
                        }
                        m_tableCache.remove(it.key());
                        m_tableResults.remove(it.key());
                        m_tableEntryTargets.remove(it.key());
                        it = m_tableEntries.erase(it);
                    }
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
                    for (auto it = m_latexEntries.begin(); it != m_latexEntries.end();) {
                        if (it->model != model) {
                            ++it;
                            continue;
                        }
                        m_latexCache.remove(it.key());
                        m_latexResults.remove(it.key());
                        m_latexEntryTargets.remove(it.key());
                        it = m_latexEntries.erase(it);
                    }
#endif
                    if (affected) {
                        failModelRecognition(
                            tr("Model configuration changed. Retry to use the updated settings."));
                    }
                    updateRecognitionModels();
                    emit recognitionResultsChanged();
                });
#endif
    }
#else
    Q_UNUSED(tableRecognition)
#endif
#if !SNOW_SHOT_ENABLE_QR_RECOGNITION
    Q_UNUSED(qrRecognition)
#endif
}

void ScreenshotRecognitionSessionController::setTarget(ScreenshotRecognitionTarget target) {
    if (target.key == m_target.key && target.canvasRect == m_target.canvasRect &&
        target.formattedTextDocument == m_target.formattedTextDocument &&
        target.formattedPlainText == m_target.formattedPlainText) {
        return;
    }
    resetTargetState();
    m_target = std::move(target);
    if (m_target.hasFormattedText()) {
        TextCacheEntry entry;
        entry.formatted = true;
        entry.formattedDocument = m_target.formattedTextDocument;
        const QString original = m_target.formattedPlainText.isEmpty()
                                     ? entry.formattedDocument->toPlainText()
                                     : m_target.formattedPlainText;
        entry.editingSession = std::make_shared<ScreenshotOcrTextEditingSession>(original);
        connect(entry.editingSession->document(), &QTextDocument::contentsChanged, this,
                [this, key = m_target.key]() { handleTextDocumentChanged(key); });
        m_textCache.insert(m_target.key, std::move(entry));
    }
}

void ScreenshotRecognitionSessionController::seedRecognitionResults(
    ScreenshotRecognitionResults results) {
    sanitizeEditionRecognitionResults(results);
    if (!hasTarget() || !results.isValidFor(m_target.key)) {
        return;
    }

#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    m_conversion->seed(m_target.key, results.conversions);
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (results.latexEntries.isEmpty() && results.latex && results.latex->succeeded()) {
        ScreenshotLatexRecognitionEntry legacy;
        legacy.model = screenshotDedicatedRecognitionModelId();
        legacy.result = *results.latex;
        legacy.draft = results.latexDraft;
        results.latexEntries.append(std::move(legacy));
        if (results.latexModelSelection.isEmpty())
            results.latexModelSelection = screenshotDedicatedRecognitionModelId();
    }
    if (!results.latexModelSelection.isEmpty())
        m_latexModelSelection = results.latexModelSelection;
    for (const auto& entry : results.latexEntries) {
        if (!entry.isValid() || !recognitionEntryValid(entry.model, entry.modelFingerprint))
            continue;
        const QString key = recognitionCacheKey(Mode::Latex, entry.model);
        if (m_latexCache.contains(key))
            continue;
        auto session = std::make_shared<ScreenshotOcrTextEditingSession>(entry.result.latex, true);
        if (entry.draft)
            session->establishHistory(*entry.draft);
        connect(session->document(), &QTextDocument::contentsChanged, this,
                [this, key] { handleLatexDocumentChanged(key); });
        m_latexResults.insert(key, entry.result);
        m_latexEntries.insert(key, entry);
        m_latexEntryTargets.insert(key, m_target.key);
        m_latexCache.insert(key, std::move(session));
    }
    if (m_active && m_mode == Mode::Latex)
        applyLatexContents(latexDraft());
#endif
    bool textInserted = false;
    if (!m_target.hasFormattedText() && results.text.has_value() && results.text->error.isEmpty() &&
        results.text->presentation != nullptr && !m_textCache.contains(m_target.key)) {
        const QString original =
            snow_shot::presentation::originalOcrText(*results.text->presentation);
        auto editingSession = std::make_shared<ScreenshotOcrTextEditingSession>(original);
        connect(editingSession->document(), &QTextDocument::contentsChanged, this,
                [this, key = m_target.key]() { handleTextDocumentChanged(key); });
        TextCacheEntry entry;
        entry.recognitionResult = *results.text;
        entry.recognitionResult.filteredImage = {};
        entry.presentation = results.text->presentation;
        entry.editingSession = std::move(editingSession);
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        if (results.translatedText != nullptr &&
            translationMatchesSource(*results.translatedText, *entry.presentation)) {
            entry.overlayTranslation.presentation = copyTextPresentation(results.translatedText);
            entry.overlayTranslation.status = TextCacheEntry::TranslationStatus::Completed;
            entry.overlayTranslation.captured = true;
            entry.translationConfiguration =
                snow_shot::storage::ScreenshotTranslationSettings().configuration();
            entry.hasTranslationConfiguration = true;
        }
#endif
        m_textCache.insert(m_target.key, std::move(entry));
        textInserted = true;
    }

#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (results.tableEntries.isEmpty() && results.table && results.table->succeeded()) {
        ScreenshotTableRecognitionEntry legacy;
        legacy.model = screenshotDedicatedRecognitionModelId();
        legacy.result = *results.table;
        results.tableEntries.append(std::move(legacy));
        if (results.tableModelSelection.isEmpty())
            results.tableModelSelection = screenshotDedicatedRecognitionModelId();
    }
    if (!results.tableModelSelection.isEmpty() &&
        results.tableModelSelection != m_tableModelSelection && m_active && m_mode == Mode::Table) {
        if (content())
            content()->commitActiveTableEdit();
        cancelModelRecognition(Mode::Table);
        m_recognitionEffectiveModel.clear();
        clearContent(true);
        m_tableSession.reset();
        m_tableCacheKey.clear();
    }
    if (!results.tableModelSelection.isEmpty())
        m_tableModelSelection = results.tableModelSelection;
    for (const auto& entry : results.tableEntries) {
        if (!entry.isValid() || !recognitionEntryValid(entry.model, entry.modelFingerprint))
            continue;
        const QString key = recognitionCacheKey(Mode::Table, entry.model);
        if (m_tableEntries.contains(key))
            continue;
        m_tableResults.insert(key, entry.result);
        m_tableEntries.insert(key, entry);
        m_tableEntryTargets.insert(key, m_target.key);
    }
#endif

#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    if (results.qr.has_value() && results.qr->error.isEmpty() && !results.qr->contents.isEmpty() &&
        !m_qrCache.contains(m_target.key)) {
        m_qrResults.insert(m_target.key, *results.qr);
        m_qrCache.insert(m_target.key, results.qr->contents);
    }
#endif

    if (textInserted && m_active && m_mode == Mode::Text) {
        m_textCacheKey = m_target.key;
        m_presentation = m_textCache.value(m_target.key).presentation;
        applyPresentation(m_presentation);
        startTextRender();
        emit textResultChanged(true);
    }
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_active && m_mode == Mode::Table) {
        const auto table = m_tableCache.constFind(recognitionCacheKey(Mode::Table));
        if (table != m_tableCache.cend()) {
            m_tableCacheKey = recognitionCacheKey(Mode::Table);
            applyTableSession(table.value());
        } else if (m_tableEntries.contains(recognitionCacheKey(Mode::Table))) {
            cancelModelRecognition(Mode::Table);
            activateModelResult();
        }
    }
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    if (m_active && m_mode == Mode::Qr) {
        const auto qr = m_qrCache.constFind(m_target.key);
        if (qr != m_qrCache.cend()) {
            m_qrCacheKey = m_target.key;
            applyQrContents(qr.value());
        }
    }
#endif
    updateBusyState();
    updateTextState();
    emit recognitionResultsChanged();
}

ScreenshotRecognitionResults
ScreenshotRecognitionSessionController::cachedRecognitionResults() const {
    ScreenshotRecognitionResults results;
    if (!hasTarget()) {
        return results;
    }
    results.key = m_target.key;
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    results.latexModelSelection = m_latexModelSelection;
    results.latexEffectiveModel = resolvedRecognitionModel(Mode::Latex);
    for (auto it = m_latexEntries.cbegin(); it != m_latexEntries.cend(); ++it) {
        if (m_latexEntryTargets.value(it.key()) != m_target.key ||
            !recognitionEntryValid(it->model, it->modelFingerprint))
            continue;
        auto entry = it.value();
        if (const auto session = m_latexCache.value(it.key())) {
            if (session->text() != entry.result.latex)
                entry.draft = session->text();
            else
                entry.draft.reset();
        }
        results.latexEntries.append(entry);
        if (entry.model == screenshotDedicatedRecognitionModelId()) {
            results.latex = entry.result;
            results.latexDraft = entry.draft;
        }
    }
    results.visibleLatex = m_active && m_mode == Mode::Latex;
#endif
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    results.conversions = m_conversion->entries(m_target.key);
#endif
    if (const auto text = m_textCache.constFind(m_target.key);
        text != m_textCache.cend() && text->recognitionResult.error.isEmpty() &&
        text->recognitionResult.presentation != nullptr) {
        results.text = text->recognitionResult;
        results.text->filteredImage = {};
    }
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    results.tableModelSelection = m_tableModelSelection;
    results.tableEffectiveModel = resolvedRecognitionModel(Mode::Table);
    for (auto it = m_tableEntries.cbegin(); it != m_tableEntries.cend(); ++it) {
        if (m_tableEntryTargets.value(it.key()) != m_target.key ||
            !recognitionEntryValid(it->model, it->modelFingerprint))
            continue;
        auto entry = it.value();
        if (const auto session = m_tableCache.value(it.key())) {
            const QString draft = session->document.toHtml();
            if (draft != session->baseline.toHtml())
                entry.draftHtml = draft;
            else
                entry.draftHtml.reset();
        }
        results.tableEntries.append(entry);
        if (entry.model == screenshotDedicatedRecognitionModelId())
            results.table = entry.result;
    }
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    if (const auto qr = m_qrResults.constFind(m_target.key);
        qr != m_qrResults.cend() && qr->error.isEmpty() && !qr->contents.isEmpty()) {
        results.qr = qr.value();
    }
#endif
    if (results.isEmpty()) {
        results.key.clear();
    }
    return results;
}

ScreenshotRecognitionResults
ScreenshotRecognitionSessionController::recognitionResultsSnapshot() const {
    ScreenshotRecognitionResults results = cachedRecognitionResults();
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (conversionModeActive() &&
        m_conversion->state() == ScreenshotImageConversionController::State::Completed) {
        results.visibleConversion = m_conversion->format();
    }
#endif
    if (results.text.has_value()) {
        results.text->presentation = copyTextPresentation(results.text->presentation);
    }
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (const auto entry = m_textCache.constFind(m_target.key); entry != m_textCache.cend()) {
        // Freeze partial translation independently of the source session's live requests.
        results.translatedText = copyTextPresentation(entry->overlayTranslation.presentation);
    }
#endif
    return results;
}

bool ScreenshotRecognitionSessionController::hasTarget() const {
    return m_target.isValid();
}

QString ScreenshotRecognitionSessionController::recognitionModelSelection(Mode mode) const {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (mode == Mode::Table)
        return m_tableModelSelection;
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (mode == Mode::Latex)
        return m_latexModelSelection;
#endif
    Q_UNUSED(mode)
    return {};
}

QString ScreenshotRecognitionSessionController::resolvedRecognitionModel(Mode mode) const {
    const QString selected = recognitionModelSelection(mode);
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (selected == screenshotDefaultVisionRecognitionModelId()) {
        if (m_active && mode == m_mode && !m_recognitionEffectiveModel.isEmpty())
            return m_recognitionEffectiveModel;
        return m_tableRecognition ? m_tableRecognition->builtInVisionModel() : QString{};
    }
#endif
    return selected;
}

QString ScreenshotRecognitionSessionController::recognitionCacheKey(Mode mode,
                                                                    const QString& model) const {
    const QString effective = model.isEmpty() ? resolvedRecognitionModel(mode) : model;
    if (effective.isEmpty())
        return {};
    QByteArray identity;
    QDataStream stream(&identity, QIODevice::WriteOnly);
    QString fingerprint;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_tableRecognition)
        fingerprint = m_tableRecognition->modelFingerprint(effective);
#endif
    stream << m_target.key << static_cast<int>(mode) << effective << fingerprint << 1;
    return QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

bool ScreenshotRecognitionSessionController::recognitionEntryValid(
    const QString& model, const QString& fingerprint) const {
    if (model == screenshotDedicatedRecognitionModelId() && fingerprint.isEmpty())
        return true;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_tableRecognition) {
        if (model.startsWith(QStringLiteral("custom:")) &&
            !m_tableRecognition->isCustomModel(model))
            return false;
        return fingerprint == m_tableRecognition->modelFingerprint(model);
    }
#endif
    return !model.startsWith(QStringLiteral("custom:")) && fingerprint.isEmpty();
}

void ScreenshotRecognitionSessionController::updateRecognitionModels() const {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (!m_actions.setRecognitionModelState)
        return;
    ScreenshotRecognitionModelState state;
    state.loading = m_recognitionModelsLoading;
    state.error = m_recognitionModelsError;
    if (m_tableRecognition)
        state.models = m_tableRecognition->cachedChatModels();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    state.selection = m_tableModelSelection;
    state.effectiveModel =
        m_active && m_mode == Mode::Table ? m_recognitionEffectiveModel : QString{};
    m_actions.setRecognitionModelState(static_cast<int>(Mode::Table), state);
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    state.selection = m_latexModelSelection;
    state.effectiveModel =
        m_active && m_mode == Mode::Latex ? m_recognitionEffectiveModel : QString{};
    m_actions.setRecognitionModelState(static_cast<int>(Mode::Latex), state);
#endif
#endif
}

bool ScreenshotRecognitionSessionController::recognitionModelAvailable(const QString& model) const {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    return m_tableRecognition &&
           (model.startsWith(QStringLiteral("custom:")) ||
            m_tableRecognition->hasBuiltInModels(
                snow_shot::presentation::LanguageManager::instance().currentLocale().name())) &&
           std::any_of(m_tableRecognition->cachedChatModels().cbegin(),
                       m_tableRecognition->cachedChatModels().cend(), [&model](const auto& item) {
                           return item.id == model && item.supportsVision;
                       });
#else
    Q_UNUSED(model)
    return false;
#endif
}

void ScreenshotRecognitionSessionController::loadRecognitionModels(bool refresh) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (!m_active || (m_mode != Mode::Table && m_mode != Mode::Latex) || !m_tableRecognition ||
        m_recognitionModelsToken) {
        updateRecognitionModels();
        return;
    }
    m_recognitionModelsLoading = true;
    m_recognitionModelsError.clear();
    updateRecognitionModels();
    const quint64 generation = ++m_recognitionModelsGeneration;
    const QString locale =
        snow_shot::presentation::LanguageManager::instance().currentLocale().name();
    m_recognitionModelsToken = m_tableRecognition->ensureChatModels(
        locale, this,
        [this, generation](SnowShotChatModelsResult result) {
            if (generation != m_recognitionModelsGeneration || !m_active)
                return;
            m_recognitionModelsToken = 0;
            m_recognitionModelsLoading = false;
            if (!result.error.isEmpty() ||
                !m_tableRecognition->hasBuiltInModels(
                    snow_shot::presentation::LanguageManager::instance().currentLocale().name())) {
                m_recognitionModelsError = tr("Unable to load Snow Shot vision models");
            }
            updateRecognitionModels();
            if ((m_mode == Mode::Table || m_mode == Mode::Latex) && m_recognitionAwaitingModel)
                activateModelResult();
            updateBusyState();
        },
        refresh ? SnowShotApiClient::ChatModelsCachePolicy::Refresh
                : SnowShotApiClient::ChatModelsCachePolicy::UseCached);
    if (!m_recognitionModelsToken) {
        m_recognitionModelsLoading = false;
        m_recognitionModelsError = tr("Unable to load Snow Shot vision models");
        updateRecognitionModels();
    }
#else
    Q_UNUSED(refresh)
#endif
}

void ScreenshotRecognitionSessionController::cancelModelRecognition(Mode mode) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (mode == m_mode)
        m_recognitionAwaitingModel = false;
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (mode == Mode::Table) {
        ++m_tableGeneration;
        if (m_tableParsingCancellation)
            m_tableParsingCancellation->store(true, std::memory_order_relaxed);
        m_tableParsingCancellation.reset();
        if (m_tableRecognition && m_tableRequestToken)
            m_tableRecognition->cancel(m_tableRequestToken);
        m_tableRequestToken = 0;
        m_tableParsing = false;
    }
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (mode == Mode::Latex) {
        ++m_latexGeneration;
        if (m_tableRecognition && m_latexRequestToken)
            m_tableRecognition->cancel(m_latexRequestToken);
        m_latexRequestToken = 0;
    }
#endif
    Q_UNUSED(mode)
}

void ScreenshotRecognitionSessionController::failModelRecognition(const QString& message) {
    hideRecognitionMessage();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    m_recognitionAwaitingModel = false;
#endif
    showStatus(message, true);
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_mode == Mode::Table)
        m_tableSession.reset();
#endif
    if (content())
        content()->showRecognitionError(message);
    updateBusyState();
    updateTextState();
}

void ScreenshotRecognitionSessionController::setRecognitionModel(const QString& selection) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (!m_active || (m_mode != Mode::Table && m_mode != Mode::Latex) || selection.isEmpty() ||
        selection.size() > 256 || selection == recognitionModelSelection(m_mode) ||
        std::any_of(selection.cbegin(), selection.cend(), [](QChar character) {
            return character.isSpace() || character.category() == QChar::Other_Control;
        }))
        return;
    if (content() && m_mode == Mode::Table)
        content()->commitActiveTableEdit();
    cancelModelRecognition(m_mode);
    m_recognitionEffectiveModel.clear();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_mode == Mode::Table)
        m_tableModelSelection = selection;
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_mode == Mode::Latex)
        m_latexModelSelection = selection;
#endif
    m_savingRecognitionModel = true;
    const auto settings = snow_shot::storage::ScreenshotRecognitionModelSettings();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_mode == Mode::Table)
        settings.setTableModel(selection);
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_mode == Mode::Latex)
        settings.setLatexModel(selection);
#endif
    m_savingRecognitionModel = false;
    clearContent(m_mode == Mode::Table);
    m_workflowError.clear();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableSession.reset();
    m_tableCacheKey.clear();
#endif
    updateRecognitionModels();
    activateModelResult();
    emit recognitionResultsChanged();
#else
    Q_UNUSED(selection)
#endif
}

void ScreenshotRecognitionSessionController::retryRecognition() {
    if (!m_active || (m_mode != Mode::Table && m_mode != Mode::Latex) || busy(m_mode))
        return;
    m_workflowError.clear();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    m_recognitionEffectiveModel.clear();
    const QString model = resolvedRecognitionModel(m_mode);
    const bool refresh = model != screenshotDedicatedRecognitionModelId() &&
                         !model.startsWith(QStringLiteral("custom:")) &&
                         !recognitionModelAvailable(model);
#else
    constexpr bool refresh = false;
#endif
    if (content())
        content()->clearRecognitionError();
    loadRecognitionModels(refresh);
    activateModelResult();
}

void ScreenshotRecognitionSessionController::activateModelResult() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (!m_active)
        return;
    const bool unresolvedDefault =
        recognitionModelSelection(m_mode) == screenshotDefaultVisionRecognitionModelId() &&
        (!m_tableRecognition ||
         !m_tableRecognition->hasBuiltInModels(
             snow_shot::presentation::LanguageManager::instance().currentLocale().name()));
    const QString model = unresolvedDefault ? QString{} : resolvedRecognitionModel(m_mode);
    if (model.isEmpty()) {
        if (m_recognitionModelsLoading) {
            m_recognitionAwaitingModel = true;
            showRecognitionMessage();
            updateBusyState();
        } else {
            m_recognitionAwaitingModel = false;
            failModelRecognition(tr("Snow Shot's visual understanding model is unavailable. Choose "
                                    "another model or retry."));
        }
        return;
    }
    const QString key = recognitionCacheKey(m_mode, model);
    m_recognitionEffectiveModel = model;
    updateRecognitionModels();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_mode == Mode::Table && m_tableCache.contains(key)) {
        m_recognitionAwaitingModel = false;
        m_tableCacheKey = key;
        applyTableSession(m_tableCache.value(key));
        hideRecognitionMessage();
        updateBusyState();
        updateTextState();
        return;
    }
    if (m_mode == Mode::Table && m_tableEntries.contains(key)) {
        m_recognitionAwaitingModel = false;
        materializeTableEntry(++m_tableGeneration, key, m_tableEntries.value(key), true);
        return;
    }
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_mode == Mode::Latex && m_latexResults.contains(key)) {
        m_recognitionAwaitingModel = false;
        applyLatexContents(latexDraft());
        hideRecognitionMessage();
        updateBusyState();
        updateTextState();
        return;
    }
#endif
    if (model != screenshotDedicatedRecognitionModelId()) {
        if (!recognitionModelAvailable(model)) {
            if (m_recognitionModelsLoading && !model.startsWith(QStringLiteral("custom:"))) {
                m_recognitionAwaitingModel = true;
                showRecognitionMessage();
                updateBusyState();
                return;
            }
            failModelRecognition(tr(
                "The selected recognition model is unavailable. Choose another model or retry."));
            return;
        }
    }
    m_recognitionAwaitingModel = false;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_mode == Mode::Table)
        startTableRecognition();
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_mode == Mode::Latex)
        startLatexRecognition();
#endif
    updateBusyState();
    updateTextState();
#endif
}

void ScreenshotRecognitionSessionController::prefetchText() {
    if (!hasTarget() || m_textCache.contains(m_target.key) || m_textRequestToken != 0) {
        return;
    }
    startTextRecognition(ScreenshotOcrRequestPriority::Prefetch);
}

void ScreenshotRecognitionSessionController::activate(Mode mode) {
    if (!snow_shot::presentation::editionRecognitionModeAvailable(static_cast<int>(mode)))
        return;
    if (m_active && m_mode == mode && (mode == Mode::Table || mode == Mode::Latex) && busy(mode)) {
        updateRecognitionModels();
        return;
    }
    if (m_active && m_mode != mode)
        cancelModelRecognition(m_mode);
    if (content() && m_active && m_mode == Mode::Table)
        content()->commitActiveTableEdit();
    if (!hasTarget()) {
        showStatus(tr("Unable to read the selected screenshot"), true);
        return;
    }
    if (!m_active || mode != m_mode) {
        setShowOriginalImage(false);
    }
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    m_conversion->deactivate();
#endif
    clearTextEditingState();
    if (mode != Mode::Text && m_textRenderRequestToken != 0) {
        if (m_recognition != nullptr) {
            m_recognition->cancel(m_textRenderRequestToken);
        }
        m_textRenderRequestToken = 0;
        ++m_textRenderGeneration;
    }
    m_workflowError.clear();
    m_mode = mode;
    m_active = true;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    m_recognitionEffectiveModel.clear();
#endif
    ensureContent();
    if (m_actions.setRecognitionVisualState) {
        m_actions.setRecognitionVisualState(true);
    }
    if (m_actions.setActiveMode) {
        m_actions.setActiveMode(static_cast<int>(mode));
    }

    if (ScreenshotRecognitionWindow* window = content()) {
        window->clearOcrPresentation();
        window->clearTableSession();
        window->clearQrContents();
    }
    m_presentation.reset();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableSession.reset();
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    m_qrContents.clear();
#endif
    m_textCacheKey.clear();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableCacheKey.clear();
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    m_qrCacheKey.clear();
#endif
    clearContent();
    if (m_actions.clearOcrBackground) {
        m_actions.clearOcrBackground();
    }
    if (mode == Mode::Text) {
        const auto cached = m_textCache.constFind(m_target.key);
        if (cached != m_textCache.cend()) {
            m_textCacheKey = m_target.key;
            if (cached->formatted) {
                m_presentation.reset();
                applyFormattedText(cached->formattedDocument);
            } else {
                m_presentation = cached->presentation;
                applyPresentation(m_presentation);
                startTextRender();
            }
            emit textResultChanged(true);
            if (cached->editing) {
                m_editing = true;
                m_editingKey = m_target.key;
                beginTextEditing();
            }
        } else if (m_textRequestToken != 0) {
            setPendingTextRecognitionRendering(true);
            static_cast<void>(m_recognition->reprioritize(
                m_textRequestToken, ScreenshotOcrRequestPriority::Interactive));
            if (m_textModelDownloadInProgress && textModelDownloading()) {
                showModelDownloadMessage();
            } else {
                showRecognitionMessage();
            }
        } else {
            startTextRecognition(ScreenshotOcrRequestPriority::Interactive);
        }
    } else if (mode == Mode::Table || mode == Mode::Latex) {
        setPendingTextRecognitionRendering(false);
        loadRecognitionModels();
        activateModelResult();
    } else if (mode == Mode::Qr) {
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
        setPendingTextRecognitionRendering(false);
        const auto cached = m_qrCache.constFind(m_target.key);
        if (cached != m_qrCache.cend()) {
            m_qrCacheKey = m_target.key;
            applyQrContents(cached.value());
        } else {
            startQrRecognition();
        }
#endif
    } else {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
        setPendingTextRecognitionRendering(false);
        m_conversion->activate(m_target.key, m_target.image,
                               mode == Mode::Markdown ? SnowShotImageConversionFormat::Markdown
                                                      : SnowShotImageConversionFormat::Html);
#endif
    }
    updateBusyState();
    updateTextState();
}

void ScreenshotRecognitionSessionController::deactivate() {
    if (content() && m_active && m_mode == Mode::Table)
        content()->commitActiveTableEdit();
    cancelModelRecognition(Mode::Table);
    cancelModelRecognition(Mode::Latex);
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_tableRecognition && m_recognitionModelsToken)
        m_tableRecognition->cancel(m_recognitionModelsToken);
    m_recognitionModelsToken = 0;
    m_recognitionModelsLoading = false;
    ++m_recognitionModelsGeneration;
#endif
    setShowOriginalImage(false);
    if (!m_active && m_content == nullptr) {
        return;
    }
    clearTextEditingState();
    const bool wasActive = m_active;
    m_active = false;
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    m_conversion->deactivate();
#endif
    hideModelDownloadMessage();
    if (m_recognition != nullptr && m_textRenderRequestToken != 0) {
        m_recognition->cancel(m_textRenderRequestToken);
    }
    m_textRenderRequestToken = 0;
    ++m_textRenderGeneration;
    if (m_recognition != nullptr && m_textRequestToken != 0) {
        // A deactivated OCR consumer no longer needs queued or running work.
        // Completed entries remain cached, but outstanding requests are
        // canceled so the shared OCR process can retire immediately.
        m_recognition->cancel(m_textRequestToken);
        m_textRequestToken = 0;
        ++m_textGeneration;
        setPendingTextRecognitionRendering(false);
    }
    m_presentation.reset();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableSession.reset();
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    m_qrContents.clear();
#endif
    clearContent();
    if (m_actions.clearOcrBackground) {
        m_actions.clearOcrBackground();
    }
    // The content widget is retained between sessions. Repeated cleanup must not
    // announce another exit and overwrite the host's newly selected drawing tool.
    if (wasActive && m_actions.setRecognitionVisualState) {
        m_actions.setRecognitionVisualState(false);
    }
    if (wasActive && m_actions.setActiveMode) {
        m_actions.setActiveMode(-1);
    }
    hideRecognitionMessage();
    updateBusyState();
    updateTextState();
    updateTableState({});
}

void ScreenshotRecognitionSessionController::invalidate() {
    resetTargetState();
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    m_conversion->invalidate();
#endif
    m_textCache.clear();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableCache.clear();
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    m_qrCache.clear();
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    m_latexResults.clear();
    m_latexEntries.clear();
    m_latexEntryTargets.clear();
    m_latexCache.clear();
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableResults.clear();
    m_tableEntries.clear();
    m_tableEntryTargets.clear();
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    m_qrResults.clear();
#endif
    m_textCacheKey.clear();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableCacheKey.clear();
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    m_qrCacheKey.clear();
#endif
    m_editingKey.clear();
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    m_translationKey.clear();
#endif
    m_target = {};
    emit textResultChanged(false);
}

void ScreenshotRecognitionSessionController::resetTargetState() {
    deactivate();
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (m_translationSettingsModal != nullptr) {
        m_translationSettingsModal->reject();
    }
#endif

    // Raw recognition payloads survive a target change, but editing state belongs to
    // the target that is currently visible and must not leak into another target.
    for (auto it = m_textCache.begin(); it != m_textCache.end(); ++it) {
        if (it->editingSession != nullptr) {
            it->editingSession->establishHistory(it->editingSession->originalText());
        }
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        if (it->translationJob != nullptr) {
            it->translationJob->cancel();
            it->translationJob->deleteLater();
            it->translationJob = nullptr;
        }

        it->translationSession.reset();
        it->overlayTranslation = {};
        it->hasTranslationConfiguration = false;
        it->translationText.clear();
        it->successfulTranslation.clear();
        it->translationStatus = TextCacheEntry::TranslationStatus::Absent;
        it->hasSuccessfulTranslation = false;
#endif
        it->editing = false;
        it->defaultTransformsApplied = false;
    }
    cancelOutstandingRequests();
    ++m_textGeneration;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    ++m_tableGeneration;
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    ++m_qrGeneration;
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    ++m_latexGeneration;
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    ++m_translationGeneration;
#endif
    m_textCacheKey.clear();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableCacheKey.clear();
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    m_qrCacheKey.clear();
#endif
    m_editingKey.clear();
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    m_translationKey.clear();
#endif
    m_target = {};
    updateTextState();
}

bool ScreenshotRecognitionSessionController::active() const {
    return m_active;
}

bool ScreenshotRecognitionSessionController::busy() const {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    return busy(Mode::Latex) || busy(Mode::Text) || busy(Mode::Table) || busy(Mode::Qr) ||
           m_conversion->busy();
#else
    return busy(Mode::Latex) || busy(Mode::Text) || busy(Mode::Table) || busy(Mode::Qr) ||
           busy(Mode::Markdown) || busy(Mode::Html);
#endif
}

bool ScreenshotRecognitionSessionController::busy(Mode mode) const {
    switch (mode) {
    case Mode::Text:
        return m_textRequestToken != 0 || m_textRenderRequestToken != 0;
    case Mode::Table:
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
        return m_tableRequestToken != 0 || m_tableParsing ||
               (m_active && m_mode == mode && m_recognitionAwaitingModel &&
                m_recognitionModelsLoading);
#else
        return false;
#endif
    case Mode::Latex:
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
        return m_latexRequestToken != 0 ||
               (m_active && m_mode == mode && m_recognitionAwaitingModel &&
                m_recognitionModelsLoading);
#else
        return false;
#endif
    case Mode::Qr:
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
        return m_qrRequestToken != 0;
#else
        return false;
#endif
    case Mode::Markdown:
    case Mode::Html:
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
        return m_mode == mode && m_conversion->busy();
#else
        return false;
#endif
    }
    return false;
}

ScreenshotRecognitionSessionController::Mode ScreenshotRecognitionSessionController::mode() const {
    return m_mode;
}

bool ScreenshotRecognitionSessionController::tableModeActive() const {
    return snow_shot::app::edition::tableRecognition && m_active && m_mode == Mode::Table;
}

bool ScreenshotRecognitionSessionController::qrModeActive() const {
    return snow_shot::app::edition::qrRecognition && m_active && m_mode == Mode::Qr;
}

bool ScreenshotRecognitionSessionController::conversionModeActive() const {
    return snow_shot::app::edition::imageConversion && m_active &&
           (m_mode == Mode::Markdown || m_mode == Mode::Html);
}

void ScreenshotRecognitionSessionController::openImageConversionSettings() {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    QWidget* owner =
        m_actions.translationSettingsOwner ? m_actions.translationSettingsOwner() : content();
    m_conversion->openSettings(owner);
#endif
}

void ScreenshotRecognitionSessionController::updateConversionState() const {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    emit workflowStateChanged();
    if (conversionModeActive() && m_conversion->active() && content() != nullptr) {
        content()->showImageConversion(m_conversion->format(), m_conversion->source(),
                                       m_conversion->busy(), m_conversion->error());
    }
    if (m_actions.setConversionState) {
        m_actions.setConversionState(conversionModeActive(), m_conversion->busy(),
                                     m_conversion->format());
    }
#endif
}

void ScreenshotRecognitionSessionController::updateConversionMessage() {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    const bool loading = conversionModeActive() && m_conversion->busy();
    if (loading == m_conversionMessageShown) {
        return;
    }
    // Streaming updates must not restart the shared message's entrance animation.
    m_conversionMessageShown = loading;
    if (loading) {
        showRecognitionMessage();
    } else if (m_actions.hideLoading) {
        // Background OCR may still be busy; it must not keep this conversion prompt alive.
        m_actions.hideLoading();
    }
#endif
}

void ScreenshotRecognitionSessionController::mergeTableSelection() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (tableModeActive() && content() != nullptr) {
        content()->mergeTableSelection();
    }
#endif
}

void ScreenshotRecognitionSessionController::splitTableSelection() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (tableModeActive() && content() != nullptr) {
        content()->splitTableSelection();
    }
#endif
}

void ScreenshotRecognitionSessionController::resetTable() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (tableModeActive() && content() != nullptr) {
        content()->resetTable();
    }
#endif
}

void ScreenshotRecognitionSessionController::undoTableEdit() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (tableModeActive() && content() != nullptr) {
        content()->undoTableEdit();
    }
#endif
}

void ScreenshotRecognitionSessionController::redoTableEdit() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (tableModeActive() && content() != nullptr) {
        content()->redoTableEdit();
    }
#endif
}

void ScreenshotRecognitionSessionController::undoTextEdit() {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_active && m_mode == Mode::Latex) {
        if (const auto session = m_latexCache.value(recognitionCacheKey(Mode::Latex)))
            session->undo();
        return;
    }
#endif
    if (!m_active || (!m_editing && !m_translating) || m_textDocument == nullptr) {
        return;
    }
    const auto entry = m_textCache.value(m_editingKey);
    const auto session = currentEditingSession(entry);
    if (session != nullptr) {
        session->undo();
    }
}

void ScreenshotRecognitionSessionController::redoTextEdit() {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_active && m_mode == Mode::Latex) {
        if (const auto session = m_latexCache.value(recognitionCacheKey(Mode::Latex)))
            session->redo();
        return;
    }
#endif
    if (!m_active || (!m_editing && !m_translating) || m_textDocument == nullptr) {
        return;
    }
    const auto entry = m_textCache.value(m_editingKey);
    const auto session = currentEditingSession(entry);
    if (session != nullptr) {
        session->redo();
    }
}

void ScreenshotRecognitionSessionController::beginTextEditing() {
    if (!m_active || m_mode != Mode::Text || !hasTextResult()) {
        return;
    }
    m_editing = true;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    m_translating = false;
#endif
    m_editingKey = m_textCacheKey.isEmpty() ? m_target.key : m_textCacheKey;
    auto it = m_textCache.find(m_editingKey);
    if (it != m_textCache.end()) {
        it->editing = true;
        if (!it->formatted && !it->defaultTransformsApplied && it->editingSession != nullptr) {
            it->defaultTransformsApplied = true;
            const snow_shot::storage::TextRecognitionSettings settings;
            const QString formatting = settings.defaultFormatting();
            const QString smartText = formatting == QStringLiteral("smart") && it->presentation
                                          ? snow_shot::presentation::smartOcrText(*it->presentation)
                                          : QString{};
            static_cast<void>(it->editingSession->applyInitialTransforms(
                formatting, settings.defaultPunctuation(), smartText));
        }
        m_textDocument = it->editingSession != nullptr ? it->editingSession->document() : nullptr;
    }
    if (content() != nullptr && m_textDocument != nullptr) {
        if (m_actions.clearOcrBackground) {
            m_actions.clearOcrBackground();
        }
        content()->clearOcrPresentation();
        content()->showTextEditor(m_textDocument.data());
    }
    emit textEditingChanged(true);
    updateTextState();
}

void ScreenshotRecognitionSessionController::beginTextTranslation() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    m_workflowError.clear();
    if (!m_active || m_mode != Mode::Text || !hasTextResult() || m_translating) {
        return;
    }
    const auto cached = m_textCache.constFind(m_target.key);
    if (cached != m_textCache.cend() && cached->overlayTranslation.captured &&
        (!cached->hasTranslationConfiguration ||
         cached->translationConfiguration ==
             snow_shot::storage::ScreenshotTranslationSettings().configuration()) &&
        activateCachedTextTranslation()) {
        return;
    }
    m_editing = false;
    m_translating = true;
    m_editingKey = m_textCacheKey.isEmpty() ? m_target.key : m_textCacheKey;
    auto it = m_textCache.find(m_editingKey);
    if (it == m_textCache.end()) {
        m_translating = false;
        return;
    }
    it->editing = false;
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    const bool originalImage =
        settings.originalImageTranslationEnabled() && !it->formatted && it->presentation != nullptr;
    if (originalImage != m_translationInImage) {
        cancelTranslationRequests();
    }
    m_translationInImage = originalImage;
    m_translationKey = m_editingKey;
    if (it->hasTranslationConfiguration &&
        it->translationConfiguration != settings.configuration()) {
        invalidateCurrentTranslation(false);
    }
    if (originalImage) {
        prepareOverlayTranslation(*it);
        m_textDocument = nullptr;
        applyPresentation(it->overlayTranslation.presentation);
        startTextRender();
        emit textEditingChanged(false);
        updateTextState();
        reportOverlayTranslationFailure();
        if (it->overlayTranslation.status == TextCacheEntry::TranslationStatus::Absent ||
            it->overlayTranslation.status == TextCacheEntry::TranslationStatus::Failed) {
            startTranslation();
        }
        return;
    }
    if (it->translationSession == nullptr) {
        it->translationSession = std::make_shared<ScreenshotOcrTextEditingSession>(QString());
        connect(it->translationSession->document(), &QTextDocument::contentsChanged, this,
                [this, key = m_editingKey]() { handleTranslationDocumentChanged(key); });
    }
    m_translationKey = m_editingKey;
    m_textDocument = it->translationSession->document();
    if (content() != nullptr) {
        if (m_actions.clearOcrBackground) {
            m_actions.clearOcrBackground();
        }
        const bool streaming =
            it->translationStatus == TextCacheEntry::TranslationStatus::Streaming;
        content()->showTextEditor(m_textDocument.data(), streaming, streaming);
    }
    emit textEditingChanged(true);
    updateTextState();
    if (it->translationStatus == TextCacheEntry::TranslationStatus::Absent ||
        it->translationStatus == TextCacheEntry::TranslationStatus::Failed) {
        startTranslation();
    }
#endif
}

bool ScreenshotRecognitionSessionController::activateCachedTextTranslation() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (!m_active || m_mode != Mode::Text) {
        return false;
    }
    auto entry = m_textCache.find(m_target.key);
    if (entry == m_textCache.end() || !entry->overlayTranslation.captured ||
        entry->overlayTranslation.presentation == nullptr) {
        return false;
    }
    if (originalImageTranslationActive()) {
        return true;
    }
    m_editing = false;
    m_translating = true;
    m_translationInImage = true;
    m_editingKey = m_target.key;
    m_translationKey = m_target.key;
    m_textDocument = nullptr;
    entry->editing = false;
    applyPresentation(entry->overlayTranslation.presentation);
    startTextRender();
    emit textEditingChanged(false);
    synchronizeUiState();
    return true;
#else
    return false;
#endif
}

void ScreenshotRecognitionSessionController::synchronizeUiState() const {
    updateRecognitionModels();
    if (m_actions.setShowOriginalImage) {
        m_actions.setShowOriginalImage(m_showOriginalImage);
    }
    if (m_active && m_actions.setActiveMode) {
        m_actions.setActiveMode(static_cast<int>(m_mode));
    }
    updateTextState();
    updateConversionState();
    if (m_active && m_mode == Mode::Table && content() != nullptr) {
        updateTableState(content()->tableCommandState());
    }
}

void ScreenshotRecognitionSessionController::endTextEditing() {
    if ((!m_editing && !m_translating) || !hasTextResult()) {
        return;
    }
    auto it = m_textCache.find(m_editingKey);
    if (it != m_textCache.end()) {
        it->editing = false;
    }
    m_editing = false;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    m_translating = false;
#endif
    m_editingKey.clear();
    m_textDocument = nullptr;
    if (content() != nullptr) {
        content()->hideTextEditor();
    }
    const auto entry = m_textCache.value(m_textCacheKey);
    if (entry.formatted) {
        applyFormattedText(entry.formattedDocument);
    } else {
        applyPresentation(entry.presentation);
        startTextRender();
    }
    emit textEditingChanged(false);
    updateTextState();
}

void ScreenshotRecognitionSessionController::resetTextEditing() {
    if (originalImageTranslationActive()) {
        return;
    }
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (m_translating) {
        auto it = m_textCache.find(m_editingKey);
        if (it != m_textCache.end() && it->translationSession != nullptr &&
            it->hasSuccessfulTranslation &&
            it->translationStatus != TextCacheEntry::TranslationStatus::Streaming) {
            it->translationSession->clearTransforms();
            it->translationSession->replaceText(it->successfulTranslation);
        }
        updateTextState();
        return;
    }
#endif
    if (m_editing) {
        auto it = m_textCache.find(m_editingKey);
        if (it != m_textCache.end() && it->editingSession != nullptr) {
            it->editingSession->clearTransforms();
            static_cast<void>(it->editingSession->replaceText(it->editingSession->originalText()));
        }
    }
    updateTextState();
}

void ScreenshotRecognitionSessionController::applyTextTargetLanguage(const QString& language) {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (m_translationService == nullptr)
        return;
    auto preferences = m_translationService->preferences();
    if (preferences.targetLanguage != language) {
        preferences.targetLanguage = language;
        if (!m_translationService->savePreferences(preferences))
            showStatus(m_translationService->errorText(), true);
    }
    if (m_actions.setTextTargetLanguage)
        m_actions.setTextTargetLanguage(m_translationService->preferences().targetLanguage);
#else
    Q_UNUSED(language);
#endif
}

void ScreenshotRecognitionSessionController::openTranslationSettings() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (m_translationService == nullptr) {
        showStatus(tr("Translation service is unavailable"), true);
        return;
    }
    if (m_translationSettingsModal != nullptr)
        return;
    QWidget* owner =
        m_actions.translationSettingsOwner ? m_actions.translationSettingsOwner() : content();
    if (owner == nullptr)
        owner = content();
    if (owner == nullptr)
        return;
    m_translationSettingsModal = snow_shot::presentation::createScreenshotTranslationSettingsDialog(
        *m_translationService, owner, this,
        [this, restart = std::make_shared<bool>(false)](bool before) {
            if (before) {
                *restart = m_translating;
                if (*restart)
                    endTextEditing();
            } else if (*restart) {
                invalidateCurrentTranslation(false);
                beginTextTranslation();
            }
        });
    connect(m_translationSettingsModal, &adqt::widgets::AdModal::finished, this,
            [this] { m_translationSettingsModal = nullptr; });
#endif
}

void ScreenshotRecognitionSessionController::startTranslation() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    auto it = m_textCache.find(m_translationKey);
    if (it == m_textCache.end())
        return;
    if (it->translationJob != nullptr && it->translationJob->busy())
        return;
    if (m_translationService == nullptr) {
        failTranslationPreparation(tr("Translation service is unavailable"));
        return;
    }
    it->translationConfiguration =
        snow_shot::storage::ScreenshotTranslationSettings().configuration();
    it->hasTranslationConfiguration = true;
    const QString key = m_translationKey;
    const quint64 generation = ++m_translationGeneration;
    const bool inImage = m_translationInImage;
    if (inImage)
        it->overlayTranslation.failureReported = false;
    if (it->translationJob != nullptr && it->jobInImage != inImage) {
        it->translationJob->cancel();
        it->translationJob->deleteLater();
        it->translationJob = nullptr;
    }
    if (it->translationJob == nullptr) {
        QStringList texts;
        if (inImage) {
            for (const auto& line : it->overlayTranslation.presentation->lines)
                texts.append(line.text);
        } else {
            texts.append(it->editingSession != nullptr ? it->editingSession->originalText()
                                                       : QString());
            it->translationText.clear();
            if (it->translationSession != nullptr)
                it->translationSession->replaceTextWithoutHistory({});
        }
        it->translationJob = m_translationService->createJob(texts, this);
        it->jobInImage = inImage;
    }
    auto* job = it->translationJob.data();
    disconnect(job, nullptr, this, nullptr);
    connect(job, &TranslationJob::unitChanged, this,
            [this, job, key, generation, inImage](int index) {
                if (generation != m_translationGeneration || key != m_translationKey)
                    return;
                auto entry = m_textCache.find(key);
                if (entry == m_textCache.end())
                    return;
                const auto& text = job->units().at(index).text;
                if (!inImage) {
                    entry->translationText = text;
                    if (entry->translationSession != nullptr)
                        entry->translationSession->replaceTextWithoutHistory(text);
                    return;
                }
                entry->overlayTranslation.presentation->setLineText(index, text);
                if (m_active && m_mode == Mode::Text && originalImageTranslationActive() &&
                    key == m_editingKey) {
                    if (m_actions.updateOcrText)
                        m_actions.updateOcrText(index, text);
                    else if (content() != nullptr)
                        content()->updateOcrText(index, text);
                }
            });
    connect(job, &TranslationJob::stateChanged, this, [this, job, key, generation, inImage] {
        if (generation != m_translationGeneration)
            return;
        auto entry = m_textCache.find(key);
        if (entry == m_textCache.end())
            return;
        const auto status = job->busy() ? TextCacheEntry::TranslationStatus::Streaming
                            : job->state() == TranslationJob::State::Completed
                                ? TextCacheEntry::TranslationStatus::Completed
                                : TextCacheEntry::TranslationStatus::Failed;
        if (inImage)
            entry->overlayTranslation.status = status;
        else
            entry->translationStatus = status;
        if (job->state() != TranslationJob::State::WaitingForModels)
            entry->translationConfiguration.modelId = job->preferences().modelId;
        if (content() != nullptr && m_translating && !inImage)
            content()->setTextEditorStreaming(job->busy());
        updateTextState();
    });
    connect(job, &TranslationJob::finished, this, [this, job, key, generation, inImage] {
        if (generation != m_translationGeneration)
            return;
        if (inImage)
            reportOverlayTranslationFailure();
        else
            handleTranslationFinished(generation, key, job->result());
    });
    job->retry();
#endif
}

void ScreenshotRecognitionSessionController::prepareOverlayTranslation(TextCacheEntry& entry) {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    auto& overlay = entry.overlayTranslation;
    if (overlay.presentation != nullptr || entry.presentation == nullptr) {
        return;
    }
    overlay.presentation = std::make_shared<ScreenshotOcrPresentation>();
    overlay.presentation->selection = entry.presentation->selection;
    overlay.presentation->lines =
        snow_shot::storage::ScreenshotTranslationSettings().layoutProcessing() ==
                QStringLiteral("original")
            ? entry.presentation->lines
            : snow_shot::presentation::mergeOcrLayout(entry.presentation->lines,
                                                      entry.presentation->selection.topLeft());
    overlay.presentation->prepareForRendering();
#else
    Q_UNUSED(entry)
#endif
}

void ScreenshotRecognitionSessionController::reportOverlayTranslationFailure() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (!m_active || m_mode != Mode::Text || !originalImageTranslationActive()) {
        return;
    }
    auto it = m_textCache.find(m_translationKey);
    if (it != m_textCache.end() &&
        it->overlayTranslation.status == TextCacheEntry::TranslationStatus::Failed &&
        !it->overlayTranslation.failureReported) {
        it->overlayTranslation.failureReported = true;
        showStatus(tr("Some text could not be translated"), true);
    }
#endif
}

void ScreenshotRecognitionSessionController::failTranslationPreparation(const QString& message) {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    auto it = m_textCache.find(m_translationKey);
    if (it == m_textCache.end()) {
        return;
    }
    if (m_translationInImage) {
        it->overlayTranslation.status = TextCacheEntry::TranslationStatus::Failed;
        reportOverlayTranslationFailure();
    } else {
        it->translationStatus = TextCacheEntry::TranslationStatus::Failed;
        if (content() != nullptr && m_translating) {
            content()->setTextEditorStreaming(false);
        }
        if (m_translating) {
            showStatus(message.isEmpty() ? tr("Translation failed") : message, true);
        }
    }
    updateTextState();
#else
    Q_UNUSED(message)
#endif
}

void ScreenshotRecognitionSessionController::cancelTranslationRequests() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    ++m_translationGeneration;
    auto it = m_textCache.find(m_translationKey);
    if (it == m_textCache.end())
        return;
    if (it->translationJob != nullptr) {
        disconnect(it->translationJob, nullptr, this, nullptr);
        it->translationJob->cancel();
    }
    if (it->translationStatus == TextCacheEntry::TranslationStatus::Streaming)
        it->translationStatus = TextCacheEntry::TranslationStatus::Absent;
    if (it->overlayTranslation.status == TextCacheEntry::TranslationStatus::Streaming)
        it->overlayTranslation.status = TextCacheEntry::TranslationStatus::Failed;
#endif
}

void ScreenshotRecognitionSessionController::handleTranslationFinished(
    quint64 generation, const QString& key, SnowShotTranslationResult result) {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (generation != m_translationGeneration) {
        return;
    }
    auto it = m_textCache.find(key);
    if (it == m_textCache.end() || it->translationSession == nullptr) {
        return;
    }
    if (result.succeeded()) {
        it->translationStatus = TextCacheEntry::TranslationStatus::Completed;
        it->successfulTranslation = it->translationText;
        it->hasSuccessfulTranslation = true;
        it->translationSession->establishBaseline(it->translationText);
    } else {
        it->translationStatus = TextCacheEntry::TranslationStatus::Failed;
        it->translationSession->establishHistory(it->translationText);
        if (!result.cancelled) {
            showStatus(result.error.isEmpty() ? tr("Translation failed") : result.error, true);
        }
    }
    if (content() != nullptr && m_translating && key == m_editingKey) {
        content()->setTextEditorStreaming(false);
    }
    updateTextState();
#else
    Q_UNUSED(generation)
    Q_UNUSED(key)
    Q_UNUSED(result)
#endif
}

void ScreenshotRecognitionSessionController::invalidateCurrentTranslation(bool restartIfVisible) {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    auto it = m_textCache.find(m_translationKey.isEmpty() ? m_textCacheKey : m_translationKey);
    if (it == m_textCache.end()) {
        return;
    }
    cancelTranslationRequests();
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (it->translationJob != nullptr) {
        it->translationJob->deleteLater();
        it->translationJob = nullptr;
    }
#endif

    it->translationStatus = TextCacheEntry::TranslationStatus::Absent;
    it->translationText.clear();
    it->successfulTranslation.clear();
    it->hasSuccessfulTranslation = false;
    it->hasTranslationConfiguration = false;
    it->overlayTranslation = {};
    if (it->translationSession != nullptr) {
        it->translationSession->establishHistory(QString());
    }
    if (originalImageTranslationActive()) {
        prepareOverlayTranslation(*it);
        applyPresentation(it->overlayTranslation.presentation);
        startTextRender();
    }
    updateTextState();
    emit recognitionResultsChanged();
    if (restartIfVisible) {
        startTranslation();
    }
#else
    Q_UNUSED(restartIfVisible)
#endif
}

void ScreenshotRecognitionSessionController::applyTextFormatting(const QString& value) {
    if (originalImageTranslationActive()) {
        return;
    }
    if (!m_editing && !m_translating) {
        beginTextEditing();
    }
    const auto entry = m_textCache.value(m_editingKey);
    const auto session = currentEditingSession(entry);
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const bool streaming =
        m_translating && entry.translationStatus == TextCacheEntry::TranslationStatus::Streaming;
#else
    constexpr bool streaming = false;
#endif
    if (session != nullptr && !streaming) {
        QString smartText;
        if (value == QStringLiteral("smart") && entry.presentation != nullptr) {
            auto presentation = *entry.presentation;
            if (m_translating) {
                // Translated drafts have no per-line OCR geometry of their own.
                const QRectF bounds(entry.presentation->selection);
                presentation.lines = {{session->text(),
                                       1.0,
                                       {bounds.topLeft(), bounds.topRight(), bounds.bottomRight(),
                                        bounds.bottomLeft()}}};
            }
            smartText = snow_shot::presentation::smartOcrText(presentation);
        }
        static_cast<void>(session->setFormatting(value, smartText));
        updateTextState();
    }
}

void ScreenshotRecognitionSessionController::applyTextPunctuation(const QString& value) {
    if (originalImageTranslationActive()) {
        return;
    }
    if (!m_editing && !m_translating) {
        beginTextEditing();
    }
    const auto entry = m_textCache.value(m_editingKey);
    const auto session = currentEditingSession(entry);
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const bool streaming =
        m_translating && entry.translationStatus == TextCacheEntry::TranslationStatus::Streaming;
#else
    constexpr bool streaming = false;
#endif
    if (session != nullptr && !streaming) {
        static_cast<void>(session->setPunctuation(value));
        updateTextState();
    }
}

bool ScreenshotRecognitionSessionController::editing() const {
    return m_editing || (m_translating && !m_translationInImage);
}

bool ScreenshotRecognitionSessionController::translating() const {
    return m_translating;
}

bool ScreenshotRecognitionSessionController::originalImageTranslationActive() const {
    return m_translating && m_translationInImage;
}

void ScreenshotRecognitionSessionController::setShowOriginalImage(bool show) {
    show = show && m_active;
    if (m_showOriginalImage == show) {
        return;
    }
    m_showOriginalImage = show;
    if (content() != nullptr) {
        content()->setShowOriginalImage(show);
    }
    if (m_actions.setShowOriginalImage) {
        m_actions.setShowOriginalImage(show);
    }
}

bool ScreenshotRecognitionSessionController::originalImageVisible() const {
    return m_active && m_mode == Mode::Text && !editing() && !m_showOriginalImage;
}

bool ScreenshotRecognitionSessionController::hasTextResult() const {
    const QString key = m_textCacheKey.isEmpty() ? m_target.key : m_textCacheKey;
    return !key.isEmpty() && m_textCache.contains(key);
}

QString ScreenshotRecognitionSessionController::textDraft() const {
    const QString key = m_editingKey.isEmpty() ? m_textCacheKey : m_editingKey;
    const auto entry = m_textCache.value(key);
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (originalImageTranslationActive() && entry.overlayTranslation.presentation != nullptr) {
        return snow_shot::presentation::originalOcrText(*entry.overlayTranslation.presentation);
    }
#endif
    const auto session = currentEditingSession(entry);
    return session != nullptr ? session->text() : QString{};
}

QString ScreenshotRecognitionSessionController::sourceTextDraft() const {
    const QString key = m_editingKey.isEmpty() ? m_textCacheKey : m_editingKey;
    const auto session = m_textCache.value(key).editingSession;
    return session != nullptr ? session->text() : QString{};
}

QString ScreenshotRecognitionSessionController::originalText() const {
    const QString key = m_editingKey.isEmpty() ? m_textCacheKey : m_editingKey;
    const auto session = m_textCache.value(key).editingSession;
    return session != nullptr ? session->originalText() : QString{};
}

QString ScreenshotRecognitionSessionController::latexDraft() const {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (const auto session = m_latexCache.value(recognitionCacheKey(Mode::Latex)))
        return session->text();
    return m_latexResults.value(recognitionCacheKey(Mode::Latex)).latex;
#else
    return {};
#endif
}

std::optional<ScreenshotRecognitionFileSnapshot>
ScreenshotRecognitionSessionController::fileExportSnapshot() const {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION
    if (!m_active)
        return std::nullopt;
    switch (m_mode) {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    case Mode::Html:
        return ScreenshotRecognitionFileSnapshot{ScreenshotRecognitionFileKind::Html,
                                                 m_conversion->source()};
    case Mode::Markdown:
        return ScreenshotRecognitionFileSnapshot{ScreenshotRecognitionFileKind::Markdown,
                                                 m_conversion->source()};
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    case Mode::Latex:
        return ScreenshotRecognitionFileSnapshot{ScreenshotRecognitionFileKind::Latex,
                                                 latexDraft()};
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    case Mode::Qr:
        return ScreenshotRecognitionFileSnapshot{ScreenshotRecognitionFileKind::Qr,
                                                 m_qrContents.join(QLatin1Char('\n'))};
#endif
    case Mode::Text:
    case Mode::Table:
    default:
        return std::nullopt;
    }
#else
    return std::nullopt;
#endif
}

std::unique_ptr<QMimeData> ScreenshotRecognitionSessionController::recognitionClipboardMimeData(
    const ScreenshotOcrPresentation* displayedPresentation) const {
    auto mimeData = std::make_unique<QMimeData>();
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (conversionModeActive()) {
        if (m_conversion->source().isEmpty()) {
            return {};
        }
        mimeData->setText(m_conversion->source());
        return mimeData;
    }
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_mode == Mode::Latex) {
        if (!m_latexResults.contains(recognitionCacheKey(Mode::Latex)))
            return {};
        mimeData->setText(latexDraft());
        return mimeData;
    }
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    if (m_mode == Mode::Qr) {
        if (m_qrCacheKey.isEmpty() || !m_qrCache.contains(m_qrCacheKey)) {
            return {};
        }
        mimeData->setText(m_qrContents.join(QLatin1Char('\n')));
        return mimeData;
    }
#endif
    if (m_mode == Mode::Table) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
        if (m_tableSession == nullptr || m_tableSession->document.empty()) {
            return {};
        }
        return m_tableSession->document.toClipboardMimeData();
#endif
    }

    QString text;
    bool resultAvailable = false;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (originalImageTranslationActive()) {
        const auto entry = m_textCache.value(m_translationKey);
        const auto* presentation = displayedPresentation != nullptr
                                       ? displayedPresentation
                                       : entry.overlayTranslation.presentation.get();
        resultAvailable = presentation != nullptr;
        if (presentation != nullptr) {
            text = presentation->hasTextSelection()
                       ? presentation->selectedText()
                       : snow_shot::presentation::originalOcrText(*presentation);
        }
    } else
#endif
        if (editing()) {
        resultAvailable = hasTextResult();
        text = textDraft();
    } else if (displayedPresentation != nullptr) {
        resultAvailable = true;
        text = displayedPresentation->hasTextSelection()
                   ? displayedPresentation->selectedText()
                   : snow_shot::presentation::originalOcrText(*displayedPresentation);
    } else {
        resultAvailable = hasTextResult();
        text = originalText();
    }
    if (!resultAvailable) {
        return {};
    }
    const QString key = m_textCacheKey.isEmpty() ? m_target.key : m_textCacheKey;
    if (!editing() && !originalImageTranslationActive() && !m_textCache.value(key).formatted) {
        const snow_shot::storage::TextRecognitionSettings settings;
        const auto* presentation = displayedPresentation != nullptr
                                       ? displayedPresentation
                                       : m_textCache.value(key).presentation.get();
        text = presentation != nullptr
                   ? snow_shot::presentation::applyOcrTextTransforms(
                         *presentation, settings.defaultFormatting(), settings.defaultPunctuation())
                   : snow_shot::presentation::applyOcrTextTransforms(
                         text, settings.defaultFormatting(), settings.defaultPunctuation());
    }
    mimeData->setText(text);
    return mimeData;
}

void ScreenshotRecognitionSessionController::setTextDraft(const QString& text) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_active && m_mode == Mode::Latex) {
        if (const auto session = m_latexCache.value(recognitionCacheKey(Mode::Latex)))
            static_cast<void>(session->replaceText(text));
        return;
    }
#endif
    if (originalImageTranslationActive()) {
        return;
    }
    const QString key = m_editingKey.isEmpty() ? m_textCacheKey : m_editingKey;
    auto it = m_textCache.find(key);
    if (it == m_textCache.end()) {
        return;
    }
    const auto session = currentEditingSession(*it);
    if (session != nullptr) {
        static_cast<void>(session->replaceText(text));
    }
}

void ScreenshotRecognitionSessionController::startTextRecognition(
    ScreenshotOcrRequestPriority priority) {
    if (!hasTarget() || m_recognition == nullptr || m_textRequestToken != 0 ||
        !screenshotOcrImageWithinPixelLimit(m_target.image.size())) {
        if (m_active && hasTarget() && !screenshotOcrImageWithinPixelLimit(m_target.image.size())) {
            showStatus(tr("Text recognition is unavailable for screenshots larger than 4K"), false);
        }
        return;
    }
    const quint64 generation = ++m_textGeneration;
    const QString key = m_target.key;
    m_textModelDownloadShown = false;
    m_textModelDownloadInProgress = !m_recognition->modelFilesReady();
    if (m_textModelDownloadInProgress && textModelDownloading()) {
        showModelDownloadMessage();
    } else if (m_active) {
        showRecognitionMessage();
    }
    ScreenshotOcrRequest request;
    request.image = m_target.image;
    request.canvasRect = m_target.canvasRect;
    request.priority = priority;
    request.renderFilteredImage =
        m_active && m_mode == Mode::Text && shouldRenderRecognitionInWorker();
    if (request.renderFilteredImage && m_actions.ocrBackgroundColor) {
        request.backgroundColor = m_actions.ocrBackgroundColor();
    }
    const auto callbackCompleted = std::make_shared<bool>(false);
    m_textRequestToken = m_recognition->recognize(
        std::move(request), this,
        [this, generation, key, callbackCompleted](ScreenshotOcrRecognitionResult output) {
            *callbackCompleted = true;
            if (generation == m_textGeneration) {
                m_textRequestToken = 0;
            }
            handleTextOutput(generation, key, std::move(output));
        });
    if (*callbackCompleted) {
        m_textRequestToken = 0;
    }
    updateBusyState();
    if (m_textRequestToken == 0 && !*callbackCompleted) {
        if (m_active || m_textModelDownloadShown) {
            showStatus(tr("Text recognition request could not be prepared"), true);
        }
        hideModelDownloadMessage();
        m_textModelDownloadInProgress = false;
        hideRecognitionMessage();
    } else if (m_textModelDownloadInProgress) {
        pollTextModelDownload(generation);
    }
}

void ScreenshotRecognitionSessionController::renderTextBackground() {
    if (m_active && m_mode == Mode::Text) {
        startTextRender();
    }
}

void ScreenshotRecognitionSessionController::startTextRender() {
    if (!m_active || m_mode != Mode::Text || !hasTarget() || m_recognition == nullptr ||
        m_presentation == nullptr || !m_actions.applyOcrBackgroundImage) {
        return;
    }
    if (m_textRenderRequestToken != 0) {
        m_recognition->cancel(m_textRenderRequestToken);
        m_textRenderRequestToken = 0;
    }
    const quint64 generation = ++m_textRenderGeneration;
    const QString key = m_target.key;
    ScreenshotOcrRequest request;
    request.image = m_target.image;
    request.canvasRect = m_target.canvasRect;
    request.priority = ScreenshotOcrRequestPriority::Interactive;
    request.presentation = m_presentation;
    request.backgroundColor =
        m_actions.ocrBackgroundColor ? m_actions.ocrBackgroundColor() : QColor();
    if (m_actions.prepareOcrRenderRequest) {
        m_actions.prepareOcrRenderRequest(request);
    }
    // Streaming text is mutable on the GUI thread; background workers own a snapshot.
    if (request.presentation != nullptr) {
        request.presentation = std::make_shared<ScreenshotOcrPresentation>(*request.presentation);
    }
    const auto callbackCompleted = std::make_shared<bool>(false);
    m_textRenderRequestToken = m_recognition->render(
        std::move(request), this,
        [this, generation, key, callbackCompleted](ScreenshotOcrRecognitionResult output) {
            *callbackCompleted = true;
            if (generation == m_textRenderGeneration) {
                m_textRenderRequestToken = 0;
            }
            const bool current = generation == m_textRenderGeneration && key == m_target.key &&
                                 m_active && m_mode == Mode::Text;
            if (!current) {
                updateBusyState();
                return;
            }
            if (!output.error.isEmpty() || output.filteredImage.isNull()) {
                updateBusyState();
                hideRecognitionMessage();
                return;
            }
            if (m_actions.applyOcrBackgroundImage) {
                m_actions.applyOcrBackgroundImage(m_presentation, std::move(output.filteredImage),
                                                  output.filteredImageCanvasRect);
            }
            updateBusyState();
            hideRecognitionMessage();
        });
    if (*callbackCompleted) {
        m_textRenderRequestToken = 0;
    }
    updateBusyState();
    emit recognitionResultsChanged();
}

void ScreenshotRecognitionSessionController::startTableRecognition() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (!hasTarget() || m_tableRequestToken || m_tableParsing)
        return;
    if (!m_tableRecognition) {
        failModelRecognition(tr("Table recognition service is unavailable"));
        return;
    }
    const QString model = resolvedRecognitionModel(Mode::Table);
    if (model == screenshotDedicatedRecognitionModelId() &&
        !screenshotOcrImageWithinPixelLimit(m_target.image.size())) {
        failModelRecognition(tr("Table recognition is unavailable for screenshots larger than 4K"));
        return;
    }
    const quint64 generation = ++m_tableGeneration;
    const QString key = recognitionCacheKey(Mode::Table, model);
    showRecognitionMessage();
    if (content())
        content()->clearRecognitionError();
    const auto callbackCompleted = std::make_shared<bool>(false);
    const auto completion = [this, generation, key, callbackCompleted](SnowShotTableResult result) {
        *callbackCompleted = true;
        if (generation == m_tableGeneration)
            m_tableRequestToken = 0;
        handleTableOutput(generation, key, std::move(result));
    };
    m_tableRequestToken =
        model == screenshotDedicatedRecognitionModelId()
            ? m_tableRecognition->extractTable(m_target.image, this, completion)
            : m_tableRecognition->extractTableVision(m_target.image, model, this, completion);
    if (*callbackCompleted)
        m_tableRequestToken = 0;
    updateBusyState();
    if (!m_tableRequestToken && !*callbackCompleted)
        failModelRecognition(tr("Table recognition request could not be prepared"));
#endif
}

void ScreenshotRecognitionSessionController::startLatexRecognition() {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (!hasTarget() || m_latexRequestToken)
        return;
    if (!m_tableRecognition) {
        failModelRecognition(tr("LaTeX recognition service is unavailable"));
        return;
    }
    const QString model = resolvedRecognitionModel(Mode::Latex);
    if (model == screenshotDedicatedRecognitionModelId() &&
        !screenshotOcrImageWithinPixelLimit(m_target.image.size())) {
        failModelRecognition(tr("LaTeX recognition is unavailable for screenshots larger than 4K"));
        return;
    }
    const quint64 generation = ++m_latexGeneration;
    const QString key = recognitionCacheKey(Mode::Latex, model);
    showRecognitionMessage();
    if (content())
        content()->clearRecognitionError();
    const auto callbackCompleted = std::make_shared<bool>(false);
    const auto completion = [this, generation, key, callbackCompleted](SnowShotLatexResult result) {
        *callbackCompleted = true;
        if (generation == m_latexGeneration)
            m_latexRequestToken = 0;
        handleLatexOutput(generation, key, std::move(result));
    };
    m_latexRequestToken =
        model == screenshotDedicatedRecognitionModelId()
            ? m_tableRecognition->extractLatex(m_target.image, this, completion)
            : m_tableRecognition->extractLatexVision(m_target.image, model, this, completion);
    if (*callbackCompleted)
        m_latexRequestToken = 0;
    updateBusyState();
    if (!m_latexRequestToken && !*callbackCompleted)
        failModelRecognition(tr("LaTeX recognition request could not be prepared"));
#endif
}

void ScreenshotRecognitionSessionController::startQrRecognition() {
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    if (!hasTarget() || m_qrRecognition == nullptr || m_qrRequestToken != 0 ||
        !screenshotOcrImageWithinPixelLimit(m_target.image.size())) {
        if (m_qrRecognition == nullptr) {
            showStatus(tr("Barcode recognition is unavailable"), true);
        }
        return;
    }
    const quint64 generation = ++m_qrGeneration;
    const QString key = m_target.key;
    showRecognitionMessage();
    const auto callbackCompleted = std::make_shared<bool>(false);
    m_qrRequestToken = m_qrRecognition->recognize(
        m_target.image, this,
        [this, generation, key, callbackCompleted](ScreenshotQrRecognitionResult result) {
            *callbackCompleted = true;
            if (generation == m_qrGeneration) {
                m_qrRequestToken = 0;
            }
            handleQrOutput(generation, key, std::move(result));
        });
    if (*callbackCompleted) {
        m_qrRequestToken = 0;
    }
    updateBusyState();
    if (m_qrRequestToken == 0 && !*callbackCompleted) {
        showStatus(tr("Barcode recognition request could not be prepared"), true);
        hideRecognitionMessage();
    }
#endif
}

void ScreenshotRecognitionSessionController::handleTextOutput(
    quint64 generation, const QString& key, ScreenshotOcrRecognitionResult output) {
    if (generation != m_textGeneration || key != m_target.key) {
        return;
    }
    const bool modelDownloadWasShown = m_textModelDownloadShown;
    hideModelDownloadMessage();
    m_textModelDownloadInProgress = false;
    if (!output.error.isEmpty() || output.presentation == nullptr) {
        if ((m_active && m_mode == Mode::Text) || modelDownloadWasShown) {
            showStatus(output.error.isEmpty() ? tr("Text recognition failed") : output.error, true);
        }
        hideRecognitionMessage();
        updateBusyState();
        return;
    }
    const QString original = snow_shot::presentation::originalOcrText(*output.presentation);
    auto editingSession = std::make_shared<ScreenshotOcrTextEditingSession>(original);
    connect(editingSession->document(), &QTextDocument::contentsChanged, this,
            [this, key]() { handleTextDocumentChanged(key); });
    QImage filteredImage = std::move(output.filteredImage);
    if (backgroundFillEnabled()) {
        // An already-running blur may complete after the fill preference changes.
        filteredImage = {};
    }
    QRectF filteredImageCanvasRect = output.filteredImageCanvasRect;
    TextCacheEntry entry;
    entry.recognitionResult = output;
    entry.presentation = output.presentation;
    entry.editingSession = std::move(editingSession);
    m_textCache.insert(key, std::move(entry));
    if (m_active && m_mode == Mode::Text) {
        m_textCacheKey = key;
        m_presentation = m_textCache.value(key).presentation;
        applyPresentation(m_presentation, filteredImage, filteredImageCanvasRect);
        if (filteredImage.isNull()) {
            startTextRender();
        }
        emit textResultChanged(true);
    }
    hideRecognitionMessage();
    updateBusyState();
    updateTextState();
    emit recognitionResultsChanged();
}

void ScreenshotRecognitionSessionController::handleTableOutput(quint64 generation,
                                                               const QString& key,
                                                               SnowShotTableResult result) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (generation != m_tableGeneration || key != recognitionCacheKey(Mode::Table))
        return;
    if (!result.succeeded()) {
        failModelRecognition(result.error.isEmpty() ? tr("Table recognition failed")
                                                    : result.error);
        return;
    }
    ScreenshotTableRecognitionEntry entry;
    entry.model = resolvedRecognitionModel(Mode::Table);
    entry.modelFingerprint = m_tableRecognition->modelFingerprint(entry.model);
    entry.result = std::move(result);
    materializeTableEntry(generation, key, std::move(entry), false);
#else
    Q_UNUSED(generation)
    Q_UNUSED(key)
    Q_UNUSED(result)
#endif
}

void ScreenshotRecognitionSessionController::materializeTableEntry(
    quint64 generation, const QString& key, ScreenshotTableRecognitionEntry entry,
    bool allowSynchronous) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_tableParsing || generation != m_tableGeneration)
        return;
    m_tableParsing = true;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_tableParsingCancellation = cancelled;
    const QString target = m_target.key;
    const QPointer<ScreenshotRecognitionSessionController> guard(this);
    using Documents = std::pair<ScreenshotTableDocument, std::optional<ScreenshotTableDocument>>;
    const auto parse = [entry, cancelled]() -> Documents {
        if (cancelled->load(std::memory_order_relaxed))
            return {};
        const bool vision = entry.model != screenshotDedicatedRecognitionModelId();
        auto baseline = ScreenshotTableDocument::fromHtml(entry.result.html, vision);
        if (baseline.empty() || cancelled->load(std::memory_order_relaxed))
            return {};
        std::optional<ScreenshotTableDocument> draft;
        if (entry.draftHtml) {
            draft = ScreenshotTableDocument::fromHtml(*entry.draftHtml, vision);
            if (draft->empty() || cancelled->load(std::memory_order_relaxed))
                return {};
        }
        return {std::move(baseline), std::move(draft)};
    };
    auto complete = [guard, cancelled, generation, key, target,
                     entry](Documents documents) mutable {
        if (!guard || cancelled->load(std::memory_order_relaxed) ||
            generation != guard->m_tableGeneration || target != guard->m_target.key)
            return;
        guard->m_tableParsing = false;
        guard->m_tableParsingCancellation.reset();
        if (key != guard->recognitionCacheKey(Mode::Table) ||
            !guard->recognitionEntryValid(entry.model, entry.modelFingerprint)) {
            guard->failModelRecognition(
                tr("Recognition service changed. Retry to use the updated settings."));
            return;
        }
        if (documents.first.empty()) {
            guard->m_tableEntries.remove(key);
            guard->m_tableResults.remove(key);
            guard->m_tableEntryTargets.remove(key);
            guard->m_tableCache.remove(key);
            guard->failModelRecognition(tr("No table cells were recognized"));
            emit guard->recognitionResultsChanged();
            return;
        }
        // QUndoStack belongs to the GUI thread; workers return only value documents.
        auto session = std::make_shared<ScreenshotTableEditingSession>(std::move(documents.first));
        if (documents.second)
            session->document = std::move(*documents.second);
        guard->m_tableResults.insert(key, entry.result);
        guard->m_tableEntries.insert(key, std::move(entry));
        guard->m_tableEntryTargets.insert(key, target);
        guard->m_tableCache.insert(key, session);
        if (guard->m_active && guard->m_mode == Mode::Table) {
            guard->m_tableCacheKey = key;
            guard->applyTableSession(session);
        }
        guard->hideRecognitionMessage();
        guard->updateBusyState();
        guard->updateTextState();
        emit guard->recognitionResultsChanged();
    };
    constexpr qsizetype synchronousSourceBytes = 32 * 1024;
    const qsizetype sourceBytes =
        (entry.result.html.size() + (entry.draftHtml ? entry.draftHtml->size() : 0)) *
        static_cast<qsizetype>(sizeof(QChar));
    if (allowSynchronous && sourceBytes <= synchronousSourceBytes) {
        complete(parse());
        return;
    }
    showRecognitionMessage();
    updateBusyState();
    QThreadPool::globalInstance()->start([parse, complete, cancelled]() mutable {
        if (cancelled->load(std::memory_order_relaxed))
            return;
        snow_shot::platform::applyApplicationQoSToCurrentThread();
        auto documents = parse();
        if (cancelled->load(std::memory_order_relaxed))
            return;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [complete = std::move(complete), documents = std::move(documents)]() mutable {
                complete(std::move(documents));
            },
            Qt::QueuedConnection);
    });
#else
    Q_UNUSED(generation)
    Q_UNUSED(key)
    Q_UNUSED(entry)
    Q_UNUSED(allowSynchronous)
#endif
}

void ScreenshotRecognitionSessionController::handleLatexOutput(quint64 generation,
                                                               const QString& key,
                                                               SnowShotLatexResult result) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (generation != m_latexGeneration || key != recognitionCacheKey(Mode::Latex))
        return;
    if (result.succeeded()) {
        auto session = std::make_shared<ScreenshotOcrTextEditingSession>(result.latex, true);
        connect(session->document(), &QTextDocument::contentsChanged, this,
                [this, key]() { handleLatexDocumentChanged(key); });
        // A result can arrive after an active target was seeded. The editor borrows
        // that cached document until applyLatexContents binds the replacement.
        const auto previousSession = m_latexCache.value(key);
        ScreenshotLatexRecognitionEntry entry;
        entry.model = resolvedRecognitionModel(Mode::Latex);
        entry.modelFingerprint = m_tableRecognition->modelFingerprint(entry.model);
        entry.result = result;
        m_latexEntries.insert(key, std::move(entry));
        m_latexEntryTargets.insert(key, m_target.key);
        m_latexResults.insert(key, result);
        m_latexCache.insert(key, std::move(session));
        if (m_active && m_mode == Mode::Latex)
            applyLatexContents(result.latex);
        emit recognitionResultsChanged();
    } else if (m_active && m_mode == Mode::Latex) {
        failModelRecognition(result.error.isEmpty() ? tr("LaTeX recognition failed")
                                                    : result.error);
    }
    hideRecognitionMessage();
    updateBusyState();
#else
    Q_UNUSED(generation)
    Q_UNUSED(key)
    Q_UNUSED(result)
#endif
}

void ScreenshotRecognitionSessionController::applyLatexContents(const QString& source) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    Q_UNUSED(source)
    ensureContent();
    if (content())
        content()->clearRecognitionError();
    if (content() != nullptr) {
        if (const auto session = m_latexCache.value(recognitionCacheKey(Mode::Latex))) {
            content()->showLatexEditor(
                session->document(),
                [weakSession = std::weak_ptr<ScreenshotOcrTextEditingSession>(session)] {
                    const auto current = weakSession.lock();
                    return current ? current->text() : QString();
                });
        }
    }
    updateTextState();
#else
    Q_UNUSED(source)
#endif
}

void ScreenshotRecognitionSessionController::handleLatexDocumentChanged(const QString& key) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (key == recognitionCacheKey(Mode::Latex)) {
        updateTextState();
        emit recognitionResultsChanged();
    }
#else
    Q_UNUSED(key)
#endif
}

void ScreenshotRecognitionSessionController::handleQrOutput(quint64 generation, const QString& key,
                                                            ScreenshotQrRecognitionResult result) {
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    if (generation != m_qrGeneration || key != m_target.key) {
        return;
    }
    if (!result.error.isEmpty()) {
        if (m_active && m_mode == Mode::Qr) {
            showStatus(result.error, true);
        }
        hideRecognitionMessage();
        updateBusyState();
        return;
    }
    if (result.contents.isEmpty()) {
        m_qrCache.insert(key, result.contents);
        m_qrResults.insert(key, result);
        if (m_active && m_mode == Mode::Qr) {
            m_qrCacheKey = key;
            applyQrContents(result.contents);
            showStatus(tr("No barcode was recognized"), false);
        }
        hideRecognitionMessage();
        updateBusyState();
        emit recognitionResultsChanged();
        return;
    }
    m_qrCache.insert(key, result.contents);
    m_qrResults.insert(key, result);
    if (m_active && m_mode == Mode::Qr) {
        m_qrCacheKey = key;
        applyQrContents(result.contents);
    }
    hideRecognitionMessage();
    updateBusyState();
    emit recognitionResultsChanged();
#else
    Q_UNUSED(generation)
    Q_UNUSED(key)
    Q_UNUSED(result)
#endif
}

void ScreenshotRecognitionSessionController::ensureContent() {
    if (m_content == nullptr && m_actions.ensureContent) {
        m_content = m_actions.ensureContent();
        if (m_content != nullptr) {
            m_content->setShowOriginalImage(m_showOriginalImage);
            connect(m_content, &ScreenshotRecognitionWindow::recognitionRetryRequested, this,
                    &ScreenshotRecognitionSessionController::retryRecognition);
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
            connect(m_content, &ScreenshotRecognitionWindow::imageConversionRetryRequested,
                    m_conversion, &ScreenshotImageConversionController::retry);
#endif
        }
    }
    updateOriginalImagePreview();
}

void ScreenshotRecognitionSessionController::clearContent(bool retainTableEditor) {
    if (m_content != nullptr) {
        m_content->clearRecognitionError();
        m_content->clearOcrPresentation();
        m_content->clearFormattedText();
        m_content->clearTableSession(retainTableEditor);
        m_content->clearQrContents();
        m_content->clearImageConversion();
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
        m_content->clearLatexPreview();
#endif
    }
}

void ScreenshotRecognitionSessionController::applyPresentation(
    const std::shared_ptr<ScreenshotOcrPresentation>& presentation, QImage filteredImage,
    QRectF filteredImageCanvasRect) {
    m_presentation = presentation;
    ensureContent();
    if (presentation != nullptr) {
        prepareScreenshotOcrFillColors(*presentation, m_target.image, m_target.canvasRect,
                                       backgroundFillEnabled());
    }
    if (content() != nullptr) {
        content()->setOcrCopyDefaultsEnabled(!originalImageTranslationActive());
    }
    if (m_actions.applyOcrPresentation) {
        m_actions.applyOcrPresentation(presentation);
    } else if (content() != nullptr) {
        content()->setOcrPresentation(presentation);
    }
    if (m_actions.applyOcrBackground) {
        m_actions.applyOcrBackground(presentation);
    }
    if (!filteredImage.isNull() && m_actions.applyOcrBackgroundImage) {
        m_actions.applyOcrBackgroundImage(presentation, std::move(filteredImage),
                                          filteredImageCanvasRect);
    }
}

void ScreenshotRecognitionSessionController::applyTableSession(
    const std::shared_ptr<ScreenshotTableEditingSession>& session) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (session == nullptr || session->document.empty()) {
        return;
    }
    m_tableSession = session;
    ensureContent();
    if (content())
        content()->clearRecognitionError();
    if (content() != nullptr) {
        content()->setTableSession(session);
        updateTableState(content()->tableCommandState());
    }
#else
    Q_UNUSED(session)
#endif
}

void ScreenshotRecognitionSessionController::applyQrContents(const QStringList& contents) {
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    m_qrContents = contents;
    ensureContent();
    if (content() != nullptr) {
        content()->showQrContents(contents);
    }
#else
    Q_UNUSED(contents)
#endif
}

void ScreenshotRecognitionSessionController::handleTextDocumentChanged(const QString& key) {
    auto it = m_textCache.find(key);
    if (it == m_textCache.end() || it->editingSession == nullptr) {
        return;
    }
    const auto session = it->editingSession;
    session->recordCurrentText();
    if (key == (m_editingKey.isEmpty() ? m_textCacheKey : m_editingKey)) {
        emit textDraftChanged(session->text());
    }
    updateTextState();
}

bool ScreenshotRecognitionSessionController::shouldRenderRecognitionInWorker() const {
    return m_actions.applyOcrBackgroundImage && !backgroundFillEnabled() &&
           (!m_actions.renderRecognitionInWorker || m_actions.renderRecognitionInWorker());
}

void ScreenshotRecognitionSessionController::setPendingTextRecognitionRendering(bool enabled) {
    if (m_recognition == nullptr || m_textRequestToken == 0) {
        return;
    }
    const bool shouldRender = enabled && shouldRenderRecognitionInWorker();
    const QColor backgroundColor =
        shouldRender && m_actions.ocrBackgroundColor ? m_actions.ocrBackgroundColor() : QColor();
    static_cast<void>(
        m_recognition->setRenderFilteredImage(m_textRequestToken, shouldRender, backgroundColor));
}

void ScreenshotRecognitionSessionController::pollTextModelDownload(quint64 generation) {
    if (generation != m_textGeneration || m_textRequestToken == 0 ||
        !m_textModelDownloadInProgress || m_recognition == nullptr) {
        return;
    }
    if (m_recognition->modelFilesReady()) {
        m_textModelDownloadInProgress = false;
        if (m_textModelDownloadShown) {
            hideModelDownloadMessage();
            if (m_active && m_mode == Mode::Text) {
                showRecognitionMessage();
            }
        }
        return;
    }
    if (textModelDownloading()) {
        showModelDownloadMessage();
    } else if (m_textModelDownloadShown) {
        hideModelDownloadMessage();
        if (m_active && m_mode == Mode::Text) {
            showRecognitionMessage();
        }
    }
    QTimer::singleShot(100, this, [this, generation]() { pollTextModelDownload(generation); });
}

void ScreenshotRecognitionSessionController::applyFormattedText(
    const std::shared_ptr<QTextDocument>& document) {
    ensureContent();
    if (m_actions.applyFormattedText) {
        m_actions.applyFormattedText(document);
    } else if (content() != nullptr) {
        content()->showFormattedText(document);
    }
}

void ScreenshotRecognitionSessionController::handleTranslationDocumentChanged(const QString& key) {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    auto it = m_textCache.find(key);
    if (it == m_textCache.end() || it->translationSession == nullptr) {
        return;
    }
    if (it->translationStatus != TextCacheEntry::TranslationStatus::Streaming) {
        it->translationSession->recordCurrentText();
    }
    if (m_translating && key == m_editingKey) {
        emit textDraftChanged(it->translationSession->text());
        updateTextState();
    }
#else
    Q_UNUSED(key)
#endif
}

void ScreenshotRecognitionSessionController::clearTextEditingState() {
    for (auto it = m_textCache.begin(); it != m_textCache.end(); ++it) {
        it->editing = false;
    }
    m_editing = false;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    m_translating = false;
#endif
    m_editingKey.clear();
    m_textDocument = nullptr;
    if (content() != nullptr) {
        content()->hideTextEditor();
    }
    emit textEditingChanged(false);
}

void ScreenshotRecognitionSessionController::handleTableCommandState(
    const ScreenshotTableCommandState& state) {
    updateTableState(state);
}

void ScreenshotRecognitionSessionController::updateBusyState() const {
    emit workflowStateChanged();
    if (m_actions.setBusyState) {
        m_actions.setBusyState(busy(Mode::Text), busy(Mode::Table), busy(Mode::Qr));
    }
}

void ScreenshotRecognitionSessionController::updateTextState() const {
    updateOriginalImagePreview();
    emit workflowStateChanged();
    const bool available = hasTextResult() && m_active && m_mode == Mode::Text;
    const auto entry = m_textCache.value(m_editingKey);
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (m_translationService != nullptr && m_actions.setTextTargetLanguage)
        m_actions.setTextTargetLanguage(m_translationService->preferences().targetLanguage);
    const bool overlay = originalImageTranslationActive();
    const auto translation = m_textCache.constFind(m_translationKey);
    const bool streaming = translation != m_textCache.cend() &&
                           (m_translationInImage ? translation->overlayTranslation.status
                                                 : translation->translationStatus) ==
                               TextCacheEntry::TranslationStatus::Streaming;
#endif
    if (m_actions.setTextEditingState) {
        m_actions.setTextEditingState(
            available, m_editing,
            m_editing && entry.editingSession != nullptr && entry.editingSession->canUndo(),
            m_editing && entry.editingSession != nullptr && entry.editingSession->canRedo());
    }
    if (m_actions.setLatexEditingState) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
        const auto session = m_latexCache.value(recognitionCacheKey(Mode::Latex));
        const bool latexAvailable = m_active && m_mode == Mode::Latex && session != nullptr;
        m_actions.setLatexEditingState(latexAvailable, latexAvailable && session->canUndo(),
                                       latexAvailable && session->canRedo());
#else
        m_actions.setLatexEditingState(false, false, false);
#endif
    }
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (snow_shot::app::edition::textTranslation && m_actions.setTextTranslationState) {
        m_actions.setTextTranslationState(
            available, m_translating, streaming,
            m_translating && !overlay && !streaming && entry.translationSession != nullptr &&
                entry.translationSession->canUndo(),
            m_translating && !overlay && !streaming && entry.translationSession != nullptr &&
                entry.translationSession->canRedo(),
            m_translating && !overlay && !streaming && entry.hasSuccessfulTranslation, overlay);
    }
#endif
    if (m_actions.setTextTransformState) {
        const auto session = currentEditingSession(entry);
        m_actions.setTextTransformState(
            editing() && session != nullptr ? session->formatting() : QString{},
            editing() && session != nullptr ? session->punctuation() : QString{});
    }
}

void ScreenshotRecognitionSessionController::updateOriginalImagePreview() const {
    if (m_content == nullptr) {
        return;
    }
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    m_content->setLatexPreviewEnabled(m_active && m_mode == Mode::Latex &&
                                      m_latexCache.contains(recognitionCacheKey(Mode::Latex)));
#endif
    const bool textActive = m_active && m_mode == Mode::Text;
    if (!textActive) {
        m_content->setOriginalImagePreviewEnabled(false);
        m_content->setOriginalImagePreviewSource({}, {});
        return;
    }
    // The default image describes the screenshot selection. Embedded hosts can
    // provide their displayed image and viewport mapping without replacing it.
    m_content->setOriginalImagePreviewSource(m_target.image, m_target.canvasRect);
    m_content->setOriginalImagePreviewEnabled(
        snow_shot::storage::TextRecognitionSettings().showOriginalImagePreview());
}

void ScreenshotRecognitionSessionController::updateTableState(
    const ScreenshotTableCommandState& state) const {
    if (!m_actions.setTableEditingState) {
        return;
    }
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    const bool available = m_active && m_mode == Mode::Table && m_tableSession != nullptr;
#else
    constexpr bool available = false;
#endif
    m_actions.setTableEditingState(available, available && state.canUndo,
                                   available && state.canRedo, available && state.canMerge,
                                   available && state.canSplit, available && state.canReset);
}

bool ScreenshotRecognitionSessionController::textModelDownloading() const {
    return m_recognition != nullptr &&
           m_recognition->assetStatus().phase == ScreenshotOcrAssetPhase::Downloading;
}

void ScreenshotRecognitionSessionController::showModelDownloadMessage() {
    // Cache verification and helper start-up are already covered by the plain
    // recognition message; this prompt is reserved for real downloads.
    if (!textModelDownloading()) {
        return;
    }
    QString message = tr("Preparing text recognition components");
    if (m_recognition != nullptr) {
        const ScreenshotOcrAssetStatus status = m_recognition->assetStatus();
        if (status.totalBytes > 0) {
            const int percent = static_cast<int>(
                std::clamp<qint64>(status.receivedBytes * 100 / status.totalBytes, 0, 100));
            message = tr("Preparing text recognition components (%1%)").arg(percent);
        }
    }
    m_textModelDownloadShown = true;
    if (m_actions.showModelDownload) {
        m_actions.showModelDownload(message);
    } else if (m_actions.showStatus) {
        m_actions.showStatus(message, false);
    }
}

void ScreenshotRecognitionSessionController::hideModelDownloadMessage() {
    if (!m_textModelDownloadShown) {
        return;
    }
    m_textModelDownloadShown = false;
    if (m_actions.hideModelDownload) {
        m_actions.hideModelDownload();
    } else if (m_actions.hideLoading) {
        m_actions.hideLoading();
    }
}

void ScreenshotRecognitionSessionController::showRecognitionMessage() const {
    const QString message = m_mode == Mode::Table      ? tr("Recognizing table")
                            : m_mode == Mode::Latex    ? tr("Recognizing LaTeX formula")
                            : m_mode == Mode::Qr       ? tr("Recognizing barcode")
                            : m_mode == Mode::Markdown ? tr("Converting to Markdown")
                            : m_mode == Mode::Html     ? tr("Converting to HTML")
                                                       : tr("Recognizing text");
    if (m_actions.showRecognition) {
        m_actions.showRecognition(message);
    } else if (m_actions.showStatus) {
        m_actions.showStatus(message, false);
    }
}

void ScreenshotRecognitionSessionController::hideRecognitionMessage() const {
    if ((!m_active || !busy()) && m_actions.hideLoading) {
        m_actions.hideLoading();
    }
}

void ScreenshotRecognitionSessionController::showStatus(const QString& message, bool error) const {
    if (error)
        m_workflowError = message;
    emit workflowStateChanged();
    if (!message.isEmpty() && m_actions.showStatus) {
        m_actions.showStatus(message, error);
    }
}

void ScreenshotRecognitionSessionController::cancelOutstandingRequests() {
    cancelTranslationRequests();
    cancelModelRecognition(Mode::Table);
    cancelModelRecognition(Mode::Latex);
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_tableRecognition && m_latexRequestToken)
        m_tableRecognition->cancel(m_latexRequestToken);
#endif

#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    m_latexRequestToken = 0;
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    ++m_latexGeneration;
#endif
    if (m_recognition != nullptr && m_textRequestToken != 0) {
        m_recognition->cancel(m_textRequestToken);
    }
    if (m_recognition != nullptr && m_textRenderRequestToken != 0) {
        m_recognition->cancel(m_textRenderRequestToken);
    }
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    if (m_qrRecognition != nullptr && m_qrRequestToken != 0) {
        m_qrRecognition->cancel(m_qrRequestToken);
    }
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_tableRecognition != nullptr && m_tableRequestToken != 0) {
        m_tableRecognition->cancel(m_tableRequestToken);
    }
#endif
    m_textRequestToken = 0;
    m_textRenderRequestToken = 0;
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    m_qrRequestToken = 0;
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableRequestToken = 0;
#endif
    ++m_textRenderGeneration;
    hideModelDownloadMessage();
    m_textModelDownloadInProgress = false;
    hideRecognitionMessage();
}

void ScreenshotRecognitionSessionController::handleRecognitionProviderDestroyed(Mode mode) {
    bool requestWasPending = false;
    bool translationWasPending = false;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const auto translation = m_textCache.constFind(m_translationKey);
    const bool overlayWasPending =
        m_translationInImage && translation != m_textCache.cend() &&
        translation->overlayTranslation.status == TextCacheEntry::TranslationStatus::Streaming;
    if (mode != Mode::Qr && overlayWasPending) {
        cancelTranslationRequests();
        reportOverlayTranslationFailure();
    }
#else
    constexpr bool overlayWasPending = false;
#endif
    switch (mode) {
    case Mode::Markdown:
    case Mode::Html:
        return;
    case Mode::Text:
        requestWasPending = m_textRequestToken != 0;
        m_textRequestToken = 0;
        m_textRenderRequestToken = 0;
        ++m_textRenderGeneration;
        hideModelDownloadMessage();
        m_textModelDownloadInProgress = false;
        ++m_textGeneration;
        break;
    case Mode::Table:
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
        requestWasPending = m_tableRequestToken != 0 || m_tableParsing;
        m_tableRequestToken = 0;
        if (m_tableParsingCancellation)
            m_tableParsingCancellation->store(true, std::memory_order_relaxed);
        m_tableParsingCancellation.reset();
        m_tableParsing = false;
        ++m_tableGeneration;
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        ++m_translationGeneration;
        if (auto it = m_textCache.find(m_translationKey);
            it != m_textCache.end() &&
            it->translationStatus == TextCacheEntry::TranslationStatus::Streaming) {
            it->translationStatus = TextCacheEntry::TranslationStatus::Failed;
            if (it->translationSession != nullptr) {
                it->translationSession->establishHistory(it->translationText);
            }
        }
        if (content() != nullptr && m_translating) {
            content()->setTextEditorStreaming(false);
        }
        if (m_translationSettingsModal != nullptr) {
            m_translationSettingsModal->reject();
        }
#endif
        break;
    case Mode::Latex:
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
        requestWasPending = m_latexRequestToken != 0;
        m_latexRequestToken = 0;
        ++m_latexGeneration;
#endif
        break;
    case Mode::Qr:
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
        requestWasPending = m_qrRequestToken != 0;
        m_qrRequestToken = 0;
        ++m_qrGeneration;
#endif
        break;
    }

    updateBusyState();
    updateTextState();
    hideRecognitionMessage();
    if (requestWasPending && m_active && !overlayWasPending) {
        const QString message = translationWasPending ? tr("Translation failed")
                                : mode == Mode::Text  ? tr("Text recognition failed")
                                : mode == Mode::Latex ? tr("LaTeX recognition failed")
                                : mode == Mode::Table ? tr("Table recognition failed")
                                                      : tr("Barcode recognition failed");
        if (m_mode == mode && (mode == Mode::Table || mode == Mode::Latex))
            failModelRecognition(message);
        else
            showStatus(message, true);
    }
}

ScreenshotRecognitionWindow* ScreenshotRecognitionSessionController::content() const {
    return m_content;
}

QJsonObject ScreenshotRecognitionSessionController::workflowState() const {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const auto entry = m_textCache.constFind(m_translationKey);
    const bool translating =
        entry != m_textCache.cend() &&
        (m_translationInImage ? entry->overlayTranslation.status : entry->translationStatus) ==
            TextCacheEntry::TranslationStatus::Streaming;
#else
    constexpr bool translating = false;
#endif
    const QStringList modes{QStringLiteral("text"), QStringLiteral("table"),
                            QStringLiteral("qr"),   QStringLiteral("markdown"),
                            QStringLiteral("html"), QStringLiteral("latex")};
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    const QString error = conversionModeActive() ? m_conversion->error() : m_workflowError;
#else
    const QString error = m_workflowError;
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    const bool latexEditing = m_active && m_mode == Mode::Latex &&
                              m_latexCache.contains(recognitionCacheKey(Mode::Latex));
#else
    constexpr bool latexEditing = false;
#endif
    return {{QStringLiteral("active"), m_active},
            {QStringLiteral("kind"), modes.at(static_cast<int>(m_mode))},
            {QStringLiteral("busy"), busy(m_mode) || translating},
            {QStringLiteral("error"), error},
            {QStringLiteral("editing"), m_editing || latexEditing},
            {QStringLiteral("translating"), m_translating}};
}
QJsonObject ScreenshotRecognitionSessionController::workflowResult() const {
    QJsonObject result;
    if (!m_active)
        return result;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if ((m_mode == Mode::Table || m_mode == Mode::Latex) && m_recognitionAwaitingModel)
        return result;
#endif
    if (m_mode == Mode::Text && hasTextResult()) {
        result.insert(QStringLiteral("kind"), QStringLiteral("text"));
        result.insert(QStringLiteral("text"), textDraft());
        result.insert(QStringLiteral("source_text"), sourceTextDraft());
        const auto entry = m_textCache.value(m_target.key);
        QJsonArray lines;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        const auto presentation =
            originalImageTranslationActive() && entry.overlayTranslation.presentation
                ? entry.overlayTranslation.presentation
                : entry.presentation;
#else
        const auto presentation = entry.presentation;
#endif
        if (presentation)
            for (const auto& line : presentation->lines) {
                QJsonArray quad;
                for (const auto& point : line.quad)
                    quad.append(QJsonArray{point.x(), point.y()});
                lines.append(QJsonObject{{QStringLiteral("text"), line.text},
                                         {QStringLiteral("confidence"), line.confidence},
                                         {QStringLiteral("quad"), quad}});
            }
        result.insert(QStringLiteral("lines"), lines);
    }
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    else if (m_mode == Mode::Table && m_tableSession) {
        const auto& document = m_tableSession->document;
        QJsonArray cells;
        for (int row = 0; row < document.rowCount(); ++row)
            for (int column = 0; column < document.columnCount(); ++column) {
                if (!document.isAnchor(row, column))
                    continue;
                const auto* cell = document.cellAt(row, column);
                cells.append(QJsonObject{{QStringLiteral("row"), row},
                                         {QStringLiteral("column"), column},
                                         {QStringLiteral("text"), cell->text},
                                         {QStringLiteral("row_span"), cell->rowSpan},
                                         {QStringLiteral("column_span"), cell->columnSpan},
                                         {QStringLiteral("header"), cell->header}});
            }
        result = {{QStringLiteral("kind"), QStringLiteral("table")},
                  {QStringLiteral("rows"), document.rowCount()},
                  {QStringLiteral("columns"), document.columnCount()},
                  {QStringLiteral("cells"), cells},
                  {QStringLiteral("html"), document.toHtml()},
                  {QStringLiteral("text"), document.toPlainText()}};
    }
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    else if (m_mode == Mode::Latex && m_latexResults.contains(recognitionCacheKey(Mode::Latex))) {
        result = {{QStringLiteral("kind"), QStringLiteral("latex")},
                  {QStringLiteral("text"), latexDraft()}};
    }
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    else if (m_mode == Mode::Qr && m_qrResults.contains(m_target.key)) {
        result = {{QStringLiteral("kind"), QStringLiteral("qr")},
                  {QStringLiteral("contents"), QJsonArray::fromStringList(m_qrContents)},
                  {QStringLiteral("text"), m_qrContents.join(u'\n')}};
    }
#endif
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    else if (conversionModeActive() &&
             m_conversion->state() == ScreenshotImageConversionController::State::Completed) {
        result = {{QStringLiteral("kind"),
                   m_mode == Mode::Markdown ? QStringLiteral("markdown") : QStringLiteral("html")},
                  {QStringLiteral("text"), m_conversion->source()}};
        result.insert(m_mode == Mode::Markdown ? QStringLiteral("markdown")
                                               : QStringLiteral("html"),
                      m_conversion->source());
    }
#endif
    return result;
}
bool ScreenshotRecognitionSessionController::editWorkflow(const QJsonObject& params) {
    if (!m_active || workflowState().value(QStringLiteral("busy")).toBool())
        return false;
    const auto action = params.value(QStringLiteral("action")).toString();
    if (action == QStringLiteral("show_original")) {
        setShowOriginalImage(params.value(QStringLiteral("enabled")).toBool());
        return true;
    }
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_mode == Mode::Latex) {
        const auto session = m_latexCache.value(recognitionCacheKey(Mode::Latex));
        if (!session)
            return false;
        if (action == QStringLiteral("set_text"))
            static_cast<void>(
                session->replaceText(params.value(QStringLiteral("text")).toString()));
        else if (action == QStringLiteral("reset_text"))
            static_cast<void>(session->reset());
        else if (action == QStringLiteral("undo"))
            session->undo();
        else if (action == QStringLiteral("redo"))
            session->redo();
        else
            return false;
        return true;
    }
#endif
    if (m_mode == Mode::Text && hasTextResult()) {
        if (action == QStringLiteral("set_text")) {
            beginTextEditing();
            setTextDraft(params.value(QStringLiteral("text")).toString());
        } else if (action == QStringLiteral("reset_text"))
            resetTextEditing();
        else if (action == QStringLiteral("format") || action == QStringLiteral("punctuation")) {
            const auto value = params.value(QStringLiteral("value")).toString();
            const QStringList allowed =
                action == QStringLiteral("format")
                    ? QStringList{QStringLiteral("none"), QStringLiteral("keep"),
                                  QStringLiteral("remove"), QStringLiteral("smart")}
                    : QStringList{QStringLiteral("none"), QStringLiteral("half"),
                                  QStringLiteral("full")};
            if (!allowed.contains(value))
                return false;
            beginTextEditing();
            if (action == QStringLiteral("format"))
                applyTextFormatting(value == QStringLiteral("none") ? QString() : value);
            else
                applyTextPunctuation(value == QStringLiteral("none") ? QString() : value);
        } else if (action == QStringLiteral("undo"))
            undoTextEdit();
        else if (action == QStringLiteral("redo"))
            redoTextEdit();
        else
            return false;
        return true;
    }
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_mode != Mode::Table || !m_tableSession)
        return false;
    auto& table = *m_tableSession;
    auto replacement = table.document;
    if (action == QStringLiteral("select_cells")) {
        const auto range = params.value(QStringLiteral("range")).toArray();
        if (range.size() != 4)
            return false;
        const ScreenshotTableRange selection{range[0].toInt(-1), range[1].toInt(-1),
                                             range[2].toInt(-1), range[3].toInt(-1)};
        if (!selection.isValid() || selection.bottom >= replacement.rowCount() ||
            selection.right >= replacement.columnCount())
            return false;
        table.selection = replacement.expandedRange(selection);
    } else if (action == QStringLiteral("set_cell")) {
        if (!replacement.setCellText(params.value(QStringLiteral("row")).toInt(-1),
                                     params.value(QStringLiteral("column")).toInt(-1),
                                     params.value(QStringLiteral("text")).toString()))
            return false;
    } else if (action == QStringLiteral("merge_cells")) {
        if (!replacement.merge(table.selection))
            return false;
    } else if (action == QStringLiteral("split_cells")) {
        if (!replacement.split(table.selection))
            return false;
    } else if (action == QStringLiteral("reset_table"))
        replacement = table.baseline;
    else if (action == QStringLiteral("undo")) {
        if (!table.undoStack.canUndo())
            return false;
        table.undoStack.undo();
    } else if (action == QStringLiteral("redo")) {
        if (!table.undoStack.canRedo())
            return false;
        table.undoStack.redo();
    } else
        return false;
    if (action != QStringLiteral("undo") && action != QStringLiteral("redo"))
        ScreenshotTableEditingSession::applyDocument(m_tableSession, replacement, QString());
    if (content())
        content()->setTableSession(m_tableSession);
    emit recognitionResultsChanged();
    return true;
#else
    return false;
#endif
}
void ScreenshotRecognitionSessionController::cancelWorkflow() {
    cancelOutstandingRequests();
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    m_conversion->invalidate();
#endif
}
