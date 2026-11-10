#include "screenshotsaveresult.h"
#include "screenshotexportmemory_p.h"

#include "screenshotsaveexportpipeline.h"
#include "snow_shot/platform/filemanager.h"
#include "widgets/button.h"
#include "widgets/detail/flow_layout.h"
#include "widgets/result.h"
#include "widgets/scroll_area.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QVBoxLayout>
#include <algorithm>

namespace {
using namespace adqt::widgets;

class ResultActions final : public QWidget {
  public:
    explicit ResultActions(QWidget* parent) : QWidget(parent) {
        new detail::FlowLayout(this, 0, 8, 8);
    }
    QSize sizeHint() const override {
        // Prefer one natural-width row; the flow layout wraps when necessary.
        QSize size(0, 0);
        for (int index = 0; index < layout()->count(); ++index) {
            const auto itemSize = layout()->itemAt(index)->sizeHint();
            size.rwidth() += itemSize.width() + (index > 0 ? 8 : 0);
            size.setHeight(std::max(size.height(), itemSize.height()));
        }
        return size;
    }
};
} // namespace

ScreenshotSaveResult::ScreenshotSaveResult(QWidget* parent, std::function<void()> continueEditing,
                                           std::function<void()> dismiss)
    : QObject(parent), m_continueEditing(std::move(continueEditing)),
      m_dismiss(std::move(dismiss)) {
    m_view = new AdScrollArea(parent);
    m_view->setObjectName(QStringLiteral("saveResultScroll"));
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setWidgetResizable(true);
    m_view->viewport()->setAutoFillBackground(false);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* page = new QWidget;
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addStretch();
    m_result = new AdResult(page);
    m_result->setObjectName(QStringLiteral("saveResult"));
    m_result->setStatus(AdResult::Status::Success);
    pageLayout->addWidget(m_result);
    m_errorLabel = new QLabel(page);
    m_errorLabel->setObjectName(QStringLiteral("saveResultError"));
    m_errorLabel->setContentsMargins(12, 0, 12, 0);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->hide();
    pageLayout->addWidget(m_errorLabel);
    pageLayout->addStretch();
    m_view->setContentWidget(page);
    page->setAutoFillBackground(false);

    m_actions = new QWidget(parent);
    auto* actionsLayout = new QHBoxLayout(m_actions);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(0);
    actionsLayout->addStretch();
    auto* buttons = new ResultActions(m_actions);
    m_continue = new AdButton(buttons);
    m_continue->setObjectName(QStringLiteral("saveContinueEditingButton"));
    m_copyFile = new AdButton(buttons);
    m_copyFile->setObjectName(QStringLiteral("saveCopyImageFileButton"));
    m_copyPath = new AdButton(buttons);
    m_copyPath->setObjectName(QStringLiteral("saveCopyImagePathButton"));
    m_openFolder = new AdButton(buttons);
    m_openFolder->setObjectName(QStringLiteral("saveOpenFolderButton"));
    m_openFolder->setAccentRole(AdButton::AccentRole::Primary);
    m_openFolder->setButtonStyle(AdButton::ButtonStyle::Solid);
    for (auto* button : {m_continue, m_copyFile, m_copyPath, m_openFolder}) {
        button->setAutoDefault(false);
        buttons->layout()->addWidget(button);
        button->show();
    }
    actionsLayout->addWidget(buttons);
    connect(m_continue, &AdButton::clicked, this, [this] {
        if (m_state != State::Ready)
            return;
        m_state = State::Inactive;
        m_output = {};
        updateActions();
        const auto callback = m_continueEditing;
        callback();
    });
    connect(m_copyFile, &AdButton::clicked, this, [this] { copyFile(); });
    connect(m_copyPath, &AdButton::clicked, this, [this] { copyPath(); });
    connect(m_openFolder, &AdButton::clicked, this, [this] { activateDefaultAction(); });
    connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged, this,
            [this] { updateError(); });
    m_view->installEventFilter(this);
    retranslate();
    updateActions();
}

ScreenshotSaveResult::~ScreenshotSaveResult() {
    cancel();
}
QWidget* ScreenshotSaveResult::view() const {
    return m_view;
}
QWidget* ScreenshotSaveResult::actions() const {
    return m_actions;
}
void ScreenshotSaveResult::present(Output output) {
    Q_ASSERT(m_state == State::Inactive);
    Q_ASSERT(output.pixels && !output.path.isEmpty());
    m_output = std::move(output);
    m_state = State::Ready;
    m_error = Error::None;
    m_result->setSubTitle(QDir::toNativeSeparators(m_output.path));
    updateError();
    updateActions();
}
void ScreenshotSaveResult::cancel() {
    m_state = State::Closed;
    m_job.cancel();
    m_clipboard.cancelAll();
    m_output = {};
}
void ScreenshotSaveResult::focusDefaultAction() {
    m_openFolder->setFocus();
}
bool ScreenshotSaveResult::eventFilter(QObject* object, QEvent* event) {
    if (object == m_view && event->type() == QEvent::LanguageChange)
        retranslate();
    return QObject::eventFilter(object, event);
}
void ScreenshotSaveResult::retranslate() {
    m_result->setTitle(tr("Saved successfully"));
    m_continue->setText(tr("Continue editing"));
    m_copyFile->setText(tr("Copy image file"));
    m_copyPath->setText(tr("Copy image path"));
    m_openFolder->setText(tr("Open folder"));
    updateError();
}
void ScreenshotSaveResult::updateError() {
    QString error;
    switch (m_error) {
    case Error::None:
        break;
    case Error::MissingFile:
        error = tr("The saved file no longer exists");
        break;
    case Error::ClipboardPreparation:
        error = tr("The image could not be prepared for the clipboard");
        break;
    case Error::Queue:
        error = tr("The screenshot export queue is full");
        break;
    case Error::Clipboard:
        error = tr("The clipboard could not be updated: %1").arg(m_clipboardFailure.errorString());
        break;
    case Error::Folder:
        error = tr("The folder could not be opened");
        break;
    }
    m_errorLabel->setText(error);
    m_errorLabel->setVisible(!error.isEmpty());
    QPalette palette = m_errorLabel->palette();
    palette.setColor(QPalette::WindowText,
                     adqt::theme::ThemeManager::instance().resolveTheme(m_view).colorError);
    m_errorLabel->setPalette(palette);
}
void ScreenshotSaveResult::updateActions() {
    for (auto* button : {m_continue, m_copyFile, m_copyPath, m_openFolder}) {
        button->setBusy(m_state == State::Busy && button == m_busyButton);
        button->setEnabled(m_state == State::Ready);
    }
    m_openFolder->setDefault(m_state == State::Ready || m_state == State::Busy);
}
bool ScreenshotSaveResult::beginAction(AdButton* button) {
    if (m_state != State::Ready)
        return false;
    m_state = State::Busy;
    m_error = Error::None;
    m_busyButton = button;
    updateError();
    updateActions();
    return true;
}
void ScreenshotSaveResult::fail(Error error) {
    if (m_state == State::Closed)
        return;
    m_state = State::Ready;
    m_error = error;
    m_busyButton = nullptr;
    updateActions();
    updateError();
}
void ScreenshotSaveResult::clipboardFinished(ScreenshotClipboardCommitResult result) {
    if (m_state == State::Closed)
        return;
    if (!result.succeeded()) {
        m_clipboardFailure = result;
        fail(Error::Clipboard);
        return;
    }
    const auto callback = m_dismiss;
    callback();
}
void ScreenshotSaveResult::copyFile() {
    if (!beginAction(m_copyFile))
        return;
    const auto publication = ScreenshotClipboardService::reservePublication();
    auto payload = std::make_shared<ScreenshotClipboardPayload>();
    m_job = ScreenshotExportCoordinator::shared().submit(
        this, ScreenshotExportCoordinator::Priority::Foreground,
        [output = m_output, payload](const ScreenshotExportCancellation& cancellation) {
            if (!QFileInfo(output.path).isFile())
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::File, {});
            auto rows = output.pixels->rows;
            rows.cancellationRequested = [&cancellation, cancelled = rows.cancellationRequested] {
                return cancellation.isCancellationRequested() || (cancelled && cancelled());
            };
            *payload = ScreenshotClipboardService::withFile(
                ScreenshotClipboardService::prepare(rows, output.encoding), output.path);
            if (!payload->isValid())
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Render,
                                                           {});
            return ScreenshotExportTaskResult{};
        },
        [this, payload, publication](ScreenshotExportTaskResult result) {
            m_job = {};
            if (m_state == State::Closed)
                return;
            if (!result.succeeded()) {
                fail(result.failureStage == ScreenshotExportFailureStage::File
                         ? Error::MissingFile
                         : Error::ClipboardPreparation);
                return;
            }
            static_cast<void>(m_clipboard.commit(
                QApplication::clipboard(), this, std::move(*payload),
                [this](ScreenshotClipboardCommitResult committed) { clipboardFinished(committed); },
                publication));
        },
        screenshot_export_memory::encodingWorkingBytes(
            m_output.pixels->size, ScreenshotImageFileFormat::Png, m_output.encoding,
            std::numeric_limits<qint64>::max(), true));
    if (!m_job.isValid())
        fail(Error::Queue);
}
void ScreenshotSaveResult::copyPath() {
    if (!beginAction(m_copyPath))
        return;
    auto* mime = new QMimeData;
    mime->setText(QDir::toNativeSeparators(m_output.path));
    static_cast<void>(m_clipboard.commitMimeData(
        QApplication::clipboard(), this, mime,
        [this](ScreenshotClipboardCommitResult result) { clipboardFinished(result); }));
}
void ScreenshotSaveResult::activateDefaultAction() {
    if (!beginAction(m_openFolder))
        return;
    const QPointer<ScreenshotSaveResult> guard(this);
    const bool opened = snow_shot::platform::revealInFileManager(m_output.path);
    if (!guard || m_state == State::Closed)
        return;
    if (opened) {
        const auto callback = m_dismiss;
        callback();
    } else {
        fail(Error::Folder);
    }
}
