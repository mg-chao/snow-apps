#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotlatexrenderer.h"
#include <QTemporaryDir>
#include <QDesktopServices>
#include <QPainter>
#include <QThreadPool>
#include <QJsonArray>
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "snow_shot/presentation/components/screenshottranslationsettingsdialog.h"
#endif
#include "snow_shot/presentation/screenshotocrlayout.h"

#include "snow_shot/app/editionfeatures.h"

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrvisuals.h"
#include "snow_shot/presentation/screenshotocrtexteditingsession.h"
#include "snow_shot/presentation/screenshotocrtexttransform.h"
#include "snow_shot/presentation/screenshotocrtextlayout.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
#include "snow_shot/presentation/screenshotimageconversioncontroller.h"
#include "snow_shot/presentation/screenshotimageconversionclipboard.h"
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
#include "snow_shot/presentation/screenshottabledocument.h"
#include "snow_shot/presentation/screenshottableconversioncontroller.h"
#endif
#include "snow_shot/presentation/screenshottableeditor.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"

#include <QCoreApplication>
#include <QApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QThreadPool>
#include <QLocale>
#include <QMimeData>
#include <QTextDocument>
#include <QTextCursor>
#include <QThread>
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

#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
using FormulaFallback = ScreenshotRecognizedDocument::FormulaFallback;

struct DocumentFormulaBatch {
    QHash<QString, QStringList> idsBySource;
    QStringList sources;
    qsizetype next = 0;
    qint64 imageBytes = 0;
};

std::shared_ptr<QTextDocument> prepareDocumentPreview(const ScreenshotRecognizedDocument& document,
                                                      QThread* uiThread, FormulaFallback fallback) {
    auto preview = document.createTextDocument(false, fallback);
    if (!preview->moveToThread(uiThread))
        return {};
    return std::shared_ptr<QTextDocument>(preview.release(), [](QTextDocument* value) {
        if (value->thread() == QThread::currentThread())
            delete value;
        else
            value->deleteLater();
    });
}
#endif

#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
QString streamingLatexSource(QString source) {
    if (source.startsWith(QChar(0xfeff)))
        source.remove(0, 1);
    qsizetype start = 0;
    while (start < source.size() && source.at(start).isSpace())
        ++start;
    source.remove(0, start);
    if (source.isEmpty() || (source.front() != u'`' && source.front() != u'~'))
        return source;
    const QChar marker = source.front();
    qsizetype fenceLength = 0;
    while (fenceLength < source.size() && source.at(fenceLength) == marker)
        ++fenceLength;
    const qsizetype newline = source.indexOf(u'\n');
    if (newline < 0) {
        // A fence and its language tag may each arrive in several different chunks.
        const QString tag = source.mid(fenceLength).trimmed().toLower();
        if (fenceLength == source.size() ||
            (fenceLength >= 3 &&
             (QStringLiteral("latex").startsWith(tag) || QStringLiteral("tex").startsWith(tag))))
            return {};
        return source;
    }
    const QString tag = source.mid(fenceLength, newline - fenceLength).trimmed().toLower();
    if (fenceLength < 3 || (tag != QStringLiteral("latex") && tag != QStringLiteral("tex")))
        return source;
    source.remove(0, newline + 1);
    qsizetype end = source.size();
    while (end > 0 && source.at(end - 1).isSpace())
        --end;
    const qsizetype lastNewline = source.lastIndexOf(u'\n', end - 1);
    const QString tail = source.mid(lastNewline + 1, end - lastNewline - 1).trimmed();
    if (!tail.isEmpty() &&
        std::all_of(tail.cbegin(), tail.cend(), [marker](QChar c) { return c == marker; }))
        source.truncate(std::max<qsizetype>(0, lastNewline));
    return source;
}
#endif

std::shared_ptr<ScreenshotOcrPresentation>
copyTextPresentation(const std::shared_ptr<ScreenshotOcrPresentation>& presentation) {
    if (presentation == nullptr) {
        return {};
    }
    auto copy = std::make_shared<ScreenshotOcrPresentation>();
    copy->selection = presentation->selection;
    copy->lines = presentation->lines;
    // A captured result has no live request to finish its preview layout.
    for (int index = 0; index < copy->lines.size(); ++index) {
        if (copy->lines.at(index).translationStreaming)
            copy->lines[index].translationStreaming = false;
    }
    copy->prepareForRendering();
    return copy;
}

#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
using snow_shot::translation::TranslationJob;
using snow_shot::translation::TranslationService;

std::shared_ptr<ScreenshotOcrPresentation>
prepareOverlayLayout(const ScreenshotOcrPresentation& source, bool merge, const QFont& font) {
    auto overlay = std::make_shared<ScreenshotOcrPresentation>();
    overlay->selection = source.selection;
    QVector<QVector<int>> sourceIndices;
    if (merge) {
        overlay->lines = snow_shot::presentation::mergeOcrLayout(
            source.lines, source.selection.topLeft(), &sourceIndices);
    } else {
        overlay->lines = source.lines;
        sourceIndices.reserve(source.lines.size());
        for (int index = 0; index < source.lines.size(); ++index)
            sourceIndices.push_back({index});
    }
    setScreenshotOcrTranslationFontCaps(overlay->lines, source.lines, sourceIndices, font);
    overlay->prepareForRendering();
    return overlay;
}
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
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    m_translationUpdateTimer = new QTimer(this);
    m_translationUpdateTimer->setObjectName(QStringLiteral("screenshotTranslationUpdateTimer"));
    m_translationUpdateTimer->setSingleShot(true);
    m_translationUpdateTimer->setInterval(33);
    connect(m_translationUpdateTimer, &QTimer::timeout, this,
            [this] { flushTranslationUpdates(); });
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableModelSelection = snow_shot::storage::ScreenshotRecognitionModelSettings().tableModel();
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    m_latexModelSelection = snow_shot::storage::ScreenshotRecognitionModelSettings().latexModel();
    m_latexStreamTimer = new QTimer(this);
    m_latexStreamTimer->setSingleShot(true);
    m_latexStreamTimer->setInterval(50);
    connect(m_latexStreamTimer, &QTimer::timeout, this,
            &ScreenshotRecognitionSessionController::flushLatexStream);
#endif
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    m_conversion = new ScreenshotImageConversionController(this);
    connect(m_conversion, &ScreenshotImageConversionController::changed, this, [this]() {
        if (!updateConversionState())
            return;
        notify(&ScreenshotRecognitionSessionController::updateConversionMessage);
    });
    connect(m_conversion, &ScreenshotImageConversionController::resultsChanged, this,
            &ScreenshotRecognitionSessionController::recognitionResultsChanged);
#endif
    setProviders(recognition, qrRecognition, tableRecognition);
    connect(
        &snow_shot::storage::ApplicationStorage::instance().configuration(),
        &snow_shot::storage::ConfigurationStore::valueChanged, this,
        [this](const QString& key, const QJsonValue&) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
            if (!m_savingRecognitionModel && (key == QStringLiteral("screenshot_table/model") ||
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
                // Catalog aliases change future preferences, not this session's historical
                // selections. Inactive tools also own cached results and editing history.
                const QString current = recognitionModelSelection(changed);
                if (snowShotBuiltInModelAlias(current, selected))
                    return;
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
                    if (!updateRecognitionModels())
                        return;
                }
                return;
            }
#endif
            if (key == QStringLiteral("text_recognition/show_original_image_preview")) {
                if (!notify(&ScreenshotRecognitionSessionController::updateOriginalImagePreview))
                    return;
                return;
            }
            if (key == QStringLiteral("text_recognition/show_text_highlighting") ||
                key == QStringLiteral("screenshot_ui/selection_mask_color")) {
                notify(&ScreenshotRecognitionSessionController::updateTextHighlighting);
                return;
            }
            if (key == QStringLiteral("text_recognition/fill_style")) {
                if (m_active && m_mode == Mode::Text) {
                    if (!notify(&ScreenshotRecognitionSessionController::
                                    setPendingTextRecognitionRendering,
                                true))
                        return;
                    if (m_presentation != nullptr && !editing()) {
                        if (!notify(&ScreenshotRecognitionSessionController::applyPresentation,
                                    m_presentation, QImage{}, QRectF{}))
                            return;
                        if (!notify(&ScreenshotRecognitionSessionController::startTextRender))
                            return;
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
                if (!notify(&ScreenshotRecognitionSessionController::invalidateCurrentTranslation,
                            m_translating))
                    return;
            }
#endif
        });
}

ScreenshotRecognitionSessionController::~ScreenshotRecognitionSessionController() {
    m_destroying = true;
    m_actions = {};
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_latexStream && content())
        content()->hideTextEditor();
#endif
    retireContentAttachment();
    // Teardown cancels work without publishing UI state through a partially destroyed owner.
    cancelOutstandingRequests();
    QList<QPointer<QObject>> ownedChildren;
    for (QObject* child : children())
        ownedChildren.append(child);
    for (const auto& child : ownedChildren) {
        if (!child)
            continue;
        disconnect(child, nullptr, this, nullptr);
        child->blockSignals(true);
        delete child.data();
    }
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
        const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
        m_conversion->setProvider(tableRecognition);
        if (!lifetime)
            return;
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        m_translationService = &TranslationService::forClient(
            *tableRecognition, snow_shot::storage::ApplicationStorage::instance().configuration(),
            snow_shot::presentation::LanguageManager::instance().currentLocale());
        connect(m_translationService, &TranslationService::catalogChanged, this,
                [this] { updateTextState(); });
        connect(m_translationService, &TranslationService::preferencesChanged, this,
                [this](TranslationService::ChangeReason reason) {
                    if (reason != TranslationService::ChangeReason::UserPreferences) {
                        updateTextState();
                        return;
                    }
                    QTimer::singleShot(0, this, [this] {
                        const auto entry = m_textCache.constFind(m_translationKey);
                        if (entry != m_textCache.cend() && entry->hasTranslationConfiguration &&
                            entry->translationConfiguration !=
                                snow_shot::storage::ScreenshotTranslationSettings().configuration())
                            if (!notify(&ScreenshotRecognitionSessionController::
                                            invalidateCurrentTranslation,
                                        m_translating))
                                return;
                        if (!updateTextState())
                            return;
                    });
                });
        connect(
            m_translationService, &TranslationService::modelInvalidated, this,
            [this](const QString& id) {
                const auto active = m_textCache.constFind(m_translationKey);
                const bool affected =
                    active != m_textCache.cend() && active->translationConfiguration.modelId == id;
                if (affected)
                    if (!notify(
                            &ScreenshotRecognitionSessionController::invalidateCurrentTranslation,
                            false))
                        return;
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
                    if (!showStatus(TranslationService::modelConfigurationChangedText(), true))
                        return;
                notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged);
            });
#endif

        connect(tableRecognition, &QObject::destroyed, this, [this]() {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
            const bool documentRequestPending = m_documentRequestToken != 0 || m_documentParsing;
            if (documentRequestPending)
                cancelModelRecognition(Mode::Document);
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
            const bool awaitingModels = m_active &&
                                        (m_mode == Mode::Table || m_mode == Mode::Latex) &&
                                        m_recognitionAwaitingModel;
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                    \
    SNOW_SHOT_ENABLE_IMAGE_CONVERSION
            m_recognitionModelsToken = 0;
            m_recognitionModelsLoading = false;
            ++m_recognitionModelsGeneration;
#endif
            if (!notify(&ScreenshotRecognitionSessionController::handleRecognitionProviderDestroyed,
                        Mode::Table))
                return;
            if (!notify(&ScreenshotRecognitionSessionController::handleRecognitionProviderDestroyed,
                        Mode::Latex))
                return;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
            if (awaitingModels)
                if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                            m_mode == Mode::Table ? tr("Table recognition failed")
                                                  : tr("LaTeX recognition failed")))
                    return;
#endif
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
            if (documentRequestPending && m_active && m_mode == Mode::Document) {
                failModelRecognition(tr("Document recognition service is unavailable"));
                return;
            }
#endif
            if (conversionModeActive())
                updateRecognitionModels();
        });
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
        const auto documentProviderChanged = [this] {
            if (!m_documentRequestToken && !m_documentParsing)
                return;
            cancelModelRecognition(Mode::Document);
            failModelRecognition(
                tr("Recognition service changed. Retry to use the updated settings"));
        };
        connect(tableRecognition, &SnowShotApiClient::accountIdentityChanged, this,
                documentProviderChanged);
        connect(tableRecognition, &SnowShotApiClient::baseUrlChanged, this, documentProviderChanged);
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                    \
    SNOW_SHOT_ENABLE_IMAGE_CONVERSION
        connect(tableRecognition, &SnowShotApiClient::chatModelsChanged, this, [this] {
            if (!m_recognitionModelsError.isEmpty() && m_tableRecognition &&
                m_tableRecognition->hasBuiltInModels(
                    snow_shot::presentation::LanguageManager::instance().currentLocale().name()))
                m_recognitionModelsError.clear();
            if (migrateRecognitionModelPreferences())
                updateRecognitionModels();
        });
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
        connect(tableRecognition, &SnowShotApiClient::baseUrlChanged, this, [this] {
            const bool affected =
                m_active && (m_mode == Mode::Table || m_mode == Mode::Latex) &&
                !recognitionModelSelection(m_mode).startsWith(QStringLiteral("custom:"));
            if (affected) {
                cancelModelRecognition(m_mode);
                m_recognitionEffectiveModel.clear();
                if (!notify(&ScreenshotRecognitionSessionController::clearContent, false))
                    return;
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
                if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                            tr("Recognition service changed. Retry to use the updated settings")))
                    return;
            if (!updateRecognitionModels())
                return;
            notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged);
        });
        connect(
            tableRecognition, &SnowShotApiClient::customModelInvalidated, this,
            [this](const QString& model, bool, bool vision) {
                if (!vision)
                    return;
                const bool affected = m_active && recognitionModelSelection(m_mode) == model;
                if (affected) {
                    cancelModelRecognition(m_mode);
                    if (!notify(&ScreenshotRecognitionSessionController::clearContent, false))
                        return;
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
                    if (!notify(
                            &ScreenshotRecognitionSessionController::failModelRecognition,
                            tr("Model configuration changed. Retry to use the updated settings")))
                        return;
                }
                if (!updateRecognitionModels())
                    return;
                notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged);
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

void ScreenshotRecognitionSessionController::setIndependentEditing(bool enabled) {
    m_independentEditing = enabled;
    updateOriginalImagePreview();
}

void ScreenshotRecognitionSessionController::setTarget(ScreenshotRecognitionTarget target) {
    if (target.key == m_target.key && target.canvasRect == m_target.canvasRect &&
        target.formattedTextDocument == m_target.formattedTextDocument &&
        target.formattedPlainText == m_target.formattedPlainText) {
        if (m_target.image.isNull())
            m_target.image = std::move(target.image);
        return;
    }
    if (!notify(&ScreenshotRecognitionSessionController::resetTargetState))
        return;
    m_target = std::move(target);
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    m_documentPreview.reset();
    m_documentPreviewKey.clear();
#endif
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

void ScreenshotRecognitionSessionController::releaseTargetImage() {
    if (!m_active)
        m_target.image = {};
}

void ScreenshotRecognitionSessionController::refreshFormattedTarget(
    QImage image, std::shared_ptr<QTextDocument> document) {
    auto it = m_textCache.find(m_target.key);
    if (!m_target.hasFormattedText() || image.isNull() || !document || it == m_textCache.end() ||
        !it->formatted)
        return;
    m_target.image = std::move(image);
    m_target.formattedTextDocument = std::move(document);
    m_target.formattedPlainText = m_target.formattedTextDocument->toPlainText();
    it->formattedDocument = m_target.formattedTextDocument;
    if (it->editingSession &&
        !invokeOwned(it->editingSession, &ScreenshotOcrTextEditingSession::refreshOriginalText,
                     m_target.formattedPlainText))
        return;
    if (m_active && m_mode == Mode::Text && !editing())
        if (!notify(&ScreenshotRecognitionSessionController::applyFormattedText,
                    m_target.formattedTextDocument))
            return;
    static_cast<void>(updateTextState());
}

void ScreenshotRecognitionSessionController::seedRecognitionResults(
    ScreenshotRecognitionResults results) {
    sanitizeEditionRecognitionResults(results);
    if (results.document && results.isValidFor(m_target.key))
        cacheDocument(m_target.key, results.document);
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
            if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::establishHistory,
                             *entry.draft))
                return;
        connect(session->document(), &QTextDocument::contentsChanged, this,
                [this, key] { handleLatexDocumentChanged(key); });
        m_latexResults.insert(key, entry.result);
        m_latexEntries.insert(key, entry);
        m_latexEntryTargets.insert(key, m_target.key);
        m_latexCache.insert(key, std::move(session));
    }
    if (m_active && m_mode == Mode::Latex)
        if (!notify(&ScreenshotRecognitionSessionController::applyLatexContents, latexDraft()))
            return;
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
        const auto sourceIndices = results.translatedText
                                       ? snow_shot::presentation::ocrTranslationSourceIndices(
                                             *results.translatedText, *entry.presentation)
                                       : std::nullopt;
        if (sourceIndices) {
            entry.overlayTranslation.presentation = copyTextPresentation(results.translatedText);
            setScreenshotOcrTranslationFontCaps(entry.overlayTranslation.presentation->lines,
                                                entry.presentation->lines, *sourceIndices,
                                                QApplication::font());
            entry.overlayTranslation.status = TextCacheEntry::TranslationStatus::Completed;
            entry.overlayTranslation.captured = true;
            entry.translationConfiguration =
                snow_shot::storage::ScreenshotTranslationSettings().configuration();
            entry.hasTranslationConfiguration = true;
        }
#endif
        prepareIndependentTranslation(entry, m_target.key);
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
        if (!notify(&ScreenshotRecognitionSessionController::clearContent, true))
            return;
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
        if (!notify(&ScreenshotRecognitionSessionController::applyPresentation, m_presentation,
                    QImage{}, QRectF{}))
            return;
        if (!notify(&ScreenshotRecognitionSessionController::startTextRender))
            return;
        if (!notify(&ScreenshotRecognitionSessionController::textResultChanged, true))
            return;
    }
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_active && m_mode == Mode::Table) {
        const auto table = m_tableCache.constFind(recognitionCacheKey(Mode::Table));
        if (table != m_tableCache.cend()) {
            m_tableCacheKey = recognitionCacheKey(Mode::Table);
            if (!notify(&ScreenshotRecognitionSessionController::applyTableSession, table->session))
                return;
        } else if (m_tableEntries.contains(recognitionCacheKey(Mode::Table))) {
            cancelModelRecognition(Mode::Table);
            if (!notify(&ScreenshotRecognitionSessionController::activateModelResult))
                return;
        }
    }
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    if (m_active && m_mode == Mode::Qr) {
        const auto qr = m_qrCache.constFind(m_target.key);
        if (qr != m_qrCache.cend()) {
            m_qrCacheKey = m_target.key;
            if (!notify(&ScreenshotRecognitionSessionController::applyQrContents, qr.value()))
                return;
        }
    }
#endif
    if (!updateBusyState())
        return;
    if (!updateTextState())
        return;
    notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged);
}

std::optional<ScreenshotOcrRecognitionResult>
ScreenshotRecognitionSessionController::cachedTextRecognitionResult() const {
    if (!hasTarget())
        return std::nullopt;
    const auto text = m_textCache.constFind(m_target.key);
    if (text == m_textCache.cend() || !text->recognitionResult.error.isEmpty() ||
        !text->recognitionResult.presentation)
        return std::nullopt;
    auto result = text->recognitionResult;
    result.filteredImage = {};
    return result;
}

ScreenshotRecognitionResults
ScreenshotRecognitionSessionController::cachedRecognitionResults() const {
    ScreenshotRecognitionResults results;
    if (!hasTarget()) {
        return results;
    }
    results.key = m_target.key;
    results.document = cachedDocument(m_target.key);
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
    results.text = cachedTextRecognitionResult();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    results.tableModelSelection = m_tableModelSelection;
    results.tableEffectiveModel = resolvedRecognitionModel(Mode::Table);
    for (auto it = m_tableEntries.cbegin(); it != m_tableEntries.cend(); ++it) {
        if (m_tableEntryTargets.value(it.key()) != m_target.key ||
            !recognitionEntryValid(it->model, it->modelFingerprint))
            continue;
        auto entry = it.value();
        if (const auto session = m_tableCache.value(it.key()).session) {
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
        if (results.translatedText && m_target.key == m_translationKey && entry->jobInImage &&
            entry->translationJob) {
            for (const int index : m_pendingTranslationUnits) {
                const auto& text = entry->translationJob->units().at(index).text;
                if (!text.isEmpty())
                    results.translatedText->setLineText(index, text);
            }
        }
    }
#endif
    return results;
}

QHash<QString, QByteArray> ScreenshotRecognitionSessionController::contentCheckpoints() const {
    QHash<QString, QByteArray> result;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_QR_RECOGNITION ||                       \
    SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    const auto add = [&result](const QString& key, const QString& text) {
        result.insert(key, QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256));
    };
#endif
    if (const auto entry = m_textCache.constFind(m_target.key); entry != m_textCache.cend()) {
        if (entry->editingSession)
            result.insert(QStringLiteral("text"), entry->editingSession->contentCheckpoint());
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
        if (entry->translationSession)
            result.insert(QStringLiteral("translation"),
                          entry->translationSession->contentCheckpoint());
#endif
    }
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    for (auto it = m_tableEntries.cbegin(); it != m_tableEntries.cend(); ++it) {
        if (m_tableEntryTargets.value(it.key()) != m_target.key)
            continue;
        const QString key = QStringLiteral("table:") + it.key();
        const auto cached = m_tableCache.constFind(it.key());
        if (cached != m_tableCache.cend() && cached->session) {
            const auto current = cached->session->document.contentCheckpoint();
            if (current != cached->initialDocumentCheckpoint) {
                result.insert(key, current);
                continue;
            }
        }
        // Parsing can normalize HTML, but viewing the original source/draft is not an edit.
        add(key, it->draftHtml.value_or(it->result.html));
    }
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    for (auto it = m_latexCache.cbegin(); it != m_latexCache.cend(); ++it)
        if (m_latexEntryTargets.value(it.key()) == m_target.key && it.value())
            result.insert(QStringLiteral("latex:") + it.key(), it.value()->contentCheckpoint());
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    if (m_qrResults.contains(m_target.key))
        add(QStringLiteral("qr"), m_qrCache.value(m_target.key).join(u'\n'));
#endif
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    for (const auto& entry : m_conversion->entries(m_target.key))
        add(QStringLiteral("conversion:%1:%2").arg(int(entry.format)).arg(entry.model),
            entry.source);
#endif
    return result;
}

QString ScreenshotRecognitionSessionController::activeContentKey() const {
    if (m_mode == Mode::Text)
        return translating() ? QStringLiteral("translation") : QStringLiteral("text");
    if (m_mode == Mode::Table)
        return QStringLiteral("table:") + recognitionCacheKey(Mode::Table);
    if (m_mode == Mode::Latex)
        return QStringLiteral("latex:") + recognitionCacheKey(Mode::Latex);
    if (m_mode == Mode::Qr)
        return QStringLiteral("qr");
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (m_conversion->state() == ScreenshotImageConversionController::State::Completed &&
        !m_conversion->activeModel().isEmpty())
        return QStringLiteral("conversion:%1:%2")
            .arg(int(m_conversion->format()))
            .arg(m_conversion->activeModel());
#endif
    return {};
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
    const QString resolved =
        m_tableRecognition
            ? snowShotResolvedBuiltInModelId(m_tableRecognition->cachedChatModels(), selected)
            : selected;
    if (m_active && mode == m_mode && !m_recognitionEffectiveModel.isEmpty() &&
        (m_recognitionEffectiveModel == selected || m_recognitionEffectiveModel == resolved))
        return m_recognitionEffectiveModel;
    return hasRecognitionModelResult(mode, selected) ? selected : resolved;
#else
    return selected;
#endif
}

bool ScreenshotRecognitionSessionController::hasRecognitionModelResult(Mode mode,
                                                                       const QString& model) const {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (model.isEmpty())
        return false;
    const QString key = recognitionCacheKey(mode, model);
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (mode == Mode::Table)
        return m_tableCache.contains(key) || m_tableEntries.contains(key);
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (mode == Mode::Latex)
        return m_latexResults.contains(key);
#endif
#else
    Q_UNUSED(mode)
    Q_UNUSED(model)
#endif
    return false;
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

bool ScreenshotRecognitionSessionController::updateRecognitionModels() const {
    if (m_destroying)
        return true;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                    \
    SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (!m_actions.setRecognitionModelState)
        return true;
    ScreenshotRecognitionModelState state;
    state.loading = m_recognitionModelsLoading;
    state.error = m_recognitionModelsError;
    if (m_tableRecognition)
        state.models = m_tableRecognition->cachedChatModels();
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (conversionModeActive()) {
        state.selection = m_conversion->activeModel();
        if (state.selection.isEmpty())
            state.selection = snow_shot::storage::ScreenshotImageConversionSettings().visionModel();
        return notifyUiCallback(m_actions.setRecognitionModelState, static_cast<int>(m_mode),
                                state);
    }
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    state.selection = m_tableModelSelection;
    state.effectiveModel =
        m_active && m_mode == Mode::Table ? m_recognitionEffectiveModel : QString{};
    if (!notifyUiCallback(m_actions.setRecognitionModelState, static_cast<int>(Mode::Table), state))
        return false;
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    state.selection = m_latexModelSelection;
    state.effectiveModel =
        m_active && m_mode == Mode::Latex ? m_recognitionEffectiveModel : QString{};
    if (!notifyUiCallback(m_actions.setRecognitionModelState, static_cast<int>(Mode::Latex), state))
        return false;
#endif
#endif
    return true;
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

bool ScreenshotRecognitionSessionController::migrateRecognitionModelPreferences() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (!m_tableRecognition)
        return true;
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    const auto& models = m_tableRecognition->cachedChatModels();
    const auto settings = snow_shot::storage::ScreenshotRecognitionModelSettings();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    const QString table = settings.tableModel();
    const QString resolvedTable = snowShotResolvedBuiltInModelId(models, table);
    if (table != resolvedTable) {
        settings.setTableModel(resolvedTable);
        if (!lifetime)
            return false;
        if (!m_tableRecognition)
            return true;
    }
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    const QString latex = settings.latexModel();
    const QString resolvedLatex =
        snowShotResolvedBuiltInModelId(m_tableRecognition->cachedChatModels(), latex);
    if (latex != resolvedLatex) {
        settings.setLatexModel(resolvedLatex);
        if (!lifetime)
            return false;
    }
#endif
#endif
    return true;
}

void ScreenshotRecognitionSessionController::loadRecognitionModels(bool refresh) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                    \
    SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (!m_active || !m_tableRecognition || m_recognitionModelsToken) {
        if (!updateRecognitionModels())
            return;
        return;
    }
    m_recognitionModelsLoading = true;
    m_recognitionModelsError.clear();
    if (!updateRecognitionModels())
        return;
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
            if (result.succeeded() && !migrateRecognitionModelPreferences())
                return;
            if (generation != m_recognitionModelsGeneration || !m_active || !m_tableRecognition)
                return;
            if (!result.error.isEmpty() ||
                !m_tableRecognition->hasBuiltInModels(
                    snow_shot::presentation::LanguageManager::instance().currentLocale().name())) {
                m_recognitionModelsError = tr("Unable to load Snow Shot vision models");
            }
            if (!updateRecognitionModels())
                return;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
            if ((m_mode == Mode::Table || m_mode == Mode::Latex) && m_recognitionAwaitingModel)
                if (!notify(&ScreenshotRecognitionSessionController::activateModelResult))
                    return;
#endif
            if (!updateBusyState())
                return;
        },
        refresh ? SnowShotApiClient::ChatModelsCachePolicy::Refresh
                : SnowShotApiClient::ChatModelsCachePolicy::UseCached);
    if (!m_recognitionModelsToken) {
        m_recognitionModelsLoading = false;
        m_recognitionModelsError = tr("Unable to load Snow Shot vision models");
        if (!updateRecognitionModels())
            return;
    }
#else
    Q_UNUSED(refresh)
#endif
}

void ScreenshotRecognitionSessionController::cancelModelRecognition(Mode mode) {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    if (mode == Mode::Document) {
        ++m_documentGeneration;
        if (auto cancellation = std::exchange(m_documentCancellation, {}))
            cancellation->store(true, std::memory_order_relaxed);
        m_documentCopyJob.cancel();
        m_documentCopyJob = {};
        m_documentClipboardCommit.cancel();
        m_documentClipboardCommit = {};
        if (m_documentRequestToken && m_tableRecognition)
            m_tableRecognition->cancel(m_documentRequestToken);
        m_documentRequestToken = 0;
        m_documentParsing = false;
        m_openingDocument = false;
        delete m_documentPreparation.data();
    }
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (mode == m_mode)
        m_recognitionAwaitingModel = false;
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (mode == Mode::Table) {
        ++m_tableGeneration;
        if (m_tableConversion)
            m_tableConversion->cancel();
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
        clearLatexStream();
    }
#endif
    Q_UNUSED(mode)
}

void ScreenshotRecognitionSessionController::failModelRecognition(const QString& message) {
    if (!hideRecognitionMessage())
        return;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    m_recognitionAwaitingModel = false;
#endif
    if (!showStatus(message, true))
        return;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_mode == Mode::Table)
        m_tableSession.reset();
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_mode == Mode::Latex) {
        clearLatexStream();
        if (content() && !notifyCallback([this] { content()->hideTextEditor(); }))
            return;
    }
#endif
    if (content())
        content()->showRecognitionError(message);
    if (!updateBusyState())
        return;
    if (!updateTextState())
        return;
}

void ScreenshotRecognitionSessionController::setRecognitionModel(const QString& selection) {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (conversionModeActive()) {
        m_conversion->selectVisionModel(selection);
        return;
    }
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    if (!m_active || (m_mode != Mode::Table && m_mode != Mode::Latex) || selection.isEmpty() ||
        selection.size() > 256 || selection == recognitionModelSelection(m_mode) ||
        std::any_of(selection.cbegin(), selection.cend(), [](QChar character) {
            return character.isSpace() || character.category() == QChar::Other_Control;
        }))
        return;
    if (content() && m_mode == Mode::Table)
        content()->commitActiveTableEdit();
    cancelModelRecognition(m_mode);
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_mode == Mode::Latex && content()) {
        content()->hideTextEditor();
        if (!lifetime)
            return;
    }
#endif
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
    if (!lifetime)
        return;
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_mode == Mode::Latex)
        settings.setLatexModel(selection);
    if (!lifetime)
        return;
#endif
    m_savingRecognitionModel = false;
    if (!notify(&ScreenshotRecognitionSessionController::clearContent, m_mode == Mode::Table))
        return;
    m_workflowError.clear();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableSession.reset();
    m_tableCacheKey.clear();
#endif
    if (!updateRecognitionModels())
        return;
    if (!notify(&ScreenshotRecognitionSessionController::activateModelResult))
        return;
    if (!notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged))
        return;
#else
    Q_UNUSED(selection)
#endif
}

void ScreenshotRecognitionSessionController::retryRecognition() {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    if (m_active && m_mode == Mode::Document && !busy(m_mode)) {
        activate(Mode::Document);
        return;
    }
#endif
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
    if (!notify(&ScreenshotRecognitionSessionController::loadRecognitionModels, refresh))
        return;
    notify(&ScreenshotRecognitionSessionController::activateModelResult);
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
            if (!showRecognitionMessage())
                return;
            if (!updateBusyState())
                return;
        } else {
            m_recognitionAwaitingModel = false;
            if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                        tr("Snow Shot's visual understanding model is unavailable. Choose "
                           "another model or retry")))
                return;
        }
        return;
    }
    const QString key = recognitionCacheKey(m_mode, model);
    if (!hasRecognitionModelResult(m_mode, model) &&
        model != screenshotDedicatedRecognitionModelId() && !recognitionModelAvailable(model)) {
        if (m_recognitionModelsLoading && !model.startsWith(QStringLiteral("custom:"))) {
            m_recognitionAwaitingModel = true;
            if (!showRecognitionMessage())
                return;
            if (!updateBusyState())
                return;
        } else if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                           tr("The selected recognition model is unavailable. Choose another model "
                              "or retry"))) {
            return;
        }
        return;
    }
    // Only usable results and requests bind an identity. Pending discovery must remain free
    // to resolve a saved alias against the catalog that eventually arrives.
    m_recognitionEffectiveModel = model;
    if (!updateRecognitionModels())
        return;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_mode == Mode::Table && m_tableCache.contains(key)) {
        m_recognitionAwaitingModel = false;
        m_tableCacheKey = key;
        if (!notify(&ScreenshotRecognitionSessionController::applyTableSession,
                    m_tableCache.value(key).session))
            return;
        if (!hideRecognitionMessage())
            return;
        if (!updateBusyState())
            return;
        if (!updateTextState())
            return;
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
        if (!notify(&ScreenshotRecognitionSessionController::applyLatexContents, latexDraft()))
            return;
        if (!hideRecognitionMessage())
            return;
        if (!updateBusyState())
            return;
        if (!updateTextState())
            return;
        return;
    }
#endif
    m_recognitionAwaitingModel = false;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (m_mode == Mode::Table)
        if (!notify(&ScreenshotRecognitionSessionController::startTableRecognition))
            return;
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_mode == Mode::Latex)
        if (!notify(&ScreenshotRecognitionSessionController::startLatexRecognition))
            return;
#endif
    if (!updateBusyState())
        return;
    if (!updateTextState())
        return;
#endif
}

void ScreenshotRecognitionSessionController::prefetchText() {
    if (!hasTarget() || m_textCache.contains(m_target.key) || m_textRequestToken != 0) {
        return;
    }
    if (!notify(&ScreenshotRecognitionSessionController::startTextRecognition,
                ScreenshotOcrRequestPriority::Prefetch))
        return;
}

void ScreenshotRecognitionSessionController::activate(Mode mode) {
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    if (!snow_shot::app::editionRecognitionModeAvailable(static_cast<int>(mode)))
        return;
    const auto transition = ++m_transitionGeneration;
    m_uiPublicationEnabled = true;
    if (m_active && m_mode == mode &&
        (mode == Mode::Table || mode == Mode::Latex || mode == Mode::Document) && busy(mode)) {
        if (!ownsContentAttachment()) {
            if (!notify(&ScreenshotRecognitionSessionController::attachContent) || !lifetime ||
                transition != m_transitionGeneration)
                return;
            const QPointer<ScreenshotRecognitionWindow> view = content();
            if (view) {
                const auto token = m_attachmentToken;
                const auto attached = [lifetime, view, token, transition] {
                    return lifetime && view && lifetime->m_transitionGeneration == transition &&
                           view->ownsAttachment(lifetime, token);
                };
                if (m_actions.setRecognitionVisualState &&
                    !notifyUiCallback(m_actions.setRecognitionVisualState, true))
                    return;
                if (m_actions.setActiveMode &&
                    !notifyUiCallback(m_actions.setActiveMode, static_cast<int>(mode)))
                    return;
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
                if (mode == Mode::Latex && m_latexStream && m_latexStream->document) {
                    const auto stream = m_latexStream;
                    view->showLatexEditor(
                        stream->document, [stream] { return stream->source; }, true);
                    if (!attached())
                        return;
                }
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
                if (mode == Mode::Table && m_tableConversion && m_tableConversion->busy()) {
                    view->showTablePreview(m_tableConversion->preview());
                    if (!attached())
                        return;
                } else if (mode == Mode::Table && m_tableSession) {
                    view->setTableSession(m_tableSession);
                    if (!attached())
                        return;
                }
#endif
                if (!notify(&ScreenshotRecognitionSessionController::updateOriginalImagePreview) ||
                    !attached())
                    return;
                if (!showRecognitionMessage() || !attached() || !updateBusyState() || !attached() ||
                    !updateTextState() || !attached())
                    return;
            }
        }
        if (!updateRecognitionModels())
            return;
        return;
    }
    if (m_active && m_mode != mode)
        cancelModelRecognition(m_mode);
    if (content() && m_active && m_mode == Mode::Table)
        content()->commitActiveTableEdit();
    if (!lifetime || transition != m_transitionGeneration)
        return;
    if (!hasTarget()) {
        if (!showStatus(tr("Unable to read the selected screenshot"), true))
            return;
        return;
    }
    if (!m_active || mode != m_mode) {
        if (!notify(&ScreenshotRecognitionSessionController::setShowOriginalImage, false))
            return;
    }
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    m_conversion->deactivate();
    if (!lifetime || transition != m_transitionGeneration)
        return;
#endif
    if (!notify(&ScreenshotRecognitionSessionController::clearTextEditingState))
        return;
    if (!lifetime || transition != m_transitionGeneration)
        return;
    const bool renderBackground = mode == Mode::Text && shouldRenderTextBackground();
    if (!lifetime || transition != m_transitionGeneration)
        return;
    if (!renderBackground && m_textRenderRequestToken != 0) {
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
    if (!notify(&ScreenshotRecognitionSessionController::attachContent) || !lifetime ||
        transition != m_transitionGeneration)
        return;
    if (m_actions.setRecognitionVisualState) {
        if (!notifyUiCallback(m_actions.setRecognitionVisualState, true))
            return;
    }
    if (m_actions.setActiveMode) {
        if (!notifyUiCallback(m_actions.setActiveMode, static_cast<int>(mode)))
            return;
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
    if (!notify(&ScreenshotRecognitionSessionController::clearContent, false))
        return;
    if (!lifetime || transition != m_transitionGeneration)
        return;
    if (m_actions.clearOcrBackground) {
        if (!notifyUiCallback(m_actions.clearOcrBackground))
            return;
    }
    if (mode == Mode::Text) {
        const auto cached = m_textCache.constFind(m_target.key);
        if (cached != m_textCache.cend()) {
            m_textCacheKey = m_target.key;
            if (cached->formatted) {
                m_presentation.reset();
                if (!notify(&ScreenshotRecognitionSessionController::applyFormattedText,
                            cached->formattedDocument))
                    return;
            } else {
                m_presentation = cached->presentation;
                if (!notify(&ScreenshotRecognitionSessionController::applyPresentation,
                            m_presentation, QImage{}, QRectF{}))
                    return;
                if (!notify(&ScreenshotRecognitionSessionController::startTextRender))
                    return;
            }
            if (!notify(&ScreenshotRecognitionSessionController::textResultChanged, true))
                return;
            if (!m_independentEditing && cached->editing) {
                m_editing = true;
                m_editingKey = m_target.key;
                if (!notify(&ScreenshotRecognitionSessionController::beginTextEditing))
                    return;
            }
        } else if (m_textRequestToken != 0) {
            if (!notify(&ScreenshotRecognitionSessionController::setPendingTextRecognitionRendering,
                        true))
                return;
            static_cast<void>(m_recognition->reprioritize(
                m_textRequestToken, ScreenshotOcrRequestPriority::Interactive));
            if (m_textModelDownloadInProgress && textModelDownloading()) {
                if (!notify(&ScreenshotRecognitionSessionController::showModelDownloadMessage))
                    return;
            } else {
                if (!showRecognitionMessage())
                    return;
            }
        } else {
            if (!notify(&ScreenshotRecognitionSessionController::startTextRecognition,
                        ScreenshotOcrRequestPriority::Interactive))
                return;
        }
    } else if (mode == Mode::Table || mode == Mode::Latex) {
        if (!notify(&ScreenshotRecognitionSessionController::setPendingTextRecognitionRendering,
                    false))
            return;
        if (!notify(&ScreenshotRecognitionSessionController::loadRecognitionModels, false))
            return;
        if (!notify(&ScreenshotRecognitionSessionController::activateModelResult))
            return;
    } else if (mode == Mode::Document) {
        if (!notify(&ScreenshotRecognitionSessionController::setPendingTextRecognitionRendering,
                    false))
            return;
        if (cachedDocument(m_target.key))
            applyDocument();
        else
            startDocumentRecognition();
        if (!lifetime || transition != m_transitionGeneration)
            return;
    } else if (mode == Mode::Qr) {
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
        if (!notify(&ScreenshotRecognitionSessionController::setPendingTextRecognitionRendering,
                    false))
            return;
        const auto cached = m_qrCache.constFind(m_target.key);
        if (cached != m_qrCache.cend()) {
            m_qrCacheKey = m_target.key;
            if (!notify(&ScreenshotRecognitionSessionController::applyQrContents, cached.value()))
                return;
        } else {
            if (!notify(&ScreenshotRecognitionSessionController::startQrRecognition))
                return;
        }
#endif
    } else {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
        if (!notify(&ScreenshotRecognitionSessionController::setPendingTextRecognitionRendering,
                    false))
            return;
        if (m_actions.setRecognitionModelState &&
            !notify(&ScreenshotRecognitionSessionController::loadRecognitionModels, false))
            return;
        m_conversion->activate(m_target.key, m_target.image,
                               mode == Mode::Markdown ? SnowShotImageConversionFormat::Markdown
                                                      : SnowShotImageConversionFormat::Html);
        if (!lifetime || transition != m_transitionGeneration)
            return;
#endif
    }
    if (!updateBusyState())
        return;
    if (!lifetime || transition != m_transitionGeneration)
        return;
    if (!updateTextState())
        return;
}

void ScreenshotRecognitionSessionController::deactivate() {
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    const auto transition = ++m_transitionGeneration;
    if (content() && m_active && m_mode == Mode::Table)
        content()->commitActiveTableEdit();
    if (!lifetime || transition != m_transitionGeneration)
        return;
    cancelModelRecognition(Mode::Table);
    cancelModelRecognition(Mode::Latex);
    cancelModelRecognition(Mode::Document);
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                    \
    SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (m_tableRecognition && m_recognitionModelsToken)
        m_tableRecognition->cancel(m_recognitionModelsToken);
    m_recognitionModelsToken = 0;
    m_recognitionModelsLoading = false;
    ++m_recognitionModelsGeneration;
#endif
    if (!notify(&ScreenshotRecognitionSessionController::setShowOriginalImage, false))
        return;
    if (!lifetime || transition != m_transitionGeneration)
        return;
    if (!m_active && !ownsContentAttachment()) {
        retireContentAttachment();
        return;
    }
    if (!notify(&ScreenshotRecognitionSessionController::clearTextEditingState))
        return;
    if (!lifetime || transition != m_transitionGeneration)
        return;
    const bool wasActive = m_active;
    m_active = false;
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    m_conversion->deactivate();
    if (!lifetime || transition != m_transitionGeneration)
        return;
#endif
    if (!notify(&ScreenshotRecognitionSessionController::hideModelDownloadMessage))
        return;
    if (!lifetime || transition != m_transitionGeneration)
        return;
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
        if (!notify(&ScreenshotRecognitionSessionController::setPendingTextRecognitionRendering,
                    false))
            return;
    }
    m_presentation.reset();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableSession.reset();
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    m_qrContents.clear();
#endif
    if (!notify(&ScreenshotRecognitionSessionController::clearContent, false))
        return;
    if (!lifetime || transition != m_transitionGeneration)
        return;
    if (m_actions.clearOcrBackground) {
        if (!notifyUiCallback(m_actions.clearOcrBackground))
            return;
    }
    // The content widget is retained between sessions. Repeated cleanup must not
    // announce another exit and overwrite the host's newly selected drawing tool.
    if (wasActive && m_actions.setRecognitionVisualState) {
        if (!notifyUiCallback(m_actions.setRecognitionVisualState, false))
            return;
    }
    if (wasActive && m_actions.setActiveMode) {
        if (!notifyUiCallback(m_actions.setActiveMode, -1))
            return;
    }
    if (!hideRecognitionMessage())
        return;
    if (!updateBusyState())
        return;
    if (!updateTextState())
        return;
    if (!notify(&ScreenshotRecognitionSessionController::updateTableState,
                ScreenshotTableCommandState{}))
        return;
    if (!lifetime || transition != m_transitionGeneration)
        return;
    if (lifetime && transition == m_transitionGeneration)
        retireContentAttachment();
}

void ScreenshotRecognitionSessionController::invalidate() {
    if (!notify(&ScreenshotRecognitionSessionController::resetTargetState))
        return;
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    m_conversion->invalidate();
    if (!lifetime)
        return;
#endif
    m_textCache.clear();
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    m_tableCache.clear();
#endif
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    m_documents.clear();
    m_documentPreview.reset();
    m_documentPreviewKey.clear();
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
    notify(&ScreenshotRecognitionSessionController::textResultChanged, false);
}

void ScreenshotRecognitionSessionController::resetTargetState() {
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    const auto transition = m_transitionGeneration;
    // Cancellation belongs to the outgoing attachment: publish its final busy
    // state before deactivate retires that attachment. Ordinary tool switching
    // still preserves cached background translation work.
    cancelOutstandingRequests();
    if (!lifetime || transition != m_transitionGeneration)
        return;
    if (!notify(&ScreenshotRecognitionSessionController::deactivate) || !lifetime || m_active)
        return;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (m_translationSettingsModal != nullptr) {
        m_translationSettingsModal->reject();
        if (!lifetime)
            return;
    }
#endif

    // Raw recognition payloads survive a target change, but editing state belongs to
    // the target that is currently visible and must not leak into another target.
    for (auto it = m_textCache.begin(); it != m_textCache.end(); ++it) {
        if (it->editingSession != nullptr) {
            if (!invokeOwned(it->editingSession, &ScreenshotOcrTextEditingSession::establishHistory,
                             it->editingSession->originalText()))
                return;
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
    if (!lifetime)
        return;
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
    if (!updateTextState())
        return;
}

bool ScreenshotRecognitionSessionController::active() const {
    return m_active;
}

bool ScreenshotRecognitionSessionController::busy() const {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    return busy(Mode::Document) || busy(Mode::Latex) || busy(Mode::Text) || busy(Mode::Table) ||
           busy(Mode::Qr) || m_conversion->busy();
#else
    return busy(Mode::Document) || busy(Mode::Latex) || busy(Mode::Text) || busy(Mode::Table) ||
           busy(Mode::Qr) || busy(Mode::Markdown) || busy(Mode::Html);
#endif
}

bool ScreenshotRecognitionSessionController::busy(Mode mode) const {
    switch (mode) {
    case Mode::Document:
        return m_documentRequestToken != 0 || m_documentParsing || m_openingDocument;
    case Mode::Text:
        return m_textRequestToken != 0 || m_textRenderRequestToken != 0;
    case Mode::Table:
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
        return m_tableRequestToken != 0 || m_tableParsing ||
               (m_tableConversion && m_tableConversion->busy()) ||
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
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    const auto ownerCallback = m_actions.translationSettingsOwner;
    QWidget* owner = ownerCallback ? ownerCallback() : content();
    if (!lifetime)
        return;
    m_conversion->openSettings(owner);
#endif
}

bool ScreenshotRecognitionSessionController::updateConversionState() const {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (!notify(&ScreenshotRecognitionSessionController::workflowStateChanged))
        return false;
    if (conversionModeActive() && m_conversion->active() && !updateRecognitionModels())
        return false;
    if (conversionModeActive() && m_conversion->active() && content() != nullptr) {
        content()->showImageConversion(m_conversion->format(), m_conversion->source(),
                                       m_conversion->busy(), m_conversion->error());
    }
    if (m_actions.setConversionState) {
        if (!notifyUiCallback(m_actions.setConversionState, conversionModeActive(),
                              m_conversion->busy(), m_conversion->format()))
            return false;
    }
#endif
    return true;
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
        if (!showRecognitionMessage())
            return;
    } else if (m_actions.hideLoading) {
        // Background OCR may still be busy; it must not keep this conversion prompt alive.
        if (!notifyUiCallback(m_actions.hideLoading))
            return;
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

bool ScreenshotRecognitionSessionController::openSpreadsheetFile() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (!tableModeActive() || busy(Mode::Table) || m_openingSpreadsheet)
        return false;
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    const QString targetKey = m_target.key;
    m_openingSpreadsheet = true;
    const auto snapshot = fileExportSnapshot();
    if (!lifetime)
        return false;
    if (!snapshot || snapshot->kind != ScreenshotRecognitionFileKind::Table) {
        m_openingSpreadsheet = false;
        return false;
    }
    const auto result = ScreenshotRecognitionFileExport::openTemporaryTable(
        *snapshot, m_actions.openSpreadsheetUrl);
    if (!lifetime)
        return false;
    m_openingSpreadsheet = false;
    if (!tableModeActive() || m_target.key != targetKey)
        return false;
    if (!result.succeeded()) {
        static_cast<void>(notifyUiCallback(m_actions.showStatus, result.error, true));
        return false;
    }
    return true;
#else
    return false;
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
            if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::undo))
                return;
        return;
    }
#endif
    if (!m_active || (!m_editing && !m_translating) || m_textDocument == nullptr) {
        return;
    }
    const auto entry = m_textCache.value(m_editingKey);
    const auto session = currentEditingSession(entry);
    if (session != nullptr) {
        if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::undo))
            return;
    }
}

void ScreenshotRecognitionSessionController::redoTextEdit() {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_active && m_mode == Mode::Latex) {
        if (const auto session = m_latexCache.value(recognitionCacheKey(Mode::Latex)))
            if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::redo))
                return;
        return;
    }
#endif
    if (!m_active || (!m_editing && !m_translating) || m_textDocument == nullptr) {
        return;
    }
    const auto entry = m_textCache.value(m_editingKey);
    const auto session = currentEditingSession(entry);
    if (session != nullptr) {
        if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::redo))
            return;
    }
}

void ScreenshotRecognitionSessionController::beginTextEditing() {
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    if (!m_active || m_mode != Mode::Text || !hasTextResult()) {
        return;
    }
    m_editing = true;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    m_translating = false;
#endif
    m_editingKey = m_textCacheKey.isEmpty() ? m_target.key : m_textCacheKey;
    auto it = m_textCache.find(m_editingKey);
    std::shared_ptr<QTextDocument> documentOwner;
    if (it != m_textCache.end()) {
        it->editing = true;
        if (!it->formatted && !it->defaultTransformsApplied && it->editingSession != nullptr) {
            it->defaultTransformsApplied = true;
            const snow_shot::storage::TextRecognitionSettings settings;
            const QString formatting = settings.defaultFormatting();
            const QString smartText = formatting == QStringLiteral("smart") && it->presentation
                                          ? snow_shot::presentation::smartOcrText(*it->presentation)
                                          : QString{};
            if (!invokeOwned(it->editingSession,
                             &ScreenshotOcrTextEditingSession::applyInitialTransforms, formatting,
                             settings.defaultPunctuation(), smartText))
                return;
        }
        m_textDocument = it->editingSession != nullptr ? it->editingSession->document() : nullptr;
        if (it->editingSession)
            documentOwner = {it->editingSession, m_textDocument.data()};
    }
    if (content() != nullptr && m_textDocument != nullptr) {
        if (m_actions.clearOcrBackground) {
            if (!notifyUiCallback(m_actions.clearOcrBackground))
                return;
        }
        content()->clearOcrPresentation();
        content()->showTextEditor(std::move(documentOwner));
        if (!lifetime)
            return;
    }
    if (!notify(&ScreenshotRecognitionSessionController::textEditingChanged, true))
        return;
    if (!updateTextState())
        return;
}

void ScreenshotRecognitionSessionController::beginTextTranslation() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    m_workflowError.clear();
    if (!m_active || m_mode != Mode::Text || !hasTextResult() || m_translating) {
        return;
    }
    const snow_shot::storage::ScreenshotTranslationSettings settings;
    const auto cached = m_textCache.constFind(m_target.key);
    if (cached != m_textCache.cend() && cached->overlayTranslation.captured &&
        (!m_independentEditing || settings.originalImageTranslationEnabled()) &&
        (!cached->hasTranslationConfiguration ||
         cached->translationConfiguration == settings.configuration()) &&
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
    const bool originalImage =
        settings.originalImageTranslationEnabled() && !it->formatted && it->presentation != nullptr;
    if (originalImage != m_translationInImage) {
        if (!cancelTranslationRequests())
            return;
    }
    m_translationInImage = originalImage;
    m_translationKey = m_editingKey;
    if (it->hasTranslationConfiguration &&
        it->translationConfiguration != settings.configuration()) {
        if (!notify(&ScreenshotRecognitionSessionController::invalidateCurrentTranslation, false))
            return;
        it = m_textCache.find(m_editingKey);
        if (it == m_textCache.end())
            return;
    }
    prepareIndependentTranslation(*it, m_editingKey);
    if (originalImage) {
        m_textDocument = nullptr;
        showOverlayTranslation();
        return;
    }
    if (it->translationSession == nullptr) {
        it->translationSession = std::make_shared<ScreenshotOcrTextEditingSession>(QString());
        connect(it->translationSession->document(), &QTextDocument::contentsChanged, this,
                [this, key = m_editingKey]() { handleTranslationDocumentChanged(key); });
    }
    m_translationKey = m_editingKey;
    m_textDocument = it->translationSession->document();
    const std::shared_ptr<QTextDocument> documentOwner(it->translationSession,
                                                       m_textDocument.data());
    if (content() != nullptr) {
        if (m_actions.clearOcrBackground) {
            if (!notifyUiCallback(m_actions.clearOcrBackground))
                return;
        }
        const bool streaming =
            it->translationStatus == TextCacheEntry::TranslationStatus::Streaming;
        content()->showTextEditor(documentOwner, streaming, streaming);
        if (!lifetime)
            return;
    }
    if (!notify(&ScreenshotRecognitionSessionController::textEditingChanged, true))
        return;
    if (!updateTextState())
        return;
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
    if (!notify(&ScreenshotRecognitionSessionController::applyPresentation,
                entry->overlayTranslation.presentation, QImage{}, QRectF{}))
        return false;
    if (!notify(&ScreenshotRecognitionSessionController::startTextRender))
        return false;
    if (!notify(&ScreenshotRecognitionSessionController::textEditingChanged, false))
        return false;
    if (!notify(&ScreenshotRecognitionSessionController::synchronizeUiState))
        return false;
    return true;
#else
    return false;
#endif
}

void ScreenshotRecognitionSessionController::synchronizeUiState() const {
    if (!updateBusyState())
        return;
    if (!updateRecognitionModels())
        return;
    if (m_actions.setShowOriginalImage) {
        if (!notifyUiCallback(m_actions.setShowOriginalImage, m_showOriginalImage))
            return;
    }
    if (m_active && m_actions.setActiveMode) {
        if (!notifyUiCallback(m_actions.setActiveMode, static_cast<int>(m_mode)))
            return;
    }
    if (!updateTextState())
        return;
    if (!updateConversionState())
        return;
    if (m_active && m_mode == Mode::Table && content() != nullptr) {
        if (!notify(&ScreenshotRecognitionSessionController::updateTableState,
                    content()->tableCommandState()))
            return;
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
        if (!notify(&ScreenshotRecognitionSessionController::applyFormattedText,
                    entry.formattedDocument))
            return;
    } else {
        if (!notify(&ScreenshotRecognitionSessionController::applyPresentation, entry.presentation,
                    QImage{}, QRectF{}))
            return;
        if (!notify(&ScreenshotRecognitionSessionController::startTextRender))
            return;
    }
    if (!notify(&ScreenshotRecognitionSessionController::textEditingChanged, false))
        return;
    if (!updateTextState())
        return;
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
            if (!invokeOwned(it->translationSession, &ScreenshotOcrTextEditingSession::replaceText,
                             it->successfulTranslation))
                return;
        }
        if (!updateTextState())
            return;
        return;
    }
#endif
    if (m_editing) {
        auto it = m_textCache.find(m_editingKey);
        if (it != m_textCache.end() && it->editingSession != nullptr) {
            it->editingSession->clearTransforms();
            if (!invokeOwned(it->editingSession, &ScreenshotOcrTextEditingSession::replaceText,
                             it->editingSession->originalText()))
                return;
        }
    }
    if (!updateTextState())
        return;
}

void ScreenshotRecognitionSessionController::applyTextTargetLanguage(const QString& language) {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    if (m_translationService == nullptr)
        return;
    auto preferences = m_translationService->preferences();
    if (preferences.targetLanguage != language) {
        preferences.targetLanguage = language;
        const bool saved = m_translationService->savePreferences(preferences);
        if (!lifetime || !m_translationService)
            return;
        if (!saved)
            if (!showStatus(m_translationService->errorText(), true))
                return;
    }
    if (m_actions.setTextTargetLanguage)
        if (!notifyUiCallback(m_actions.setTextTargetLanguage,
                              m_translationService->preferences().targetLanguage))
            return;
#else
    Q_UNUSED(language);
#endif
}

void ScreenshotRecognitionSessionController::requestTextTranslationServices() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (m_translationService != nullptr)
        m_translationService->refreshModels();
#endif
}

void ScreenshotRecognitionSessionController::applyTextTranslationService(const QString& service) {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    if (m_translationService == nullptr)
        return;
    auto preferences = m_translationService->preferences();
    if (preferences.modelId != service) {
        preferences.modelId = service;
        const bool saved = m_translationService->savePreferences(preferences);
        if (!lifetime || !m_translationService)
            return;
        if (!saved)
            if (!showStatus(m_translationService->errorText(), true))
                return;
    }
    updateTextState();
#else
    Q_UNUSED(service);
#endif
}

void ScreenshotRecognitionSessionController::openTranslationSettings() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    if (m_translationService == nullptr) {
        if (!showStatus(tr("Translation service is unavailable"), true))
            return;
        return;
    }
    if (m_translationSettingsModal != nullptr)
        return;
    const auto ownerCallback = m_actions.translationSettingsOwner;
    QWidget* owner = ownerCallback ? ownerCallback() : content();
    if (!lifetime)
        return;
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
                if (!notify(&ScreenshotRecognitionSessionController::invalidateCurrentTranslation,
                            false))
                    return;
                if (!notify(&ScreenshotRecognitionSessionController::beginTextTranslation))
                    return;
            }
        });
    connect(m_translationSettingsModal, &adqt::widgets::AdModal::finished, this,
            [this] { m_translationSettingsModal = nullptr; });
#endif
}

void ScreenshotRecognitionSessionController::prepareIndependentTranslation(TextCacheEntry& entry,
                                                                           const QString& key) {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const auto& overlay = entry.overlayTranslation;
    if (!m_independentEditing || entry.translationSession || !overlay.captured ||
        !overlay.presentation || overlay.status != TextCacheEntry::TranslationStatus::Completed ||
        (entry.hasTranslationConfiguration &&
         entry.translationConfiguration !=
             snow_shot::storage::ScreenshotTranslationSettings().configuration()))
        return;
    // Captured translations belong to the source content, while their display mode belongs to
    // the host. Seed a text baseline before the independent host records its clean checkpoint.
    const QString text = snow_shot::presentation::originalOcrText(*overlay.presentation);
    entry.translationSession = std::make_shared<ScreenshotOcrTextEditingSession>(text);
    connect(entry.translationSession->document(), &QTextDocument::contentsChanged, this,
            [this, key] { handleTranslationDocumentChanged(key); });
    entry.translationText = text;
    entry.successfulTranslation = text;
    entry.hasSuccessfulTranslation = true;
    entry.translationStatus = TextCacheEntry::TranslationStatus::Completed;
#else
    Q_UNUSED(entry)
    Q_UNUSED(key)
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
        if (!notify(&ScreenshotRecognitionSessionController::failTranslationPreparation,
                    tr("Translation service is unavailable")))
            return;
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
                if (!invokeOwned(it->translationSession,
                                 &ScreenshotOcrTextEditingSession::replaceTextWithoutHistory,
                                 QString{}))
                    return;
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
                if (!inImage)
                    entry->translationText = job->units().at(index).text;
                m_pendingTranslationUnits.insert(index);
                if (!m_translationUpdateTimer->isActive())
                    m_translationUpdateTimer->start();
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
        if (inImage && !finishOverlayTranslationLines(job))
            return;
        if (!job->busy() && !flushTranslationUpdates())
            return;
        if (content() != nullptr && m_translating && !inImage)
            content()->setTextEditorStreaming(job->busy());
        if (!updateTextState())
            return;
    });
    connect(job, &TranslationJob::finished, this, [this, job, key, generation, inImage] {
        if (generation != m_translationGeneration)
            return;
        if (inImage) {
            if (!notify(&ScreenshotRecognitionSessionController::reportOverlayTranslationFailure))
                return;
        } else if (!notify(&ScreenshotRecognitionSessionController::handleTranslationFinished,
                           generation, key, job->result()))
            return;
    });
    job->retry();
#endif
}

bool ScreenshotRecognitionSessionController::showOverlayTranslation() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const auto generation = m_translationGeneration;
    const QString key = m_translationKey;
    auto entry = m_textCache.find(key);
    if (entry == m_textCache.end() || !entry->presentation)
        return true;
    const auto configuration = snow_shot::storage::ScreenshotTranslationSettings().configuration();
    const QFont font = QApplication::font();
    if (entry->overlayLayoutFont != font) {
        entry->overlayLayouts.clear();
        entry->overlayLayoutFont = font;
    }
    const auto source = entry->presentation;
    const auto publish = [this, generation, key, source] {
        const auto entry = m_textCache.constFind(key);
        if (generation != m_translationGeneration || key != m_translationKey ||
            entry == m_textCache.cend() || entry->presentation != source)
            return false;
        const auto presentation = entry->overlayTranslation.presentation;
        const auto status = entry->overlayTranslation.status;
        if (m_active && m_mode == Mode::Text && originalImageTranslationActive() &&
            m_editingKey == key) {
            if (!notify(&ScreenshotRecognitionSessionController::applyPresentation, presentation,
                        QImage{}, QRectF{}) ||
                generation != m_translationGeneration || m_presentation != presentation)
                return false;
            if (!notify(&ScreenshotRecognitionSessionController::startTextRender) ||
                generation != m_translationGeneration ||
                !notify(&ScreenshotRecognitionSessionController::textEditingChanged, false) ||
                generation != m_translationGeneration || !updateTextState() ||
                generation != m_translationGeneration ||
                !notify(&ScreenshotRecognitionSessionController::reportOverlayTranslationFailure) ||
                generation != m_translationGeneration)
                return false;
        }
        if (status == TextCacheEntry::TranslationStatus::Absent ||
            status == TextCacheEntry::TranslationStatus::Failed)
            return notify(&ScreenshotRecognitionSessionController::startTranslation);
        return true;
    };
    if (entry->overlayTranslation.presentation)
        return publish();

    const QString layout = configuration.layoutProcessing;
    const bool merge = layout != QStringLiteral("original");
    if (const auto cached = entry->overlayLayouts.value(layout)) {
        entry->overlayTranslation.presentation =
            std::make_shared<ScreenshotOcrPresentation>(*cached);
        return publish();
    }

    // Small overlays can appear immediately. Dense OCR is prepared from a worker snapshot.
    constexpr qsizetype synchronousLines = 8;
    constexpr qsizetype synchronousCharacters = 512;
    qsizetype characters = 0;
    if (source->lines.size() <= synchronousLines)
        for (const auto& line : std::as_const(source->lines))
            characters += line.text.size();
    if (!m_translationService)
        return notify(&ScreenshotRecognitionSessionController::startTranslation);
    if (source->lines.size() <= synchronousLines && characters <= synchronousCharacters) {
        const auto prepared = prepareOverlayLayout(*source, merge, font);
        entry->overlayLayouts.insert(layout, prepared);
        entry->overlayTranslation.presentation =
            std::make_shared<ScreenshotOcrPresentation>(*prepared);
        return publish();
    }

    const bool preparing = static_cast<bool>(m_overlayPreparation);
    if (!preparing) {
        entry->translationConfiguration = configuration;
        entry->hasTranslationConfiguration = true;
        entry->overlayTranslation.status = TextCacheEntry::TranslationStatus::Streaming;
        m_overlayPreparation = std::make_shared<std::atomic_bool>(false);
    }
    const auto cancelled = m_overlayPreparation;
    const auto* window = content();
    if (m_active && m_mode == Mode::Text && originalImageTranslationActive() &&
        m_editingKey == key && (m_presentation != source || (window && !window->showsOcrLayer()))) {
        if (!notify(&ScreenshotRecognitionSessionController::applyPresentation, source, QImage{},
                    QRectF{}) ||
            generation != m_translationGeneration ||
            !notify(&ScreenshotRecognitionSessionController::startTextRender) ||
            generation != m_translationGeneration ||
            !notify(&ScreenshotRecognitionSessionController::textEditingChanged, false) ||
            generation != m_translationGeneration)
            return false;
    }
    if (preparing)
        return updateTextState();
    ScreenshotOcrPresentation snapshot;
    snapshot.selection = source->selection;
    snapshot.lines = source->lines;
    const QPointer<ScreenshotRecognitionSessionController> guard(this);
    QThreadPool::globalInstance()->start(snow_shot::runtime::trackRuntimeWork(
        [guard, cancelled, generation, key, snapshot = std::move(snapshot), font, merge, layout,
         publish]() {
            if (cancelled->load(std::memory_order_relaxed))
                return;
            snow_shot::platform::applyApplicationQoSToCurrentThread();
            auto prepared = prepareOverlayLayout(snapshot, merge, font);
            if (cancelled->load(std::memory_order_relaxed))
                return;
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                snow_shot::runtime::trackRuntimeWork([guard, cancelled, generation, key, layout,
                                                      font, prepared = std::move(prepared),
                                                      publish] {
                    if (!guard || cancelled->load(std::memory_order_relaxed) ||
                        guard->m_overlayPreparation != cancelled ||
                        guard->m_translationGeneration != generation)
                        return;
                    guard->m_overlayPreparation.reset();
                    auto entry = guard->m_textCache.find(key);
                    if (entry == guard->m_textCache.end() || key != guard->m_translationKey)
                        return;
                    entry->overlayTranslation.status = TextCacheEntry::TranslationStatus::Absent;
                    if (QApplication::font() != font) {
                        guard->showOverlayTranslation();
                        return;
                    }
                    entry->overlayLayouts.insert(layout, prepared);
                    entry->overlayTranslation.presentation =
                        std::make_shared<ScreenshotOcrPresentation>(*prepared);
                    publish();
                }),
                Qt::QueuedConnection);
        }));
    return updateTextState();
#else
    return true;
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
        const QString detail = it->translationJob ? it->translationJob->errorText() : QString{};
        const QString message = detail.isEmpty()
                                    ? tr("Some text could not be translated")
                                    : tr("Some text could not be translated: %1").arg(detail);
        if (!showStatus(message, true))
            return;
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
        if (!notify(&ScreenshotRecognitionSessionController::reportOverlayTranslationFailure))
            return;
    } else {
        it->translationStatus = TextCacheEntry::TranslationStatus::Failed;
        if (content() != nullptr && m_translating) {
            content()->setTextEditorStreaming(false);
        }
        if (m_translating) {
            if (!showStatus(message.isEmpty() ? tr("Translation failed") : message, true))
                return;
        }
    }
    if (!updateTextState())
        return;
#else
    Q_UNUSED(message)
#endif
}

#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
bool ScreenshotRecognitionSessionController::publishOverlayUpdates(
    const QString& key, ScreenshotOcrPresentation& presentation,
    const QVector<ScreenshotOcrTextUpdate>& updates) {
    if (updates.isEmpty())
        return true;
    for (const auto& update : updates)
        presentation.setLineText(update.index, update.text, update.streaming);
    if (m_active && m_mode == Mode::Text && originalImageTranslationActive() &&
        key == m_editingKey) {
        if (m_actions.updateOcrTexts)
            return notifyUiCallback(m_actions.updateOcrTexts, std::cref(updates));
        if (auto* window = content())
            return notifyCallback([window, &updates] { window->updateOcrTexts(updates); });
    }
    return true;
}

bool ScreenshotRecognitionSessionController::finishOverlayTranslationLines(TranslationJob* job) {
    auto entry = m_textCache.find(m_translationKey);
    if (entry == m_textCache.end() || !entry->jobInImage || !job)
        return true;
    auto& streaming = entry->overlayTranslation.streamingLines;
    bool finishedLine = false;
    for (auto it = streaming.begin(); it != streaming.end();) {
        if (job->units().at(*it).state == TranslationJob::State::Streaming) {
            ++it;
            continue;
        }
        m_pendingTranslationUnits.insert(*it);
        it = streaming.erase(it);
        finishedLine = true;
    }
    return !finishedLine || flushTranslationUpdates();
}
#endif

bool ScreenshotRecognitionSessionController::flushTranslationUpdates() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    m_translationUpdateTimer->stop();
    auto pending = std::exchange(m_pendingTranslationUnits, {});
    const QString key = m_translationKey;
    auto entry = m_textCache.find(key);
    if (pending.isEmpty() || entry == m_textCache.end() || !entry->translationJob)
        return true;
    const auto generation = m_translationGeneration;
    if (!entry->jobInImage) {
        return (!entry->translationSession ||
                invokeOwned(entry->translationSession,
                            &ScreenshotOcrTextEditingSession::appendStreamedText,
                            entry->translationText)) &&
               generation == m_translationGeneration;
    }

    const auto presentation = entry->overlayTranslation.presentation;
    if (!presentation)
        return true;
    const auto& units = entry->translationJob->units();
    QVector<ScreenshotOcrTextUpdate> updates;
    updates.reserve(pending.size());
    for (const int index : std::as_const(pending)) {
        const auto& unit = units.at(index);
        const auto& line = presentation->lines.at(index);
        const bool streaming = unit.state == TranslationJob::State::Streaming;
        if (streaming)
            entry->overlayTranslation.streamingLines.insert(index);
        if (line.text != unit.text || line.translationStreaming != streaming) {
            // An invalidated job clears its payload. Retain any text already presented.
            updates.push_back({index, unit.text.isEmpty() ? line.text : unit.text, streaming});
        }
    }
    return publishOverlayUpdates(key, *presentation, updates) &&
           m_translationGeneration == generation;
#else
    return true;
#endif
}

bool ScreenshotRecognitionSessionController::cancelTranslationRequests() {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (m_overlayPreparation) {
        m_overlayPreparation->store(true, std::memory_order_relaxed);
        m_overlayPreparation.reset();
    }
    ++m_translationGeneration;
    auto it = m_textCache.find(m_translationKey);
    if (it == m_textCache.end()) {
        m_translationUpdateTimer->stop();
        m_pendingTranslationUnits.clear();
        return true;
    }
    if (it->translationJob != nullptr) {
        disconnect(it->translationJob, nullptr, this, nullptr);
        it->translationJob->cancel();
    }
    if (it->translationStatus == TextCacheEntry::TranslationStatus::Streaming)
        it->translationStatus = TextCacheEntry::TranslationStatus::Absent;
    if (it->overlayTranslation.status == TextCacheEntry::TranslationStatus::Streaming)
        it->overlayTranslation.status = TextCacheEntry::TranslationStatus::Failed;
    return finishOverlayTranslationLines(it->translationJob) && flushTranslationUpdates();
#else
    return true;
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
        if (!invokeOwned(it->translationSession,
                         &ScreenshotOcrTextEditingSession::establishBaseline, it->translationText))
            return;
    } else {
        it->translationStatus = TextCacheEntry::TranslationStatus::Failed;
        if (!invokeOwned(it->translationSession, &ScreenshotOcrTextEditingSession::establishHistory,
                         it->translationText))
            return;
        if (!result.cancelled) {
            if (!showStatus(result.error.isEmpty() ? tr("Translation failed") : result.error, true))
                return;
        }
    }
    if (content() != nullptr && m_translating && key == m_editingKey) {
        content()->setTextEditorStreaming(false);
    }
    if (!updateTextState())
        return;
#else
    Q_UNUSED(generation)
    Q_UNUSED(key)
    Q_UNUSED(result)
#endif
}

void ScreenshotRecognitionSessionController::invalidateCurrentTranslation(bool restartIfVisible) {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const QString key = m_translationKey.isEmpty() ? m_textCacheKey : m_translationKey;
    auto it = m_textCache.find(key);
    if (it == m_textCache.end()) {
        return;
    }
    if (!cancelTranslationRequests())
        return;
    it = m_textCache.find(key);
    if (it == m_textCache.end())
        return;
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
        if (!invokeOwned(it->translationSession, &ScreenshotOcrTextEditingSession::establishHistory,
                         QString()))
            return;
    }
    const bool inImage = originalImageTranslationActive();
    if (inImage) {
        if (restartIfVisible) {
            if (!showOverlayTranslation())
                return;
        } else if (!notify(&ScreenshotRecognitionSessionController::applyPresentation,
                           it->presentation, QImage{}, QRectF{}) ||
                   !notify(&ScreenshotRecognitionSessionController::startTextRender)) {
            return;
        }
    }
    if (!updateTextState())
        return;
    if (!notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged))
        return;
    if (restartIfVisible && !inImage) {
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
        if (!notify(&ScreenshotRecognitionSessionController::beginTextEditing))
            return;
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
        if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::setFormatting, value,
                         smartText))
            return;
        if (!updateTextState())
            return;
    }
}

void ScreenshotRecognitionSessionController::applyTextPunctuation(const QString& value) {
    if (originalImageTranslationActive()) {
        return;
    }
    if (!m_editing && !m_translating) {
        if (!notify(&ScreenshotRecognitionSessionController::beginTextEditing))
            return;
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
        if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::setPunctuation, value))
            return;
        if (!updateTextState())
            return;
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
        if (!notifyUiCallback(m_actions.setShowOriginalImage, show))
            return;
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
        const auto& presentation = *entry.overlayTranslation.presentation;
        if (m_pendingTranslationUnits.isEmpty() || key != m_translationKey ||
            !entry.translationJob || !entry.jobInImage)
            return snow_shot::presentation::originalOcrText(presentation);
        QStringList texts;
        texts.reserve(presentation.lines.size());
        for (int index = 0; index < presentation.lines.size(); ++index) {
            const auto& pending = entry.translationJob->units().at(index).text;
            texts.append(m_pendingTranslationUnits.contains(index) && !pending.isEmpty()
                             ? pending
                             : presentation.lines.at(index).text);
        }
        return texts.join(u'\n');
    }
    if (m_translating && !entry.jobInImage && key == m_translationKey &&
        m_pendingTranslationUnits.contains(0))
        return entry.translationText;
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
ScreenshotRecognitionSessionController::fileExportSnapshot() {
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION ||                     \
    SNOW_SHOT_ENABLE_QR_RECOGNITION || SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (!m_active)
        return std::nullopt;
    switch (m_mode) {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    case Mode::Document: {
        ScreenshotRecognitionFileSnapshot snapshot;
        snapshot.kind = ScreenshotRecognitionFileKind::DocumentWord;
        snapshot.document = cachedDocument(m_target.key);
        return snapshot.hasContent() ? std::optional(snapshot) : std::nullopt;
    }
#endif
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
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    case Mode::Table: {
        const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
        if (content())
            content()->commitActiveTableEdit();
        if (!lifetime || !m_active || m_mode != Mode::Table)
            return std::nullopt;
        ScreenshotRecognitionFileSnapshot snapshot;
        snapshot.kind = ScreenshotRecognitionFileKind::Table;
        if (m_tableSession) {
            snapshot.table = m_tableSession->document;
            if (content())
                snapshot.tableColumnWidths = content()->tableColumnWidths();
        }
        return snapshot;
    }
#endif
    case Mode::Text:
    default:
        return std::nullopt;
    }
#else
    return std::nullopt;
#endif
}

#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
ScreenshotDocumentCopyRequest ScreenshotRecognitionSessionController::documentCopyRequest() const {
    if (!m_active || m_mode != Mode::Document || (m_attachmentToken && !ownsContentAttachment()))
        return {};
    auto document = cachedDocument(m_target.key);
    if (!document)
        return {};
    if (snow_shot::storage::ScreenshotSettings().copyImageFileToClipboard())
        return {std::move(document), {}, true};
    return {std::move(document), content() ? content()->documentSelectionHtml() : QString{}};
}

bool ScreenshotRecognitionSessionController::copyDocumentPreview(QString selection) {
    if (!m_active || m_mode != Mode::Document || !ownsContentAttachment())
        return false;
    auto document = cachedDocument(m_target.key);
    if (!document)
        return false;
    m_documentCopyJob.cancel();
    m_documentClipboardCommit.cancel();
    const auto attachment = m_attachmentToken;
    const auto generation = m_documentGeneration;
    const auto publication = ScreenshotClipboardService::reservePublication();
    m_documentCopyPublication = publication;
    m_documentCopyJob =
        ScreenshotDocumentCopyRequest{std::move(document), std::move(selection)}.start(
            this, [this, attachment, generation, publication](auto mime, const QString& error) {
                if (m_documentCopyPublication != publication || m_attachmentToken != attachment ||
                    m_documentGeneration != generation || !ownsContentAttachment())
                    return;
                m_documentCopyJob = {};
                if (!mime) {
                    static_cast<void>(showStatus(error, true));
                    return;
                }
                m_documentClipboardCommit = ScreenshotClipboardService::commitMimeData(
                    QApplication::clipboard(), this, mime.release(), publication, [](auto) {});
            });
    return m_documentCopyJob.isValid();
}
#endif

std::unique_ptr<QMimeData> ScreenshotRecognitionSessionController::recognitionClipboardMimeData(
    const ScreenshotOcrPresentation* displayedPresentation) {
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const QPointer<const ScreenshotRecognitionSessionController> lifetime(this);
    const auto transition = m_transitionGeneration;
    if (!flushTranslationUpdates() || !lifetime || transition != m_transitionGeneration)
        return {};
#endif
    if (m_attachmentToken && !ownsContentAttachment())
        return {};
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (conversionModeActive()) {
        return snow_shot::presentation::imageConversionClipboardMimeData(m_conversion->format(),
                                                                         m_conversion->source());
    }
#endif
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    if (m_mode == Mode::Document)
        return documentCopyRequest().mimeData();
#endif
    // Result widgets own their text and table selections. Resolve those before falling
    // back to complete cached results; conversion Copy above intentionally exports source.
    if (m_active && !originalImageTranslationActive() && content()) {
        if (auto selected = content()->textSelectionClipboardMimeData())
            return selected;
    }
    auto mimeData = std::make_unique<QMimeData>();
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
    if (translating()) {
        const auto entry = m_textCache.constFind(m_translationKey);
        if (entry == m_textCache.cend())
            return {};
        const auto* presentation = displayedPresentation != nullptr
                                       ? displayedPresentation
                                       : (entry->overlayTranslation.presentation
                                              ? entry->overlayTranslation.presentation.get()
                                              : entry->presentation.get());
        if (originalImageTranslationActive() && presentation && presentation->hasTextSelection()) {
            mimeData->setText(presentation->selectedText());
            return mimeData;
        }
        if (snow_shot::storage::ScreenshotTranslationSettings().copyTranslationResultsAsImage()) {
            const QImage image =
                originalImageTranslationActive()
                    ? (m_actions.renderTranslationImage ? m_actions.renderTranslationImage()
                                                        : QImage{})
                    : (content() ? content()->translationDocumentImage() : QImage{});
            if (!lifetime || transition != m_transitionGeneration ||
                (m_attachmentToken && !ownsContentAttachment()) || image.isNull())
                return {};
            mimeData->setImageData(image);
            return mimeData;
        }
    }
    if (originalImageTranslationActive()) {
        const auto entry = m_textCache.value(m_translationKey);
        const auto* presentation = displayedPresentation != nullptr
                                       ? displayedPresentation
                                       : (entry.overlayTranslation.presentation
                                              ? entry.overlayTranslation.presentation.get()
                                              : entry.presentation.get());
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
            if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::replaceText, text))
                return;
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
        if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::replaceText, text))
            return;
    }
}

void ScreenshotRecognitionSessionController::startTextRecognition(
    ScreenshotOcrRequestPriority priority) {
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    if (!hasTarget() || m_recognition == nullptr || m_textRequestToken != 0 ||
        !screenshotOcrImageWithinPixelLimit(m_target.image.size())) {
        if (m_active && hasTarget() && !screenshotOcrImageWithinPixelLimit(m_target.image.size())) {
            if (!showStatus(tr("Text recognition is unavailable for screenshots larger than 4K"),
                            false))
                return;
        }
        return;
    }
    const quint64 generation = ++m_textGeneration;
    const QString key = m_target.key;
    m_textModelDownloadShown = false;
    m_textModelDownloadInProgress = !m_recognition->modelFilesReady();
    if (m_textModelDownloadInProgress && textModelDownloading()) {
        if (!notify(&ScreenshotRecognitionSessionController::showModelDownloadMessage))
            return;
    } else if (m_active) {
        if (!showRecognitionMessage())
            return;
    }
    ScreenshotOcrRequest request;
    request.image = m_target.image;
    request.canvasRect = m_target.canvasRect;
    request.priority = priority;
    request.renderFilteredImage =
        m_active && m_mode == Mode::Text && shouldRenderRecognitionInWorker();
    request.backgroundFill = request.renderFilteredImage && backgroundFillEnabled();
    if (!lifetime)
        return;
    if (request.renderFilteredImage && m_actions.ocrBackgroundColor) {
        const auto backgroundColor = m_actions.ocrBackgroundColor;
        request.backgroundColor = backgroundColor();
        if (!lifetime)
            return;
    }
    const auto callbackCompleted = std::make_shared<bool>(false);
    const auto token = m_recognition->recognize(
        std::move(request), this,
        [this, generation, key, callbackCompleted](ScreenshotOcrRecognitionResult output) {
            *callbackCompleted = true;
            if (generation == m_textGeneration) {
                m_textRequestToken = 0;
            }
            if (!notify(&ScreenshotRecognitionSessionController::handleTextOutput, generation, key,
                        std::move(output)))
                return;
        });
    if (!lifetime)
        return;
    m_textRequestToken = token;
    if (*callbackCompleted) {
        m_textRequestToken = 0;
    }
    if (!updateBusyState())
        return;
    if (m_textRequestToken == 0 && !*callbackCompleted) {
        if (m_active || m_textModelDownloadShown) {
            if (!showStatus(tr("Text recognition request could not be prepared"), true))
                return;
        }
        if (!notify(&ScreenshotRecognitionSessionController::hideModelDownloadMessage))
            return;
        m_textModelDownloadInProgress = false;
        if (!hideRecognitionMessage())
            return;
    } else if (m_textModelDownloadInProgress) {
        pollTextModelDownload(generation);
    }
}

void ScreenshotRecognitionSessionController::renderTextBackground() {
    if (m_active && m_mode == Mode::Text) {
        if (!notify(&ScreenshotRecognitionSessionController::startTextRender))
            return;
    }
}

void ScreenshotRecognitionSessionController::startTextRender() {
    if (!shouldRenderTextBackground())
        return;
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    if (!m_active || m_mode != Mode::Text || !hasTarget() || m_recognition == nullptr ||
        editing() || m_presentation == nullptr ||
        (!m_actions.applyOcrBackgroundImage && !m_actions.applyOcrPresentation && !content())) {
        return;
    }
    if (m_textRenderRequestToken != 0) {
        m_recognition->cancel(m_textRenderRequestToken);
        m_textRenderRequestToken = 0;
    }
    const quint64 generation = ++m_textRenderGeneration;
    const QString key = m_target.key;
    const auto presentation = m_presentation;
    ScreenshotOcrRequest request;
    request.image = m_target.image;
    request.canvasRect = m_target.canvasRect;
    request.priority = ScreenshotOcrRequestPriority::Interactive;
    request.presentation = presentation;
    request.renderFilteredImage = static_cast<bool>(m_actions.applyOcrBackgroundImage);
    request.prepareVisualColors = true;
    request.backgroundFill = backgroundFillEnabled();
    const auto backgroundColor = m_actions.ocrBackgroundColor;
    request.backgroundColor = backgroundColor ? backgroundColor() : QColor();
    if (!lifetime)
        return;
    if (m_actions.prepareOcrRenderRequest) {
        const auto prepare = m_actions.prepareOcrRenderRequest;
        prepare(request);
        if (!lifetime)
            return;
    }
    if (generation != m_textRenderGeneration || presentation != m_presentation || !m_recognition)
        return;
    // Streaming text and selection stay on the GUI thread. A transformed host still samples
    // colors in the original image's pixel space.
    if (request.presentation != presentation ||
        request.image.cacheKey() != m_target.image.cacheKey() ||
        request.canvasRect != m_target.canvasRect) {
        request.colorSource =
            ScreenshotOcrColorSource{m_target.image, m_target.canvasRect,
                                     std::make_shared<ScreenshotOcrPresentation>(*presentation)};
    }
    if (request.presentation)
        request.presentation = std::make_shared<ScreenshotOcrPresentation>(*request.presentation);
    const auto callbackCompleted = std::make_shared<bool>(false);
    const auto token = m_recognition->render(
        std::move(request), this,
        [this, generation, key, presentation,
         callbackCompleted](ScreenshotOcrRecognitionResult output) {
            *callbackCompleted = true;
            if (generation == m_textRenderGeneration) {
                m_textRenderRequestToken = 0;
            }
            const QPointer<ScreenshotRecognitionSessionController> guard(this);
            const bool current = generation == m_textRenderGeneration && key == m_target.key &&
                                 m_presentation == presentation && m_active &&
                                 m_mode == Mode::Text && !editing();
            if (!current || !shouldRenderTextBackground() || !guard ||
                generation != m_textRenderGeneration || m_presentation != presentation) {
                if (!guard)
                    return;
                if (!updateBusyState())
                    return;
                return;
            }
            if (output.presentation && presentation->applyPreparedColors(*output.presentation)) {
                if (m_actions.applyOcrColors) {
                    if (!notifyUiCallback(m_actions.applyOcrColors, std::cref(*presentation)))
                        return;
                } else if (auto* window = content()) {
                    window->updateOcrColors();
                }
                if (!guard || generation != m_textRenderGeneration ||
                    m_presentation != presentation)
                    return;
            }
            if (!output.error.isEmpty() || output.filteredImage.isNull()) {
                if (!updateBusyState())
                    return;
                if (!hideRecognitionMessage())
                    return;
                return;
            }
            if (m_actions.applyOcrBackgroundImage) {
                if (!notifyUiCallback(m_actions.applyOcrBackgroundImage, m_presentation,
                                      std::move(output.filteredImage),
                                      output.filteredImageCanvasRect))
                    return;
            }
            if (!updateBusyState())
                return;
            hideRecognitionMessage();
        });
    if (!lifetime)
        return;
    m_textRenderRequestToken = token;
    if (*callbackCompleted) {
        m_textRenderRequestToken = 0;
    }
    if (!updateBusyState())
        return;
    notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged);
}

void ScreenshotRecognitionSessionController::startDocumentRecognition() {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    if (!hasTarget() || busy(Mode::Document))
        return;
    if (!m_tableRecognition) {
        static_cast<void>(showStatus(tr("Document recognition service is unavailable"), true));
        return;
    }
    const auto generation = ++m_documentGeneration;
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_documentCancellation = cancellation;
    const auto key = m_target.key;
    const auto original = m_target.image;
    const QPointer<ScreenshotRecognitionSessionController> guard(this);
    if (!showRecognitionMessage() || !guard)
        return;
    m_documentRequestToken = m_tableRecognition->extractDocument(
        original, this,
        [this, generation, key, original, cancellation](SnowShotDocumentResult result) {
            if (generation != m_documentGeneration || key != m_target.key)
                return;
            m_documentRequestToken = 0;
            if (!result.succeeded()) {
                m_documentCancellation.reset();
                m_workflowError = result.error;
                if (!hideRecognitionMessage() || !updateBusyState())
                    return;
                static_cast<void>(showStatus(result.error, true));
                return;
            }
            m_documentParsing = true;
            const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
            auto* application = QCoreApplication::instance();
            auto* uiThread = thread();
            QThreadPool::globalInstance()->start(snow_shot::runtime::trackRuntimeWork(
                [application, uiThread, lifetime, generation, key, original, cancellation,
                 raw = std::move(result.raw)] {
                    const auto cancelled = [&] {
                        return cancellation->load(std::memory_order_relaxed);
                    };
                    if (cancelled())
                        return;
                    QString error;
                    auto document =
                        ScreenshotRecognizedDocument::parse(raw, original, &error, cancelled);
                    if (cancelled())
                        return;
                    auto preview = document ? prepareDocumentPreview(*document, uiThread,
                                                                     FormulaFallback::Placeholder)
                                            : nullptr;
                    if (!cancelled())
                        QMetaObject::invokeMethod(
                            application,
                            [lifetime, generation, key, document, preview, error] {
                                if (!lifetime || generation != lifetime->m_documentGeneration ||
                                    key != lifetime->m_target.key)
                                    return;
                                if (!document) {
                                    lifetime->m_documentCancellation.reset();
                                    lifetime->m_documentParsing = false;
                                    lifetime->m_workflowError = error;
                                    if (!lifetime->hideRecognitionMessage() || !lifetime ||
                                        !lifetime->updateBusyState() || !lifetime)
                                        return;
                                    static_cast<void>(lifetime->showStatus(error, true));
                                    return;
                                }
                                lifetime->prepareDocument(generation, key, document, preview);
                            },
                            Qt::QueuedConnection);
                }));
        });
    static_cast<void>(updateBusyState());
#endif
}

void ScreenshotRecognitionSessionController::prepareDocument(
    quint64 generation, const QString& key, std::shared_ptr<ScreenshotRecognizedDocument> document,
    std::shared_ptr<QTextDocument> preview) {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    const QPointer<ScreenshotRecognitionSessionController> guard(this);
    const auto current = [guard, generation, key] {
        return guard && guard->m_documentGeneration == generation && guard->m_target.key == key;
    };
    if (preview && content()) {
        auto firstFrame = std::make_shared<ScreenshotRecognizedDocument>(*document);
        if (!notify(&ScreenshotRecognitionSessionController::hideRecognitionMessage) || !current())
            return;
        if (auto* view = content())
            view->showDocument(std::move(firstFrame), preview);
        if (!current())
            return;
        updateOriginalImagePreview();
        if (!current())
            return;
    }
    if (document->formulas.isEmpty()) {
        finishDocumentPreparation(generation, key, std::move(document), std::move(preview));
        return;
    }
    auto batch = std::make_shared<DocumentFormulaBatch>();
    for (const auto& image : std::as_const(document->images))
        batch->imageBytes += image.sizeInBytes();
    for (const auto& formula : document->formulas) {
        auto& ids = batch->idsBySource[formula.source];
        if (ids.isEmpty())
            batch->sources.append(formula.source);
        ids.append(formula.id);
    }
    auto* renderer = new ScreenshotLatexRenderer(this);
    m_documentPreparation = renderer;
    const auto requestNext = [renderer, batch] {
        static_cast<void>(
            renderer->request(batch->sources[batch->next], QSize(1200, 800), 2, Qt::black));
    };
    connect(renderer, &ScreenshotLatexRenderer::rendered, this,
            [this, renderer, generation, key, document, preview, current, batch,
             requestNext](quint64, const QImage& image, ScreenshotLatexRenderer::Error) {
                if (!current())
                    return;
                const auto& ids = batch->idsBySource[batch->sources[batch->next]];
                const auto bytes = image.sizeInBytes() * ids.size();
                if (!image.isNull() &&
                    bytes <= ScreenshotRecognizedDocument::maximumImageBytes - batch->imageBytes) {
                    for (const auto& id : ids)
                        document->images.insert(id, image);
                    batch->imageBytes += bytes;
                }
                if (++batch->next < batch->sources.size()) {
                    requestNext();
                    return;
                }
                m_documentPreparation = nullptr;
                renderer->deleteLater();
                finishDocumentPreparation(generation, key, document, preview);
            });
    requestNext();
#else
    Q_UNUSED(generation)
    Q_UNUSED(key)
    Q_UNUSED(document)
    Q_UNUSED(preview)
#endif
}

void ScreenshotRecognitionSessionController::finishDocumentPreparation(
    quint64 generation, const QString& key, std::shared_ptr<ScreenshotRecognizedDocument> document,
    std::shared_ptr<QTextDocument> preview) {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    const QPointer<ScreenshotRecognitionSessionController> guard(this);
    const auto cancellation = m_documentCancellation;
    auto* application = QCoreApplication::instance();
    QThreadPool::globalInstance()->start(snow_shot::runtime::trackRuntimeWork(
        [application, guard, generation, key, document = std::move(document),
         preview = std::move(preview), cancellation] {
            const auto cancelled = [&] { return cancellation->load(std::memory_order_relaxed); };
            if (cancelled())
                return;
            document->preparedClipboard = document->clipboardContent(cancelled);
            if (cancelled())
                return;
            QMetaObject::invokeMethod(
                application,
                [guard, generation, key, document, preview] {
                    if (!guard || generation != guard->m_documentGeneration ||
                        key != guard->m_target.key)
                        return;
                    guard->m_documentCancellation.reset();
                    guard->m_documentParsing = false;
                    if (preview && !document->formulas.isEmpty()) {
                        // Keep the same preview document and selection while adding the images.
                        preview->setLayoutEnabled(false);
                        document->updateTextDocumentImages(*preview, FormulaFallback::Source);
                        preview->setLayoutEnabled(true);
                        if (!guard || generation != guard->m_documentGeneration)
                            return;
                    }
                    guard->cacheDocument(key, document);
                    guard->m_documentPreview = preview;
                    guard->m_documentPreviewKey = key;
                    if (!guard || generation != guard->m_documentGeneration ||
                        !guard->hideRecognitionMessage() || !guard ||
                        generation != guard->m_documentGeneration)
                        return;
                    if (guard->m_active && guard->m_mode == Mode::Document)
                        guard->applyDocument();
                    if (guard && generation == guard->m_documentGeneration &&
                        guard->notify(
                            &ScreenshotRecognitionSessionController::recognitionResultsChanged))
                        static_cast<void>(guard->updateBusyState());
                },
                Qt::QueuedConnection);
        }));
#else
    Q_UNUSED(generation)
    Q_UNUSED(key)
    Q_UNUSED(document)
    Q_UNUSED(preview)
#endif
}

#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
void ScreenshotRecognitionSessionController::cacheDocument(
    const QString& key, std::shared_ptr<const ScreenshotRecognizedDocument> document) {
    const auto cost = document->retainedBytes();
    if (key == m_documentPreviewKey && cachedDocument(key) != document) {
        m_documentPreview.reset();
        m_documentPreviewKey.clear();
    }
    m_documents.remove(key);
    m_documents.insert(key,
                       new std::shared_ptr<const ScreenshotRecognizedDocument>(std::move(document)),
                       static_cast<qsizetype>(cost));
}

std::shared_ptr<const ScreenshotRecognizedDocument>
ScreenshotRecognitionSessionController::cachedDocument(const QString& key) const {
    const auto* cached = m_documents.object(key);
    return cached ? *cached : nullptr;
}
#else
void ScreenshotRecognitionSessionController::cacheDocument(
    const QString&, std::shared_ptr<const ScreenshotRecognizedDocument>) {}

std::shared_ptr<const ScreenshotRecognizedDocument>
ScreenshotRecognitionSessionController::cachedDocument(const QString&) const {
    return {};
}
#endif

void ScreenshotRecognitionSessionController::applyDocument() {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    const auto document = cachedDocument(m_target.key);
    if (!document || !notify(&ScreenshotRecognitionSessionController::ensureContent))
        return;
    const QPointer<ScreenshotRecognitionSessionController> guard(this);
    if (content())
        showDocumentPreview(document);
    if (guard)
        updateOriginalImagePreview();
#endif
}

void ScreenshotRecognitionSessionController::showDocumentPreview(
    std::shared_ptr<const ScreenshotRecognizedDocument> document) {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    if (m_documentPreview && m_documentPreviewKey == m_target.key) {
        content()->showDocument(std::move(document), m_documentPreview);
        return;
    }
    // Small cached results can be shown immediately. Import large cached results on a worker.
    if (document->html.size() <= 32 * 1024 && document->images.size() <= 8) {
        m_documentPreview = document->createTextDocument(false, FormulaFallback::Source);
        m_documentPreviewKey = m_target.key;
        content()->showDocument(std::move(document), m_documentPreview);
        return;
    }
    if (m_documentParsing)
        return;
    m_documentParsing = true;
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    m_documentCancellation = cancellation;
    const auto key = m_target.key;
    const auto generation = m_documentGeneration;
    const QPointer<ScreenshotRecognitionSessionController> guard(this);
    auto* application = QCoreApplication::instance();
    auto* uiThread = thread();
    QThreadPool::globalInstance()->start(
        snow_shot::runtime::trackRuntimeWork([application, uiThread, guard, generation, key,
                                              cancellation, document = std::move(document)] {
            if (cancellation->load(std::memory_order_relaxed))
                return;
            auto preview = prepareDocumentPreview(*document, uiThread, FormulaFallback::Source);
            if (cancellation->load(std::memory_order_relaxed))
                return;
            QMetaObject::invokeMethod(
                application,
                [guard, generation, key, document, preview] {
                    if (!guard || generation != guard->m_documentGeneration ||
                        guard->m_target.key != key)
                        return;
                    guard->m_documentParsing = false;
                    guard->m_documentCancellation.reset();
                    if (guard->cachedDocument(key) != document) {
                        static_cast<void>(guard->updateBusyState());
                        return;
                    }
                    guard->m_documentPreviewKey = key;
                    guard->m_documentPreview = preview;
                    if (preview && guard->m_active && guard->m_mode == Mode::Document)
                        guard->applyDocument();
                    if (guard && generation == guard->m_documentGeneration)
                        static_cast<void>(guard->updateBusyState());
                },
                Qt::QueuedConnection);
        }));
#else
    Q_UNUSED(document)
#endif
}

bool ScreenshotRecognitionSessionController::openDocumentFile(std::function<void()> opened) {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    if (!m_active || m_mode != Mode::Document || busy(Mode::Document) || m_openingDocument)
        return false;
    auto snapshot = fileExportSnapshot();
    if (!snapshot || !snapshot->hasContent())
        return false;
    const QPointer<ScreenshotRecognitionSessionController> guard(this);
    const auto key = m_target.key;
    const auto generation = m_documentGeneration;
    m_openingDocument = true;
    if (!updateBusyState())
        return false;
    QThreadPool::globalInstance()->start([guard, key, generation, snapshot = *snapshot,
                                          opened = std::move(opened)] {
        auto directory = std::make_shared<QTemporaryDir>(
            QDir::tempPath() + QStringLiteral("/SnowShot-document-XXXXXX"));
        const auto result =
            directory->isValid()
                ? ScreenshotRecognitionFileExport::saveToPath(
                      snapshot, directory->filePath(QStringLiteral("Document.docx")))
                : ScreenshotRecognitionFileSaveResult{
                      {},
                      QCoreApplication::translate("ScreenshotRecognitionFileExport",
                                                  "The temporary directory could not be created")};
        if (!guard)
            return;
        QMetaObject::invokeMethod(
            guard,
            [guard, key, generation, directory, result, opened] {
                if (!guard || generation != guard->m_documentGeneration)
                    return;
                guard->m_openingDocument = false;
                if (!guard->updateBusyState() || !guard->m_active ||
                    guard->m_mode != Mode::Document || guard->m_target.key != key)
                    return;
                if (!result.succeeded()) {
                    static_cast<void>(guard->showStatus(result.error, true));
                    return;
                }
                if (!QDesktopServices::openUrl(QUrl::fromLocalFile(result.path))) {
                    if (guard)
                        static_cast<void>(
                            guard->showStatus(tr("The document file could not be opened"), true));
                    return;
                }
                directory->setAutoRemove(false);
                if (guard && opened)
                    opened();
            },
            Qt::QueuedConnection);
    });
    return true;
#else
    Q_UNUSED(opened)
    return false;
#endif
}

void ScreenshotRecognitionSessionController::startTableRecognition() {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (!hasTarget() || m_tableRequestToken || m_tableParsing ||
        (m_tableConversion && m_tableConversion->busy()))
        return;
    if (!m_tableRecognition) {
        if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                    tr("Table recognition service is unavailable")))
            return;
        return;
    }
    const QString model = resolvedRecognitionModel(Mode::Table);
    if (model == screenshotDedicatedRecognitionModelId() &&
        !screenshotOcrImageWithinPixelLimit(m_target.image.size())) {
        if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                    tr("Table recognition is unavailable for screenshots larger than 4K")))
            return;
        return;
    }
    const quint64 generation = ++m_tableGeneration;
    const QString key = recognitionCacheKey(Mode::Table, model);
    if (!showRecognitionMessage())
        return;
    if (content())
        content()->clearRecognitionError();
    if (model != screenshotDedicatedRecognitionModelId()) {
        if (!m_tableConversion)
            m_tableConversion = new ScreenshotTableConversionController(this);
        disconnect(m_tableConversion, nullptr, this, nullptr);
        connect(m_tableConversion, &ScreenshotTableConversionController::changed, this,
                [this, generation, key] {
                    if (generation != m_tableGeneration ||
                        key != recognitionCacheKey(Mode::Table) || !m_active ||
                        m_mode != Mode::Table)
                        return;
                    if (!notify(&ScreenshotRecognitionSessionController::ensureContent))
                        return;
                    const QPointer<ScreenshotRecognitionSessionController> guard(this);
                    const auto preview = m_tableConversion->preview();
                    if (content())
                        content()->showTablePreview(preview);
                    if (!guard)
                        return;
                    updateBusyState();
                });
        connect(m_tableConversion, &ScreenshotTableConversionController::completed, this,
                [this, generation, key](ScreenshotTableDocument document, const QString& error) {
                    if (generation != m_tableGeneration || key != recognitionCacheKey(Mode::Table))
                        return;
                    SnowShotTableResult result;
                    result.error = error;
                    if (error.isEmpty())
                        result.html = document.toHtml();
                    handleTableOutput(
                        generation, key, std::move(result),
                        std::make_shared<ScreenshotTableDocument>(std::move(document)));
                });
        m_tableConversion->start(m_tableRecognition, m_target.image, model);
        return;
    }
    const auto callbackCompleted = std::make_shared<bool>(false);
    const auto completion = [this, generation, key, callbackCompleted](SnowShotTableResult result) {
        *callbackCompleted = true;
        if (generation == m_tableGeneration)
            m_tableRequestToken = 0;
        handleTableOutput(generation, key, std::move(result));
    };
    m_tableRequestToken = m_tableRecognition->extractTable(m_target.image, this, completion);
    if (*callbackCompleted)
        m_tableRequestToken = 0;
    if (!updateBusyState())
        return;
    if (!m_tableRequestToken && !*callbackCompleted)
        if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                    tr("Table recognition request could not be prepared")))
            return;
#endif
}

void ScreenshotRecognitionSessionController::startLatexRecognition() {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    if (!hasTarget() || m_latexRequestToken)
        return;
    if (!m_tableRecognition) {
        if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                    tr("LaTeX recognition service is unavailable")))
            return;
        return;
    }
    const QString model = resolvedRecognitionModel(Mode::Latex);
    if (model == screenshotDedicatedRecognitionModelId() &&
        !screenshotOcrImageWithinPixelLimit(m_target.image.size())) {
        if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                    tr("LaTeX recognition is unavailable for screenshots larger than 4K")))
            return;
        return;
    }
    const quint64 generation = ++m_latexGeneration;
    const QString key = recognitionCacheKey(Mode::Latex, model);
    if (!showRecognitionMessage())
        return;
    if (content())
        content()->clearRecognitionError();
    const auto callbackCompleted = std::make_shared<bool>(false);
    const auto completion = [this, generation, key, callbackCompleted](SnowShotLatexResult result) {
        *callbackCompleted = true;
        if (generation == m_latexGeneration)
            m_latexRequestToken = 0;
        handleLatexOutput(generation, key, std::move(result));
    };
    if (model != screenshotDedicatedRecognitionModelId()) {
        m_latexStream = std::make_shared<LatexStreamState>();
        m_latexStream->key = key;
        m_latexStream->generation = generation;
    }
    const auto token = model == screenshotDedicatedRecognitionModelId()
                           ? m_tableRecognition->extractLatex(m_target.image, this, completion)
                           : m_tableRecognition->extractLatexVision(
                                 m_target.image, model, this, completion,
                                 [this, generation, key](const QString& chunk) {
                                     if (!m_latexStream || generation != m_latexGeneration ||
                                         key != recognitionCacheKey(Mode::Latex) || !m_active ||
                                         m_mode != Mode::Latex)
                                         return;
                                     m_latexStream->rawSource += chunk;
                                     if (!m_latexStreamTimer->isActive())
                                         m_latexStreamTimer->start();
                                 });
    if (!lifetime)
        return;
    m_latexRequestToken = token;
    if (*callbackCompleted)
        m_latexRequestToken = 0;
    if (!updateBusyState())
        return;
    if (!m_latexRequestToken && !*callbackCompleted)
        if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                    tr("LaTeX recognition request could not be prepared")))
            return;
#endif
}

#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
void ScreenshotRecognitionSessionController::clearLatexStream() {
    m_latexStreamTimer->stop();
    m_latexStream.reset();
}

void ScreenshotRecognitionSessionController::flushLatexStream() {
    const auto state = m_latexStream;
    if (!state || state->generation != m_latexGeneration ||
        state->key != recognitionCacheKey(Mode::Latex) || !m_active || m_mode != Mode::Latex)
        return;
    const QString source = streamingLatexSource(state->rawSource);
    if (source == state->source)
        return;
    state->source = source;
    const bool firstUpdate = !state->document;
    if (firstUpdate) {
        state->document = std::make_shared<QTextDocument>();
        state->document->setUndoRedoEnabled(false);
    }
    // Match QTextDocument's paragraph and plain-text mapping while retaining exact source
    // separately for formula rendering and selection copying.
    QString display = source;
    display.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    display.replace(u'\r', u'\n');
    display.replace(QChar::ParagraphSeparator, u'\n');
    display.replace(QChar::LineSeparator, u'\n');
    display.replace(QChar::Nbsp, u' ');
    const QString previous = state->document->toPlainText();
    qsizetype prefix = 0;
    while (prefix < previous.size() && prefix < display.size() &&
           previous.at(prefix) == display.at(prefix))
        ++prefix;
    if (prefix > 0 && prefix < display.size() && display.at(prefix).isLowSurrogate())
        --prefix;
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    QTextCursor cursor(state->document.get());
    cursor.setPosition(static_cast<int>(prefix));
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cursor.insertText(display.mid(prefix));
    if (!lifetime || m_latexStream != state)
        return;
    if (firstUpdate) {
        if (!notify(&ScreenshotRecognitionSessionController::ensureContent))
            return;
        if (m_latexStream != state)
            return;
        if (content()) {
            content()->showLatexEditor(state->document, [state] { return state->source; }, true);
            if (!lifetime)
                return;
        }
        notify(&ScreenshotRecognitionSessionController::updateOriginalImagePreview);
    }
}
#endif

void ScreenshotRecognitionSessionController::startQrRecognition() {
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    if (!hasTarget() || m_qrRecognition == nullptr || m_qrRequestToken != 0 ||
        !screenshotOcrImageWithinPixelLimit(m_target.image.size())) {
        if (m_qrRecognition == nullptr) {
            if (!showStatus(tr("Barcode recognition service is unavailable"), true))
                return;
        }
        return;
    }
    const quint64 generation = ++m_qrGeneration;
    const QString key = m_target.key;
    if (!showRecognitionMessage())
        return;
    const auto callbackCompleted = std::make_shared<bool>(false);
    const auto token = m_qrRecognition->recognize(
        m_target.image, this,
        [this, generation, key, callbackCompleted](ScreenshotQrRecognitionResult result) {
            *callbackCompleted = true;
            if (generation == m_qrGeneration) {
                m_qrRequestToken = 0;
            }
            handleQrOutput(generation, key, std::move(result));
        });
    if (!lifetime)
        return;
    m_qrRequestToken = token;
    if (*callbackCompleted) {
        m_qrRequestToken = 0;
    }
    if (!updateBusyState())
        return;
    if (m_qrRequestToken == 0 && !*callbackCompleted) {
        if (!showStatus(tr("Barcode recognition request could not be prepared"), true))
            return;
        if (!hideRecognitionMessage())
            return;
    }
#endif
}

void ScreenshotRecognitionSessionController::handleTextOutput(
    quint64 generation, const QString& key, ScreenshotOcrRecognitionResult output) {
    if (generation != m_textGeneration || key != m_target.key) {
        return;
    }
    const bool modelDownloadWasShown = m_textModelDownloadShown;
    if (!notify(&ScreenshotRecognitionSessionController::hideModelDownloadMessage))
        return;
    m_textModelDownloadInProgress = false;
    if (!output.error.isEmpty() || output.presentation == nullptr) {
        if ((m_active && m_mode == Mode::Text) || modelDownloadWasShown) {
            if (!showStatus(output.error.isEmpty() ? tr("Text recognition failed") : output.error,
                            true))
                return;
        }
        if (!hideRecognitionMessage())
            return;
        if (!updateBusyState())
            return;
        return;
    }
    const QString original = snow_shot::presentation::originalOcrText(*output.presentation);
    auto editingSession = std::make_shared<ScreenshotOcrTextEditingSession>(original);
    connect(editingSession->document(), &QTextDocument::contentsChanged, this,
            [this, key]() { handleTextDocumentChanged(key); });
    QImage filteredImage = std::move(output.filteredImage);
    if (backgroundFillEnabled() != output.presentation->solidBackgroundFill) {
        // An already-running render may complete after the fill preference changes.
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
        if (!notify(&ScreenshotRecognitionSessionController::applyPresentation, m_presentation,
                    filteredImage, filteredImageCanvasRect))
            return;
        if (filteredImage.isNull()) {
            if (!notify(&ScreenshotRecognitionSessionController::startTextRender))
                return;
        }
        if (!notify(&ScreenshotRecognitionSessionController::textResultChanged, true))
            return;
    }
    if (!hideRecognitionMessage())
        return;
    if (!updateBusyState())
        return;
    if (!updateTextState())
        return;
    notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged);
}

void ScreenshotRecognitionSessionController::handleTableOutput(
    quint64 generation, const QString& key, SnowShotTableResult result,
    std::shared_ptr<ScreenshotTableDocument> prepared) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (generation != m_tableGeneration || key != recognitionCacheKey(Mode::Table))
        return;
    if (!result.succeeded()) {
        if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                    result.error.isEmpty() ? tr("Table recognition failed") : result.error))
            return;
        return;
    }
    ScreenshotTableRecognitionEntry entry;
    entry.model = resolvedRecognitionModel(Mode::Table);
    entry.modelFingerprint = m_tableRecognition->modelFingerprint(entry.model);
    entry.result = std::move(result);
    materializeTableEntry(generation, key, std::move(entry), false, std::move(prepared));
#else
    Q_UNUSED(generation)
    Q_UNUSED(key)
    Q_UNUSED(result)
    Q_UNUSED(prepared)
#endif
}

void ScreenshotRecognitionSessionController::materializeTableEntry(
    quint64 generation, const QString& key, ScreenshotTableRecognitionEntry entry,
    bool allowSynchronous, std::shared_ptr<ScreenshotTableDocument> prepared) {
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
            if (!guard->notify(
                    &ScreenshotRecognitionSessionController::failModelRecognition,
                    tr("Recognition service changed. Retry to use the updated settings")))
                return;
            return;
        }
        if (documents.first.empty()) {
            guard->m_tableEntries.remove(key);
            guard->m_tableResults.remove(key);
            guard->m_tableEntryTargets.remove(key);
            guard->m_tableCache.remove(key);
            if (!guard->notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                               tr("No table cells were recognized")))
                return;
            if (!guard->notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged))
                return;
            return;
        }
        // QUndoStack belongs to the GUI thread; workers return only value documents.
        auto session = std::make_shared<ScreenshotTableEditingSession>(std::move(documents.first));
        if (documents.second)
            session->document = std::move(*documents.second);
        guard->m_tableResults.insert(key, entry.result);
        guard->m_tableEntries.insert(key, std::move(entry));
        guard->m_tableEntryTargets.insert(key, target);
        guard->m_tableCache.insert(key, {session, session->document.contentCheckpoint()});
        if (guard->m_active && guard->m_mode == Mode::Table) {
            guard->m_tableCacheKey = key;
            if (!guard->notify(&ScreenshotRecognitionSessionController::applyTableSession, session))
                return;
        }
        if (!guard->hideRecognitionMessage())
            return;
        if (!guard->updateBusyState())
            return;
        if (!guard->updateTextState())
            return;
        guard->notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged);
    };
    if (prepared) {
        complete({std::move(*prepared), std::nullopt});
        return;
    }
    constexpr qsizetype synchronousSourceBytes = 32 * 1024;
    const qsizetype sourceBytes =
        (entry.result.html.size() + (entry.draftHtml ? entry.draftHtml->size() : 0)) *
        static_cast<qsizetype>(sizeof(QChar));
    if (allowSynchronous && sourceBytes <= synchronousSourceBytes) {
        complete(parse());
        return;
    }
    if (!showRecognitionMessage())
        return;
    if (!updateBusyState())
        return;
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
    Q_UNUSED(prepared)
#endif
}

void ScreenshotRecognitionSessionController::handleLatexOutput(quint64 generation,
                                                               const QString& key,
                                                               SnowShotLatexResult result) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (generation != m_latexGeneration || key != recognitionCacheKey(Mode::Latex))
        return;
    if (result.succeeded()) {
        if (!notify(&ScreenshotRecognitionSessionController::flushLatexStream))
            return;
        if (generation != m_latexGeneration || key != recognitionCacheKey(Mode::Latex))
            return;
        clearLatexStream();
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
            if (!notify(&ScreenshotRecognitionSessionController::applyLatexContents, result.latex))
                return;
        if (!notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged))
            return;
    } else if (m_active && m_mode == Mode::Latex) {
        if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition,
                    result.error.isEmpty() ? tr("LaTeX recognition failed") : result.error))
            return;
    }
    if (!hideRecognitionMessage())
        return;
    if (!updateBusyState())
        return;
#else
    Q_UNUSED(generation)
    Q_UNUSED(key)
    Q_UNUSED(result)
#endif
}

void ScreenshotRecognitionSessionController::applyLatexContents(const QString& source) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    Q_UNUSED(source)
    if (!notify(&ScreenshotRecognitionSessionController::ensureContent))
        return;
    if (content())
        content()->clearRecognitionError();
    if (content() != nullptr) {
        if (const auto session = m_latexCache.value(recognitionCacheKey(Mode::Latex))) {
            content()->showLatexEditor(
                std::shared_ptr<QTextDocument>(session, session->document()),
                [weakSession = std::weak_ptr<ScreenshotOcrTextEditingSession>(session)] {
                    const auto current = weakSession.lock();
                    return current ? current->text() : QString();
                });
            if (!lifetime)
                return;
        }
    }
    if (!updateTextState())
        return;
#else
    Q_UNUSED(source)
#endif
}

void ScreenshotRecognitionSessionController::handleLatexDocumentChanged(const QString& key) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (key == recognitionCacheKey(Mode::Latex)) {
        if (!updateTextState())
            return;
        if (!notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged))
            return;
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
            if (!showStatus(result.error, true))
                return;
        }
        if (!hideRecognitionMessage())
            return;
        if (!updateBusyState())
            return;
        return;
    }
    if (result.contents.isEmpty()) {
        m_qrCache.insert(key, result.contents);
        m_qrResults.insert(key, result);
        if (m_active && m_mode == Mode::Qr) {
            m_qrCacheKey = key;
            if (!notify(&ScreenshotRecognitionSessionController::applyQrContents, result.contents))
                return;
            if (!showStatus(tr("No barcode was recognized"), false))
                return;
        }
        if (!hideRecognitionMessage())
            return;
        if (!updateBusyState())
            return;
        if (!notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged))
            return;
        return;
    }
    m_qrCache.insert(key, result.contents);
    m_qrResults.insert(key, result);
    if (m_active && m_mode == Mode::Qr) {
        m_qrCacheKey = key;
        if (!notify(&ScreenshotRecognitionSessionController::applyQrContents, result.contents))
            return;
    }
    if (!hideRecognitionMessage())
        return;
    if (!updateBusyState())
        return;
    if (!notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged))
        return;
#else
    Q_UNUSED(generation)
    Q_UNUSED(key)
    Q_UNUSED(result)
#endif
}

bool ScreenshotRecognitionSessionController::ownsContentAttachment() const {
    return m_content && m_content->ownsAttachment(this, m_attachmentToken);
}

bool ScreenshotRecognitionSessionController::canPublishUi() const {
    return !m_destroying && m_uiPublicationEnabled &&
           (!m_actions.isCurrentSession || m_actions.isCurrentSession()) &&
           (m_attachmentToken ? ownsContentAttachment() : m_content == nullptr);
}

void ScreenshotRecognitionSessionController::retireContentAttachment() {
    ++m_transitionGeneration;
    m_uiPublicationEnabled = false;
    QObject::disconnect(m_recognitionRetryConnection);
    QObject::disconnect(m_conversionRetryConnection);
    m_recognitionRetryConnection = {};
    m_conversionRetryConnection = {};
    if (m_content)
        m_content->detachSession(this, m_attachmentToken);
    m_attachmentToken = 0;
}

void ScreenshotRecognitionSessionController::attachContent() {
    if (!m_active || (m_actions.isCurrentSession && !m_actions.isCurrentSession()))
        return;
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    const auto generation = m_transitionGeneration;
    if (!m_content && m_actions.ensureContent) {
        const auto create = m_actions.ensureContent;
        const QPointer<ScreenshotRecognitionWindow> created = create();
        if (!lifetime || generation != m_transitionGeneration || !m_active)
            return;
        if (m_actions.isCurrentSession && !m_actions.isCurrentSession())
            return;
        m_content = created;
    }
    if (!m_content)
        return;
    {
        QObject::disconnect(m_recognitionRetryConnection);
        QObject::disconnect(m_conversionRetryConnection);
        const QPointer<ScreenshotRecognitionSessionController> session(this);
        const QPointer<ScreenshotRecognitionWindow> view = m_content;
        m_attachmentToken = view->attachSession(
            this, {[session] { return session && session->active() && session->translating(); },
                   [session]() -> std::unique_ptr<QMimeData> {
                       if (!session || !session->active() || !session->ownsContentAttachment())
                           return {};
                       auto mime = session->recognitionClipboardMimeData();
                       if (!mime && session)
                           session->showStatus(tr("Unable to copy translation results"), true);
                       return mime;
                   },
                   [session](QString selection) {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
                       return session && session->copyDocumentPreview(std::move(selection));
#else
                       Q_UNUSED(session)
                       Q_UNUSED(selection)
                       return false;
#endif
                   }});
        const auto token = m_attachmentToken;
        m_recognitionRetryConnection =
            connect(view, &ScreenshotRecognitionWindow::recognitionRetryRequested, this,
                    [session, view, token] {
                        if (session && view && view->ownsAttachment(session, token))
                            session->retryRecognition();
                    });
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
        m_conversionRetryConnection =
            connect(view, &ScreenshotRecognitionWindow::imageConversionRetryRequested, this,
                    [session, view, token] {
                        if (session && view && view->ownsAttachment(session, token))
                            session->m_conversion->retry();
                    });
#endif
    }
    m_uiPublicationEnabled = true;
    content()->setShowOriginalImage(m_showOriginalImage);
    if (!lifetime || generation != m_transitionGeneration)
        return;
    if (!notify(&ScreenshotRecognitionSessionController::updateTextHighlighting))
        return;
    ensureContent();
}

void ScreenshotRecognitionSessionController::ensureContent() {
    // Background completions may publish to an existing attachment, never acquire one.
    notify(&ScreenshotRecognitionSessionController::updateOriginalImagePreview);
}

void ScreenshotRecognitionSessionController::updateTextHighlighting() const {
    if (content() != nullptr) {
        content()->setTextHighlighting(
            snow_shot::storage::TextRecognitionSettings().showTextHighlighting(),
            snow_shot::storage::ScreenshotUiSettings().selectionMaskColor());
    }
}

void ScreenshotRecognitionSessionController::clearContent(bool retainTableEditor) {
    const QPointer<ScreenshotRecognitionSessionController> session(this);
    const QPointer<ScreenshotRecognitionWindow> view = content();
    const auto token = m_attachmentToken;
    const auto generation = m_transitionGeneration;
    const auto clear = [session, view, token, generation](auto method, auto... arguments) {
        if (session && view && session->m_transitionGeneration == generation &&
            view->ownsAttachment(session, token))
            (view.data()->*method)(arguments...);
    };
    clear(&ScreenshotRecognitionWindow::clearRecognitionError);
    clear(&ScreenshotRecognitionWindow::clearOcrPresentation);
    clear(&ScreenshotRecognitionWindow::clearFormattedText);
    clear(&ScreenshotRecognitionWindow::clearTableSession, retainTableEditor);
    clear(&ScreenshotRecognitionWindow::clearQrContents);
    clear(&ScreenshotRecognitionWindow::clearImageConversion);
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    clear(&ScreenshotRecognitionWindow::clearLatexPreview);
#endif
}

void ScreenshotRecognitionSessionController::applyPresentation(
    const std::shared_ptr<ScreenshotOcrPresentation>& presentation, QImage filteredImage,
    QRectF filteredImageCanvasRect) {
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    m_presentation = presentation;
    if (!notify(&ScreenshotRecognitionSessionController::ensureContent))
        return;
    const bool renderBackground = shouldRenderTextBackground();
    if (!lifetime)
        return;
    if (renderBackground && presentation && presentation->solidBackgroundFill &&
        !backgroundFillEnabled()) {
        prepareScreenshotOcrFillColors(*presentation, {}, {}, false);
    }
    if (content() != nullptr) {
        content()->setOcrCopyDefaultsEnabled(!originalImageTranslationActive());
    }
    if (m_actions.applyOcrPresentation) {
        if (!notifyUiCallback(m_actions.applyOcrPresentation, presentation))
            return;
    } else if (content() != nullptr) {
        content()->setOcrPresentation(presentation);
    }
    if (!shouldRenderTextBackground())
        return;
    if (m_actions.applyOcrBackground) {
        if (!notifyUiCallback(m_actions.applyOcrBackground, presentation))
            return;
    }
    if (!filteredImage.isNull() && m_actions.applyOcrBackgroundImage) {
        if (!notifyUiCallback(m_actions.applyOcrBackgroundImage, presentation,
                              std::move(filteredImage), filteredImageCanvasRect))
            return;
    }
}

void ScreenshotRecognitionSessionController::applyTableSession(
    const std::shared_ptr<ScreenshotTableEditingSession>& session) {
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (session == nullptr || session->document.empty()) {
        return;
    }
    m_tableSession = session;
    if (!notify(&ScreenshotRecognitionSessionController::ensureContent))
        return;
    if (content())
        content()->clearRecognitionError();
    if (content() != nullptr) {
        content()->setTableSession(session);
        if (!notify(&ScreenshotRecognitionSessionController::updateTableState,
                    content()->tableCommandState()))
            return;
    }
#else
    Q_UNUSED(session)
#endif
}

void ScreenshotRecognitionSessionController::applyQrContents(const QStringList& contents) {
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    m_qrContents = contents;
    if (!notify(&ScreenshotRecognitionSessionController::ensureContent))
        return;
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
        if (!notify(&ScreenshotRecognitionSessionController::textDraftChanged, session->text()))
            return;
    }
    if (!updateTextState())
        return;
}

bool ScreenshotRecognitionSessionController::shouldRenderTextBackground() const {
    const QPointer<const ScreenshotRecognitionSessionController> lifetime(this);
    const auto shouldRender = m_actions.shouldRenderTextBackground;
    const bool enabled = !shouldRender || shouldRender();
    return lifetime && enabled;
}

bool ScreenshotRecognitionSessionController::shouldRenderRecognitionInWorker() const {
    if (!shouldRenderTextBackground())
        return false;
    const auto renderInWorker = m_actions.renderRecognitionInWorker;
    return m_actions.applyOcrBackgroundImage && (!renderInWorker || renderInWorker());
}

void ScreenshotRecognitionSessionController::setPendingTextRecognitionRendering(bool enabled) {
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    if (m_recognition == nullptr || m_textRequestToken == 0) {
        return;
    }
    const bool shouldRender = enabled && shouldRenderRecognitionInWorker();
    if (!lifetime)
        return;
    const auto getBackgroundColor = m_actions.ocrBackgroundColor;
    const QColor backgroundColor =
        shouldRender && getBackgroundColor ? getBackgroundColor() : QColor();
    if (!lifetime)
        return;
    static_cast<void>(
        m_recognition->setRenderFilteredImage(m_textRequestToken, shouldRender, backgroundColor,
                                              shouldRender && backgroundFillEnabled()));
}

void ScreenshotRecognitionSessionController::pollTextModelDownload(quint64 generation) {
    if (generation != m_textGeneration || m_textRequestToken == 0 ||
        !m_textModelDownloadInProgress || m_recognition == nullptr) {
        return;
    }
    if (m_recognition->modelFilesReady()) {
        m_textModelDownloadInProgress = false;
        if (m_textModelDownloadShown) {
            if (!notify(&ScreenshotRecognitionSessionController::hideModelDownloadMessage))
                return;
            if (m_active && m_mode == Mode::Text) {
                if (!showRecognitionMessage())
                    return;
            }
        }
        return;
    }
    if (textModelDownloading()) {
        if (!notify(&ScreenshotRecognitionSessionController::showModelDownloadMessage))
            return;
    } else if (m_textModelDownloadShown) {
        if (!notify(&ScreenshotRecognitionSessionController::hideModelDownloadMessage))
            return;
        if (m_active && m_mode == Mode::Text) {
            if (!showRecognitionMessage())
                return;
        }
    }
    QTimer::singleShot(100, this, [this, generation]() { pollTextModelDownload(generation); });
}

void ScreenshotRecognitionSessionController::applyFormattedText(
    const std::shared_ptr<QTextDocument>& document) {
    if (!notify(&ScreenshotRecognitionSessionController::ensureContent))
        return;
    if (m_actions.applyFormattedText) {
        if (!notifyUiCallback(m_actions.applyFormattedText, document))
            return;
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
        if (!notify(&ScreenshotRecognitionSessionController::textDraftChanged,
                    it->translationSession->text()))
            return;
        if (!updateTextState())
            return;
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
    notify(&ScreenshotRecognitionSessionController::textEditingChanged, false);
}

void ScreenshotRecognitionSessionController::handleTableCommandState(
    const ScreenshotTableCommandState& state) {
    notify(&ScreenshotRecognitionSessionController::updateTableState, state);
}

bool ScreenshotRecognitionSessionController::updateBusyState() const {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    if (m_actions.setRecognitionModelState) {
        ScreenshotRecognitionModelState state;
        state.selection = screenshotDedicatedRecognitionModelId();
        state.loading = busy(Mode::Document);
        state.effectiveModel =
            m_documents.contains(m_target.key) ? QStringLiteral("glm-ocr") : QString{};
        if (!notifyUiCallback(m_actions.setRecognitionModelState, int(Mode::Document), state))
            return false;
    }
#endif
    if (!notify(&ScreenshotRecognitionSessionController::workflowStateChanged))
        return false;
    if (m_actions.setBusyState) {
        if (!notifyUiCallback(m_actions.setBusyState, busy(Mode::Text), busy(Mode::Table),
                              busy(Mode::Qr)))
            return false;
    }
    return true;
}

bool ScreenshotRecognitionSessionController::updateTextState() const {
    if (!notify(&ScreenshotRecognitionSessionController::updateOriginalImagePreview))
        return false;
    if (!notify(&ScreenshotRecognitionSessionController::workflowStateChanged))
        return false;
    const bool available = hasTextResult() && m_active && m_mode == Mode::Text;
    const auto entry = m_textCache.value(m_editingKey);
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (m_actions.setTextTranslationServices)
        if (!notifyUiCallback(m_actions.setTextTranslationServices,
                              m_translationService ? m_translationService->models()
                                                   : QVector<SnowShotChatModel>{},
                              m_translationService ? m_translationService->preferences().modelId
                                                   : QString{},
                              m_translationService && m_translationService->loadingModels()))
            return false;
    if (m_translationService != nullptr && m_actions.setTextTargetLanguage)
        if (!notifyUiCallback(m_actions.setTextTargetLanguage,
                              m_translationService->preferences().targetLanguage))
            return false;
    const bool overlay = originalImageTranslationActive();
    const auto translation = m_textCache.constFind(m_translationKey);
    const bool streaming = translation != m_textCache.cend() &&
                           (m_translationInImage ? translation->overlayTranslation.status
                                                 : translation->translationStatus) ==
                               TextCacheEntry::TranslationStatus::Streaming;
#endif
    if (m_actions.setTextEditingState) {
        if (!notifyUiCallback(
                m_actions.setTextEditingState, available, m_editing,
                m_editing && entry.editingSession != nullptr && entry.editingSession->canUndo(),
                m_editing && entry.editingSession != nullptr && entry.editingSession->canRedo()))
            return false;
    }
    if (m_actions.setLatexEditingState) {
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
        const auto session = m_latexCache.value(recognitionCacheKey(Mode::Latex));
        const bool latexAvailable =
            m_active && m_mode == Mode::Latex && session != nullptr && !m_latexStream;
        if (!notifyUiCallback(m_actions.setLatexEditingState, latexAvailable,
                              latexAvailable && session->canUndo(),
                              latexAvailable && session->canRedo()))
            return false;
#else
        if (!notifyUiCallback(m_actions.setLatexEditingState, false, false, false))
            return false;
#endif
    }
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (snow_shot::app::edition::textTranslation && m_actions.setTextTranslationState) {
        if (!notifyUiCallback(
                m_actions.setTextTranslationState, available, m_translating, streaming,
                m_translating && !overlay && !streaming && entry.translationSession != nullptr &&
                    entry.translationSession->canUndo(),
                m_translating && !overlay && !streaming && entry.translationSession != nullptr &&
                    entry.translationSession->canRedo(),
                m_translating && !overlay && !streaming && entry.hasSuccessfulTranslation, overlay))
            return false;
    }
#endif
    if (m_actions.setTextTransformState) {
        const auto session = currentEditingSession(entry);
        if (!notifyUiCallback(m_actions.setTextTransformState,
                              editing() && session != nullptr ? session->formatting() : QString{},
                              editing() && session != nullptr ? session->punctuation() : QString{}))
            return false;
    }
    return true;
}

void ScreenshotRecognitionSessionController::updateOriginalImagePreview() const {
    const QPointer<const ScreenshotRecognitionSessionController> session(this);
    const QPointer<ScreenshotRecognitionWindow> view = content();
    if (!view) {
        return;
    }
    const auto token = m_attachmentToken;
    const auto generation = m_transitionGeneration;
    const auto publish = [session, view, token, generation](auto method, auto... arguments) {
        if (session && view && session->m_transitionGeneration == generation &&
            view->ownsAttachment(session, token))
            (view.data()->*method)(arguments...);
    };
    const bool textActive = m_active && (m_mode == Mode::Text || m_mode == Mode::Document);
    const QImage source = textActive ? m_target.image : QImage{};
    const QRectF canvasRect = textActive ? m_target.canvasRect : QRectF{};
    const bool originalPreview =
        textActive && !m_independentEditing &&
        snow_shot::storage::TextRecognitionSettings().showOriginalImagePreview();
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    const bool latexPreview = !m_independentEditing && m_active && m_mode == Mode::Latex &&
                              ((m_latexStream && m_latexStream->document) ||
                               m_latexCache.contains(recognitionCacheKey(Mode::Latex)));
    publish(&ScreenshotRecognitionWindow::setLatexPreviewEnabled, latexPreview);
#endif
    if (!textActive) {
        publish(&ScreenshotRecognitionWindow::setOriginalImagePreviewEnabled, false);
        publish(&ScreenshotRecognitionWindow::setOriginalImagePreviewSource, source, canvasRect);
        return;
    }
    // The default image describes the screenshot selection. Embedded hosts can
    // provide their displayed image and viewport mapping without replacing it.
    publish(&ScreenshotRecognitionWindow::setOriginalImagePreviewSource, source, canvasRect);
    publish(&ScreenshotRecognitionWindow::setOriginalImagePreviewEnabled, originalPreview);
}

void ScreenshotRecognitionSessionController::updateTableState(
    const ScreenshotTableCommandState& state) const {
    if (!m_actions.setTableEditingState) {
        return;
    }
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    const bool available = m_active && m_mode == Mode::Table && m_tableSession != nullptr &&
                           !m_tableSession->document.empty();
#else
    constexpr bool available = false;
#endif
    if (!notifyUiCallback(m_actions.setTableEditingState, available, available && state.canUndo,
                          available && state.canRedo, available && state.canMerge,
                          available && state.canSplit, available && state.canReset))
        return;
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
        if (!notifyCurrentHostCallback(m_actions.showModelDownload, message))
            return;
    } else if (m_actions.showStatus) {
        if (!notifyCurrentHostCallback(m_actions.showStatus, message, false))
            return;
    }
}

void ScreenshotRecognitionSessionController::hideModelDownloadMessage() {
    if (!m_textModelDownloadShown) {
        return;
    }
    m_textModelDownloadShown = false;
    if (m_actions.hideModelDownload) {
        if (!notifyCurrentHostCallback(m_actions.hideModelDownload))
            return;
    } else if (m_actions.hideLoading) {
        if (!notifyCurrentHostCallback(m_actions.hideLoading))
            return;
    }
}

bool ScreenshotRecognitionSessionController::showRecognitionMessage() const {
    if (m_destroying)
        return true;
    const QString message = m_mode == Mode::Document   ? tr("Recognizing document")
                            : m_mode == Mode::Table    ? tr("Recognizing table")
                            : m_mode == Mode::Latex    ? tr("Recognizing LaTeX formula")
                            : m_mode == Mode::Qr       ? tr("Recognizing barcode")
                            : m_mode == Mode::Markdown ? tr("Converting to Markdown")
                            : m_mode == Mode::Html     ? tr("Converting to HTML")
                                                       : tr("Recognizing text");
    if (m_actions.showRecognition) {
        return notifyUiCallback(m_actions.showRecognition, message);
    } else if (m_actions.showStatus) {
        return notifyUiCallback(m_actions.showStatus, message, false);
    }
    return true;
}

bool ScreenshotRecognitionSessionController::hideRecognitionMessage() const {
    if (m_destroying)
        return true;
    if ((!m_active || !busy()) && m_actions.hideLoading) {
        return notifyUiCallback(m_actions.hideLoading);
    }
    return true;
}

bool ScreenshotRecognitionSessionController::showStatus(QString message, bool error) const {
    if (m_destroying)
        return true;
    if (error)
        m_workflowError = message;
    if (!notify(&ScreenshotRecognitionSessionController::workflowStateChanged))
        return false;
    if (!message.isEmpty() && m_actions.showStatus) {
        return notifyCurrentHostCallback(m_actions.showStatus, message, error);
    }
    return true;
}

void ScreenshotRecognitionSessionController::cancelOutstandingRequests() {
    if (!cancelTranslationRequests())
        return;
    cancelModelRecognition(Mode::Table);
    cancelModelRecognition(Mode::Latex);
    cancelModelRecognition(Mode::Document);
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
    if (!notify(&ScreenshotRecognitionSessionController::hideModelDownloadMessage))
        return;
    m_textModelDownloadInProgress = false;
    hideRecognitionMessage();
}

void ScreenshotRecognitionSessionController::handleRecognitionProviderDestroyed(Mode mode) {
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    bool requestWasPending = false;
    bool translationWasPending = false;
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    const auto translation = m_textCache.constFind(m_translationKey);
    const bool overlayWasPending =
        m_translationInImage && translation != m_textCache.cend() &&
        translation->overlayTranslation.status == TextCacheEntry::TranslationStatus::Streaming;
    if (mode != Mode::Qr && overlayWasPending) {
        if (!cancelTranslationRequests())
            return;
        if (!notify(&ScreenshotRecognitionSessionController::reportOverlayTranslationFailure))
            return;
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
        if (!notify(&ScreenshotRecognitionSessionController::hideModelDownloadMessage))
            return;
        m_textModelDownloadInProgress = false;
        ++m_textGeneration;
        break;
    case Mode::Table:
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
        requestWasPending = m_tableRequestToken != 0 || m_tableParsing ||
                            (m_tableConversion && m_tableConversion->busy());
        if (m_tableConversion)
            m_tableConversion->cancel();
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
                if (!invokeOwned(it->translationSession,
                                 &ScreenshotOcrTextEditingSession::establishHistory,
                                 it->translationText))
                    return;
            }
        }
        if (content() != nullptr && m_translating) {
            content()->setTextEditorStreaming(false);
        }
        if (m_translationSettingsModal != nullptr) {
            m_translationSettingsModal->reject();
            if (!lifetime)
                return;
        }
#endif
        break;
    case Mode::Latex:
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
        requestWasPending = m_latexRequestToken != 0;
        m_latexRequestToken = 0;
        ++m_latexGeneration;
        clearLatexStream();
#endif
        break;
    case Mode::Document:
        return;
    case Mode::Qr:
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
        requestWasPending = m_qrRequestToken != 0;
        m_qrRequestToken = 0;
        ++m_qrGeneration;
#endif
        break;
    }

    if (!updateBusyState())
        return;
    if (!updateTextState())
        return;
    if (!hideRecognitionMessage())
        return;
    if (requestWasPending && m_active && !overlayWasPending) {
        const QString message = translationWasPending ? tr("Translation failed")
                                : mode == Mode::Text  ? tr("Text recognition failed")
                                : mode == Mode::Latex ? tr("LaTeX recognition failed")
                                : mode == Mode::Table ? tr("Table recognition failed")
                                                      : tr("Barcode recognition failed");
        if (m_mode == mode && (mode == Mode::Table || mode == Mode::Latex)) {
            if (!notify(&ScreenshotRecognitionSessionController::failModelRecognition, message))
                return;
        } else if (!showStatus(message, true))
            return;
    }
}

ScreenshotRecognitionWindow* ScreenshotRecognitionSessionController::content() const {
    return ownsContentAttachment() ? m_content.data() : nullptr;
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
    const QStringList modes{QStringLiteral("text"),    QStringLiteral("table"),
                            QStringLiteral("qr"),      QStringLiteral("markdown"),
                            QStringLiteral("html"),    QStringLiteral("latex"),
                            QStringLiteral("document")};
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
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    if (m_mode == Mode::Document) {
        if (const auto document = cachedDocument(m_target.key)) {
            const auto content = document->clipboardContent();
            return {{QStringLiteral("kind"), QStringLiteral("document")},
                    {QStringLiteral("html"), content.html},
                    {QStringLiteral("text"), content.text}};
        }
    }
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
        if (!notify(&ScreenshotRecognitionSessionController::setShowOriginalImage,
                    params.value(QStringLiteral("enabled")).toBool()))
            return false;
        return true;
    }
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (m_mode == Mode::Latex) {
        const auto session = m_latexCache.value(recognitionCacheKey(Mode::Latex));
        if (!session)
            return false;
        if (action == QStringLiteral("set_text")) {
            if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::replaceText,
                             params.value(QStringLiteral("text")).toString()))
                return false;
        } else if (action == QStringLiteral("reset_text")) {
            if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::reset))
                return false;
        } else if (action == QStringLiteral("undo")) {
            if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::undo))
                return false;
        } else if (action == QStringLiteral("redo")) {
            if (!invokeOwned(session, &ScreenshotOcrTextEditingSession::redo))
                return false;
        } else
            return false;
        return true;
    }
#endif
    if (m_mode == Mode::Text && hasTextResult()) {
        if (action == QStringLiteral("set_text")) {
            if (!notify(&ScreenshotRecognitionSessionController::beginTextEditing))
                return false;
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
            if (!notify(&ScreenshotRecognitionSessionController::beginTextEditing))
                return false;
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
    if (!notify(&ScreenshotRecognitionSessionController::recognitionResultsChanged))
        return false;
    return true;
#else
    return false;
#endif
}
void ScreenshotRecognitionSessionController::cancelWorkflow() {
    const QPointer<ScreenshotRecognitionSessionController> lifetime(this);
    cancelOutstandingRequests();
    if (!lifetime)
        return;
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    if (m_independentEditing)
        m_conversion->deactivate();
    else
        m_conversion->invalidate();
#endif
}
