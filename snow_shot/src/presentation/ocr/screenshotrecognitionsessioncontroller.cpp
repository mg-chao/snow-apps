#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/components/screenshottranslationsettingsdialog.h"
#include "snow_shot/presentation/screenshotocrlayout.h"

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrvisuals.h"
#include "snow_shot/presentation/screenshotocrtexteditingsession.h"
#include "snow_shot/presentation/screenshotocrtexttransform.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotimageconversioncontroller.h"
#include "snow_shot/presentation/screenshottabledocument.h"
#include "snow_shot/presentation/screenshottableeditor.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"

#include <QCoreApplication>
#include <QLocale>
#include <QMimeData>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include "widgets/alert.h"
#include "widgets/button.h"
#include "widgets/form.h"
#include "widgets/modal.h"
#include "widgets/select.h"
#include "widgets/switch.h"

#include <algorithm>
#include <utility>

namespace {
constexpr auto kRecognitionMessageKey = "screenshot-recognition-session";

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

using snow_shot::translation::TranslationJob;
using snow_shot::translation::TranslationService;

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
    m_conversion = new ScreenshotImageConversionController(this);
    connect(m_conversion, &ScreenshotImageConversionController::changed, this, [this]() {
        updateConversionState();
        updateConversionMessage();
    });
    connect(m_conversion, &ScreenshotImageConversionController::resultsChanged, this,
            &ScreenshotRecognitionSessionController::recognitionResultsChanged);
    setProviders(recognition, qrRecognition, tableRecognition);
    connect(&snow_shot::storage::ApplicationStorage::instance().configuration(),
            &snow_shot::storage::ConfigurationStore::valueChanged, this,
            [this](const QString& key, const QJsonValue&) {
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
                if (key != QStringLiteral("screenshot_translation/layout_processing")) {
                    return;
                }
                const auto it = m_textCache.constFind(m_translationKey);
                if (it != m_textCache.cend() && it->hasTranslationConfiguration &&
                    it->translationConfiguration !=
                        snow_shot::storage::ScreenshotTranslationSettings().configuration()) {
                    invalidateCurrentTranslation(m_translating);
                }
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
    if (m_qrRecognition == nullptr && qrRecognition != nullptr) {
        m_qrRecognition = qrRecognition;
        connect(qrRecognition, &QObject::destroyed, this,
                [this]() { handleRecognitionProviderDestroyed(Mode::Qr); });
    }
    if (m_tableRecognition == nullptr && tableRecognition != nullptr) {
        m_tableRecognition = tableRecognition;
        m_conversion->setProvider(tableRecognition);
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
        connect(tableRecognition, &QObject::destroyed, this,
                [this]() { handleRecognitionProviderDestroyed(Mode::Table); });
    }
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
    if (!hasTarget() || !results.isValidFor(m_target.key)) {
        return;
    }

    m_conversion->seed(m_target.key, results.conversions);
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
        if (results.translatedText != nullptr &&
            translationMatchesSource(*results.translatedText, *entry.presentation)) {
            entry.overlayTranslation.presentation = copyTextPresentation(results.translatedText);
            entry.overlayTranslation.status = TextCacheEntry::TranslationStatus::Completed;
            entry.overlayTranslation.captured = true;
            entry.translationConfiguration =
                snow_shot::storage::ScreenshotTranslationSettings().configuration();
            entry.hasTranslationConfiguration = true;
        }
        m_textCache.insert(m_target.key, std::move(entry));
        textInserted = true;
    }

    if (results.table.has_value() && results.table->succeeded() &&
        !m_tableCache.contains(m_target.key)) {
        ScreenshotTableDocument document = ScreenshotTableDocument::fromHtml(results.table->html);
        if (!document.empty()) {
            m_tableResults.insert(m_target.key, *results.table);
            m_tableCache.insert(
                m_target.key, std::make_shared<ScreenshotTableEditingSession>(std::move(document)));
        }
    }

    if (results.qr.has_value() && results.qr->error.isEmpty() && !results.qr->contents.isEmpty() &&
        !m_qrCache.contains(m_target.key)) {
        m_qrResults.insert(m_target.key, *results.qr);
        m_qrCache.insert(m_target.key, results.qr->contents);
    }

    if (textInserted && m_active && m_mode == Mode::Text) {
        m_textCacheKey = m_target.key;
        m_presentation = m_textCache.value(m_target.key).presentation;
        applyPresentation(m_presentation);
        startTextRender();
        emit textResultChanged(true);
    }
    if (m_active && m_mode == Mode::Table) {
        const auto table = m_tableCache.constFind(m_target.key);
        if (table != m_tableCache.cend()) {
            m_tableCacheKey = m_target.key;
            applyTableSession(table.value());
        }
    }
    if (m_active && m_mode == Mode::Qr) {
        const auto qr = m_qrCache.constFind(m_target.key);
        if (qr != m_qrCache.cend()) {
            m_qrCacheKey = m_target.key;
            applyQrContents(qr.value());
        }
    }
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
    results.conversions = m_conversion->entries(m_target.key);
    if (const auto text = m_textCache.constFind(m_target.key);
        text != m_textCache.cend() && text->recognitionResult.error.isEmpty() &&
        text->recognitionResult.presentation != nullptr) {
        results.text = text->recognitionResult;
        results.text->filteredImage = {};
    }
    if (const auto table = m_tableResults.constFind(m_target.key);
        table != m_tableResults.cend() && table->succeeded()) {
        results.table = table.value();
    }
    if (const auto qr = m_qrResults.constFind(m_target.key);
        qr != m_qrResults.cend() && qr->error.isEmpty() && !qr->contents.isEmpty()) {
        results.qr = qr.value();
    }
    if (results.isEmpty()) {
        results.key.clear();
    }
    return results;
}

ScreenshotRecognitionResults
ScreenshotRecognitionSessionController::recognitionResultsSnapshot() const {
    ScreenshotRecognitionResults results = cachedRecognitionResults();
    if (conversionModeActive() &&
        m_conversion->state() == ScreenshotImageConversionController::State::Completed) {
        results.visibleConversion = m_conversion->format();
    }
    if (results.text.has_value()) {
        results.text->presentation = copyTextPresentation(results.text->presentation);
    }
    if (const auto entry = m_textCache.constFind(m_target.key); entry != m_textCache.cend()) {
        // Freeze partial translation independently of the source session's live requests.
        results.translatedText = copyTextPresentation(entry->overlayTranslation.presentation);
    }
    return results;
}

bool ScreenshotRecognitionSessionController::hasTarget() const {
    return m_target.isValid();
}

void ScreenshotRecognitionSessionController::prefetchText() {
    if (!hasTarget() || m_textCache.contains(m_target.key) || m_textRequestToken != 0) {
        return;
    }
    startTextRecognition(ScreenshotOcrRequestPriority::Prefetch);
}

void ScreenshotRecognitionSessionController::activate(Mode mode) {
    if (!hasTarget()) {
        showStatus(tr("Unable to read the selected screenshot"), true);
        return;
    }
    m_conversion->deactivate();
    clearTextEditingState();
    if (mode != Mode::Text && m_textRenderRequestToken != 0) {
        if (m_recognition != nullptr) {
            m_recognition->cancel(m_textRenderRequestToken);
        }
        m_textRenderRequestToken = 0;
        ++m_textRenderGeneration;
    }
    m_mode = mode;
    m_active = true;
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
    m_tableSession.reset();
    m_qrContents.clear();
    m_textCacheKey.clear();
    m_tableCacheKey.clear();
    m_qrCacheKey.clear();
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
    } else if (mode == Mode::Table) {
        setPendingTextRecognitionRendering(false);
        auto cached = m_tableCache.constFind(m_target.key);
        if (cached == m_tableCache.cend()) {
            const auto result = m_tableResults.constFind(m_target.key);
            if (result != m_tableResults.cend()) {
                ScreenshotTableDocument document = ScreenshotTableDocument::fromHtml(result->html);
                if (!document.empty()) {
                    cached = m_tableCache.insert(
                        m_target.key,
                        std::make_shared<ScreenshotTableEditingSession>(std::move(document)));
                }
            }
        }
        if (cached != m_tableCache.cend()) {
            m_tableCacheKey = m_target.key;
            applyTableSession(cached.value());
        } else {
            startTableRecognition();
        }
    } else if (mode == Mode::Qr) {
        setPendingTextRecognitionRendering(false);
        const auto cached = m_qrCache.constFind(m_target.key);
        if (cached != m_qrCache.cend()) {
            m_qrCacheKey = m_target.key;
            applyQrContents(cached.value());
        } else {
            startQrRecognition();
        }
    } else {
        setPendingTextRecognitionRendering(false);
        m_conversion->activate(m_target.key, m_target.image,
                               mode == Mode::Markdown ? SnowShotImageConversionFormat::Markdown
                                                      : SnowShotImageConversionFormat::Html);
    }
    updateBusyState();
    updateTextState();
}

void ScreenshotRecognitionSessionController::deactivate() {
    if (!m_active && m_content == nullptr) {
        return;
    }
    clearTextEditingState();
    m_active = false;
    m_conversion->deactivate();
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
    m_tableSession.reset();
    m_qrContents.clear();
    clearContent();
    if (m_actions.clearOcrBackground) {
        m_actions.clearOcrBackground();
    }
    if (m_actions.setRecognitionVisualState) {
        m_actions.setRecognitionVisualState(false);
    }
    if (m_actions.setActiveMode) {
        m_actions.setActiveMode(-1);
    }
    hideRecognitionMessage();
    updateBusyState();
    updateTextState();
    updateTableState({});
}

void ScreenshotRecognitionSessionController::invalidate() {
    resetTargetState();
    m_conversion->invalidate();
    m_textCache.clear();
    m_tableCache.clear();
    m_qrCache.clear();
    m_tableResults.clear();
    m_qrResults.clear();
    m_textCacheKey.clear();
    m_tableCacheKey.clear();
    m_qrCacheKey.clear();
    m_editingKey.clear();
    m_translationKey.clear();
    m_target = {};
    emit textResultChanged(false);
}

void ScreenshotRecognitionSessionController::resetTargetState() {
    deactivate();
    if (m_translationSettingsModal != nullptr) {
        m_translationSettingsModal->reject();
    }

    // Raw recognition payloads survive a target change, but editing state belongs to
    // the target that is currently visible and must not leak into another target.
    for (auto it = m_textCache.begin(); it != m_textCache.end(); ++it) {
        if (it->editingSession != nullptr) {
            it->editingSession->establishHistory(it->editingSession->originalText());
        }
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
        it->editing = false;
    }
    m_tableCache.clear();
    cancelOutstandingRequests();
    ++m_textGeneration;
    ++m_tableGeneration;
    ++m_qrGeneration;
    ++m_translationGeneration;
    m_textCacheKey.clear();
    m_tableCacheKey.clear();
    m_qrCacheKey.clear();
    m_editingKey.clear();
    m_translationKey.clear();
    m_target = {};
    updateTextState();
}

bool ScreenshotRecognitionSessionController::active() const {
    return m_active;
}

bool ScreenshotRecognitionSessionController::busy() const {
    return busy(Mode::Text) || busy(Mode::Table) || busy(Mode::Qr) || m_conversion->busy();
}

bool ScreenshotRecognitionSessionController::busy(Mode mode) const {
    switch (mode) {
    case Mode::Text:
        return m_textRequestToken != 0 || m_textRenderRequestToken != 0;
    case Mode::Table:
        return m_tableRequestToken != 0;
    case Mode::Qr:
        return m_qrRequestToken != 0;
    case Mode::Markdown:
    case Mode::Html:
        return m_mode == mode && m_conversion->busy();
    }
    return false;
}

ScreenshotRecognitionSessionController::Mode ScreenshotRecognitionSessionController::mode() const {
    return m_mode;
}

bool ScreenshotRecognitionSessionController::tableModeActive() const {
    return m_active && m_mode == Mode::Table;
}

bool ScreenshotRecognitionSessionController::qrModeActive() const {
    return m_active && m_mode == Mode::Qr;
}

bool ScreenshotRecognitionSessionController::conversionModeActive() const {
    return m_active && (m_mode == Mode::Markdown || m_mode == Mode::Html);
}

void ScreenshotRecognitionSessionController::openImageConversionSettings() {
    QWidget* owner =
        m_actions.translationSettingsOwner ? m_actions.translationSettingsOwner() : content();
    m_conversion->openSettings(owner);
}

void ScreenshotRecognitionSessionController::updateConversionState() const {
    if (conversionModeActive() && m_conversion->active() && content() != nullptr) {
        content()->showImageConversion(m_conversion->format(), m_conversion->source(),
                                       m_conversion->busy(), m_conversion->error());
    }
    if (m_actions.setConversionState) {
        m_actions.setConversionState(conversionModeActive(), m_conversion->busy(),
                                     m_conversion->format());
    }
}

void ScreenshotRecognitionSessionController::updateConversionMessage() {
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
}

void ScreenshotRecognitionSessionController::mergeTableSelection() {
    if (tableModeActive() && content() != nullptr) {
        content()->mergeTableSelection();
    }
}

void ScreenshotRecognitionSessionController::splitTableSelection() {
    if (tableModeActive() && content() != nullptr) {
        content()->splitTableSelection();
    }
}

void ScreenshotRecognitionSessionController::resetTable() {
    if (tableModeActive() && content() != nullptr) {
        content()->resetTable();
    }
}

void ScreenshotRecognitionSessionController::undoTableEdit() {
    if (tableModeActive() && content() != nullptr) {
        content()->undoTableEdit();
    }
}

void ScreenshotRecognitionSessionController::redoTableEdit() {
    if (tableModeActive() && content() != nullptr) {
        content()->redoTableEdit();
    }
}

void ScreenshotRecognitionSessionController::undoTextEdit() {
    if (!m_active || (!m_editing && !m_translating) || m_textDocument == nullptr) {
        return;
    }
    const auto entry = m_textCache.value(m_editingKey);
    const auto session = m_translating ? entry.translationSession : entry.editingSession;
    if (session != nullptr) {
        session->undo();
    }
}

void ScreenshotRecognitionSessionController::redoTextEdit() {
    if (!m_active || (!m_editing && !m_translating) || m_textDocument == nullptr) {
        return;
    }
    const auto entry = m_textCache.value(m_editingKey);
    const auto session = m_translating ? entry.translationSession : entry.editingSession;
    if (session != nullptr) {
        session->redo();
    }
}

void ScreenshotRecognitionSessionController::beginTextEditing() {
    if (!m_active || m_mode != Mode::Text || !hasTextResult()) {
        return;
    }
    m_editing = true;
    m_translating = false;
    m_editingKey = m_textCacheKey.isEmpty() ? m_target.key : m_textCacheKey;
    auto it = m_textCache.find(m_editingKey);
    if (it != m_textCache.end()) {
        it->editing = true;
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
}

bool ScreenshotRecognitionSessionController::activateCachedTextTranslation() {
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
}

void ScreenshotRecognitionSessionController::synchronizeUiState() const {
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
    m_translating = false;
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
    if (m_translating) {
        auto it = m_textCache.find(m_editingKey);
        if (it != m_textCache.end() && it->translationSession != nullptr &&
            it->hasSuccessfulTranslation &&
            it->translationStatus != TextCacheEntry::TranslationStatus::Streaming) {
            it->translationSession->clearTransforms();
            it->translationSession->replaceText(it->successfulTranslation);
        }
    } else if (m_editing) {
        auto it = m_textCache.find(m_editingKey);
        if (it != m_textCache.end() && it->editingSession != nullptr) {
            it->editingSession->clearTransforms();
            static_cast<void>(it->editingSession->replaceText(it->editingSession->originalText()));
        }
    }
    updateTextState();
}

void ScreenshotRecognitionSessionController::openTranslationSettings() {
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
}

void ScreenshotRecognitionSessionController::startTranslation() {
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
}

void ScreenshotRecognitionSessionController::prepareOverlayTranslation(TextCacheEntry& entry) {
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
}

void ScreenshotRecognitionSessionController::reportOverlayTranslationFailure() {
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
}

void ScreenshotRecognitionSessionController::failTranslationPreparation(const QString& message) {
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
}

void ScreenshotRecognitionSessionController::cancelTranslationRequests() {
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
}

void ScreenshotRecognitionSessionController::handleTranslationFinished(
    quint64 generation, const QString& key, SnowShotTranslationResult result) {
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
}

void ScreenshotRecognitionSessionController::invalidateCurrentTranslation(bool restartIfVisible) {
    auto it = m_textCache.find(m_translationKey.isEmpty() ? m_textCacheKey : m_translationKey);
    if (it == m_textCache.end()) {
        return;
    }
    cancelTranslationRequests();
    if (it->translationJob != nullptr) {
        it->translationJob->deleteLater();
        it->translationJob = nullptr;
    }
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
}

void ScreenshotRecognitionSessionController::applyTextFormatting(const QString& value) {
    if (originalImageTranslationActive()) {
        return;
    }
    if (!m_editing && !m_translating) {
        beginTextEditing();
    }
    const auto entry = m_textCache.value(m_editingKey);
    const auto session = m_translating ? entry.translationSession : entry.editingSession;
    const bool streaming =
        m_translating && entry.translationStatus == TextCacheEntry::TranslationStatus::Streaming;
    if (session != nullptr && !streaming) {
        static_cast<void>(session->setFormatting(value));
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
    const auto session = m_translating ? entry.translationSession : entry.editingSession;
    const bool streaming =
        m_translating && entry.translationStatus == TextCacheEntry::TranslationStatus::Streaming;
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

bool ScreenshotRecognitionSessionController::originalImageVisible() const {
    return m_active && m_mode == Mode::Text && !editing();
}

bool ScreenshotRecognitionSessionController::hasTextResult() const {
    const QString key = m_textCacheKey.isEmpty() ? m_target.key : m_textCacheKey;
    return !key.isEmpty() && m_textCache.contains(key);
}

QString ScreenshotRecognitionSessionController::textDraft() const {
    const QString key = m_editingKey.isEmpty() ? m_textCacheKey : m_editingKey;
    const auto entry = m_textCache.value(key);
    if (originalImageTranslationActive() && entry.overlayTranslation.presentation != nullptr) {
        return snow_shot::presentation::originalOcrText(*entry.overlayTranslation.presentation);
    }
    const auto session = m_translating ? entry.translationSession : entry.editingSession;
    return session != nullptr ? session->text() : QString{};
}

QString ScreenshotRecognitionSessionController::originalText() const {
    const QString key = m_editingKey.isEmpty() ? m_textCacheKey : m_editingKey;
    const auto session = m_textCache.value(key).editingSession;
    return session != nullptr ? session->originalText() : QString{};
}

std::unique_ptr<QMimeData> ScreenshotRecognitionSessionController::recognitionClipboardMimeData(
    const ScreenshotOcrPresentation* displayedPresentation) const {
    auto mimeData = std::make_unique<QMimeData>();
    if (conversionModeActive()) {
        if (m_conversion->source().isEmpty()) {
            return {};
        }
        mimeData->setText(m_conversion->source());
        return mimeData;
    }
    if (m_mode == Mode::Qr) {
        if (m_qrCacheKey.isEmpty() || !m_qrCache.contains(m_qrCacheKey)) {
            return {};
        }
        mimeData->setText(m_qrContents.join(QLatin1Char('\n')));
        return mimeData;
    }
    if (m_mode == Mode::Table) {
        if (m_tableSession == nullptr || m_tableSession->document.empty()) {
            return {};
        }
        mimeData->setHtml(m_tableSession->document.toHtml());
        mimeData->setText(m_tableSession->document.toPlainText());
        return mimeData;
    }

    QString text;
    bool resultAvailable = false;
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
    } else if (editing()) {
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
    mimeData->setText(text);
    return mimeData;
}

void ScreenshotRecognitionSessionController::setTextDraft(const QString& text) {
    if (originalImageTranslationActive()) {
        return;
    }
    const QString key = m_editingKey.isEmpty() ? m_textCacheKey : m_editingKey;
    auto it = m_textCache.find(key);
    if (it == m_textCache.end()) {
        return;
    }
    const auto session = m_translating ? it->translationSession : it->editingSession;
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
    if (!hasTarget() || m_tableRecognition == nullptr || m_tableRequestToken != 0 ||
        !screenshotOcrImageWithinPixelLimit(m_target.image.size())) {
        if (m_tableRecognition == nullptr) {
            showStatus(tr("Table recognition service is unavailable"), true);
        }
        return;
    }
    const quint64 generation = ++m_tableGeneration;
    const QString key = m_target.key;
    showRecognitionMessage();
    const auto callbackCompleted = std::make_shared<bool>(false);
    m_tableRequestToken = m_tableRecognition->extractTable(
        m_target.image, this,
        [this, generation, key, callbackCompleted](SnowShotTableResult result) {
            *callbackCompleted = true;
            if (generation == m_tableGeneration) {
                m_tableRequestToken = 0;
            }
            handleTableOutput(generation, key, std::move(result));
        });
    if (*callbackCompleted) {
        m_tableRequestToken = 0;
    }
    updateBusyState();
    if (m_tableRequestToken == 0 && !*callbackCompleted) {
        showStatus(tr("Table recognition request could not be prepared"), true);
        hideRecognitionMessage();
    }
}

void ScreenshotRecognitionSessionController::startQrRecognition() {
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
    if (generation != m_tableGeneration || key != m_target.key) {
        return;
    }
    if (!result.succeeded()) {
        if (m_active && m_mode == Mode::Table) {
            showStatus(result.error.isEmpty() ? tr("Table recognition failed") : result.error,
                       true);
        }
        hideRecognitionMessage();
        updateBusyState();
        return;
    }
    ScreenshotTableDocument document = ScreenshotTableDocument::fromHtml(result.html);
    if (document.empty()) {
        if (m_active && m_mode == Mode::Table) {
            showStatus(tr("No table cells were recognized"), false);
        }
        hideRecognitionMessage();
        updateBusyState();
        return;
    }
    auto session = std::make_shared<ScreenshotTableEditingSession>(std::move(document));
    m_tableResults.insert(key, result);
    m_tableCache.insert(key, session);
    if (m_active && m_mode == Mode::Table) {
        m_tableCacheKey = key;
        applyTableSession(session);
    }
    hideRecognitionMessage();
    updateBusyState();
    emit recognitionResultsChanged();
}

void ScreenshotRecognitionSessionController::handleQrOutput(quint64 generation, const QString& key,
                                                            ScreenshotQrRecognitionResult result) {
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
}

void ScreenshotRecognitionSessionController::ensureContent() {
    if (m_content == nullptr && m_actions.ensureContent) {
        m_content = m_actions.ensureContent();
        if (m_content != nullptr) {
            connect(m_content, &ScreenshotRecognitionWindow::imageConversionRetryRequested,
                    m_conversion, &ScreenshotImageConversionController::retry);
        }
    }
}

void ScreenshotRecognitionSessionController::clearContent() {
    if (m_content != nullptr) {
        m_content->clearOcrPresentation();
        m_content->clearFormattedText();
        m_content->clearTableSession();
        m_content->clearQrContents();
        m_content->clearImageConversion();
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
    if (session == nullptr || session->document.empty()) {
        return;
    }
    m_tableSession = session;
    ensureContent();
    if (content() != nullptr) {
        content()->setTableSession(session);
        updateTableState(content()->tableCommandState());
    }
}

void ScreenshotRecognitionSessionController::applyQrContents(const QStringList& contents) {
    m_qrContents = contents;
    ensureContent();
    if (content() != nullptr) {
        content()->showQrContents(contents);
    }
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
}

void ScreenshotRecognitionSessionController::clearTextEditingState() {
    for (auto it = m_textCache.begin(); it != m_textCache.end(); ++it) {
        it->editing = false;
    }
    m_editing = false;
    m_translating = false;
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
    if (m_actions.setBusyState) {
        m_actions.setBusyState(busy(Mode::Text), busy(Mode::Table), busy(Mode::Qr));
    }
}

void ScreenshotRecognitionSessionController::updateTextState() const {
    const bool available = hasTextResult() && m_active && m_mode == Mode::Text;
    const auto entry = m_textCache.value(m_editingKey);
    const bool overlay = originalImageTranslationActive();
    const auto translation = m_textCache.constFind(m_translationKey);
    const bool streaming = translation != m_textCache.cend() &&
                           (m_translationInImage ? translation->overlayTranslation.status
                                                 : translation->translationStatus) ==
                               TextCacheEntry::TranslationStatus::Streaming;
    if (m_actions.setTextEditingState) {
        m_actions.setTextEditingState(
            available, m_editing,
            m_editing && entry.editingSession != nullptr && entry.editingSession->canUndo(),
            m_editing && entry.editingSession != nullptr && entry.editingSession->canRedo());
    }
    if (m_actions.setTextTranslationState) {
        m_actions.setTextTranslationState(
            available, m_translating, streaming,
            m_translating && !overlay && !streaming && entry.translationSession != nullptr &&
                entry.translationSession->canUndo(),
            m_translating && !overlay && !streaming && entry.translationSession != nullptr &&
                entry.translationSession->canRedo(),
            m_translating && !overlay && !streaming && entry.hasSuccessfulTranslation, overlay);
    }
    if (m_actions.setTextTransformState) {
        const auto session = m_translating ? entry.translationSession : entry.editingSession;
        m_actions.setTextTransformState(
            editing() && session != nullptr ? session->formatting() : QString{},
            editing() && session != nullptr ? session->punctuation() : QString{});
    }
}

void ScreenshotRecognitionSessionController::updateTableState(
    const ScreenshotTableCommandState& state) const {
    if (!m_actions.setTableEditingState) {
        return;
    }
    const bool available = m_active && m_mode == Mode::Table && m_tableSession != nullptr;
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
    if (!message.isEmpty() && m_actions.showStatus) {
        m_actions.showStatus(message, error);
    }
}

void ScreenshotRecognitionSessionController::cancelOutstandingRequests() {
    cancelTranslationRequests();
    if (m_recognition != nullptr && m_textRequestToken != 0) {
        m_recognition->cancel(m_textRequestToken);
    }
    if (m_recognition != nullptr && m_textRenderRequestToken != 0) {
        m_recognition->cancel(m_textRenderRequestToken);
    }
    if (m_qrRecognition != nullptr && m_qrRequestToken != 0) {
        m_qrRecognition->cancel(m_qrRequestToken);
    }
    if (m_tableRecognition != nullptr && m_tableRequestToken != 0) {
        m_tableRecognition->cancel(m_tableRequestToken);
    }
    m_textRequestToken = 0;
    m_textRenderRequestToken = 0;
    m_qrRequestToken = 0;
    m_tableRequestToken = 0;
    ++m_textRenderGeneration;
    hideModelDownloadMessage();
    m_textModelDownloadInProgress = false;
    hideRecognitionMessage();
}

void ScreenshotRecognitionSessionController::handleRecognitionProviderDestroyed(Mode mode) {
    bool requestWasPending = false;
    bool translationWasPending = false;
    const auto translation = m_textCache.constFind(m_translationKey);
    const bool overlayWasPending =
        m_translationInImage && translation != m_textCache.cend() &&
        translation->overlayTranslation.status == TextCacheEntry::TranslationStatus::Streaming;
    if (mode != Mode::Qr && overlayWasPending) {
        cancelTranslationRequests();
        reportOverlayTranslationFailure();
    }
    switch (mode) {
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
        requestWasPending = m_tableRequestToken != 0;
        m_tableRequestToken = 0;
        ++m_tableGeneration;
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
        break;
    case Mode::Qr:
        requestWasPending = m_qrRequestToken != 0;
        m_qrRequestToken = 0;
        ++m_qrGeneration;
        break;
    }

    updateBusyState();
    updateTextState();
    hideRecognitionMessage();
    if (requestWasPending && m_active && !overlayWasPending) {
        const QString message = translationWasPending ? tr("Translation failed")
                                : mode == Mode::Text  ? tr("Text recognition failed")
                                : mode == Mode::Table ? tr("Table recognition failed")
                                                      : tr("Barcode recognition failed");
        showStatus(message, true);
    }
}

ScreenshotRecognitionWindow* ScreenshotRecognitionSessionController::content() const {
    return m_content;
}
