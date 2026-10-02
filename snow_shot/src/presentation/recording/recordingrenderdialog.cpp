#include "recordingrenderdialog.h"
#include "snow_shot/presentation/components/screenrecordingmodal.h"
#include "widgets/button.h"
#include "widgets/progress.h"
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <cmath>

namespace {
QString renderText(const char* source) {
    return QCoreApplication::translate("RecordingRenderDialog", source);
}
} // namespace

RecordingRenderDialog::RecordingRenderDialog(QObject* parent, QScreen* screen,
                                             const QRect& anchorGeometry, QWidget* windowOwner)
    : QObject(parent) {
    using namespace adqt::widgets;
    modal = new AdModal(this);
    modal->setObjectName(QStringLiteral("screenRecordingRenderModal"));
    snow_shot::presentation::configureScreenRecordingModal(*modal, windowOwner);
    modal->setWindowScreen(screen);
    modal->setWindowAnchorGeometry(anchorGeometry);
    modal->setPreferredWidth(500);
    modal->setClosePolicy(AdModal::ClosePolicy::Manual);
    modal->setStandardButtons(AdModal::StandardButton::NoButton);
    auto* body = new QWidget;
    body->installEventFilter(this);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    status = new QLabel(body);
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    progress = new AdProgress(body);
    progress->setObjectName(QStringLiteral("screenRecordingRenderProgress"));
    progress->setType(AdProgress::Type::Line);
    progress->setAnimationEnabled(false);
    layout->addWidget(progress);
    details = new QLabel(body);
    details->setTextFormat(Qt::PlainText);
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(details);
    modal->setContentWidget(body);
    auto* footer = new QWidget;
    auto* actions = new QHBoxLayout(footer);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->addStretch();
    cancelButton = new AdButton(footer);
    retryButton = new AdButton(footer);
    keepButton = new AdButton(footer);
    discardButton = new AdButton(footer);
    discardButton->setAccentRole(AdButton::AccentRole::Danger);
    cancelButton->setObjectName(QStringLiteral("screenRecordingRenderCancel"));
    retryButton->setObjectName(QStringLiteral("screenRecordingRenderRetry"));
    keepButton->setObjectName(QStringLiteral("screenRecordingRenderKeepSource"));
    discardButton->setObjectName(QStringLiteral("screenRecordingRenderDiscard"));
    retryButton->setButtonStyle(AdButton::ButtonStyle::Solid);
    retryButton->setAccentRole(AdButton::AccentRole::Primary);
    for (auto* button : {cancelButton, retryButton, keepButton, discardButton})
        actions->addWidget(button);
    modal->setFooterWidget(footer);

    connect(cancelButton, &AdButton::clicked, this, [this] {
        if (cancel)
            cancel();
    });
    connect(retryButton, &AdButton::clicked, this, [this] {
        if (retry)
            retry();
    });
    connect(keepButton, &AdButton::clicked, this, [this] {
        if (keep)
            keep();
    });
    connect(discardButton, &AdButton::clicked, this, [this] {
        if (discard)
            discard();
    });
    connect(modal, &AdModal::closeRequested, this, [this](AdModal::CloseReason) {
        if (retained) {
            if (keep)
                keep();
        } else if (cancel)
            cancel();
    });
    refresh();
}

void RecordingRenderDialog::refresh() {
    using adqt::widgets::AdProgress;
    modal->setWindowTitle(
        renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Rendering recording")));
    progress->setAccessibleName(
        renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Rendering progress")));
    progress->setPercent(std::floor(percent));
    progress->setStatus(retained && !lastError.isEmpty() ? AdProgress::Status::Exception
                        : terminalSucceeded              ? AdProgress::Status::Success
                                                         : AdProgress::Status::Active);
    const char* text =
        retained          ? (lastError.isEmpty()
                                 ? QT_TRANSLATE_NOOP("RecordingRenderDialog", "Rendering canceled")
                                 : QT_TRANSLATE_NOOP("RecordingRenderDialog", "Rendering failed"))
        : cancelRequested ? QT_TRANSLATE_NOOP("RecordingRenderDialog", "Cancelling rendering...")
        : stage == SNOW_RECORDING_RENDER_STAGE_PREPARE
            ? QT_TRANSLATE_NOOP("RecordingRenderDialog", "Preparing recording...")
        : stage == SNOW_RECORDING_RENDER_STAGE_FINALIZE
            ? QT_TRANSLATE_NOOP("RecordingRenderDialog", "Finalizing recording...")
            : QT_TRANSLATE_NOOP("RecordingRenderDialog", "Rendering video...");
    status->setText(renderText(text));
    details->setVisible(retained);
    details->setText(
        (lastError.isEmpty() ? QString() : lastError + QStringLiteral("\n\n")) +
        renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Source files are preserved in:\n%1"))
            .arg(path));
    cancelButton->setText(renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Cancel")));
    retryButton->setText(renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Retry")));
    keepButton->setText(renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Keep Source")));
    discardButton->setText(renderText(QT_TRANSLATE_NOOP("RecordingRenderDialog", "Discard")));
    cancelButton->setVisible(!retained);
    cancelButton->setEnabled(!cancelRequested && !busy);
    for (auto* button : {retryButton, keepButton, discardButton}) {
        button->setVisible(retained);
        button->setEnabled(!busy);
    }
    modal->setInitialFocusWidget(retained ? retryButton : cancelButton);
}

bool RecordingRenderDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        refresh();
        if (modal->isOpen())
            modal->open();
    }
    return QObject::eventFilter(watched, event);
}
