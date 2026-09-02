#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrtexteditingsession.h"
#include "snow_shot/presentation/screenshotocrtexttransform.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshottabledocument.h"
#include "snow_shot/presentation/screenshottableeditor.h"

#include <QDesktopServices>
#include <QCoreApplication>
#include <QLocale>
#include <QMimeData>
#include <QPushButton>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "widgets/alert.h"
#include "widgets/button.h"
#include "widgets/form.h"
#include "widgets/modal.h"
#include "widgets/select.h"

#include <utility>

namespace {
constexpr auto kRecognitionMessageKey = "screenshot-recognition-session";

struct TranslationLanguage {
    const char* code;
    const char* name;
};

const QVector<TranslationLanguage> kTranslationLanguages{
    {"ar", QT_TRANSLATE_NOOP("ScreenshotRecognitionSessionController", "Arabic")},
    {"de", QT_TRANSLATE_NOOP("ScreenshotRecognitionSessionController", "German")},
    {"en", QT_TRANSLATE_NOOP("ScreenshotRecognitionSessionController", "English")},
    {"es", QT_TRANSLATE_NOOP("ScreenshotRecognitionSessionController", "Spanish")},
    {"fr", QT_TRANSLATE_NOOP("ScreenshotRecognitionSessionController", "French")},
    {"it", QT_TRANSLATE_NOOP("ScreenshotRecognitionSessionController", "Italian")},
    {"ja", QT_TRANSLATE_NOOP("ScreenshotRecognitionSessionController", "Japanese")},
    {"pt", QT_TRANSLATE_NOOP("ScreenshotRecognitionSessionController", "Portuguese")},
    {"ru", QT_TRANSLATE_NOOP("ScreenshotRecognitionSessionController", "Russian")},
    {"tr", QT_TRANSLATE_NOOP("ScreenshotRecognitionSessionController", "Turkish")},
    {"zh-Hans", QT_TRANSLATE_NOOP("ScreenshotRecognitionSessionController", "Simplified Chinese")},
    {"zh-Hant", QT_TRANSLATE_NOOP("ScreenshotRecognitionSessionController", "Traditional Chinese")},
};

QString languageName(const QString& code) {
    if (code == QStringLiteral("auto")) {
        return ScreenshotRecognitionSessionController::tr("auto-detect language");
    }
    for (const TranslationLanguage& language : kTranslationLanguages) {
        if (code == QLatin1StringView(language.code)) {
            return QCoreApplication::translate("ScreenshotRecognitionSessionController",
                                               language.name);
        }
    }
    return code;
}

QString defaultTargetLanguage() {
    const QLocale locale = snow_shot::presentation::LanguageManager::instance().currentLocale();
    switch (locale.language()) {
    case QLocale::Arabic: return QStringLiteral("ar");
    case QLocale::German: return QStringLiteral("de");
    case QLocale::Spanish: return QStringLiteral("es");
    case QLocale::French: return QStringLiteral("fr");
    case QLocale::Italian: return QStringLiteral("it");
    case QLocale::Japanese: return QStringLiteral("ja");
    case QLocale::Portuguese: return QStringLiteral("pt");
    case QLocale::Russian: return QStringLiteral("ru");
    case QLocale::Turkish: return QStringLiteral("tr");
    case QLocale::Chinese:
        return locale.script() == QLocale::TraditionalHanScript ? QStringLiteral("zh-Hant")
                                                                : QStringLiteral("zh-Hans");
    default: return QStringLiteral("en");
    }
}
}

ScreenshotRecognitionSessionController::ScreenshotRecognitionSessionController(
    ScreenshotOcrRecognitionPort* recognition, ScreenshotQrRecognitionPort* qrRecognition,
    SnowShotApiClient* tableRecognition, ScreenshotRecognitionSessionActions actions,
    QObject* parent)
    : QObject(parent),
      m_actions(std::move(actions)) {
    setProviders(recognition, qrRecognition, tableRecognition);
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

    bool textInserted = false;
    if (!m_target.hasFormattedText() && results.text.has_value() &&
        results.text->error.isEmpty() && results.text->presentation != nullptr &&
        !m_textCache.contains(m_target.key)) {
        const QString original = snow_shot::presentation::originalOcrText(
            *results.text->presentation);
        auto editingSession = std::make_shared<ScreenshotOcrTextEditingSession>(original);
        connect(editingSession->document(), &QTextDocument::contentsChanged, this,
                [this, key = m_target.key]() { handleTextDocumentChanged(key); });
        TextCacheEntry entry;
        entry.recognitionResult = *results.text;
        entry.recognitionResult.filteredImage = {};
        entry.presentation = results.text->presentation;
        entry.editingSession = std::move(editingSession);
        m_textCache.insert(m_target.key, std::move(entry));
        textInserted = true;
    }

    if (results.table.has_value() && results.table->succeeded() &&
        !m_tableCache.contains(m_target.key)) {
        ScreenshotTableDocument document = ScreenshotTableDocument::fromHtml(results.table->html);
        if (!document.empty()) {
            m_tableResults.insert(m_target.key, *results.table);
            m_tableCache.insert(
                m_target.key,
                std::make_shared<ScreenshotTableEditingSession>(std::move(document)));
        }
    }

    if (results.qr.has_value() && results.qr->error.isEmpty() &&
        !results.qr->contents.isEmpty() && !m_qrCache.contains(m_target.key)) {
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
}

ScreenshotRecognitionResults
ScreenshotRecognitionSessionController::cachedRecognitionResults() const {
    ScreenshotRecognitionResults results;
    if (!hasTarget()) {
        return results;
    }
    results.key = m_target.key;
    if (const auto text = m_textCache.constFind(m_target.key); text != m_textCache.cend() &&
        text->recognitionResult.error.isEmpty() &&
        text->recognitionResult.presentation != nullptr) {
        results.text = text->recognitionResult;
        results.text->filteredImage = {};
    }
    if (const auto table = m_tableResults.constFind(m_target.key);
        table != m_tableResults.cend() && table->succeeded()) {
        results.table = table.value();
    }
    if (const auto qr = m_qrResults.constFind(m_target.key); qr != m_qrResults.cend() &&
        qr->error.isEmpty() && !qr->contents.isEmpty()) {
        results.qr = qr.value();
    }
    if (results.isEmpty()) {
        results.key.clear();
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
            if (m_textModelDownloadInProgress) {
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
                ScreenshotTableDocument document =
                    ScreenshotTableDocument::fromHtml(result->html);
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
    } else {
        setPendingTextRecognitionRendering(false);
        const auto cached = m_qrCache.constFind(m_target.key);
        if (cached != m_qrCache.cend()) {
            m_qrCacheKey = m_target.key;
            applyQrContents(cached.value());
        } else {
            startQrRecognition();
        }
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
    if (m_textModelDownloadInProgress) {
        hideModelDownloadMessage();
    }
    if (m_recognition != nullptr && m_textRenderRequestToken != 0) {
        m_recognition->cancel(m_textRenderRequestToken);
    }
    m_textRenderRequestToken = 0;
    ++m_textRenderGeneration;
    if (m_recognition != nullptr && m_textRequestToken != 0) {
        setPendingTextRecognitionRendering(false);
        static_cast<void>(m_recognition->reprioritize(m_textRequestToken,
                                                      ScreenshotOcrRequestPriority::Prefetch));
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
        it->translationSession.reset();
        it->translationSettings = {};
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
}

bool ScreenshotRecognitionSessionController::active() const {
    return m_active;
}

bool ScreenshotRecognitionSessionController::busy() const {
    return busy(Mode::Text) || busy(Mode::Table) || busy(Mode::Qr);
}

bool ScreenshotRecognitionSessionController::busy(Mode mode) const {
    switch (mode) {
    case Mode::Text:
        return m_textRequestToken != 0 || m_textRenderRequestToken != 0;
    case Mode::Table:
        return m_tableRequestToken != 0;
    case Mode::Qr:
        return m_qrRequestToken != 0;
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
    m_editing = false;
    m_translating = true;
    m_editingKey = m_textCacheKey.isEmpty() ? m_target.key : m_textCacheKey;
    auto it = m_textCache.find(m_editingKey);
    if (it == m_textCache.end()) {
        m_translating = false;
        return;
    }
    it->editing = false;
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
    if (it->translationStatus == TextCacheEntry::TranslationStatus::Absent) {
        startTranslation();
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
        applyPresentation(m_presentation);
    }
    emit textEditingChanged(false);
    updateTextState();
}

void ScreenshotRecognitionSessionController::resetTextEditing() {
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
    if (m_tableRecognition == nullptr) {
        showStatus(tr("Translation service is unavailable"), true);
        return;
    }
    if (m_translationSettingsModal != nullptr) {
        return;
    }
    showTranslationSettingsModal(m_tableRecognition->cachedChatModels());
}

void ScreenshotRecognitionSessionController::startTranslation() {
    if (m_tableRecognition == nullptr) {
        showStatus(tr("Translation service is unavailable"), true);
        return;
    }
    if (m_translationRequestToken != 0 || m_modelsRequestToken != 0) {
        return;
    }
    if (!m_tableRecognition->cachedChatModels().isEmpty()) {
        startTranslationWithModels(m_tableRecognition->cachedChatModels());
        return;
    }
    const QString key = m_translationKey;
    const quint64 generation = m_translationGeneration;
    auto it = m_textCache.find(key);
    if (it != m_textCache.end()) {
        it->translationStatus = TextCacheEntry::TranslationStatus::Streaming;
    }
    if (content() != nullptr && m_translating && key == m_editingKey) {
        content()->setTextEditorStreaming(true);
    }
    updateTextState();
    m_modelsRequestToken = m_tableRecognition->fetchChatModels(
        snow_shot::presentation::LanguageManager::instance().currentLocale().name(), this,
        [this, generation, key](SnowShotChatModelsResult result) {
            m_modelsRequestToken = 0;
            if (generation != m_translationGeneration || key != m_translationKey) {
                return;
            }
            if (!result.succeeded()) {
                auto it = m_textCache.find(key);
                if (it != m_textCache.end()) {
                    it->translationStatus = TextCacheEntry::TranslationStatus::Failed;
                }
                if (content() != nullptr && m_translating && key == m_editingKey) {
                    content()->setTextEditorStreaming(false);
                }
                updateTextState();
                showStatus(result.error, true);
                return;
            }
            startTranslationWithModels(result.models);
        });
    if (m_modelsRequestToken == 0) {
        auto failed = m_textCache.find(key);
        if (failed != m_textCache.end()) {
            failed->translationStatus = TextCacheEntry::TranslationStatus::Failed;
        }
        if (content() != nullptr && m_translating && key == m_editingKey) {
            content()->setTextEditorStreaming(false);
        }
        updateTextState();
        showStatus(tr("Translation service request could not be prepared"), true);
    }
}

void ScreenshotRecognitionSessionController::startTranslationWithModels(
    const QVector<SnowShotChatModel>& models) {
    auto it = m_textCache.find(m_translationKey);
    if (it == m_textCache.end() || models.isEmpty() || it->translationSession == nullptr) {
        return;
    }
    auto settings = snow_shot::storage::ScreenshotTranslationSettings().configuration();
    if (settings.targetLanguage.isEmpty()) {
        settings.targetLanguage = defaultTargetLanguage();
    }
    if (settings.sourceLanguage.isEmpty()) {
        settings.sourceLanguage = QStringLiteral("auto");
    }
    const auto selected = std::find_if(models.cbegin(), models.cend(), [&settings](const auto& model) {
        return model.id == settings.modelId;
    });
    if (selected == models.cend()) {
        settings.modelId = models.first().id;
    }
    it->translationSettings = settings;
    it->translationText.clear();
    it->translationSession->replaceTextWithoutHistory(QString());
    it->translationStatus = TextCacheEntry::TranslationStatus::Streaming;
    const QString key = m_translationKey;
    const quint64 generation = ++m_translationGeneration;
    if (content() != nullptr && m_translating) {
        content()->setTextEditorStreaming(true);
    }
    updateTextState();
    m_translationRequestToken = m_tableRecognition->streamTranslation(
        SnowShotTranslationRequest{settings.modelId, languageName(settings.sourceLanguage),
                                   languageName(settings.targetLanguage),
                                   it->editingSession != nullptr
                                       ? it->editingSession->originalText()
                                       : QString{}},
        this,
        [this, generation, key](const QString& delta) {
            handleTranslationDelta(generation, key, delta);
        },
        [this, generation, key](SnowShotTranslationResult result) {
            handleTranslationFinished(generation, key, std::move(result));
        });
    if (m_translationRequestToken == 0) {
        it->translationStatus = TextCacheEntry::TranslationStatus::Failed;
        if (content() != nullptr && m_translating) {
            content()->setTextEditorStreaming(false);
        }
        updateTextState();
        showStatus(tr("Translation request could not be prepared"), true);
    }
}

void ScreenshotRecognitionSessionController::handleTranslationDelta(
    quint64 generation, const QString& key, const QString& delta) {
    if (generation != m_translationGeneration) {
        return;
    }
    auto it = m_textCache.find(key);
    if (it == m_textCache.end() || it->translationSession == nullptr) {
        return;
    }
    it->translationText += delta;
    it->translationSession->replaceTextWithoutHistory(it->translationText);
}

void ScreenshotRecognitionSessionController::handleTranslationFinished(
    quint64 generation, const QString& key, SnowShotTranslationResult result) {
    if (generation != m_translationGeneration) {
        return;
    }
    m_translationRequestToken = 0;
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

void ScreenshotRecognitionSessionController::showTranslationSettingsModal(
    const QVector<SnowShotChatModel>& models) {
    QWidget* owner = m_actions.translationSettingsOwner ? m_actions.translationSettingsOwner()
                                                        : content();
    if (owner == nullptr) {
        owner = content();
    }
    if (owner == nullptr || m_translationSettingsModal != nullptr) {
        return;
    }
    auto current = snow_shot::storage::ScreenshotTranslationSettings().configuration();
    if (current.sourceLanguage.isEmpty()) {
        current.sourceLanguage = QStringLiteral("auto");
    }
    if (current.targetLanguage.isEmpty()) {
        current.targetLanguage = defaultTargetLanguage();
    }
    if (!models.isEmpty() &&
        std::none_of(models.cbegin(), models.cend(), [&current](const auto& model) {
            return model.id == current.modelId;
        })) {
        current.modelId = models.first().id;
    }

    auto* body = new QWidget;
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(12);

    auto* errorAlert = new adqt::widgets::AdAlert(body);
    errorAlert->setObjectName(QStringLiteral("screenshotTranslationSettingsError"));
    errorAlert->setSeverity(adqt::widgets::AdAlert::Severity::Error);
    errorAlert->setText(tr("Unable to load translation services"));
    auto* retryButton = new adqt::widgets::AdButton(tr("Retry"), errorAlert);
    retryButton->setObjectName(QStringLiteral("screenshotTranslationSettingsRetry"));
    retryButton->setButtonStyle(adqt::widgets::AdButton::ButtonStyle::Text);
    retryButton->setAccentRole(adqt::widgets::AdButton::AccentRole::Primary);
    retryButton->setSizeClass(adqt::widgets::AdButton::SizeClass::Small);
    errorAlert->setActionsWidget(retryButton);
    errorAlert->hide();
    bodyLayout->addWidget(errorAlert);

    auto* form = new adqt::widgets::AdForm(body);
    form->setFormLayout(adqt::widgets::AdForm::FormLayout::Vertical);
    auto* source = new adqt::widgets::AdSelect(form);
    auto* target = new adqt::widgets::AdSelect(form);
    auto* service = new adqt::widgets::AdSelect(form);
    QVector<adqt::widgets::AdSelect::Option> sourceOptions{
        {QStringLiteral("auto"), tr("auto-detect language")}};
    QVector<adqt::widgets::AdSelect::Option> targetOptions;
    for (const TranslationLanguage& language : kTranslationLanguages) {
        const adqt::widgets::AdSelect::Option option{QString::fromLatin1(language.code),
                                                     languageName(QString::fromLatin1(language.code))};
        sourceOptions.push_back(option);
        targetOptions.push_back(option);
    }
    QVector<adqt::widgets::AdSelect::Option> serviceOptions;
    for (const SnowShotChatModel& model : models) {
        serviceOptions.push_back({model.id, model.name});
    }
    source->setOptions(sourceOptions);
    target->setOptions(targetOptions);
    source->setCurrentValue(current.sourceLanguage);
    target->setCurrentValue(current.targetLanguage);
    if (!serviceOptions.isEmpty()) {
        service->setOptions(serviceOptions);
        service->setCurrentValue(current.modelId);
    }
    service->setEnabled(!serviceOptions.isEmpty());
    form->addField(tr("Source language"), source, QStringLiteral("source"));
    form->addField(tr("Target language"), target, QStringLiteral("target"));
    form->addField(tr("Translation service"), service, QStringLiteral("service"));
    bodyLayout->addWidget(form);

    auto* modal = new adqt::widgets::AdModal(this);
    modal->setObjectName(QStringLiteral("screenshotTranslationSettingsModal"));
    modal->setOwnerWindow(owner);
    modal->setMode(adqt::widgets::AdModal::Mode::Window);
    modal->setWindowModality(Qt::ApplicationModal);
    modal->setWindowTitle(tr("Translation settings"));
    modal->setCentered(true);
    modal->setPreferredWidth(440);
    modal->setMaskVisible(false);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(adqt::widgets::AdModal::ClosePolicy::Manual);
    modal->setAcceptText(tr("OK"));
    modal->setRejectText(tr("Cancel"));
    modal->setStandardButtons(adqt::widgets::AdModal::StandardButton::Ok |
                              adqt::widgets::AdModal::StandardButton::Cancel);
    modal->setContentWidget(body);
    modal->setInitialFocusWidget(source);
    m_translationSettingsModal = modal;
    connect(modal, &adqt::widgets::AdModal::closeRequested, modal,
            [this, modal, source, target, service, current](adqt::widgets::AdModal::CloseReason reason) {
                if (reason != adqt::widgets::AdModal::CloseReason::OkAction) {
                    modal->reject();
                    return;
                }
                const snow_shot::storage::ScreenshotTranslationConfiguration selected{
                    source->currentValue().toString(), target->currentValue().toString(),
                    service->currentValue().toString()};
                if (selected.sourceLanguage.isEmpty() || selected.targetLanguage.isEmpty() ||
                    selected.modelId.isEmpty()) {
                    return;
                }
                snow_shot::storage::ScreenshotTranslationSettings().setConfiguration(selected);
                if (selected != current) {
                    invalidateCurrentTranslation(m_translating);
                }
                modal->accept();
            });
    connect(modal, &adqt::widgets::AdModal::finished, modal,
            [this, modal](adqt::widgets::AdModal::DialogCode) {
                if (m_settingsModelsRequestToken != 0 && m_tableRecognition != nullptr) {
                    m_tableRecognition->cancel(m_settingsModelsRequestToken);
                    m_settingsModelsRequestToken = 0;
                }
                if (m_translationSettingsModal == modal) {
                    m_translationSettingsModal = nullptr;
                }
                modal->deleteLater();
            });
    modal->open();

    const auto applyModels = [service, current](const QVector<SnowShotChatModel>& availableModels) {
        QVector<adqt::widgets::AdSelect::Option> options;
        options.reserve(availableModels.size());
        for (const SnowShotChatModel& model : availableModels) {
            options.push_back({model.id, model.name});
        }
        service->setOptions(options);
        const bool currentAvailable =
            std::any_of(availableModels.cbegin(), availableModels.cend(),
                        [&current](const SnowShotChatModel& model) {
                            return model.id == current.modelId;
                        });
        service->setCurrentValue(currentAvailable ? current.modelId
                                                   : availableModels.first().id);
        service->setLoading(false);
        service->setEnabled(true);
    };
    if (!models.isEmpty()) {
        applyModels(models);
        return;
    }

    const QPointer<adqt::widgets::AdModal> modalGuard(modal);
    const QPointer<adqt::widgets::AdAlert> alertGuard(errorAlert);
    const QPointer<adqt::widgets::AdButton> retryGuard(retryButton);
    const QPointer<adqt::widgets::AdSelect> serviceGuard(service);
    auto requestModels = std::make_shared<std::function<void()>>();
    *requestModels = [this, modalGuard, alertGuard, retryGuard, serviceGuard, applyModels]() {
        if (modalGuard == nullptr || alertGuard == nullptr || retryGuard == nullptr ||
            serviceGuard == nullptr || m_tableRecognition == nullptr ||
            m_settingsModelsRequestToken != 0) {
            return;
        }
        alertGuard->hide();
        retryGuard->setBusy(true);
        serviceGuard->setLoading(true);
        serviceGuard->setEnabled(false);
        m_settingsModelsRequestToken = m_tableRecognition->fetchChatModels(
            snow_shot::presentation::LanguageManager::instance().currentLocale().name(), this,
            [this, modalGuard, alertGuard, retryGuard, serviceGuard,
             applyModels](SnowShotChatModelsResult result) {
                m_settingsModelsRequestToken = 0;
                if (modalGuard == nullptr || alertGuard == nullptr || retryGuard == nullptr ||
                    serviceGuard == nullptr) {
                    return;
                }
                serviceGuard->setLoading(false);
                retryGuard->setBusy(false);
                if (!result.succeeded()) {
                    serviceGuard->setEnabled(false);
                    alertGuard->setInformativeText(
                        result.error.isEmpty() ? tr("Translation service request failed")
                                               : result.error);
                    alertGuard->show();
                    return;
                }
                alertGuard->hide();
                applyModels(result.models);
            });
        if (m_settingsModelsRequestToken == 0) {
            serviceGuard->setLoading(false);
            retryGuard->setBusy(false);
            alertGuard->setInformativeText(
                tr("Translation service request could not be prepared"));
            alertGuard->show();
        }
    };
    connect(retryButton, &adqt::widgets::AdButton::clicked, modal,
            [requestModels]() { (*requestModels)(); });
    (*requestModels)();
}

void ScreenshotRecognitionSessionController::invalidateCurrentTranslation(bool restartIfVisible) {
    auto it = m_textCache.find(m_textCacheKey);
    if (it == m_textCache.end()) {
        return;
    }
    if (m_tableRecognition != nullptr && m_translationRequestToken != 0) {
        m_tableRecognition->cancel(m_translationRequestToken);
    }
    if (m_tableRecognition != nullptr && m_modelsRequestToken != 0) {
        m_tableRecognition->cancel(m_modelsRequestToken);
    }
    m_modelsRequestToken = 0;
    m_translationRequestToken = 0;
    ++m_translationGeneration;
    it->translationStatus = TextCacheEntry::TranslationStatus::Absent;
    it->translationText.clear();
    if (it->translationSession != nullptr) {
        it->translationSession->establishHistory(QString());
    }
    updateTextState();
    if (restartIfVisible) {
        startTranslation();
    }
}

void ScreenshotRecognitionSessionController::applyTextFormatting(const QString& value) {
    if (!m_editing && !m_translating) {
        beginTextEditing();
    }
    const auto entry = m_textCache.value(m_editingKey);
    const auto session = m_translating ? entry.translationSession : entry.editingSession;
    const bool streaming = m_translating &&
                           entry.translationStatus == TextCacheEntry::TranslationStatus::Streaming;
    if (session != nullptr && !streaming) {
        static_cast<void>(session->setFormatting(value));
        updateTextState();
    }
}

void ScreenshotRecognitionSessionController::applyTextPunctuation(const QString& value) {
    if (!m_editing && !m_translating) {
        beginTextEditing();
    }
    const auto entry = m_textCache.value(m_editingKey);
    const auto session = m_translating ? entry.translationSession : entry.editingSession;
    const bool streaming = m_translating &&
                           entry.translationStatus == TextCacheEntry::TranslationStatus::Streaming;
    if (session != nullptr && !streaming) {
        static_cast<void>(session->setPunctuation(value));
        updateTextState();
    }
}

bool ScreenshotRecognitionSessionController::editing() const {
    return m_editing || m_translating;
}

bool ScreenshotRecognitionSessionController::translating() const {
    return m_translating;
}

bool ScreenshotRecognitionSessionController::hasTextResult() const {
    const QString key = m_textCacheKey.isEmpty() ? m_target.key : m_textCacheKey;
    return !key.isEmpty() && m_textCache.contains(key);
}

QString ScreenshotRecognitionSessionController::textDraft() const {
    const QString key = m_editingKey.isEmpty() ? m_textCacheKey : m_editingKey;
    const auto entry = m_textCache.value(key);
    const auto session = m_translating ? entry.translationSession : entry.editingSession;
    return session != nullptr ? session->text() : QString{};
}

QString ScreenshotRecognitionSessionController::originalText() const {
    const QString key = m_editingKey.isEmpty() ? m_textCacheKey : m_editingKey;
    const auto session = m_textCache.value(key).editingSession;
    return session != nullptr ? session->originalText() : QString{};
}

std::unique_ptr<QMimeData>
ScreenshotRecognitionSessionController::recognitionClipboardMimeData(
    const ScreenshotOcrPresentation* displayedPresentation) const {
    auto mimeData = std::make_unique<QMimeData>();
    if (m_mode == Mode::Qr) {
        if (m_qrContents.isEmpty()) {
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
    if (editing()) {
        text = textDraft();
    } else if (displayedPresentation != nullptr) {
        text = displayedPresentation->hasTextSelection()
                   ? displayedPresentation->selectedText()
                   : snow_shot::presentation::originalOcrText(*displayedPresentation);
    } else {
        text = originalText();
    }
    if (text.isEmpty()) {
        return {};
    }
    mimeData->setText(text);
    return mimeData;
}

std::shared_ptr<ScreenshotTableEditingSession>
ScreenshotRecognitionSessionController::tableSession() const {
    return m_tableSession;
}

QStringList ScreenshotRecognitionSessionController::qrContents() const {
    return m_qrContents;
}

void ScreenshotRecognitionSessionController::setTextDraft(const QString& text) {
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
        if (m_active && hasTarget() &&
            !screenshotOcrImageWithinPixelLimit(m_target.image.size())) {
            showStatus(tr("Text recognition is unavailable for screenshots larger than 4K"), false);
        }
        return;
    }
    const quint64 generation = ++m_textGeneration;
    const QString key = m_target.key;
    m_textModelDownloadShown = !m_recognition->modelFilesReady();
    m_textModelDownloadInProgress = m_textModelDownloadShown;
    if (m_textModelDownloadInProgress) {
        showModelDownloadMessage();
    } else if (m_active) {
        showRecognitionMessage();
    }
    ScreenshotOcrRequest request;
    request.image = m_target.image;
    request.canvasRect = m_target.canvasRect;
    request.priority = priority;
    request.renderFilteredImage = m_active && m_mode == Mode::Text &&
                                  shouldRenderRecognitionInWorker();
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
        if (m_textModelDownloadInProgress) {
            hideModelDownloadMessage();
        }
        m_textModelDownloadShown = false;
        m_textModelDownloadInProgress = false;
        hideRecognitionMessage();
    } else if (m_textModelDownloadShown) {
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
    request.backgroundColor = m_actions.ocrBackgroundColor ? m_actions.ocrBackgroundColor()
                                                            : QColor();
    if (m_actions.prepareOcrRenderRequest) {
        m_actions.prepareOcrRenderRequest(request);
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
    const bool modelDownloadWasInProgress = m_textModelDownloadInProgress;
    m_textModelDownloadShown = false;
    m_textModelDownloadInProgress = false;
    if (modelDownloadWasInProgress) {
        hideModelDownloadMessage();
    }
    if (!output.error.isEmpty() || output.presentation == nullptr) {
        if ((m_active && m_mode == Mode::Text) || modelDownloadWasShown) {
            showStatus(output.error.isEmpty() ? tr("Text recognition failed") : output.error,
                       true);
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
}

void ScreenshotRecognitionSessionController::handleTableOutput(
    quint64 generation, const QString& key, SnowShotTableResult result) {
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
}

void ScreenshotRecognitionSessionController::handleQrOutput(
    quint64 generation, const QString& key, ScreenshotQrRecognitionResult result) {
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
        if (m_active && m_mode == Mode::Qr) {
            showStatus(tr("No barcode was recognized"), false);
        }
        hideRecognitionMessage();
        updateBusyState();
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
}

void ScreenshotRecognitionSessionController::ensureContent() {
    if (m_content == nullptr && m_actions.ensureContent) {
        m_content = m_actions.ensureContent();
    }
}

void ScreenshotRecognitionSessionController::clearContent() {
    if (m_content != nullptr) {
        m_content->clearOcrPresentation();
        m_content->clearFormattedText();
        m_content->clearTableSession();
        m_content->clearQrContents();
    }
}

void ScreenshotRecognitionSessionController::applyPresentation(
    const std::shared_ptr<ScreenshotOcrPresentation>& presentation, QImage filteredImage,
    QRectF filteredImageCanvasRect) {
    m_presentation = presentation;
    ensureContent();
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
    return m_actions.applyOcrBackgroundImage &&
           (!m_actions.renderRecognitionInWorker || m_actions.renderRecognitionInWorker());
}

void ScreenshotRecognitionSessionController::setPendingTextRecognitionRendering(bool enabled) {
    if (m_recognition == nullptr || m_textRequestToken == 0) {
        return;
    }
    const bool shouldRender = enabled && shouldRenderRecognitionInWorker();
    const QColor backgroundColor = shouldRender && m_actions.ocrBackgroundColor
                                       ? m_actions.ocrBackgroundColor()
                                       : QColor();
    static_cast<void>(m_recognition->setRenderFilteredImage(
        m_textRequestToken, shouldRender, backgroundColor));
}

void ScreenshotRecognitionSessionController::pollTextModelDownload(quint64 generation) {
    if (generation != m_textGeneration || m_textRequestToken == 0 ||
        !m_textModelDownloadInProgress || m_recognition == nullptr) {
        return;
    }
    if (m_recognition->modelFilesReady()) {
        m_textModelDownloadInProgress = false;
        hideModelDownloadMessage();
        if (m_active && m_mode == Mode::Text) {
            showRecognitionMessage();
        }
        return;
    }
    showModelDownloadMessage();
    QTimer::singleShot(100, this,
                       [this, generation]() { pollTextModelDownload(generation); });
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

void ScreenshotRecognitionSessionController::handleTranslationDocumentChanged(
    const QString& key) {
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
    const bool streaming = m_translating &&
                           entry.translationStatus == TextCacheEntry::TranslationStatus::Streaming;
    if (m_actions.setTextEditingState) {
        m_actions.setTextEditingState(available, m_editing,
                                      m_editing && entry.editingSession != nullptr &&
                                          entry.editingSession->canUndo(),
                                      m_editing && entry.editingSession != nullptr &&
                                          entry.editingSession->canRedo());
    }
    if (m_actions.setTextTranslationState) {
        m_actions.setTextTranslationState(
            available, m_translating, streaming,
            m_translating && !streaming && entry.translationSession != nullptr &&
                entry.translationSession->canUndo(),
            m_translating && !streaming && entry.translationSession != nullptr &&
                entry.translationSession->canRedo(),
            m_translating && !streaming && entry.hasSuccessfulTranslation);
    }
    if (m_actions.setTextTransformState) {
        const auto session = m_translating ? entry.translationSession : entry.editingSession;
        m_actions.setTextTransformState(
            (m_editing || m_translating) && session != nullptr ? session->formatting() : QString{},
            (m_editing || m_translating) && session != nullptr ? session->punctuation()
                                                               : QString{});
    }
}

void ScreenshotRecognitionSessionController::updateTableState(
    const ScreenshotTableCommandState& state) const {
    if (!m_actions.setTableEditingState) {
        return;
    }
    const bool available = m_active && m_mode == Mode::Table && m_tableSession != nullptr;
    m_actions.setTableEditingState(available, available && state.canUndo, available && state.canRedo,
                                   available && state.canMerge, available && state.canSplit,
                                   available && state.canReset);
}

void ScreenshotRecognitionSessionController::showModelDownloadMessage() const {
    QString message = tr("Preparing text recognition components");
    if (m_recognition != nullptr) {
        const ScreenshotOcrAssetStatus status = m_recognition->assetStatus();
        if (status.phase == ScreenshotOcrAssetPhase::Downloading && status.totalBytes > 0) {
            const int percent = static_cast<int>(
                std::clamp<qint64>(status.receivedBytes * 100 / status.totalBytes, 0, 100));
            message = tr("Preparing text recognition components (%1%)").arg(percent);
        }
    }
    if (m_actions.showModelDownload) {
        m_actions.showModelDownload(message);
    } else if (m_actions.showLoading) {
        m_actions.showLoading(message);
    } else if (m_actions.showStatus) {
        m_actions.showStatus(message, false);
    }
}

void ScreenshotRecognitionSessionController::hideModelDownloadMessage() const {
    if (m_actions.hideModelDownload) {
        m_actions.hideModelDownload();
    } else if (m_actions.hideLoading) {
        m_actions.hideLoading();
    }
}

void ScreenshotRecognitionSessionController::showRecognitionMessage() const {
    const QString message = m_mode == Mode::Table ? tr("Recognizing table")
                        : m_mode == Mode::Qr ? tr("Recognizing barcode")
                                             : tr("Recognizing text");
    if (m_actions.showRecognition) {
        m_actions.showRecognition(message);
    } else if (m_actions.showLoading) {
        m_actions.showLoading(message);
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
    const bool modelDownloadWasInProgress = m_textModelDownloadInProgress;
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
    if (m_tableRecognition != nullptr && m_modelsRequestToken != 0) {
        m_tableRecognition->cancel(m_modelsRequestToken);
    }
    if (m_tableRecognition != nullptr && m_settingsModelsRequestToken != 0) {
        m_tableRecognition->cancel(m_settingsModelsRequestToken);
    }
    if (m_tableRecognition != nullptr && m_translationRequestToken != 0) {
        m_tableRecognition->cancel(m_translationRequestToken);
    }
    m_textRequestToken = 0;
    m_textRenderRequestToken = 0;
    m_qrRequestToken = 0;
    m_tableRequestToken = 0;
    m_modelsRequestToken = 0;
    m_settingsModelsRequestToken = 0;
    m_translationRequestToken = 0;
    m_textModelDownloadShown = false;
    ++m_textRenderGeneration;
    m_textModelDownloadInProgress = false;
    if (modelDownloadWasInProgress) {
        hideModelDownloadMessage();
    }
    hideRecognitionMessage();
}

void ScreenshotRecognitionSessionController::handleRecognitionProviderDestroyed(Mode mode) {
    bool requestWasPending = false;
    bool translationWasPending = false;
    bool modelDownloadWasInProgress = false;
    switch (mode) {
    case Mode::Text:
        requestWasPending = m_textRequestToken != 0;
        modelDownloadWasInProgress = m_textModelDownloadInProgress;
        m_textRequestToken = 0;
        m_textRenderRequestToken = 0;
        ++m_textRenderGeneration;
        m_textModelDownloadShown = false;
        m_textModelDownloadInProgress = false;
        ++m_textGeneration;
        break;
    case Mode::Table:
        translationWasPending = m_modelsRequestToken != 0 ||
                                m_settingsModelsRequestToken != 0 ||
                                m_translationRequestToken != 0;
        requestWasPending = m_tableRequestToken != 0 || m_modelsRequestToken != 0 ||
                            m_settingsModelsRequestToken != 0 || m_translationRequestToken != 0;
        m_tableRequestToken = 0;
        m_modelsRequestToken = 0;
        m_settingsModelsRequestToken = 0;
        m_translationRequestToken = 0;
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

    if (modelDownloadWasInProgress) {
        hideModelDownloadMessage();
    }
    updateBusyState();
    updateTextState();
    hideRecognitionMessage();
    if (requestWasPending && m_active) {
        const QString message = translationWasPending ? tr("Translation failed")
                                : mode == Mode::Text   ? tr("Text recognition failed")
                                : mode == Mode::Table  ? tr("Table recognition failed")
                                                       : tr("Barcode recognition failed");
        showStatus(message, true);
    }
}

ScreenshotRecognitionWindow* ScreenshotRecognitionSessionController::content() const {
    return m_content;
}
