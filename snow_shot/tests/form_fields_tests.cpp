#include "snow_shot/presentation/components/formfields.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "widgets/modal.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QTranslator>
#include <QVBoxLayout>

#include <cstdlib>
#include <iostream>
#include <utility>

namespace fields = snow_shot::presentation::components::form_fields;
namespace styles = snow_shot::presentation::styles;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::PolishRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::processEvents();
}

fields::Metadata metadata(const char* id) {
    fields::Metadata result;
    result.id = QString::fromLatin1(id);
    result.label = {"FormFieldsTests", "Field label"};
    result.description = {"FormFieldsTests", "Field description"};
    result.placeholder = {"FormFieldsTests", "Enter a value"};
    result.suffix = {"FormFieldsTests", " units"};
    return result;
}

class Translator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }
    QString translate(const char* context, const char* source, const char*, int) const override {
        return QByteArray(context) == QByteArrayLiteral("FormFieldsTests")
                   ? QStringLiteral("Translated: ") + QString::fromUtf8(source)
                   : QString();
    }
};

void optionalMetadataAndChoiceLabels() {
    QWidget owner;
    fields::Options options;
    options.parent = &owner;
    const auto text = fields::text({QStringLiteral("minimal-text")}, options);
    text.field->syncValue(QStringLiteral("draft"));
    require(text.item()->fieldKey() == QStringLiteral("minimal-text") &&
                text.item()->label().isEmpty() && text.editor->placeholderText().isEmpty() &&
                text.editor->accessibleDescription().isEmpty() &&
                text.field->value() == QStringLiteral("draft"),
            "ID-only metadata creates a usable field without optional copy");

    const auto select =
        fields::select({QStringLiteral("minimal-select"), {"FormFieldsTests", "Field label"}},
                       {{QStringLiteral("translated"), {"FormFieldsTests", "Option label"}},
                        {QStringLiteral("raw"), {}, QStringLiteral("Runtime label")}},
                       options);
    select.field->syncValue(QStringLiteral("translated"));
    Translator translator;
    require(QCoreApplication::installTranslator(&translator), "install minimal field translator");
    select.field->retranslateUi();
    const auto choices = select.editor->options();
    require(choices.size() == 2 &&
                choices.at(0).label == QStringLiteral("Translated: Option label") &&
                choices.at(1).label == QStringLiteral("Runtime label") && !choices.at(0).disabled &&
                !choices.at(1).disabled && select.field->value() == QStringLiteral("translated"),
            "partial choices preserve translated and runtime labels with enabled defaults");
    QCoreApplication::removeTranslator(&translator);
}

void constructionKeepsItemsInTheirOwner() {
    class ItemShowObserver final : public QObject {
      public:
        int topLevelShows = 0;
        bool eventFilter(QObject* watched, QEvent* event) override {
            if (event->type() == QEvent::Show) {
                if (auto* item = qobject_cast<adqt::widgets::AdFormItem*>(watched);
                    item && item->isWindow()) {
                    ++topLevelShows;
                }
            }
            return false;
        }
    } observer;
    QWidget owner;
    QApplication::instance()->installEventFilter(&observer);
    fields::Options options;
    options.parent = &owner;
    options.presentation = fields::Presentation::SettingsRow;
    const auto row = fields::switchField(metadata("owned-row"), options);
    options.presentation = fields::Presentation::VerticalDialog;
    const auto dialog = fields::text(metadata("owned-dialog"), options);
    options.parent = nullptr;
    const auto detachedDialog = fields::text(metadata("detached-dialog"), options);
    options.presentation = fields::Presentation::SettingsRow;
    const auto detachedRow = fields::switchField(metadata("detached-row"), options);
    detachedDialog.field->setFeedback({QStringLiteral("A pending draft")});
    detachedDialog.field->retranslateUi();
    detachedDialog.field->applyTheme(styles::ThemeManager::instance().themeColorScheme());
    detachedRow.field->setFeedback();
    detachedRow.field->retranslateUi();
    QApplication::instance()->removeEventFilter(&observer);
    require(owner.isAncestorOf(row.viewWidget()) && owner.isAncestorOf(dialog.viewWidget()) &&
                detachedDialog.viewWidget()->parentWidget() == nullptr &&
                detachedRow.viewWidget()->parentWidget() == nullptr &&
                detachedDialog.viewWidget()->isHidden() && detachedRow.viewWidget()->isHidden() &&
                observer.topLevelShows == 0,
            "field construction keeps form items owned without transient top-level windows");
    detachedDialog.viewWidget()->show();
    detachedDialog.field->retranslateUi();
    detachedDialog.field->setFeedback();
    require(detachedDialog.viewWidget()->isVisible(),
            "a caller can show a standalone field without later refresh hiding it");
    delete detachedDialog.viewWidget();
    delete detachedRow.viewWidget();
}

void presentationsAndFeedback() {
    QWidget owner;
    auto* layout = new QVBoxLayout(&owner);
    fields::Options rowOptions;
    rowOptions.parent = &owner;
    rowOptions.presentation = fields::Presentation::SettingsRow;
    const auto row = fields::text(metadata("row"), rowOptions);
    layout->addWidget(row.viewWidget());
    fields::Options dialogOptions;
    dialogOptions.parent = &owner;
    const auto dialog = fields::text(metadata("dialog"), dialogOptions);
    layout->addWidget(dialog.viewWidget());
    owner.resize(760, 320);
    owner.show();
    flushEvents();
    require(row.editor->width() == 230 && row.item()->width() == 230,
            "settings fields preserve the common control column width");
    require(dialog.viewWidget() == dialog.item() && dialog.editor->width() > 230,
            "vertical dialog editors fill the available field width");
    require(row.item()->label().isEmpty() &&
                dialog.item()->label() == QStringLiteral("Field label"),
            "the settings copy column and vertical field use their respective label presets");
    require(dialog.item()->tooltipText() == QStringLiteral("Field description"),
            "dialog descriptions use the shared label tooltip");
    row.field->setFeedback({QStringLiteral("Invalid value")});
    dialog.field->setFeedback({}, {QStringLiteral("Check this value")});
    flushEvents();
    require(row.field->feedbackLabel()->isVisible() &&
                row.field->feedbackLabel()->text() == QStringLiteral("Invalid value") &&
                row.item()->validateStatus() == adqt::widgets::AdFormItem::ValidateStatus::Error,
            "settings errors are visible inline with error status");
    require(dialog.item()->validateStatus() == adqt::widgets::AdFormItem::ValidateStatus::Warning,
            "dialog warnings use the same feedback API");
    dialog.field->reserveFeedbackHeight(48);
    dialog.field->setFeedback({QStringLiteral("Invalid")});
    flushEvents();
    const int height = dialog.item()->sizeHint().height();
    dialog.field->setFeedback();
    flushEvents();
    require(!dialog.field->feedbackLabel()->isVisible() &&
                dialog.item()->sizeHint().height() == height,
            "reserved feedback stays hidden without changing field height");
    row.field->setFieldVisible(false);
    require(row.viewWidget()->isHidden(), "visibility applies to the entire settings field");
}

void settingsRowsKeepNaturalHeight() {
    QWidget owner;
    auto* layout = new QVBoxLayout(&owner);
    layout->setAlignment(Qt::AlignTop);
    fields::Options options;
    options.parent = &owner;
    options.presentation = fields::Presentation::SettingsRow;
    auto rowMetadata = metadata("natural-height-row");
    rowMetadata.label = {};
    rowMetadata.description = {};
    const auto row = fields::text(rowMetadata, options);
    layout->addWidget(row.viewWidget());
    owner.resize(760, 320);
    owner.show();
    flushEvents();
    const auto requireNaturalHeight = [&] {
        require(row.viewWidget()->height() == row.editor->height(),
                "settings fields without copy or feedback add no space below the control");
    };
    requireNaturalHeight();
    row.field->applyTheme(styles::ThemeManager::instance().themeColorScheme());
    row.field->syncValue(QStringLiteral("refreshed"));
    flushEvents();
    requireNaturalHeight();
    row.field->setFeedback({QStringLiteral("Invalid value")});
    flushEvents();
    require(row.field->feedbackLabel()->isVisible() &&
                row.viewWidget()->height() > row.editor->height() &&
                row.viewWidget()->rect().contains(
                    QRect(row.field->feedbackLabel()->mapTo(row.viewWidget(), QPoint()),
                          row.field->feedbackLabel()->size())),
            "settings rows grow to contain inline feedback");
    row.field->setFeedback();
    // Clearing feedback posts layout requests through the item, form, row and owner.
    for (int i = 0; i < 4; ++i)
        flushEvents();
    requireNaturalHeight();
}

void descriptionOverrides() {
    QWidget owner;
    fields::Options options;
    options.parent = &owner;
    options.presentation = fields::Presentation::SettingsRow;
    auto rowMetadata = metadata("description-row");
    rowMetadata.description = {};
    const auto row = fields::text(rowMetadata, options);
    const QString temporary = QStringLiteral("Temporary runtime description");
    row.field->setDescriptionOverride(temporary);
    QLabel* description = nullptr;
    for (auto* label : row.viewWidget()->findChildren<QLabel*>()) {
        if (label->text() == temporary) {
            description = label;
            break;
        }
    }
    require(description && !description->isHidden() && row.editor->toolTip() == temporary &&
                row.item()->tooltipText() == temporary &&
                row.editor->accessibleDescription().contains(temporary),
            "runtime descriptions appear in settings copy, tooltips and accessibility");
    row.field->setDescriptionOverride(temporary, true);
    auto& themes = snow_shot::presentation::styles::ThemeManager::instance();
    require(description->palette().color(QPalette::WindowText) ==
                themes.themeColorScheme().map.colorErrorText,
            "runtime error descriptions use the theme's error color");
    row.field->applyTheme(themes.themeColorScheme());
    require(description->palette().color(QPalette::WindowText) ==
                themes.themeColorScheme().map.colorErrorText,
            "theme refreshes preserve runtime description severity");
    row.field->setDescriptionOverride(temporary);
    require(description->palette().color(QPalette::WindowText) ==
                themes.themeColorScheme().map.colorTextSecondary,
            "clearing runtime severity restores the normal description color");
    const QString error = QStringLiteral("Invalid value");
    row.field->setFeedback({error});
    row.field->setDescriptionOverride({});
    require(description->isHidden() && description->text().isEmpty() &&
                row.editor->toolTip().isEmpty() && row.item()->tooltipText().isEmpty() &&
                !row.editor->accessibleDescription().contains(temporary) &&
                row.editor->accessibleDescription().contains(error),
            "clearing an override restores an absent catalog description and keeps feedback");

    row.field->setFeedback();
    const QString callerDescription = QStringLiteral("Caller-owned description");
    row.editor->setToolTip(callerDescription);
    row.editor->setAccessibleDescription(callerDescription);
    row.item()->setTooltipText(callerDescription);
    row.field->setDescriptionOverride({});
    row.field->retranslateUi();
    require(row.editor->toolTip() == callerDescription &&
                row.item()->tooltipText() == callerDescription &&
                row.editor->accessibleDescription() == callerDescription,
            "invalid metadata preserves caller-owned descriptions after an override is cleared");

    options.presentation = fields::Presentation::VerticalDialog;
    const auto dialog = fields::text(metadata("description-dialog"), options);
    dialog.field->setDescriptionOverride(temporary);
    dialog.field->setDescriptionOverride({});
    require(dialog.editor->toolTip() == QStringLiteral("Field description") &&
                dialog.item()->tooltipText() == QStringLiteral("Field description") &&
                dialog.editor->accessibleDescription() == QStringLiteral("Field description"),
            "clearing a dialog override restores its translated catalog description");
}

void reservedFeedbackKeepsModalGeometry() {
    QWidget owner;
    owner.resize(760, 600);
    owner.show();
    flushEvents();
    adqt::widgets::AdModal modal(&owner);
    modal.setOwnerWindow(&owner);
    modal.setCentered(true);
    modal.setPreferredWidth(520);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* form = new adqt::widgets::AdForm(content);
    fields::configureForm(form);
    layout->addWidget(form);
    fields::Options options;
    options.form = form;
    const auto field = fields::text(metadata("reserved-feedback"), options);
    const int naturalMinimumHeight = field.item()->minimumHeight();
    const int naturalFieldHint = field.item()->sizeHint().height();
    const int naturalLayoutMinimum = field.item()->layout()->minimumSize().height();
    const int naturalContentHint = content->sizeHint().height();
    const QString longMessage =
        QStringLiteral("First message line\nSecond message line\nThird message line");
    field.field->reserveFeedbackHeight(QFontMetrics(field.field->feedbackLabel()->font()).height() *
                                       4);
    field.field->setFeedback({longMessage});
    modal.setContentWidget(content);
    modal.open();
    flushEvents();
    class HeightObserver final : public QObject {
      public:
        int expectedHeight = 0;
        int changes = 0;
        bool eventFilter(QObject* watched, QEvent* event) override {
            if (event->type() == QEvent::Resize) {
                if (auto* widget = qobject_cast<QWidget*>(watched);
                    widget && widget->height() != expectedHeight) {
                    ++changes;
                }
            }
            return false;
        }
    } observer;
    observer.expectedHeight = content->height();
    content->installEventFilter(&observer);
    field.field->setFeedback({QStringLiteral("Short message")});
    flushEvents();
    field.field->setFeedback();
    flushEvents();
    require(field.field->feedbackLabel()->isHidden(),
            "empty feedback keeps its reserved space without showing a message");
    field.field->setFeedback({longMessage});
    flushEvents();
    field.field->setFeedback();
    flushEvents();
    require(observer.changes == 0 && content->height() == observer.expectedHeight,
            "long, short and cleared feedback cannot resize a modal during native refresh");
    content->removeEventFilter(&observer);
    field.field->reserveFeedbackHeight(0);
    flushEvents();
    require(field.item()->minimumHeight() == naturalMinimumHeight &&
                field.item()->sizeHint().height() == naturalFieldHint &&
                field.item()->layout()->minimumSize().height() == naturalLayoutMinimum &&
                content->sizeHint().height() == naturalContentHint,
            "releasing a feedback reservation restores natural field and content sizing");
    modal.reject();
}

void editAndCommitTiming() {
    QWidget owner;
    fields::Options options;
    options.parent = &owner;
    options.commitPolicy = fields::CommitPolicy::OnFinish;
    const auto text = fields::text(metadata("text"), options);
    int edits = 0;
    int commits = 0;
    QObject::connect(text.field, &fields::FormField::valueEdited, &owner,
                     [&edits](const QVariant&) { ++edits; });
    QObject::connect(text.field, &fields::FormField::valueCommitted, &owner,
                     [&commits](const QVariant&) { ++commits; });
    text.field->syncValue(QStringLiteral("initial"));
    require(edits == 0 && commits == 0, "loading a field emits no user or persistence event");
    text.editor->setText(QStringLiteral("draft"));
    require(edits == 1 && commits == 0, "text changes update the draft without committing");
    require(QMetaObject::invokeMethod(text.editor, "editingFinished"), "text finish signal exists");
    require(commits == 1 && text.field->value() == QStringLiteral("draft"),
            "text editing completion commits the current value once");

    options.commitPolicy = fields::CommitPolicy::Immediate;
    const auto number = fields::number(metadata("number"), {1, 16, 1, 0, false}, options);
    int numberCommits = 0;
    QObject::connect(number.field, &fields::FormField::valueCommitted, &owner,
                     [&numberCommits](const QVariant&) { ++numberCommits; });
    number.field->syncValue(4);
    number.editor->setValue(7);
    require(numberCommits == 1 && number.field->value().toInt() == 7,
            "numeric changes commit once with configured bounds");

    const auto toggle = fields::switchField(metadata("toggle"), options);
    int toggles = 0;
    int toggleCommits = 0;
    QObject::connect(toggle.editor, &QAbstractButton::toggled, &owner, [&toggles] { ++toggles; });
    QObject::connect(toggle.field, &fields::FormField::valueCommitted, &owner,
                     [&toggleCommits] { ++toggleCommits; });
    toggle.field->syncValue(true);
    require(toggle.editor->isChecked() && toggles == 1 && toggleCommits == 0,
            "silent switch synchronization preserves the native signal used to render its thumb");
    toggle.editor->setChecked(false);
    require(toggleCommits == 1, "a switch change commits once");
}

void compoundsAndFormValues() {
    QWidget owner;
    adqt::widgets::AdForm form(&owner);
    fields::configureForm(&form);
    fields::Options options;
    options.form = &form;
    options.commitPolicy = fields::CommitPolicy::OnFinish;
    const auto path = fields::directoryPath(metadata("path"), options);
    int edits = 0;
    int commits = 0;
    QObject::connect(path.field, &fields::FormField::valueEdited, &owner, [&edits] { ++edits; });
    QObject::connect(path.field, &fields::FormField::valueCommitted, &owner,
                     [&commits] { ++commits; });
    path.field->syncValue(QStringLiteral("C:/initial"));
    path.editor->lineEdit()->setText(QStringLiteral("C:/draft"));
    require(edits == 1 && commits == 0 &&
                form.value(QStringLiteral("path")) == QStringLiteral("C:/draft"),
            "compound path edits notify AdForm and the shared field exactly once");
    require(QMetaObject::invokeMethod(path.editor, "editingFinished"), "path finish signal exists");
    require(commits == 1, "path editing completion commits once");
    path.field->setFeedback({QStringLiteral("Invalid path")});
    require(path.editor->lineEdit()->status() == adqt::widgets::AdLineEdit::Status::Error,
            "compound path feedback reaches the actual editor");

    options.commitPolicy = fields::CommitPolicy::Immediate;
    const auto slider = fields::slider(metadata("slider"), {0, 100, 1, 0, false}, options);
    int sliderCommits = 0;
    QObject::connect(slider.field, &fields::FormField::valueCommitted, &owner,
                     [&sliderCommits] { ++sliderCommits; });
    slider.field->syncValue(10);
    slider.editor->setValue(20);
    require(sliderCommits == 1 && form.value(QStringLiteral("slider")).toInt() == 20,
            "slider composites propagate a single edit into the registered form");

    const auto color = fields::color(metadata("color"), options);
    int colorCommits = 0;
    QObject::connect(color.field, &fields::FormField::valueCommitted, &owner,
                     [&colorCommits] { ++colorCommits; });
    color.field->syncValue(QColor(Qt::red));
    color.editor->setValue(adqt::widgets::AdColorValue::solid(QColor(Qt::blue)));
    require(colorCommits == 0, "color previews never commit prematurely");
    QFocusEvent colorFocusOut(QEvent::FocusOut, Qt::PopupFocusReason);
    QCoreApplication::sendEvent(color.editor, &colorFocusOut);
    require(colorCommits == 0,
            "moving focus into color popup controls cannot commit an unfinished preview");
    require(QMetaObject::invokeMethod(color.editor, "editingFinished",
                                      Q_ARG(adqt::widgets::AdColorValue, color.editor->value())),
            "color finish signal exists");
    require(colorCommits == 1, "color editing completion commits once");

    path.field->setFieldEnabled(false);
    form.setDisabled(true);
    form.setDisabled(false);
    require(!path.editor->isEnabled() && slider.editor->isEnabled(),
            "form enable cycles restore explicitly disabled field states");
    QPointer<fields::FormField> controller = path.field;
    form.removeItem(path.item());
    delete path.viewWidget();
    require(controller.isNull(), "deleting the field view owns and deletes its controller");
}

void translationAndPrimitiveOptions() {
    QWidget owner;
    auto* layout = new QVBoxLayout(&owner);
    fields::Options options;
    options.parent = &owner;
    const auto text = fields::text(metadata("translated-text"), options);
    const auto choice = fields::select(
        metadata("choice"),
        {{QStringLiteral("stable"), {"FormFieldsTests", "Option label"}, {}, true}}, options);
    const auto password = fields::password(metadata("password"), options);
    options.readOnly = true;
    const auto area = fields::textArea(metadata("textarea"), options);
    layout->addWidget(text.viewWidget());
    layout->addWidget(choice.viewWidget());
    layout->addWidget(password.viewWidget());
    layout->addWidget(area.viewWidget());
    text.field->syncValue(QStringLiteral("unsaved text"));
    choice.field->syncValue(QStringLiteral("stable"));
    int commits = 0;
    QObject::connect(choice.field, &fields::FormField::valueCommitted, &owner,
                     [&commits] { ++commits; });
    owner.show();
    flushEvents();
    Translator translator;
    require(QCoreApplication::installTranslator(&translator), "install test translator");
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(text.viewWidget(), &languageChange);
    QCoreApplication::sendEvent(choice.viewWidget(), &languageChange);
    flushEvents();
    require(text.item()->label() == QStringLiteral("Translated: Field label") &&
                text.editor->placeholderText() == QStringLiteral("Translated: Enter a value") &&
                text.editor->accessibleName() == QStringLiteral("Translated: Field label"),
            "labels, placeholders and accessible names retranslate together");
    require(choice.editor->options().first().label == QStringLiteral("Translated: Option label") &&
                choice.field->value() == QStringLiteral("stable") && commits == 0 &&
                text.field->value() == QStringLiteral("unsaved text"),
            "retranslation preserves stable option values and unsaved edits without commits");
    require(!password.editor->textVisible() && area.editor->isReadOnly(),
            "password and read-only textarea options retain native functionality");
    text.field->setFeedback({QStringLiteral("Error")});
    text.field->setFeedback();
    require(!text.editor->accessibleDescription().contains(QStringLiteral("Error")),
            "clearing feedback removes stale accessible errors");
    auto scheme = styles::ThemeManager::instance().themeColorScheme();
    scheme.metricAlias.marginLG = 31;
    text.field->applyTheme(scheme);
    require(text.field->value() == QStringLiteral("unsaved text"),
            "theme updates preserve field values");
    QCoreApplication::removeTranslator(&translator);
}

void multiSelectAndEditableChoices() {
    QWidget owner;
    adqt::widgets::AdForm form(&owner);
    fields::configureForm(&form);
    fields::Options options;
    options.form = &form;
    const QVector<fields::Choice> choices{
        {QStringLiteral("alpha"), {"FormFieldsTests", "Alpha"}, {}, true},
        {QStringLiteral("beta"), {"FormFieldsTests", "Beta"}, {}, true}};
    const auto multi = fields::multiSelect(metadata("multi"), choices, options);
    int edits = 0;
    int commits = 0;
    QObject::connect(multi.field, &fields::FormField::valueEdited, &owner, [&edits] { ++edits; });
    QObject::connect(multi.field, &fields::FormField::valueCommitted, &owner,
                     [&commits] { ++commits; });
    const QVariantList initial{QStringLiteral("alpha")};
    const QVariantList selected{QStringLiteral("alpha"), QStringLiteral("beta")};
    multi.field->syncValue(initial);
    require(edits == 0 && commits == 0 && multi.field->value().toList() == initial,
            "multi-select synchronization preserves selections without shared events");
    multi.editor->setSelectedValues(selected);
    require(edits == 1 && commits == 1 && form.value(QStringLiteral("multi")).toList() == selected,
            "a multi-select edit updates the registered form and commits exactly once");
    multi.editor->setSearchText(QStringLiteral("no-matching-option"));
    require(multi.editor->selectedValues() == selected && edits == 1 && commits == 1,
            "multi-select filtering keeps the selected values and never commits");
    multi.field->syncValue(initial);
    require(multi.field->value().toList() == initial && edits == 1 && commits == 1,
            "later multi-select synchronization is also silent");

    options.editable = true;
    const auto combo = fields::comboBox(metadata("editable"), choices, options);
    int comboEdits = 0;
    int comboCommits = 0;
    QObject::connect(combo.field, &fields::FormField::valueEdited, &owner,
                     [&comboEdits] { ++comboEdits; });
    QObject::connect(combo.field, &fields::FormField::valueCommitted, &owner,
                     [&comboCommits] { ++comboCommits; });
    combo.field->syncValue(QStringLiteral("alpha"));
    combo.editor->setSearchText(QStringLiteral("filter-only"));
    require(combo.field->value() == QStringLiteral("alpha") && comboCommits == 0,
            "editable selector search text cannot replace the selected stable ID");
    combo.editor->lineEdit()->selectAll();
    const QString typedId = QStringLiteral("custom-model-id");
    QKeyEvent typed(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier, typedId);
    QCoreApplication::sendEvent(combo.editor->lineEdit(), &typed);
    require(combo.field->value() == typedId && form.value(QStringLiteral("editable")) == typedId &&
                comboEdits == 1 && comboCommits == 1,
            "typing an editable selector ID produces one shared edit and one commit");
    combo.editor->setSearchText(QStringLiteral("another-filter"));
    require(combo.field->value() == typedId && comboEdits == 1 && comboCommits == 1,
            "search refreshes preserve a typed ID without producing another edit");

    options.commitPolicy = fields::CommitPolicy::OnFinish;
    const auto finishedCombo = fields::comboBox(metadata("editable-finished"), choices, options);
    int finishedEdits = 0;
    int finishedCommits = 0;
    QObject::connect(finishedCombo.field, &fields::FormField::valueEdited, &owner,
                     [&finishedEdits] { ++finishedEdits; });
    QObject::connect(finishedCombo.field, &fields::FormField::valueCommitted, &owner,
                     [&finishedCommits] { ++finishedCommits; });
    finishedCombo.field->syncValue(QStringLiteral("alpha"));
    for (const QString& draft :
         {QStringLiteral("draft-model-id"), QStringLiteral("finished-model-id")}) {
        finishedCombo.editor->lineEdit()->selectAll();
        QKeyEvent draftKey(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier, draft);
        QCoreApplication::sendEvent(finishedCombo.editor->lineEdit(), &draftKey);
    }
    require(finishedCombo.field->value() == QStringLiteral("finished-model-id") &&
                form.value(QStringLiteral("editable-finished")) ==
                    QStringLiteral("finished-model-id") &&
                finishedEdits == 2 && finishedCommits == 0,
            "finish-policy editable selectors publish typed drafts without committing them");
    require(QMetaObject::invokeMethod(finishedCombo.editor->lineEdit(), "editingFinished"),
            "editable selector finish signal exists");
    require(finishedCommits == 1,
            "editable selector editing completion commits the final typed ID exactly once");
    require(QMetaObject::invokeMethod(finishedCombo.editor->lineEdit(), "editingFinished"),
            "editable selector finish signal can be repeated");
    require(finishedCommits == 1, "repeated editable completion cannot duplicate its commit");
    finishedCombo.editor->setCurrentValue(QStringLiteral("beta"));
    require(finishedEdits == 3 && finishedCommits == 2 &&
                finishedCombo.field->value() == QStringLiteral("beta"),
            "choosing an existing option completes its selection and commits once");
}

void selectorInteractionOptions() {
    QWidget owner;
    fields::Options options;
    options.parent = &owner;
    options.searchEnabled = false;
    const QVector<fields::Choice> choices{
        {QStringLiteral("alpha"), {"FormFieldsTests", "Alpha"}, {}, true}};
    const auto single = fields::select(metadata("not-searchable"), choices, options);
    const auto multi = fields::multiSelect(metadata("multi-not-searchable"), choices, options);
    const auto combo = fields::comboBox(metadata("not-editable"), choices, options);
    require(!single.editor->searchEnabled() && !multi.editor->searchEnabled(),
            "selector options explicitly disable search for single and multiple choices");
    require(!combo.editor->editable(), "combo fields require an explicit editable option");
    options.editable = true;
    options.readOnly = true;
    const auto readOnly = fields::comboBox(metadata("read-only-editable"), choices, options);
    require(readOnly.editor->editable() && readOnly.editor->lineEdit()->isReadOnly(),
            "an editable combo respects the explicit read-only option on its actual text editor");
}

void selectorPopupOwnership() {
    QWidget owner;
    fields::Options options;
    options.parent = &owner;
    options.editable = true;
    const auto combo = fields::comboBox(
        metadata("popup-ownership"),
        {{QStringLiteral("alpha"), {"FormFieldsTests", "Alpha"}, {}, true}}, options);
    combo.field->syncValue(QStringLiteral("alpha"));
    combo.editor->setPopupVisible(true);
    require(combo.editor->popupVisible(), "a selector popup can be open during view teardown");
    const QPointer<fields::FormField> controller = combo.field;
    const QPointer<adqt::widgets::AdComboBox> editor = combo.editor;
    delete combo.viewWidget();
    require(controller.isNull() && editor.isNull(),
            "deleting a field closes its selector popup while native editor types are still alive");
}

void explicitCommitAndReset() {
    QWidget owner;
    adqt::widgets::AdForm form(&owner);
    fields::configureForm(&form);
    fields::Options options;
    options.form = &form;
    options.commitPolicy = fields::CommitPolicy::Explicit;
    const auto text = fields::text(metadata("explicit-text"), options);
    const auto select =
        fields::select(metadata("explicit-select"),
                       {{QStringLiteral("alpha"), {"FormFieldsTests", "Alpha"}, {}, true},
                        {QStringLiteral("beta"), {"FormFieldsTests", "Beta"}, {}, true}},
                       options);
    const auto color = fields::color(metadata("explicit-color"), options);
    int textEdits = 0;
    int textCommits = 0;
    int selectCommits = 0;
    int colorCommits = 0;
    QObject::connect(text.field, &fields::FormField::valueEdited, &owner,
                     [&textEdits] { ++textEdits; });
    QObject::connect(text.field, &fields::FormField::valueCommitted, &owner,
                     [&textCommits] { ++textCommits; });
    QObject::connect(select.field, &fields::FormField::valueCommitted, &owner,
                     [&selectCommits] { ++selectCommits; });
    QObject::connect(color.field, &fields::FormField::valueCommitted, &owner,
                     [&colorCommits] { ++colorCommits; });
    form.setInitialValues(
        {{QStringLiteral("explicit-text"), QStringLiteral("initial")},
         {QStringLiteral("explicit-select"), QStringLiteral("alpha")},
         {QStringLiteral("explicit-color"), QVariant::fromValue(QColor(Qt::red))}});
    form.resetFields();
    require(text.field->value() == QStringLiteral("initial") && textEdits == 0 &&
                textCommits == 0 && selectCommits == 0 && colorCommits == 0,
            "form initial values and reset synchronize explicit fields without shared events");
    text.editor->setText(QStringLiteral("draft"));
    select.editor->setCurrentValue(QStringLiteral("beta"));
    color.editor->setValue(adqt::widgets::AdColorValue::solid(QColor(Qt::blue)));
    require(QMetaObject::invokeMethod(text.editor, "editingFinished"),
            "explicit text finish signal exists");
    require(QMetaObject::invokeMethod(color.editor, "editingFinished",
                                      Q_ARG(adqt::widgets::AdColorValue, color.editor->value())),
            "explicit color finish signal exists");
    require(textEdits == 1 && textCommits == 0 && selectCommits == 0 && colorCommits == 0,
            "explicit fields retain drafts when native editing or selection completes");
    text.field->notifyCommitted();
    select.field->notifyCommitted();
    color.field->notifyCommitted();
    require(textCommits == 1 && selectCommits == 1 && colorCommits == 1,
            "caller confirmation commits each explicit field once");
    text.editor->setText(QStringLiteral("discarded draft"));
    form.resetFields();
    text.field->notifyCommitted();
    require(text.field->value() == QStringLiteral("initial") && textEdits == 2 &&
                textCommits == 1 && select.field->value() == QStringLiteral("alpha") &&
                color.field->value().value<QColor>() == QColor(Qt::red),
            "reset restores initial values and clears pending drafts without an extra commit");
}

void radioIdentityAndFocus() {
    QWidget owner;
    auto* layout = new QVBoxLayout(&owner);
    fields::Options options;
    options.parent = &owner;
    const QVector<fields::Choice> choices{
        {QStringLiteral("alpha"), {"FormFieldsTests", "Alpha"}, {}, true},
        {QStringLiteral("beta"), {"FormFieldsTests", "Beta"}, {}, true}};
    const auto radio = fields::radio(metadata("radio"), choices, options);
    layout->addWidget(radio.viewWidget());
    const QPointer<QAbstractButton> first = radio.editor->button(0);
    const QPointer<QAbstractButton> second = radio.editor->button(1);
    require(first && second, "radio choices create focusable native buttons");
    int edits = 0;
    int commits = 0;
    QObject::connect(radio.field, &fields::FormField::valueEdited, &owner, [&edits] { ++edits; });
    QObject::connect(radio.field, &fields::FormField::valueCommitted, &owner,
                     [&commits] { ++commits; });
    radio.field->syncValue(QStringLiteral("alpha"));
    require(first->isChecked() && edits == 0 && commits == 0,
            "radio synchronization selects by stable option value without shared events");
    second->click();
    require(radio.field->value() == QStringLiteral("beta") && edits == 1 && commits == 1,
            "a radio selection produces one shared edit and one commit");
    owner.show();
    flushEvents();
    first->setFocus(Qt::OtherFocusReason);
    flushEvents();
    QWidget* const focusTarget = radio.focusWidget();
    Translator translator;
    require(QCoreApplication::installTranslator(&translator), "install radio test translator");
    QEvent languageChange(QEvent::LanguageChange);
    QCoreApplication::sendEvent(radio.viewWidget(), &languageChange);
    flushEvents();
    require(first && second && radio.editor->button(0) == first &&
                radio.editor->button(1) == second && radio.focusWidget() == focusTarget &&
                first->hasFocus(),
            "radio retranslation preserves native button identity and the active focus target");
    require(first->text() == QStringLiteral("Translated: Alpha") && second->isChecked() &&
                radio.field->value() == QStringLiteral("beta") && edits == 1 && commits == 1,
            "radio labels retranslate without losing the selection or generating edits");
    radio.field->setChoices(choices);
    require(radio.editor->button(0) == first && radio.editor->button(1) == second &&
                first->hasFocus() && edits == 1 && commits == 1,
            "unchanged radio option values preserve existing buttons and focus on refresh");
    QCoreApplication::removeTranslator(&translator);
}

void radioDisabledChoicesAndFocus() {
    QWidget owner;
    fields::Options options;
    options.parent = &owner;
    const QVector<fields::Choice> choices{
        {QStringLiteral("alpha"), {"FormFieldsTests", "Alpha"}, {}, false},
        {QStringLiteral("beta"), {"FormFieldsTests", "Beta"}, {}, true}};
    const auto radio = fields::radio(metadata("enabled-radio-focus"), choices, options);
    require(radio.editor->controlSize() == adqt::widgets::AdRadio::ControlSize::Medium,
            "generic radio fields use the common medium control preset");
    const QPointer<QAbstractButton> first = radio.editor->button(0);
    const QPointer<QAbstractButton> second = radio.editor->button(1);
    require(radio.focusWidget() == second, "radio initial focus skips disabled choices");
    radio.field->setFieldEnabled(false);
    radio.field->retranslateUi();
    radio.field->setFieldEnabled(true);
    require(radio.focusWidget() == second,
            "disabling the whole field preserves its enabled-choice focus target");
    auto changed = choices;
    changed[0].enabled = true;
    changed[1].enabled = false;
    radio.field->setChoices(changed);
    require(first && second && radio.editor->button(0) == first &&
                radio.editor->button(1) == second && radio.focusWidget() == first,
            "enabled-state updates preserve radio buttons and move focus to an enabled choice");
}

void fileClearCommit() {
    QWidget owner;
    adqt::widgets::AdForm form(&owner);
    fields::configureForm(&form);
    fields::Options options;
    options.form = &form;
    options.allowClear = true;
    options.commitPolicy = fields::CommitPolicy::OnFinish;
    const auto file = fields::filePath(metadata("file"), options);
    int edits = 0;
    int commits = 0;
    QObject::connect(file.field, &fields::FormField::valueEdited, &owner, [&edits] { ++edits; });
    QObject::connect(file.field, &fields::FormField::valueCommitted, &owner,
                     [&commits] { ++commits; });
    file.field->syncValue(QStringLiteral("C:/image.png"));
    require(edits == 0 && commits == 0, "file path initialization is silent");
    auto* clear =
        file.editor->lineEdit()->findChild<QAbstractButton*>(QStringLiteral("ad-input-clear"));
    require(clear != nullptr && clear->isEnabled(), "file paths retain their native clear button");
    clear->click();
    require(file.field->value().toString().isEmpty() &&
                form.value(QStringLiteral("file")).toString().isEmpty() && edits == 1 &&
                commits == 1,
            "clearing a file path updates the form and commits immediately exactly once");
    require(QMetaObject::invokeMethod(file.editor, "editingFinished"),
            "file path finish signal exists");
    require(commits == 1, "editing completion after clear cannot duplicate its commit");
}

void customEditorBridgeAndOwnership() {
    QWidget owner;
    adqt::widgets::AdForm form(&owner);
    fields::configureForm(&form);
    auto* control = new QWidget;
    auto* focus = new QLineEdit(control);
    auto* layout = new QVBoxLayout(control);
    layout->addWidget(focus);
    fields::CustomBinding binding;
    binding.control = control;
    binding.focusWidget = focus;
    binding.readValue = [control] { return control->property("draft"); };
    binding.writeValue = [control, focus](const QVariant& value) {
        control->setProperty("draft", value);
        focus->setText(value.toString());
    };
    binding.retranslate = [focus] {
        focus->setPlaceholderText(QCoreApplication::translate("FormFieldsTests", "Custom input"));
    };
    fields::Options options;
    options.form = &form;
    options.commitPolicy = fields::CommitPolicy::Explicit;
    const auto custom = fields::custom(metadata("custom"), std::move(binding), options);
    QObject::connect(focus, &QLineEdit::textEdited, custom.field,
                     [control, field = custom.field](const QString& value) {
                         control->setProperty("draft", value);
                         field->notifyEdited();
                     });
    int edits = 0;
    int commits = 0;
    QObject::connect(custom.field, &fields::FormField::valueEdited, &owner, [&edits] { ++edits; });
    QObject::connect(custom.field, &fields::FormField::valueCommitted, &owner,
                     [&commits] { ++commits; });
    custom.field->syncValue(QStringLiteral("initial"));
    require(custom.field->value() == QStringLiteral("initial") &&
                focus->text() == QStringLiteral("initial") && edits == 0 && commits == 0 &&
                custom.focusWidget() == focus,
            "custom adapters synchronize through their writer and expose their focus target");
    focus->selectAll();
    QKeyEvent typed(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier, QStringLiteral("draft"));
    QCoreApplication::sendEvent(focus, &typed);
    require(custom.field->value() == QStringLiteral("draft") &&
                form.value(QStringLiteral("custom")) == QStringLiteral("draft") && edits == 1 &&
                commits == 0,
            "custom typed notifications update the shared value and form once without committing");
    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(focus, &focusOut);
    require(commits == 0, "custom explicit fields keep their drafts until caller confirmation");
    custom.field->notifyCommitted();
    custom.field->notifyCommitted();
    require(commits == 1, "custom editing completion commits its pending draft only once");
    const QPointer<fields::FormField> controller = custom.field;
    const QPointer<QWidget> editor = control;
    const QPointer<QLineEdit> focusGuard = focus;
    form.removeItem(custom.item());
    delete custom.viewWidget();
    require(controller.isNull() && editor.isNull() && focusGuard.isNull(),
            "the custom field view owns its controller, editor and nested focus target");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    styles::ThemeManager::instance().initialize(application);
    optionalMetadataAndChoiceLabels();
    constructionKeepsItemsInTheirOwner();
    presentationsAndFeedback();
    settingsRowsKeepNaturalHeight();
    descriptionOverrides();
    reservedFeedbackKeepsModalGeometry();
    editAndCommitTiming();
    compoundsAndFormValues();
    translationAndPrimitiveOptions();
    multiSelectAndEditableChoices();
    selectorInteractionOptions();
    selectorPopupOwnership();
    explicitCommitAndReset();
    radioIdentityAndFocus();
    radioDisabledChoicesAndFocus();
    fileClearCommit();
    customEditorBridgeAndOwnership();
    std::cout << "Form fields tests passed\n";
    return 0;
}
