// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/components/collagepdfsettingswidget.h"

#include "snow_shot/presentation/collagedocument.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/form.h"
#include "widgets/input.h"
#include "widgets/segmented.h"
#include "widgets/select.h"

#include <QCoreApplication>
#include <QEvent>
#include <QGridLayout>
#include <QLocale>
#include <QPainter>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace snow_shot::presentation::components {
namespace {
using adqt::widgets::AdButton;
using adqt::widgets::AdForm;
using adqt::widgets::AdFormItem;
using adqt::widgets::AdLineEdit;
using adqt::widgets::AdSegmented;
using adqt::widgets::AdSelect;

QString text(const char* source) {
    return QCoreApplication::translate("CollagePdfSettingsWidget", source);
}

[[maybe_unused]] constexpr const char* labels[] = {
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Stitching method"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Page-based stitching"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Template-based stitching"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Template"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Two images"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "ID card"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Three images"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Four images"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Five images"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Six images"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Seven images"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Eight images"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Nine images"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Image spacing"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Paper size"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Paper orientation"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Custom paper"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Page margin"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Export mode"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "A3"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "A4"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "A5"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "B5"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Letter"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Legal"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Custom"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Image Size"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Portrait"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Landscape"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Width"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Height"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Custom paper width"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Custom paper height"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Combined export"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "Per-page export"),
    QT_TRANSLATE_NOOP("CollagePdfSettingsWidget", "mm"),
};

struct NumberField final {
    const char* key;
    const char* name;
    const char* label;
    const char* accessibleName;
    double CollagePdfSettings::* member;
    AdFormItem* field = nullptr;
    AdLineEdit* input = nullptr;
};

double inputValue(const AdLineEdit* input) {
    bool ok = false;
    const QString entered = input->text().trimmed();
    double value = input->locale().toDouble(entered, &ok);
    if (!ok)
        value = QLocale::c().toDouble(entered, &ok);
    return ok && std::isfinite(value) ? value : std::numeric_limits<double>::quiet_NaN();
}

class TemplateButton final : public AdButton {
  public:
    TemplateButton(CollagePdfTemplateType type, QWidget* parent) : AdButton(parent), m_type(type) {
        setCheckable(true);
        setCheckedUsesActiveStyle(false);
        setMinimumSize(42, 48);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::StrongFocus);
    }

    void setSelected(bool selected, bool landscape) {
        setChecked(selected);
        setButtonStyle(selected ? ButtonStyle::Tonal : ButtonStyle::Outline);
        setAccentRole(selected ? AccentRole::Primary : AccentRole::Neutral);
        m_landscape = landscape;
        update();
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        AdButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(this);
        const QColor color = isChecked() ? theme.colorPrimary : theme.colorTextSecondary;
        painter.setPen(QPen(color, 1));
        painter.setBrush(Qt::NoBrush);
        if (!isEnabled())
            painter.setOpacity(.4);
        const QSizeF size = m_landscape ? QSizeF(30, 22) : QSizeF(22, 30);
        const QRectF paper(QRectF(rect()).center() - QPointF(size.width(), size.height()) / 2,
                           size);
        painter.drawRoundedRect(paper, 1, 1);
        const QRectF content = paper.adjusted(3, 3, -3, -3);
        if (m_type == CollagePdfTemplateType::IdCard) {
            constexpr qreal ratio = 85.6 / 53.98;
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            for (int index = 0; index < 2; ++index) {
                const QRectF section =
                    m_landscape ? QRectF(content.x() + index * content.width() / 2, content.y(),
                                         content.width() / 2, content.height())
                                : QRectF(content.x(), content.y() + index * content.height() / 2,
                                         content.width(), content.height() / 2);
                const qreal width = std::min({.41 * std::min(paper.width(), paper.height()),
                                              section.width(), section.height() * ratio});
                const QSizeF card(width, width / ratio);
                painter.drawRect(
                    QRectF(section.center() - QPointF(card.width(), card.height()) / 2, card));
            }
            return;
        }
        QVector<QRectF> cells;
        const auto column = [&cells](int index, int columns, int rows) {
            constexpr qreal gap = .065;
            const qreal width = (1 - gap * (columns - 1)) / columns;
            const qreal height = (1 - gap * (rows - 1)) / rows;
            for (int row = 0; row < rows; ++row)
                cells.append(QRectF(index * (width + gap), row * (height + gap), width, height));
        };
        switch (m_type) {
        case CollagePdfTemplateType::IdCard:
            break;
        case CollagePdfTemplateType::TwoImages:
            column(0, 1, 2);
            break;
        case CollagePdfTemplateType::ThreeImages:
            column(0, 1, 3);
            break;
        case CollagePdfTemplateType::FourImages:
            column(0, 2, 2);
            column(1, 2, 2);
            break;
        case CollagePdfTemplateType::FiveImages:
            column(0, 2, 2);
            column(1, 2, 3);
            break;
        case CollagePdfTemplateType::SixImages:
            column(0, 2, 3);
            column(1, 2, 3);
            break;
        case CollagePdfTemplateType::SevenImages:
            column(0, 2, 3);
            column(1, 2, 4);
            break;
        case CollagePdfTemplateType::EightImages:
            column(0, 2, 4);
            column(1, 2, 4);
            break;
        case CollagePdfTemplateType::NineImages:
            for (int index = 0; index < 3; ++index)
                column(index, 3, 3);
            break;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (auto cell : cells) {
            if (m_landscape)
                cell = QRectF(cell.y(), cell.x(), cell.height(), cell.width());
            painter.drawRect(QRectF(
                content.x() + cell.x() * content.width(), content.y() + cell.y() * content.height(),
                cell.width() * content.width(), cell.height() * content.height()));
        }
    }

  private:
    CollagePdfTemplateType m_type;
    bool m_landscape = false;
};
} // namespace

struct CollagePdfSettingsWidget::Impl final {
    CollagePdfSettingsWidget* owner;
    CollageDocument* document;
    AdForm* form;
    AdSelect* method;
    AdFormItem* methodField;
    AdFormItem* templateField;
    std::array<TemplateButton*, 9> templates{};
    AdFormItem* spacingField;
    QGridLayout* spacingLayout;
    AdSelect* paper;
    AdSegmented* orientation;
    AdSegmented* exportMode;
    AdFormItem* paperField;
    AdFormItem* orientationField;
    AdFormItem* customField;
    AdFormItem* exportField;
    std::array<NumberField, 4> numbers{{
        {"custom_width_mm", "collagePdfCustomWidth", "Width", "Custom paper width",
         &CollagePdfSettings::customWidthMm},
        {"custom_height_mm", "collagePdfCustomHeight", "Height", "Custom paper height",
         &CollagePdfSettings::customHeightMm},
        {"margin_mm", "collagePdfMargin", "Page margin", "Page margin",
         &CollagePdfSettings::marginTopMm},
        {"image_spacing_mm", "collagePdfImageSpacing", "Image spacing", "Image spacing",
         &CollagePdfSettings::imageSpacingMm},
    }};
    bool syncing = false;

    void edit(const QString& field, const std::function<void(CollagePdfSettings&)>& change) {
        if (syncing || !owner->isEnabled())
            return;
        auto settings = document->pdfSettings();
        change(settings);
        document->setPdfSettings(settings);
        emit owner->settingsEdited(field);
    }

    void refresh() {
        syncing = true;
        const auto settings = document->pdfSettings();
        const bool templateBased =
            settings.stitchingMethod == CollagePdfStitchingMethod::TemplateBased;
        method->setCurrentValue(static_cast<int>(settings.stitchingMethod));
        templateField->setItemHidden(!templateBased);
        for (std::size_t index = 0; index < templates.size(); ++index)
            templates[index]->setSelected(static_cast<int>(settings.templateType) ==
                                              static_cast<int>(index),
                                          settings.orientation == CollagePdfOrientation::Landscape);
        paper->setCurrentValue(static_cast<int>(settings.paperSize));
        orientation->setCurrentValue(static_cast<int>(settings.orientation));
        exportMode->setCurrentValue(static_cast<int>(settings.exportMode));
        customField->setItemHidden(settings.paperSize != CollagePdfPaperSize::Custom);
        const bool imageSize =
            settings.paperSize == CollagePdfPaperSize::ImageSize && !templateBased;
        orientationField->setItemHidden(imageSize);
        spacingField->setItemHidden(imageSize);
        numbers[3].field->setItemHidden(!templateBased);
        spacingLayout->setColumnStretch(1, templateBased ? 1 : 0);
        const auto errors = validateCollagePdfSettings(settings);
        const auto showError = [&errors](AdFormItem* field, const QString& key) {
            const auto error = errors.value(key);
            field->setValidateStatus(error.isEmpty() ? AdFormItem::ValidateStatus::None
                                                     : AdFormItem::ValidateStatus::Error);
            field->setHelpText(error);
        };
        showError(paperField, QStringLiteral("paper_size"));
        showError(methodField, QStringLiteral("stitching_method"));
        showError(templateField, QStringLiteral("template_type"));
        showError(orientationField, QStringLiteral("orientation"));
        showError(exportField, QStringLiteral("export_mode"));
        method->setStatus(errors.contains(QStringLiteral("stitching_method"))
                              ? AdSelect::Status::Error
                              : AdSelect::Status::None);
        paper->setStatus(errors.contains(QStringLiteral("paper_size")) ? AdSelect::Status::Error
                                                                       : AdSelect::Status::None);
        for (auto& number : numbers) {
            const QSignalBlocker blocker(number.input);
            const double value = settings.*number.member;
            const double entered = inputValue(number.input);
            // Preserve exact user text, including an empty or incomplete numeric draft.
            if (std::isfinite(value) && (!std::isfinite(entered) || entered != value))
                number.input->setText(number.input->locale().toString(value, 'g', 12));
            else if (!std::isfinite(value) && std::isfinite(entered))
                number.input->clear();
            QString error = errors.value(QString::fromLatin1(number.key));
            if (number.member == &CollagePdfSettings::marginTopMm) {
                number.field->setItemHidden(imageSize);
                QStringList marginErrors;
                for (const auto* key :
                     {"margin_top_mm", "margin_bottom_mm", "margin_left_mm", "margin_right_mm"}) {
                    const auto message = errors.value(QString::fromLatin1(key));
                    if (!message.isEmpty() && !marginErrors.contains(message))
                        marginErrors.append(message);
                }
                error = marginErrors.join(QLatin1Char('\n'));
            }
            number.input->setStatus(error.isEmpty() ? AdLineEdit::Status::None
                                                    : AdLineEdit::Status::Error);
            number.field->setValidateStatus(error.isEmpty() ? AdFormItem::ValidateStatus::None
                                                            : AdFormItem::ValidateStatus::Error);
            number.field->setHelpText(error);
            number.input->setAccessibleDescription(error);
        }
        syncing = false;
    }
};

CollagePdfSettingsWidget::CollagePdfSettingsWidget(CollageDocument* document, QWidget* parent)
    : QWidget(parent), m_impl(std::make_unique<Impl>()) {
    auto& d = *m_impl;
    d.owner = this;
    d.document = document;
    setObjectName(QStringLiteral("collagePdfSettings"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    d.form = new AdForm(this);
    d.form->setFormLayout(AdForm::FormLayout::Vertical);
    d.form->setRequiredMark(AdForm::RequiredMark::Hidden);
    d.form->setColon(false);
    layout->addWidget(d.form);
    d.method = new AdSelect(d.form);
    d.method->setObjectName(QStringLiteral("collagePdfStitchingMethod"));
    d.methodField = d.form->addField({}, d.method, QStringLiteral("stitching_method"));
    d.methodField->setValidateOnChange(false);
    auto* templateButtons = new QWidget(d.form);
    templateButtons->setObjectName(QStringLiteral("collagePdfTemplates"));
    auto* templateGrid = new QGridLayout(templateButtons);
    templateGrid->setContentsMargins(0, 0, 0, 0);
    templateGrid->setSpacing(6);
    constexpr std::array<const char*, 9> templateNames{"TwoImages",   "IdCard",      "ThreeImages",
                                                       "FourImages",  "FiveImages",  "SixImages",
                                                       "SevenImages", "EightImages", "NineImages"};
    for (std::size_t index = 0; index < d.templates.size(); ++index) {
        const auto type = static_cast<CollagePdfTemplateType>(index);
        auto* button = new TemplateButton(type, templateButtons);
        button->setObjectName(QStringLiteral("collagePdfTemplate") +
                              QString::fromLatin1(templateNames[index]));
        d.templates[index] = button;
        templateGrid->addWidget(button, static_cast<int>(index / 3), static_cast<int>(index % 3));
        connect(button, &AdButton::clicked, this, [this, type] {
            m_impl->edit(QStringLiteral("template_type"),
                         [type](CollagePdfSettings& settings) { settings.templateType = type; });
            m_impl->refresh();
        });
    }
    d.templateField = d.form->addField({}, templateButtons, QStringLiteral("template_type"));
    d.templateField->setValidateOnChange(false);
    d.paper = new AdSelect(d.form);
    d.paper->setObjectName(QStringLiteral("collagePdfPaperSize"));
    d.paperField = d.form->addField({}, d.paper, QStringLiteral("paper_size"));
    d.paperField->setValidateOnChange(false);
    const auto segmented = [&d](const char* name) {
        auto* control = new AdSegmented(d.form);
        control->setObjectName(QString::fromLatin1(name));
        control->setDistribution(AdSegmented::Distribution::Fill);
        control->addOption({}, 0);
        control->addOption({}, 1);
        return control;
    };
    d.orientation = segmented("collagePdfOrientation");
    d.orientationField = d.form->addField({}, d.orientation, QStringLiteral("orientation"));
    d.orientationField->setValidateOnChange(false);
    const auto input = [&d](std::size_t index, QWidget* parent) {
        auto& number = d.numbers[index];
        number.input = new AdLineEdit(parent);
        number.input->setObjectName(QString::fromLatin1(number.name));
        number.input->setInputMethodHints(Qt::ImhFormattedNumbersOnly);
        QObject::connect(number.input, &QLineEdit::textChanged, d.owner,
                         [&d, index](const QString&) {
                             auto& edited = d.numbers[index];
                             const double value = inputValue(edited.input);
                             d.edit(QString::fromLatin1(edited.key),
                                    [&edited, value](CollagePdfSettings& settings) {
                                        settings.*edited.member = value;
                                        if (edited.member == &CollagePdfSettings::marginTopMm) {
                                            settings.marginBottomMm = value;
                                            settings.marginLeftMm = value;
                                            settings.marginRightMm = value;
                                        }
                                    });
                         });
        return number.input;
    };
    const auto customPaper = [&d, &input] {
        auto* content = new QWidget(d.form);
        content->setObjectName(QStringLiteral("collagePdfCustomPaper"));
        auto* grid = new QGridLayout(content);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(0);
        for (int index = 0; index < 2; ++index) {
            auto& number = d.numbers[static_cast<std::size_t>(index)];
            number.field = new AdFormItem(content);
            number.field->setItemLayout(AdFormItem::ItemLayout::Vertical);
            number.field->setValidateOnChange(false);
            number.field->setControlWidget(input(static_cast<std::size_t>(index), number.field));
            grid->addWidget(number.field, 0, index);
        }
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        return content;
    };
    d.customField = d.form->addField({}, customPaper(), QStringLiteral("custom"));
    auto* spacing = new QWidget(d.form);
    spacing->setObjectName(QStringLiteral("collagePdfPageSpacing"));
    auto* spacingGrid = new QGridLayout(spacing);
    d.spacingLayout = spacingGrid;
    spacingGrid->setContentsMargins(0, 0, 0, 0);
    spacingGrid->setHorizontalSpacing(12);
    spacingGrid->setVerticalSpacing(0);
    for (std::size_t index = 2; index < d.numbers.size(); ++index) {
        auto& number = d.numbers[index];
        number.field = new AdFormItem(spacing);
        number.field->setItemLayout(AdFormItem::ItemLayout::Vertical);
        number.field->setValidateOnChange(false);
        number.field->setControlWidget(input(index, number.field));
        spacingGrid->addWidget(number.field, 0, static_cast<int>(index - 2));
        spacingGrid->setColumnStretch(static_cast<int>(index - 2), 1);
    }
    d.spacingField = d.form->addField({}, spacing, QStringLiteral("page_spacing"));
    d.spacingField->setNoStyle(true);
    d.exportMode = segmented("collagePdfExportMode");
    d.exportField = d.form->addField({}, d.exportMode, QStringLiteral("export_mode"));
    d.exportField->setValidateOnChange(false);
    connect(d.method, &AdSelect::currentValueChanged, this, [this](const QVariant& value) {
        m_impl->edit(QStringLiteral("stitching_method"), [&value](CollagePdfSettings& settings) {
            settings.stitchingMethod = static_cast<CollagePdfStitchingMethod>(value.toInt());
        });
    });
    connect(d.paper, &AdSelect::currentValueChanged, this, [this](const QVariant& value) {
        m_impl->edit(QStringLiteral("paper_size"), [&value](CollagePdfSettings& settings) {
            settings.paperSize = static_cast<CollagePdfPaperSize>(value.toInt());
        });
    });
    connect(d.orientation, &AdSegmented::currentValueChanged, this, [this](const QVariant& value) {
        m_impl->edit(QStringLiteral("orientation"), [&value](CollagePdfSettings& settings) {
            settings.orientation = static_cast<CollagePdfOrientation>(value.toInt());
        });
    });
    connect(d.exportMode, &AdSegmented::currentValueChanged, this, [this](const QVariant& value) {
        m_impl->edit(QStringLiteral("export_mode"), [&value](CollagePdfSettings& settings) {
            settings.exportMode = static_cast<CollagePdfExportMode>(value.toInt());
        });
    });
    connect(document, &CollageDocument::changed, this, [this] { m_impl->refresh(); });
    retranslateUi();
}

CollagePdfSettingsWidget::~CollagePdfSettingsWidget() = default;

void CollagePdfSettingsWidget::retranslateUi() {
    auto& d = *m_impl;
    d.syncing = true;
    d.method->setOptions(
        {{static_cast<int>(CollagePdfStitchingMethod::PageBased), text("Page-based stitching")},
         {static_cast<int>(CollagePdfStitchingMethod::TemplateBased),
          text("Template-based stitching")}});
    d.methodField->setLabel(text("Stitching method"));
    d.method->setAccessibleName(text("Stitching method"));
    d.templateField->setLabel(text("Template"));
    constexpr std::array<const char*, 9> templates{"Two images",   "ID card",      "Three images",
                                                   "Four images",  "Five images",  "Six images",
                                                   "Seven images", "Eight images", "Nine images"};
    for (std::size_t index = 0; index < d.templates.size(); ++index) {
        d.templates[index]->setToolTip(text(templates[index]));
        d.templates[index]->setAccessibleName(text(templates[index]));
    }
    constexpr std::array<const char*, 8> papers{"A3",     "A4",    "A5",     "B5",
                                                "Letter", "Legal", "Custom", "Image Size"};
    QVector<AdSelect::Option> options;
    for (std::size_t index = 0; index < papers.size(); ++index)
        options.append({static_cast<int>(index), text(papers[index])});
    d.paper->setOptions(options);
    d.paperField->setLabel(text("Paper size"));
    d.paper->setAccessibleName(text("Paper size"));
    d.orientationField->setLabel(text("Paper orientation"));
    d.orientation->setAccessibleName(text("Paper orientation"));
    d.orientation->setOptionLabel(0, text("Portrait"));
    d.orientation->setOptionLabel(1, text("Landscape"));
    d.customField->setLabel(text("Custom paper"));
    d.exportField->setLabel(text("Export mode"));
    d.exportMode->setAccessibleName(text("Export mode"));
    d.exportMode->setOptionLabel(0, text("Combined export"));
    d.exportMode->setOptionLabel(1, text("Per-page export"));
    for (auto& number : d.numbers) {
        number.field->setLabel(text(number.label));
        number.input->setAccessibleName(text(number.accessibleName));
        number.input->setSuffixText(text("mm"));
    }
    d.syncing = false;
    d.refresh();
}

void CollagePdfSettingsWidget::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

} // namespace snow_shot::presentation::components
