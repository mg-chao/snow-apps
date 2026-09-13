#include "snow_shot/presentation/screenshotsaveasfiledialog.h"

#include "snow_shot/presentation/components/actionpopupmenu.h"
#include "snow_shot/presentation/components/aspectratiolockbutton.h"
#include "snow_shot/presentation/components/pathinput.h"
#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snow_shot/presentation/screenshotsavepreviewcanvas.h"
#include "snow_shot/storage/settingsadapters.h"
#include "screenshotsaveexportpipeline.h"
#include "widgets/button.h"
#include "widgets/context_menu.h"
#include "widgets/detail/flow_layout.h"
#include "widgets/field_group.h"
#include "widgets/form.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_number.h"
#include "widgets/modal.h"
#include "widgets/select.h"
#include "widgets/segmented.h"
#include "widgets/scroll_area.h"
#include "widgets/slider.h"
#include "theme/theme_manager.h"
#include "antd_icons.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <optional>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace {
using namespace adqt::widgets;
namespace icons = adqt::icons::antd::outlined;
namespace pipeline = screenshot_save_export;
using Format = ScreenshotImageFileFormat;
using Shortcut = snow_shot::storage::ScreenshotSavePathShortcut;

constexpr int kSaveFormContentWidth = 328;
constexpr int kAspectRatioLockButtonSize = 32;
constexpr int kMaximumDimension = 1000000;

bool formatSupportsLossless(Format format) {
    return format == Format::Pdf || format == Format::Webp || format == Format::Jxl ||
           format == Format::Avif;
}

QString translated(const char* text) {
    return QCoreApplication::translate("ScreenshotSaveAsFileDialog", text);
}

void configureForm(AdForm* form) {
    form->setFormLayout(AdForm::FormLayout::Vertical);
    form->setLabelAlign(AdForm::LabelAlign::Left);
    form->setColon(false);
    form->setRequiredMark(AdForm::RequiredMark::Hidden);
}

class SaveContent final : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(ScreenshotSaveAsFileDialog)
  public:
    SaveContent(AdModal* modal, std::shared_ptr<ScreenshotExportArtifact> artifact,
                ScreenshotSaveAsFileDialog::Saved saved)
        : m_modal(modal), m_artifact(std::move(artifact)), m_saved(std::move(saved)) {
        setObjectName(QStringLiteral("saveDialogContent"));
        m_state = ScreenshotSaveDialogState::initial({});
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(20);
        m_preview = new ScreenshotSavePreviewCanvas(this);
        layout->addWidget(m_preview, 1);
        m_outputDescription = new QLabel(this);
        m_outputDescription->setObjectName(QStringLiteral("saveOutputDescription"));
        m_outputDescription->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_outputDescription->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* scroll = new AdScrollArea(this);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->viewport()->setAutoFillBackground(false);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFixedWidth(340);
        scroll->setMinimumHeight(200);
        m_form = new AdForm;
        configureForm(m_form);
        m_form->setContentsMargins(0, 0, 12, 0);
        scroll->setContentWidget(m_form);
        m_form->setAutoFillBackground(false);
        layout->addWidget(scroll);

        auto* directory = new QWidget(m_form);
        directory->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto* directoryLayout = new QVBoxLayout(directory);
        directoryLayout->setContentsMargins(0, 0, 0, 0);
        directoryLayout->setSpacing(8);
        m_directory = new DirectoryPathInput(directory);
        m_directory->setObjectName(QStringLiteral("saveDirectoryPathInput"));
        m_directory->lineEdit()->setObjectName(QStringLiteral("saveDirectoryInput"));
        m_directory->browseButton()->setObjectName(QStringLiteral("saveDirectoryBrowseButton"));
        m_directory->setText(m_state.directory);
        directoryLayout->addWidget(m_directory);
        addField("Save path", directory, "directory");
        connect(m_directory, &DirectoryPathInput::browseRequested, this, [this] {
            const QString path = QFileDialog::getExistingDirectory(
                window(), tr("Select save directory"), m_directory->text());
            if (!path.isEmpty())
                m_directory->setText(path);
        });
        m_shortcutsHost = new QWidget(directory);
        m_shortcutsHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        m_shortcutsLayout = new adqt::widgets::detail::FlowLayout(m_shortcutsHost, 0, 6, 6);
        m_shortcutsHost->installEventFilter(this);
        directoryLayout->addWidget(m_shortcutsHost);
        rebuildShortcuts();

        m_filename = new AdLineEdit(m_form);
        m_filename->setObjectName(QStringLiteral("saveFilenameInput"));
        m_filename->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_filename->setText(m_state.filename);
        addField("File name", m_filename, "filename");
        m_format = new AdSelect(m_form);
        m_format->setObjectName(QStringLiteral("saveFormatSelect"));
        addField("Image format", m_format, "format");

        auto* dimensions = new QWidget(m_form);
        dimensions->setObjectName(QStringLiteral("saveDimensionsForm"));
        dimensions->setFixedWidth(kSaveFormContentWidth);
        auto* dimensionsLayout = new QVBoxLayout(dimensions);
        dimensionsLayout->setContentsMargins(0, 0, 0, 0);
        dimensionsLayout->setSpacing(8);
        auto* header = new QHBoxLayout;
        header->setSpacing(8);
        m_sizeLabel = new QLabel(dimensions);
        m_sizeLabel->setObjectName(QStringLiteral("saveSizeLabel"));
        m_sizeLabel->setWordWrap(true);
        header->addWidget(m_sizeLabel, 1);
        m_sizeUnit = new AdSegmented(dimensions);
        m_sizeUnit->setObjectName(QStringLiteral("saveSizeUnitSegmented"));
        m_sizeUnit->setControlSize(AdSegmented::ControlSize::Small);
        m_sizeUnit->setDistribution(AdSegmented::Distribution::Content);
        m_sizeUnit->addOption({}, QStringLiteral("pixels"));
        m_sizeUnit->addOption({}, QStringLiteral("percentage"));
        m_sizeUnit->setCurrentValue(QStringLiteral("pixels"));
        header->addWidget(m_sizeUnit, 0, Qt::AlignRight | Qt::AlignVCenter);
        dimensionsLayout->addLayout(header);
        auto* inputs = new QHBoxLayout;
        inputs->setSpacing(0);
        m_width = number("saveWidthInput");
        m_height = number("saveHeightInput");
        inputs->addWidget(m_width, 1);
        m_lock = new AspectRatioLockButton(dimensions);
        m_lock->setObjectName(QStringLiteral("saveAspectLockButton"));
        m_lock->setFixedSize(kAspectRatioLockButtonSize, kAspectRatioLockButtonSize);
        m_lock->setChecked(true);
        inputs->addWidget(m_lock, 0, Qt::AlignVCenter);
        inputs->addWidget(m_height, 1);
        dimensionsLayout->addLayout(inputs);
        m_form->addField({}, dimensions, QStringLiteral("dimensions"));

        m_quality = new AdSlider(m_form);
        m_quality->setTracking(false);
        m_quality->setObjectName(QStringLiteral("saveQualitySlider"));
        m_quality->setRange(1, 100);
        m_quality->setValue(100);
        m_quality->setSingleStep(1);
        addField("Quality", m_quality, "quality");
        m_error = new QLabel(m_form);
        m_error->setObjectName(QStringLiteral("saveErrorLabel"));
        m_error->setWordWrap(true);
        auto* errorItem = m_form->addField({}, m_error);
        errorItem->setNoStyle(true);
        m_error->hide();
        m_form->layout()->setAlignment(Qt::AlignTop);
        connect(&adqt::theme::ThemeManager::instance(), &adqt::theme::ThemeManager::themeChanged,
                this, [this] {
                    updateSizeLabelTheme();
                    updateOutputDescriptionTheme();
                    showError(m_error->text());
                    m_preview->update();
                });

        connect(m_directory, &DirectoryPathInput::textChanged, this, [this](const QString& value) {
            m_state.directory = value;
            validateFields();
        });
        connect(m_filename, &AdLineEdit::textChanged, this, [this](const QString& value) {
            m_state.filename = value;
            if (m_state.output.format == Format::Pdf)
                changed();
            else
                validateFields();
        });
        connect(m_format, &AdSelect::currentValueChanged, this, [this](const QVariant& value) {
            m_state.output.format = ScreenshotImageFileService::formatForKey(value.toString());
            changed();
        });
        connect(m_width, &AdInputNumber::valueChanged, this,
                [this](double value) { dimensionChanged(true, value); });
        connect(m_height, &AdInputNumber::valueChanged, this,
                [this](double value) { dimensionChanged(false, value); });
        connect(m_width, &AdInputNumber::hasValueChanged, this, [this](bool hasValue) {
            if (!hasValue)
                dimensionChanged(true, 0);
        });
        connect(m_height, &AdInputNumber::hasValueChanged, this, [this](bool hasValue) {
            if (!hasValue)
                dimensionChanged(false, 0);
        });
        connect(m_lock, &QAbstractButton::toggled, this, [this](bool checked) {
            m_state.lockAspectRatio = checked;
            if (checked && m_state.output.size.width() > 0)
                dimensionChanged(true,
                                 m_percentage ? m_width->value() : m_state.output.size.width());
            updateControls();
        });
        connect(m_sizeUnit, &AdSegmented::currentValueChanged, this, [this](const QVariant& value) {
            commitDimensionEdit();
            m_percentage = value.toString() == QStringLiteral("percentage");
            syncDimensionInputs(true);
        });
        connect(m_quality, &AdSlider::valueChanged, this, [this](double value) {
            m_state.output.quality = qRound(value);
            changed();
        });
        retranslate();
        m_form->setDisabled(true);
        m_preview->setBusy(true);
        QTimer::singleShot(0, this, [this] { loadSource(); });
    }

    ~SaveContent() override {
        cancel(!m_closed);
    }
    void cancel(bool cancelSource = false) {
        m_closed = true;
        ++m_generation;
        if (m_menu)
            m_menu->hide();
        m_job.cancel();
        cancelDecode();
        m_saveJob.cancel();
        if (cancelSource)
            m_artifact->cancel();
    }
    bool saving() const {
        return m_saving;
    }
    void installFooter() {
        auto* footer = new QWidget;
        footer->setObjectName(QStringLiteral("saveDialogFooter"));
        footer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* layout = new QHBoxLayout(footer);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        layout->addWidget(m_outputDescription, 1, Qt::AlignVCenter);
        layout->addWidget(m_modal->rejectButton(), 0, Qt::AlignVCenter);
        layout->addWidget(m_modal->acceptButton(), 0, Qt::AlignVCenter);
        m_modal->setFooterWidget(footer);
    }
    void save() {
        if (m_saving || !m_source.rows.isValid())
            return;
        commitDimensionEdit();
        m_state.directory = m_directory->text();
        m_state.filename = m_filename->text();
        const QString error = m_state.validationError();
        if (!error.isEmpty()) {
            validateFields();
            showError(error);
            return;
        }
        const QString path = m_state.outputPath();
        if (QFileInfo::exists(path)) {
            auto* confirm = new AdModal(this);
            m_overwrite = confirm;
            confirm->setObjectName(QStringLiteral("saveOverwriteModal"));
            confirm->setOwnerWindow(window());
            confirm->setRenderContainer(window());
            confirm->setWindowTitle(tr("Replace file"));
            confirm->setText(
                tr("Replace the existing file \"%1\"?").arg(QFileInfo(path).fileName()));
            confirm->setProperty("filename", QFileInfo(path).fileName());
            confirm->setAcceptText(tr("Replace"));
            confirm->setRejectText(tr("Cancel"));
            confirm->setClosePolicy(AdModal::ClosePolicy::Manual);
            connect(confirm, &AdModal::closeRequested, confirm,
                    [this, confirm](AdModal::CloseReason reason) {
                        if (reason == AdModal::CloseReason::OkAction) {
                            confirm->accept();
                            beginSave();
                        } else
                            confirm->reject();
                    });
            connect(confirm, &AdModal::finished, confirm, &QObject::deleteLater);
            confirm->open();
        } else
            beginSave();
    }

  protected:
    void changeEvent(QEvent* event) override {
        if (event->type() == QEvent::LanguageChange)
            retranslate();
        QWidget::changeEvent(event);
    }
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == m_shortcutsHost &&
            (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
            updateShortcutHeight();
        }
        if (event->type() == QEvent::LanguageChange && m_editor &&
            object == m_editor->contentWidget())
            retranslateSecondary();
        return QWidget::eventFilter(object, event);
    }

  private:
    AdInputNumber* number(const char* name) {
        auto* input = new AdInputNumber;
        input->setObjectName(QString::fromLatin1(name));
        input->setDecimals(0);
        input->setKeyboardTracking(false);
        input->setRange(0, kMaximumDimension);
        input->setSingleStep(1);
        input->setStepButtonLayout(AdInputNumber::StepButtonLayout::Compact);
        input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return input;
    }
    void addField(const char* label, QWidget* editor, const char* key) {
        auto* item = m_form->addField({}, editor, QString::fromLatin1(key));
        item->setProperty("exportLabel", label);
    }
    void validateFields() {
        if (!m_source.rows.isValid())
            return;
        auto setError = [this](const QString& key, const QString& message) {
            for (auto* item : m_form->findChildren<AdFormItem*>()) {
                if (item->fieldName() == key)
                    item->setErrorMessages(message.isEmpty() ? QStringList{}
                                                             : QStringList{message});
            }
        };
        const QString imageError = m_state.imageValidationError();
        const QString error = m_state.validationError();
        const bool badPath = m_state.directory.trimmed().isEmpty() ||
                             !QDir::isAbsolutePath(m_state.directory.trimmed());
        setError(QStringLiteral("directory"), badPath ? error : QString());
        setError(QStringLiteral("filename"), !badPath && error != imageError ? error : QString());
        m_width->setStatus(imageError.isEmpty() ? AdInputNumber::Status::None
                                                : AdInputNumber::Status::Error);
        m_height->setStatus(imageError.isEmpty() ? AdInputNumber::Status::None
                                                 : AdInputNumber::Status::Error);
        m_modal->acceptButton()->setEnabled(!m_saving && error.isEmpty());
    }
    void showError(const QString& message) {
        m_error->setText(message);
        m_error->setVisible(!message.isEmpty());
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        QPalette palette = m_error->palette();
        palette.setColor(QPalette::WindowText, theme.colorError);
        m_error->setPalette(palette);
    }
    void retranslate() {
        m_modal->setWindowTitle(tr("Save as file"));
        m_modal->setAcceptText(tr("Save"));
        m_modal->setRejectText(tr("Cancel"));
        m_directory->setBrowseButtonText(tr("Select save directory"));
        m_lock->setToolTip(tr("Lock aspect ratio"));
        m_lock->setAccessibleName(m_lock->toolTip());
        m_directory->lineEdit()->setAccessibleName(tr("Save path"));
        m_filename->setAccessibleName(tr("File name"));
        m_format->setAccessibleName(tr("Image format"));
        m_width->setAccessibleName(tr("Width"));
        m_height->setAccessibleName(tr("Height"));
        m_width->setToolTip(tr("Width"));
        m_height->setToolTip(tr("Height"));
        m_sizeLabel->setText(tr("Size"));
        m_sizeUnit->setAccessibleName(tr("Size unit"));
        m_sizeUnit->setOptionLabel(0, tr("Pixels"));
        m_sizeUnit->setOptionLabel(1, tr("Percentage"));
        updateSizeLabelTheme();
        updateOutputDescriptionTheme();
        updateOutputDescription();
        m_quality->setAccessibleName(tr("Quality"));
        for (auto* item : findChildren<AdFormItem*>()) {
            const QByteArray source = item->property("exportLabel").toByteArray();
            if (!source.isEmpty())
                item->setLabel(translated(source.constData()));
        }
        const QSignalBlocker blocker(m_format);
        QVector<AdSelect::Option> options;
        for (const auto& entry : {std::pair<const char*, const char*>("png", "PNG"),
                                  {"jpeg", "JPEG"},
                                  {"bmp", "BMP"},
                                  {"webp", "WebP"},
                                  {"jxl", "JPEG XL"},
                                  {"avif", "AVIF"},
                                  {"pdf", "PDF"}}) {
            AdSelect::Option option;
            option.value = QString::fromLatin1(entry.first);
            option.label = translated(entry.second);
            options.push_back(option);
        }
        m_format->setOptions(options);
        const auto key = m_state.output.format == Format::Jpeg
                             ? QStringLiteral("jpeg")
                             : ScreenshotImageFileService::extension(m_state.output.format);
        m_format->setCurrentValue(key);
        rebuildShortcuts();
        updateControls();
        retranslateSecondary();
        validateFields();
    }
    void retranslateSecondary() {
        if (m_menu) {
            const auto actions = m_menu->actions();
            if (actions.size() == 2) {
                actions[0]->setText(tr("Edit"));
                actions[1]->setText(tr("Delete"));
            }
        }
        if (m_overwrite) {
            m_overwrite->setWindowTitle(tr("Replace file"));
            m_overwrite->setText(tr("Replace the existing file \"%1\"?")
                                     .arg(m_overwrite->property("filename").toString()));
            m_overwrite->setAcceptText(tr("Replace"));
            m_overwrite->setRejectText(tr("Cancel"));
        }
        if (m_editor) {
            m_editor->setWindowTitle(m_editor->property("editingShortcut").toBool()
                                         ? tr("Edit save path")
                                         : tr("Add save path"));
            m_editor->setAcceptText(tr("OK"));
            m_editor->setRejectText(tr("Cancel"));
            auto* form = qobject_cast<AdForm*>(m_editor->contentWidget());
            if (form) {
                form->itemForName(QStringLiteral("name"))->setLabel(tr("Name"));
                form->itemForName(QStringLiteral("path"))->setLabel(tr("Path"));
                form->findChild<AdLineEdit*>(QStringLiteral("savePathNameInput"))
                    ->setAccessibleName(tr("Name"));
                form->findChild<AdLineEdit*>(QStringLiteral("savePathValueInput"))
                    ->setAccessibleName(tr("Path"));
                form->findChild<DirectoryPathInput*>(QStringLiteral("savePathValuePathInput"))
                    ->setBrowseButtonText(tr("Select save directory"));
                if (form->property("validationAttempted").toBool())
                    static_cast<void>(
                        validateShortcut(form, m_editor->property("shortcutIndex").toInt()));
            }
        }
    }
    void rebuildShortcuts() {
        while (auto* item = m_shortcutsLayout->takeAt(0)) {
            if (item->widget()) {
                item->widget()->hide();
                item->widget()->deleteLater();
            }
            delete item;
        }
        m_shortcuts = snow_shot::storage::ScreenshotSettings().savePathShortcuts();
        auto add = [this](const QString& name, const QString& path, int index) {
            auto* group = new AdFieldGroup(m_shortcutsHost);
            group->setObjectName(QStringLiteral("savePathShortcutGroup_%1").arg(index));
            auto* button = new AdButton(name, group);
            button->setSizeClass(AdButton::SizeClass::Small);
            button->setMaximumWidth(230);
            button->setToolTip(path);
            button->setObjectName(QStringLiteral("savePathShortcut_%1").arg(index));
            group->addControl(button);
            connect(button, &QAbstractButton::clicked, this,
                    [this, path] { m_directory->setText(path); });
            if (index >= 0) {
                auto* expand = new AdButton(group);
                expand->setObjectName(QStringLiteral("savePathExpand_%1").arg(index));
                expand->setSizeClass(AdButton::SizeClass::Small);
                expand->setIconRef(icons::Down());
                expand->setFixedSize(24, 24);
                expand->setToolTip(tr("Edit or delete save path"));
                expand->setAccessibleName(expand->toolTip());
                new snow_shot::presentation::ActionPopupMenu(
                    expand, [this, expand, index] { return createShortcutMenu(expand, index); });
                group->addControl(expand);
            }
            m_shortcutsLayout->addWidget(group);
        };
        add(tr("App directory"), snow_shot::storage::ScreenshotSettings().imageSaveDirectory(), -2);
        add(tr("Desktop"), QStandardPaths::writableLocation(QStandardPaths::DesktopLocation), -1);
        for (int index = 0; index < m_shortcuts.size(); ++index)
            add(m_shortcuts.at(index).name, m_shortcuts.at(index).path, index);
        auto* button = new AdButton(m_shortcutsHost);
        button->setObjectName(QStringLiteral("savePathAddButton"));
        button->setSizeClass(AdButton::SizeClass::Small);
        button->setIconRef(icons::Plus());
        button->setFixedSize(24, 24);
        button->setToolTip(tr("Add save path"));
        button->setAccessibleName(button->toolTip());
        connect(button, &QAbstractButton::clicked, this, [this] { shortcutDialog(-1); });
        m_shortcutsLayout->addWidget(button);
        updateShortcutHeight();
        m_shortcutsHost->updateGeometry();
    }
    void updateShortcutHeight() {
        const int height = m_shortcutsLayout->heightForWidth(m_shortcutsHost->width());
        if (m_shortcutsHost->minimumHeight() != height)
            m_shortcutsHost->setFixedHeight(height);
    }
    AdContextMenu* createShortcutMenu(AdButton* trigger, int index) {
        if (index < 0 || index >= m_shortcuts.size())
            return nullptr;
        if (m_menu)
            m_menu->deleteLater();
        auto* menu = new AdContextMenu(this);
        m_menu = menu;
        menu->setObjectName(QStringLiteral("savePathMenu"));
        menu->setTriggerWidget(trigger);
        auto* edit = menu->addItem(tr("Edit"), icons::Edit());
        auto* remove = menu->addItem(tr("Delete"), icons::IconDelete());
        menu->setActionDanger(remove);
        connect(edit, &QAction::triggered, this, [this, index] { shortcutDialog(index); });
        connect(remove, &QAction::triggered, this, [this, index] {
            auto shortcuts = m_shortcuts;
            shortcuts.removeAt(index);
            if (snow_shot::storage::ScreenshotSettings().setSavePathShortcuts(shortcuts))
                rebuildShortcuts();
            else
                showError(tr("The save paths could not be stored"));
        });
        return menu;
    }
    void shortcutDialog(int index) {
        auto* modal = new AdModal(this);
        m_editor = modal;
        modal->setProperty("editingShortcut", index >= 0);
        modal->setProperty("shortcutIndex", index);
        modal->setObjectName(QStringLiteral("savePathEditorModal"));
        modal->setOwnerWindow(window());
        modal->setRenderContainer(window());
        modal->setMode(AdModal::Mode::Overlay);
        modal->setWindowTitle(index < 0 ? tr("Add save path") : tr("Edit save path"));
        modal->setPreferredWidth(400);
        modal->setCentered(true);
        modal->setCloseOnMaskClick(false);
        modal->setClosePolicy(AdModal::ClosePolicy::Manual);
        modal->setAcceptText(tr("OK"));
        modal->setRejectText(tr("Cancel"));
        auto* form = new AdForm;
        configureForm(form);
        form->setRequiredMark(AdForm::RequiredMark::Visible);
        auto* name = new AdLineEdit(form);
        name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        name->setObjectName(QStringLiteral("savePathNameInput"));
        auto* path = new DirectoryPathInput(form);
        path->setObjectName(QStringLiteral("savePathValuePathInput"));
        path->lineEdit()->setObjectName(QStringLiteral("savePathValueInput"));
        path->browseButton()->setObjectName(QStringLiteral("savePathValueBrowseButton"));
        connect(path, &DirectoryPathInput::browseRequested, path, [this, path] {
            const QString directory = QFileDialog::getExistingDirectory(
                window(), tr("Select save directory"), path->text());
            if (!directory.isEmpty())
                path->setText(directory);
        });
        if (index >= 0) {
            name->setText(m_shortcuts.at(index).name);
            path->setText(m_shortcuts.at(index).path);
        }
        auto* nameItem = form->addField(tr("Name"), name, QStringLiteral("name"));
        auto* pathItem = form->addField(tr("Path"), path, QStringLiteral("path"));
        for (auto* item : {nameItem, pathItem})
            item->setRequired(true);
        modal->setContentWidget(form);
        form->installEventFilter(this);
        modal->setInitialFocusWidget(name);
        retranslateSecondary();
        connect(modal, &AdModal::closeRequested, modal,
                [this, modal, form, name, path, pathItem, index](AdModal::CloseReason reason) {
                    if (reason != AdModal::CloseReason::OkAction) {
                        modal->reject();
                        return;
                    }
                    form->setProperty("validationAttempted", true);
                    if (!validateShortcut(form, index))
                        return;
                    const QString value = name->text().trimmed();
                    const QString target = path->text().trimmed();
                    auto shortcuts = m_shortcuts;
                    const Shortcut shortcut{value, QDir::cleanPath(target)};
                    if (index < 0)
                        shortcuts.push_back(shortcut);
                    else
                        shortcuts[index] = shortcut;
                    if (!snow_shot::storage::ScreenshotSettings().setSavePathShortcuts(shortcuts)) {
                        pathItem->setErrorMessages({tr("The save paths could not be stored")});
                        return;
                    }
                    rebuildShortcuts();
                    modal->accept();
                });
        connect(modal, &AdModal::finished, modal, &QObject::deleteLater);
        modal->open();
    }
    bool validateShortcut(AdForm* form, int index) {
        const QString name =
            form->findChild<AdLineEdit*>(QStringLiteral("savePathNameInput"))->text().trimmed();
        const QString path =
            form->findChild<AdLineEdit*>(QStringLiteral("savePathValueInput"))->text().trimmed();
        QString nameError;
        if (name.isEmpty())
            nameError = tr("Please enter a name");
        for (int other = 0; other < m_shortcuts.size(); ++other)
            if (other != index &&
                m_shortcuts.at(other).name.compare(name, Qt::CaseInsensitive) == 0)
                nameError = tr("A save path with this name already exists");
        const QString pathError = path.isEmpty() || !QDir::isAbsolutePath(path)
                                      ? tr("Please enter an absolute directory path")
                                      : QString();
        form->itemForName(QStringLiteral("name"))
            ->setErrorMessages(nameError.isEmpty() ? QStringList{} : QStringList{nameError});
        form->itemForName(QStringLiteral("path"))
            ->setErrorMessages(pathError.isEmpty() ? QStringList{} : QStringList{pathError});
        return nameError.isEmpty() && pathError.isEmpty();
    }
    void updateSizeLabelTheme() {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        auto font = m_form->font();
        font.setPixelSize(qMax(12, qRound(theme.fontSize)));
        font.setWeight(QFont::Normal);
        m_sizeLabel->setFont(font);
        auto palette = m_sizeLabel->palette();
        palette.setColor(QPalette::WindowText, theme.colorText);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, theme.colorTextDisabled);
        m_sizeLabel->setPalette(palette);
    }
    void updateOutputDescriptionTheme() {
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        auto font = m_form->font();
        font.setPixelSize(qMax(10, qRound(theme.fontSizeSM)));
        font.setWeight(QFont::Normal);
        m_outputDescription->setFont(font);
        m_outputDescription->setFixedHeight(m_outputDescription->fontMetrics().height());
        auto palette = m_outputDescription->palette();
        palette.setColor(QPalette::WindowText, theme.colorTextSecondary);
        m_outputDescription->setPalette(palette);
    }
    QString formattedOutputBytes(qint64 bytes) const {
        constexpr qint64 kibibyte = 1024;
        constexpr qint64 mebibyte = kibibyte * 1024;
        constexpr qint64 gibibyte = mebibyte * 1024;
        if (bytes < kibibyte)
            return tr("%1B").arg(bytes);
        if (bytes < mebibyte)
            return tr("%1KB").arg(qRound64(double(bytes) / kibibyte));
        if (bytes < gibibyte)
            return tr("%1MB").arg(qRound64(double(bytes) / mebibyte));
        return tr("%1GB").arg(qRound64(double(bytes) / gibibyte));
    }
    void updateOutputDescription() {
        if (!m_encoded || !m_requestedOptions || m_encoded->options != *m_requestedOptions ||
            m_state.sourceSize.width() <= 0 || !QFileInfo::exists(m_encoded->path)) {
            m_outputDescription->clear();
            return;
        }
        const QSize size = m_encoded->options.size;
        QString scale = QString::number(100.0 * size.width() / m_state.sourceSize.width(), 'f', 2);
        while (scale.endsWith(QLatin1Char('0')))
            scale.chop(1);
        if (scale.endsWith(QLatin1Char('.')))
            scale.chop(1);
        m_outputDescription->setText(
            tr("%1x%2(%3%) · %4")
                .arg(size.width())
                .arg(size.height())
                .arg(scale)
                .arg(formattedOutputBytes(QFileInfo(m_encoded->path).size())));
    }
    void commitDimensionEdit() {
        if (auto* focus = QApplication::focusWidget();
            focus && (focus == m_width || m_width->isAncestorOf(focus) || focus == m_height ||
                      m_height->isAncestorOf(focus)))
            focus->clearFocus();
    }
    void syncDimensionInputs(bool preserveEmpty = false) {
        const auto sync = [this, preserveEmpty](AdInputNumber* input, int pixels, int source) {
            const QSignalBlocker blocker(input);
            const bool empty = preserveEmpty && !input->hasValue();
            input->setDecimals(m_percentage ? 2 : 0);
            input->setRange(0, m_percentage && source > 0 ? 100.0 * kMaximumDimension / source
                                                          : kMaximumDimension);
            input->setSuffixText(m_percentage ? QStringLiteral("%") : QString());
            if (empty)
                input->clear();
            else
                input->setValue(m_percentage && source > 0 ? 100.0 * pixels / source : pixels);
        };
        // Display rounding must never feed back into the exact pixel export dimensions.
        sync(m_width, m_state.output.size.width(), m_state.sourceSize.width());
        sync(m_height, m_state.output.size.height(), m_state.sourceSize.height());
    }
    void dimensionChanged(bool width, double value) {
        const QSignalBlocker widthBlock(m_width);
        const QSignalBlocker heightBlock(m_height);
        if (m_percentage) {
            const auto pixels = [value](int source) {
                return value > 0 ? qRound(std::clamp(value * source / 100.0, 1.0,
                                                     double(kMaximumDimension)))
                                 : 0;
            };
            if (width)
                m_state.output.size.setWidth(pixels(m_state.sourceSize.width()));
            else
                m_state.output.size.setHeight(pixels(m_state.sourceSize.height()));
            if (m_state.lockAspectRatio && value > 0) {
                m_state.output.size =
                    QSize(pixels(m_state.sourceSize.width()), pixels(m_state.sourceSize.height()));
                (width ? m_height : m_width)->setValue(value);
            }
        } else {
            m_state.setDimension(width, qRound(value));
            if (width && m_state.lockAspectRatio)
                m_height->setValue(m_state.output.size.height());
            if (!width && m_state.lockAspectRatio)
                m_width->setValue(m_state.output.size.width());
        }
        changed();
    }
    void updateControls() {
        m_quality->setEnabled(m_state.output.format != Format::Png &&
                              m_state.output.format != Format::Bmp);
        AdSlider::Mark minimumMark;
        minimumMark.label = tr("0%");
        AdSlider::Mark maximumMark;
        maximumMark.label =
            formatSupportsLossless(m_state.output.format) ? tr("Lossless") : tr("100%");
        m_quality->setMarks(
            {{m_quality->minimum(), minimumMark}, {m_quality->maximum(), maximumMark}});
    }
    void changed() {
        m_state.output.pdfTitle = QFileInfo(m_state.outputPath()).completeBaseName();
        m_preview->setPdfLayout(
            m_state.output.format == Format::Pdf
                ? screenshot_pdf::layout(m_state.output.size, m_state.output.pdfPageSize)
                : screenshot_pdf::Layout{});
        updateControls();
        if (m_closed || m_saving || !m_source.rows.isValid())
            return;
        validateFields();
        const QString error = m_state.imageValidationError();
        const auto options = error.isEmpty()
                                 ? std::make_optional(pipeline::normalizedOptions(m_state.output))
                                 : std::nullopt;
        if (options && options == m_requestedOptions)
            return;
        m_requestedOptions = options;
        m_outputDescription->clear();
        ++m_generation;
        m_job.cancel();
        cancelDecode();
        showError(error);
        if (!options) {
            m_preview->setBusy(false);
            return;
        }
        m_preview->setBusy(true);
        startRender();
    }
    void loadSource() {
        const bool started = m_artifact->requestRowSource(
            this, [this](ScreenshotImageRowSource rows, QString error) {
                if (m_closed)
                    return;
                if (!error.isEmpty() || !rows.isValid()) {
                    sourceFailed(error);
                    return;
                }
                auto result = std::make_shared<pipeline::Source>();
                m_job = ScreenshotExportCoordinator::shared().submit(
                    this, ScreenshotExportCoordinator::Priority::Foreground,
                    [rows, result](const ScreenshotExportCancellation& cancellation) {
                        ScreenshotExportTaskResult status;
                        *result = pipeline::prepare(rows, cancellation, &status.error);
                        if (!result->rows.isValid())
                            status.failureStage = ScreenshotExportFailureStage::Source;
                        return status;
                    },
                    [this, result](ScreenshotExportTaskResult status) {
                        m_job = {};
                        if (m_closed)
                            return;
                        if (!status.succeeded() || !result->rows.isValid()) {
                            sourceFailed(status.error);
                            return;
                        }
                        m_source = *result;
                        m_state.sourceSize = result->rows.size;
                        m_state.output.size = m_state.sourceSize;
                        syncDimensionInputs();
                        m_form->setDisabled(false);
                        m_modal->acceptButton()->setEnabled(true);
                        m_preview->setSource(m_source.preview, m_state.sourceSize);
                        m_preview->setOutput(m_source.preview);
                        changed();
                    });
                if (!m_job.isValid())
                    sourceFailed(tr("The screenshot export queue is full"));
            });
        if (!started)
            sourceFailed(tr("The screenshot could not be prepared"));
    }
    void sourceFailed(const QString& error) {
        m_preview->setBusy(false);
        showError(tr("The screenshot could not be prepared: %1").arg(error));
    }
    void startRender() {
        if (m_closed || !m_source.rows.isValid() || !m_requestedOptions)
            return;
        if (m_encoded && m_encoded->options == *m_requestedOptions) {
            updateOutputDescription();
            if (m_saving)
                writeFile();
            else
                startDecode();
            return;
        }
        if (m_renderRunning)
            return;
        const quint64 generation = m_generation;
        const auto options = *m_requestedOptions;
        const auto prepared =
            m_preparedPixels && m_preparedPixels->size == options.size ? m_preparedPixels : nullptr;
        auto encoded = std::make_shared<std::shared_ptr<pipeline::Encoded>>();
        m_renderRunning = true;
        m_job = ScreenshotExportCoordinator::shared().submit(
            this, ScreenshotExportCoordinator::Priority::Foreground,
            [source = m_source, options, prepared, cachedPdf = m_encoded ? m_encoded->pdf : nullptr,
             encoded](const ScreenshotExportCancellation& cancellation) {
                ScreenshotExportTaskResult result;
                auto pixels = prepared;
                if (!pixels)
                    pixels =
                        pipeline::preparePixels(source, options.size, cancellation, &result.error);
                if (pixels)
                    *encoded = pipeline::render(std::move(pixels), options, cancellation,
                                                &result.error, cachedPdf);
                if (!*encoded)
                    result.failureStage = ScreenshotExportFailureStage::Render;
                return result;
            },
            [this, generation, encoded](ScreenshotExportTaskResult result) {
                m_renderRunning = false;
                m_job = {};
                if (m_closed)
                    return;
                if (*encoded && (*encoded)->pixels) {
                    m_preparedPixels = (*encoded)->pixels;
                    setProperty("preparedPixelsIdentity",
                                QVariant::fromValue<qulonglong>(
                                    reinterpret_cast<quintptr>(m_preparedPixels.get())));
                }
                if (generation != m_generation) {
                    startRender();
                    return;
                }
                if (!result.succeeded() || !*encoded) {
                    saveFailed(tr("The export could not be prepared: %1").arg(result.error));
                    return;
                }
                m_encoded = *encoded;
                updateOutputDescription();
                setProperty("encodedGeneration", QVariant::fromValue(generation));
                startRender();
            });
        if (!m_job.isValid()) {
            m_renderRunning = false;
            saveFailed(tr("The screenshot export queue is full"));
        }
    }
    void cancelDecode() {
        ++m_decodeSerial;
        m_decodeJob.cancel();
        m_decodeJob = {};
    }
    void startDecode() {
        if (m_closed || m_saving || !m_encoded || !m_requestedOptions ||
            m_encoded->options != *m_requestedOptions)
            return;
        if (m_renderedPreviewOptions == m_requestedOptions) {
            m_preview->setBusy(false);
            return;
        }
        if (m_encoded->codecResult.roundTrip == snow::image::PixelRoundTrip::exact &&
            m_encoded->pixels && !m_encoded->pixels->exactImage.isNull()) {
            m_renderedPreviewOptions = m_encoded->options;
            m_preview->setOutput(m_encoded->pixels->exactImage);
            m_preview->setBusy(false);
            setProperty("previewGeneration", QVariant::fromValue(m_generation));
            return;
        }
        if (m_decodeJob.isValid())
            return;
        m_preview->setBusy(true);
        setProperty("previewDecodeCount", property("previewDecodeCount").toInt() + 1);
        const quint64 generation = m_generation;
        const quint64 serial = ++m_decodeSerial;
        const auto options = m_encoded->options;
        m_decodeJob = ScreenshotExportCoordinator::shared().submit(
            this, ScreenshotExportCoordinator::Priority::Foreground,
            [encoded = m_encoded](const ScreenshotExportCancellation& cancellation) {
                ScreenshotExportTaskResult result;
                result.image = pipeline::decode(*encoded, cancellation, &result.error);
                if (result.image.isNull())
                    result.failureStage = ScreenshotExportFailureStage::Render;
                return result;
            },
            [this, generation, serial, options](ScreenshotExportTaskResult result) {
                if (m_closed || serial != m_decodeSerial || generation != m_generation || m_saving)
                    return;
                m_decodeJob = {};
                m_preview->setBusy(false);
                if (!result.succeeded()) {
                    showError(
                        tr("The encoded preview could not be displayed: %1").arg(result.error));
                    return;
                }
                m_renderedPreviewOptions = options;
                m_preview->setOutput(std::move(result.image));
                setProperty("previewGeneration", QVariant::fromValue(generation));
            });
        if (!m_decodeJob.isValid()) {
            m_preview->setBusy(false);
            showError(tr("The screenshot export queue is full"));
        }
    }
    void beginSave() {
        if (m_saving || m_closed)
            return;
        static_cast<void>(snow_shot::storage::ScreenshotSettings().setLastManualSaveFormat(
            ScreenshotImageFileService::formatKey(m_state.output.format)));
        m_saving = true;
        m_savePath = m_state.outputPath();
        m_form->setDisabled(true);
        m_modal->setAcceptButtonBusy(true);
        m_modal->setCloseButtonVisible(false);
        m_modal->setCloseOnEscape(false);
        if (m_modal->rejectButton())
            m_modal->rejectButton()->setEnabled(false);
        cancelDecode();
        startRender();
    }
    void saveFailed(const QString& error) {
        m_saving = false;
        m_preview->setBusy(false);
        m_form->setDisabled(false);
        m_modal->setAcceptButtonBusy(false);
        m_modal->setCloseButtonVisible(true);
        m_modal->setCloseOnEscape(true);
        if (m_modal->rejectButton())
            m_modal->rejectButton()->setEnabled(true);
        validateFields();
        showError(error);
        startDecode();
    }
    void writeFile() {
        if (m_saveJob.isValid())
            return;
        auto adoptedPng = std::make_shared<std::optional<snow_shot::storage::PreparedPngImage>>();
        m_saveJob = ScreenshotExportCoordinator::shared().submit(
            this, ScreenshotExportCoordinator::Priority::Foreground,
            [encoded = m_encoded, path = m_savePath, sourceSize = m_state.sourceSize,
             adoptedPng](const ScreenshotExportCancellation& cancellation) {
                ScreenshotExportTaskResult result;
                const auto cancelled = [&cancellation] {
                    return cancellation.isCancellationRequested();
                };
                const auto saved =
                    encoded->pdf ? ScreenshotImageFileService::writePdf(
                                       *encoded->pdf, path,
                                       ScreenshotPdfOptions{encoded->options.pdfPageSize,
                                                            encoded->options.quality,
                                                            {},
                                                            QDateTime::currentDateTimeUtc()},
                                       cancelled)
                                 : ScreenshotImageFileService::writeEncodedFile(
                                       encoded->path, path, encoded->options.format, cancelled);
                result.savedPath = saved.path;
                result.error = saved.error;
                if (!saved.succeeded())
                    result.failureStage = ScreenshotExportFailureStage::File;
                if (saved.succeeded() && encoded->options.format == Format::Png &&
                    encoded->options.size == sourceSize &&
                    !cancellation.isCancellationRequested()) {
                    QFile file(encoded->path);
                    if (file.open(QIODevice::ReadOnly)) {
                        *adoptedPng = snow_shot::storage::PreparedPngImage::fromBytes(
                            sourceSize, file.readAll());
                    }
                }
                return result;
            },
            [this, adoptedPng](ScreenshotExportTaskResult result) {
                m_saveJob = {};
                if (m_closed)
                    return;
                if (!result.succeeded()) {
                    saveFailed(tr("The screenshot could not be saved: %1").arg(result.error));
                    return;
                }
                if (adoptedPng->has_value())
                    static_cast<void>(m_artifact->adoptCanonicalPng(std::move(**adoptedPng)));
                static_cast<void>(
                    snow_shot::storage::ScreenshotSettings().setLastManualSaveDirectory(
                        QFileInfo(result.savedPath).absolutePath()));
                m_saving = false;
                const auto saved = m_saved;
                const auto path = result.savedPath;
                m_modal->accept();
                if (saved)
                    saved(path);
            });
        if (!m_saveJob.isValid())
            saveFailed(tr("The screenshot export queue is full"));
    }

    AdModal* m_modal;
    std::shared_ptr<ScreenshotExportArtifact> m_artifact;
    ScreenshotSaveAsFileDialog::Saved m_saved;
    ScreenshotSaveDialogState m_state;
    pipeline::Source m_source;
    std::optional<ScreenshotSaveExportOptions> m_requestedOptions;
    std::optional<ScreenshotSaveExportOptions> m_renderedPreviewOptions;
    std::shared_ptr<pipeline::PreparedPixels> m_preparedPixels;
    std::shared_ptr<pipeline::Encoded> m_encoded;
    QString m_savePath;
    ScreenshotExportJobHandle m_job;
    ScreenshotExportJobHandle m_decodeJob;
    ScreenshotExportJobHandle m_saveJob;
    quint64 m_generation = 0;
    quint64 m_decodeSerial = 0;
    bool m_closed = false;
    bool m_saving = false;
    bool m_renderRunning = false;
    ScreenshotSavePreviewCanvas* m_preview = nullptr;
    QLabel* m_outputDescription = nullptr;
    AdForm* m_form = nullptr;
    DirectoryPathInput* m_directory = nullptr;
    AdLineEdit* m_filename = nullptr;
    AdSelect* m_format = nullptr;
    AdInputNumber* m_width = nullptr;
    AdInputNumber* m_height = nullptr;
    QLabel* m_sizeLabel = nullptr;
    AdSegmented* m_sizeUnit = nullptr;
    bool m_percentage = false;
    AspectRatioLockButton* m_lock = nullptr;
    AdSlider* m_quality = nullptr;
    QLabel* m_error = nullptr;
    QWidget* m_shortcutsHost = nullptr;
    adqt::widgets::detail::FlowLayout* m_shortcutsLayout = nullptr;
    QVector<Shortcut> m_shortcuts;
    QPointer<AdContextMenu> m_menu;
    QPointer<AdModal> m_editor;
    QPointer<AdModal> m_overwrite;
};

// Labels translated indirectly by the reusable form builder.
[[maybe_unused]] constexpr const char* labels[] = {
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "Save path"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "File name"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "PDF"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "Image format"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "Width"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "Height"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "Quality"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "0%"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "100%"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "PNG"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "JPEG"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "BMP"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "WebP"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "JPEG XL"),
    QT_TRANSLATE_NOOP("ScreenshotSaveAsFileDialog", "AVIF"),
};
} // namespace

ScreenshotSaveDialogState ScreenshotSaveDialogState::initial(QSize size) {
    ScreenshotSaveDialogState result;
    const snow_shot::storage::ScreenshotSettings settings;
    const QFileInfo remembered(settings.lastManualSaveDirectory());
    const QString configured = settings.imageSaveDirectory().trimmed();
    if (remembered.isDir() && remembered.isReadable() && remembered.isWritable())
        result.directory = remembered.absoluteFilePath();
    else if (!configured.isEmpty() && QDir::isAbsolutePath(configured))
        result.directory = QDir::cleanPath(configured);
    else
        result.directory = ScreenshotImageFileService::saveDialogDirectory({}, configured);
    result.filename =
        ScreenshotImageFileService::suggestedBaseName(settings.manualSaveFilenameFormat());
    result.output.pdfPageSize = screenshot_pdf::pageSizeForKey(settings.pdfPageSize());
    result.sourceSize = size;
    result.output.size = size;
    result.output.format =
        ScreenshotImageFileService::formatForKey(settings.lastManualSaveFormat());
    return result;
}
void ScreenshotSaveDialogState::setDimension(bool width, int value) {
    if (width)
        output.size.setWidth(value);
    else
        output.size.setHeight(value);
    if (!lockAspectRatio || sourceSize.isEmpty() || value <= 0)
        return;
    const double other = width ? double(value) * sourceSize.height() / sourceSize.width()
                               : double(value) * sourceSize.width() / sourceSize.height();
    const int dimension = qRound(std::clamp(other, 1.0, 1000000.0));
    if (width)
        output.size.setHeight(dimension);
    else
        output.size.setWidth(dimension);
}
QString ScreenshotSaveDialogState::validationError() const {
    if (directory.trimmed().isEmpty() || !QDir::isAbsolutePath(directory.trimmed()))
        return QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                           "Please enter an absolute directory path");
    static const QRegularExpression invalid(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
    static const QRegularExpression reserved(
        QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"),
        QRegularExpression::CaseInsensitiveOption);
    const QString name = filename.trimmed();
    if (name.isEmpty() || invalid.match(name).hasMatch() || reserved.match(name).hasMatch() ||
        name == QStringLiteral(".") || name == QStringLiteral("..") ||
        name.endsWith(QLatin1Char('.')))
        return QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                           "Please enter a valid file name");
    return imageValidationError();
}
QString ScreenshotSaveDialogState::imageValidationError() const {
    const QSize limits = pipeline::encoderLimits(output.format);
    if (output.size.isEmpty() || output.size.width() > limits.width() ||
        output.size.height() > limits.height() ||
        qint64(output.size.width()) * output.size.height() > qint64{2} * 1024 * 1024 * 1024)
        return QCoreApplication::translate("ScreenshotSaveAsFileDialog",
                                           "The dimensions are not supported by this image format");
    return {};
}
QString ScreenshotSaveDialogState::outputPath() const {
    return ScreenshotImageFileService::normalizedPath(
        QDir(directory.trimmed()).filePath(filename.trimmed()), output.format);
}
bool ScreenshotSaveDialogState::lossless() const {
    return output.quality == 100 && formatSupportsLossless(output.format);
}

bool ScreenshotSaveAsFileDialog::open(QObject* lifetime, QWidget* owner,
                                      std::shared_ptr<ScreenshotExportArtifact> source, Saved saved,
                                      Finished finished, QWidget* stackingOwner) {
    if (!lifetime || !owner || !source || !source->isValid())
        return false;
    const QPointer<QWidget> stackingWindow(
        stackingOwner && stackingOwner->isVisible() ? stackingOwner : nullptr);
    auto* modal = new AdModal(lifetime);
    modal->setObjectName(QStringLiteral("screenshotSaveAsFileModal"));
    modal->setOwnerWindow(owner);
    modal->setMode(AdModal::Mode::Window);
    if (stackingWindow) {
        QObject::connect(stackingWindow, &QObject::destroyed, modal, &AdModal::reject);
    }
    modal->setWindowModality(Qt::ApplicationModal);
    modal->setCentered(true);
    modal->setMaskVisible(false);
    modal->setCloseOnMaskClick(false);
    modal->setClosePolicy(AdModal::ClosePolicy::Manual);
    const QRect available = owner->screen()->availableGeometry();
    modal->setPreferredWidth(std::min(1080, available.width() - 40));
    auto* content = new SaveContent(modal, std::move(source), std::move(saved));
    content->setFixedHeight(std::max(240, std::min(540, available.height() - 180)));
    modal->setContentWidget(content);
    QObject::connect(modal, &AdModal::closeRequested, content,
                     [modal, content](AdModal::CloseReason reason) {
                         if (content->saving())
                             return;
                         if (reason == AdModal::CloseReason::OkAction)
                             content->save();
                         else
                             modal->reject();
                     });
    QObject::connect(modal, &AdModal::finished, content,
                     [content, finished = std::move(finished)](AdModal::DialogCode code) {
                         content->cancel(code != AdModal::DialogCode::Accepted);
                         if (finished)
                             finished(code == AdModal::DialogCode::Accepted);
                     });
    QObject::connect(modal, &AdModal::finished, modal, &QObject::deleteLater);
    if (lifetime != owner)
        QObject::connect(owner, &QObject::destroyed, modal, &AdModal::reject);
    modal->open();
    // Preserve the display owner for centering and lifetime, while making the native dialog
    // an owned window of the toolbar. Raising an owner cannot cover its owned dialog.
    if (stackingWindow) {
        QWidget* surface = content->window();
        surface->windowHandle()->setTransientParent(stackingWindow->windowHandle());
#if defined(Q_OS_WIN)
        // QWidget keeps its original parent; synchronize the HWND owner as well as QWindow.
        if (QApplication::platformName() == QStringLiteral("windows")) {
            const auto dialogHwnd = reinterpret_cast<HWND>(surface->winId());
            const auto toolbarHwnd = reinterpret_cast<HWND>(stackingWindow->winId());
            SetWindowLongPtr(dialogHwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(toolbarHwnd));
        }
#endif
        surface->raise();
    }
    content->installFooter();
    modal->acceptButton()->setEnabled(false);
    return true;
}
bool ScreenshotSaveAsFileDialog::open(QObject* lifetime, QWidget* owner, const QImage& image,
                                      Saved saved, Finished finished, QWidget* stackingOwner) {
    if (image.isNull())
        return false;
    return open(
        lifetime, owner,
        std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(image)),
        std::move(saved), std::move(finished), stackingOwner);
}
