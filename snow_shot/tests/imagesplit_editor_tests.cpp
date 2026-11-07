// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/components/imagespliteditorwidget.h"

#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/form.h"
#include "widgets/input_number.h"
#include "widgets/segmented.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QTranslator>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace snow_shot::presentation;
using namespace snow_shot::presentation::components;
using adqt::widgets::AdButton;
using adqt::widgets::AdForm;
using adqt::widgets::AdInputNumber;
using adqt::widgets::AdSegmented;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
QImage image(QSize size) {
    QImage result(size, QImage::Format_ARGB32_Premultiplied);
    result.fill(QColor(20, 80, 160, 100));
    return result;
}
void pump() {
    QApplication::processEvents();
}
void mouse(QWidget* target, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, target->mapToGlobal(point.toPoint()), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}
void click(QWidget* target, QPointF point) {
    mouse(target, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    mouse(target, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    pump();
}
void click(QWidget* target) {
    click(target, QRectF(target->rect()).center());
}
void wheel(QWidget* target, QPointF point, int delta) {
    QWheelEvent event(point, target->mapToGlobal(point.toPoint()), {}, QPoint(0, delta),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(target, &event);
}
QRectF canvasViewport(const ImageSplitPreviewCanvas& canvas) {
    return {38, 28, canvas.width() - 38.0, canvas.height() - 28.0};
}
QPointF rulerPoint(const ImageSplitPreviewCanvas& canvas, bool horizontal, int coordinate) {
    const auto viewport = canvasViewport(canvas);
    return horizontal ? QPointF(canvas.imageToViewport({qreal(coordinate), 0}).x(),
                                std::max(viewport.top(), canvas.offset().y()) - 14)
                      : QPointF(std::max(viewport.left(), canvas.offset().x()) - 19,
                                canvas.imageToViewport({0, qreal(coordinate)}).y());
}
QImage renderCanvas(ImageSplitPreviewCanvas& canvas, qreal scale) {
    QImage output(QSize(qRound(canvas.width() * scale), qRound(canvas.height() * scale)),
                  QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(scale);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    canvas.render(&painter);
    return output;
}
template <class T> T* child(QWidget& editor, const char* name) {
    auto* result = editor.findChild<T*>(QString::fromLatin1(name));
    require(result != nullptr, "expected split editor control is missing");
    return result;
}
void show(ImageSplitEditorWidget& editor) {
    editor.resize(1100, 720);
    editor.show();
    pump();
}

void emptyImageShowsUploadSurface() {
    ImageSplitDocument document;
    ImageSplitEditorWidget editor(&document);
    show(editor);
    auto* empty = child<QWidget>(editor, "imageSplitEmptyState");
    auto* title = child<QLabel>(editor, "imageSplitEmptyPrompt");
    auto* description = child<QLabel>(editor, "imageSplitEmptyDescription");
    auto* select = child<AdButton>(editor, "imageSplitEmptySelectImage");
    auto* hint = child<QWidget>(editor, "imageSplitDropHint");
    const auto requireEmpty = [&] {
        require(empty->isVisible() && !editor.canvas()->isVisible() &&
                    !child<QWidget>(editor, "imageSplitCanvasToolbar")->isVisible() &&
                    child<QLabel>(editor, "imageSplitDimensions")->isHidden() &&
                    empty->isAncestorOf(select) && empty->isAncestorOf(title) &&
                    title->wordWrap() && description->wordWrap() &&
                    title->geometry().bottom() < description->geometry().top() &&
                    description->geometry().bottom() < select->geometry().top() &&
                    empty->rect().contains(select->geometry()),
                "the upload guidance and action must replace the empty preview and toolbar");
    };
    requireEmpty();
    require(select->buttonStyle() == AdButton::ButtonStyle::Solid &&
                select->accentRole() == AdButton::AccentRole::Primary &&
                select->iconRef().isValid() && select->focusPolicy() == Qt::StrongFocus &&
                !editor.configurationValid(),
            "the empty upload action must match the merge window's primary add action");
    int selections = 0;
    QObject::connect(&editor, &ImageSplitEditorWidget::reloadRequested, &editor,
                     [&] { ++selections; });
    click(select);
    select->setFocus(Qt::TabFocusReason);
    require(select->hasFocus(), "the empty action must accept keyboard focus");
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(select, &press);
    QApplication::sendEvent(select, &release);
    require(selections == 2, "mouse and keyboard activation must request image selection");
    editor.setLoading(true);
    require(!select->isEnabled() && select->busy(), "image loading must busy the empty action");
    requireEmpty();
    editor.setLoading(false);
    editor.setExportBusy(true);
    require(!select->isEnabled(), "export must block empty-state image selection");
    editor.setExportBusy(false);
    require(select->isEnabled() && !select->busy(),
            "the empty action must recover after busy work");

    QMimeData files;
    const auto path = QDir::temp().filePath(QStringLiteral("split.png"));
    files.setUrls({QUrl::fromLocalFile(path)});
    QStringList received;
    QObject::connect(&editor, &ImageSplitEditorWidget::imageFilesDropped, &editor,
                     [&](const QStringList& paths) { received = paths; });
    QDragEnterEvent enter(QRectF(empty->rect()).center(), Qt::CopyAction, &files, Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(empty, &enter);
    require(enter.isAccepted() && hint->isVisible() && hint->geometry() == empty->geometry(),
            "dragging onto the empty upload surface must highlight its full preview area");
    editor.resize(900, 600);
    pump();
    require(hint->geometry() == empty->geometry(), "empty drag feedback must follow resizing");
    QDropEvent drop({100, 100}, Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&editor, &drop);
    require(drop.isAccepted() && hint->isHidden() && received == QStringList{path},
            "an empty-state drop must request import and clear the highlight");
    document.setImage(image({800, 600}));
    const auto initialZoom = editor.canvas()->zoom();
    const auto initialOffset = editor.canvas()->offset();
    pump();
    require(editor.canvas()->zoom() == initialZoom && editor.canvas()->offset() == initialOffset,
            "restoring the preview must settle its layout before exposing the fitted viewport");
    const auto bounds = QRectF(editor.canvas()->offset(),
                               QSizeF(document.image().size()) * editor.canvas()->zoom());
    require(empty->isHidden() && editor.canvas()->isVisible() &&
                child<QWidget>(editor, "imageSplitCanvasToolbar")->isVisible() &&
                canvasViewport(*editor.canvas()).adjusted(-1, -1, 1, 1).contains(bounds),
            "loading the first image must restore its fitted preview and toolbar");
    document.setImage({});
    pump();
    requireEmpty();
    require(select->isEnabled(), "returning to an empty image must restore image selection");

    auto& themes = adqt::theme::ThemeManager::instance();
    const auto original = themes.config();
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(theme);
        pump();
        const auto tokens = themes.resolveTheme(empty);
        require(title->font().pixelSize() == qRound(tokens.fontSizeLG) &&
                    description->font().pixelSize() == qRound(tokens.fontSize) &&
                    title->palette().color(QPalette::WindowText) == tokens.colorText &&
                    description->palette().color(QPalette::WindowText) == tokens.colorTextSecondary,
                "the empty panel must use the shared upload typography in both themes");
        const auto directory = qEnvironmentVariable("SNOW_TEST_IMAGE_SPLIT_EMPTY_ARTIFACT_DIR");
        if (!directory.isEmpty()) {
            require(QDir().mkpath(directory), "create the empty-state inspection directory");
            editor.setAutoFillBackground(true);
            auto palette = editor.palette();
            palette.setColor(QPalette::Window, tokens.colorBgElevated);
            editor.setPalette(palette);
            const auto filename = theme == adqt::theme::ThemeScheme::Light
                                      ? QStringLiteral("light.png")
                                      : QStringLiteral("dark.png");
            require(editor.grab().save(QDir(directory).filePath(filename)),
                    "save the empty-state inspection image");
        }
    }
    themes.setConfig(original);
    for (const auto* locale : {"en_US", "zh_CN", "zh_TW"}) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                "empty-state translations must be available in every language");
        QApplication::installTranslator(&translator);
        QEvent language(QEvent::LanguageChange);
        QApplication::sendEvent(&editor, &language);
        require(title->text() == QCoreApplication::translate("ImageSplitEditorWidget",
                                                             "Select an image to split") &&
                    description->text() == QCoreApplication::translate(
                                               "ImageSplitEditorWidget",
                                               "Drag an image file here, or choose select image") &&
                    select->text() ==
                        QCoreApplication::translate("ImageSplitEditorWidget", "Select image") &&
                    select->accessibleName() == select->text() &&
                    empty->accessibleName() == title->text() &&
                    empty->accessibleDescription() == description->text() &&
                    (QByteArray(locale) == "en_US" ||
                     title->text() != QStringLiteral("Select an image to split")),
                "runtime language changes must translate the entire empty upload surface");
        requireEmpty();
        QApplication::removeTranslator(&translator);
        QApplication::sendEvent(&editor, &language);
    }
}

class CanvasPaintObserver final : public QObject {
  public:
    int paints = 0;
    QRegion damage;

    void reset() {
        paints = 0;
        damage = {};
    }

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Paint) {
            ++paints;
            damage += static_cast<QPaintEvent*>(event)->region();
        }
        return false;
    }
};

void unchangedHoverDoesNotRepaint() {
    ImageSplitDocument document;
    document.setImage(image({400, 240}));
    ImageSplitPreviewCanvas canvas(&document);
    canvas.resize(900, 640);
    canvas.show();
    pump();
    CanvasPaintObserver observer;
    canvas.installEventFilter(&observer);
    const auto hover = [&](QPointF point) {
        mouse(&canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
        pump();
    };
    hover({50, 50});
    hover({60, 60});
    require(observer.paints == 0, "moving through blank canvas space must not repaint");
    hover(canvas.imageToViewport({200, 80}));
    require(observer.paints > 0, "hovering a new guide must repaint its highlight");
    qint64 damagedPixels = 0;
    for (const auto& rect : observer.damage)
        damagedPixels += qint64(rect.width()) * rect.height();
    require(damagedPixels < qint64(canvas.width()) * canvas.height() / 10,
            "a guide highlight must damage only its strip and coordinate badge");
    observer.reset();
    hover(canvas.imageToViewport({200, 90}));
    require(observer.paints == 0, "moving along the same hovered guide must not repaint");
    hover(rulerPoint(canvas, true, 77));
    require(observer.paints > 0, "a new ruler coordinate must repaint its preview");
    observer.reset();
    hover(rulerPoint(canvas, true, 77) + QPointF(.2 * canvas.zoom(), 0));
    require(observer.paints == 0,
            "moving within the same rounded ruler coordinate must not repaint");
    wheel(&canvas, {50, 50}, 0);
    pump();
    require(observer.paints > 0,
            "a wheel event without zoom must erase the previous ruler preview");
    observer.reset();
    hover({60, 60});
    require(observer.paints == 0, "a move after a wheel event must not retain a stale overlay");
    hover(rulerPoint(canvas, true, 77));
    observer.reset();
    mouse(&canvas, QEvent::MouseButtonPress, {50, 50}, Qt::RightButton, Qt::RightButton);
    pump();
    require(observer.paints > 0, "a right press off the ruler must erase the previous preview");
    observer.reset();
    hover({60, 60});
    require(observer.paints == 0, "a move after a right press must not retain a stale overlay");
    hover(rulerPoint(canvas, true, 77));
    observer.reset();
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&canvas, &leave);
    pump();
    require(observer.paints > 0, "leaving the ruler must erase its preview and coordinate badge");
}

void overlayDamageMatchesFreshRendering() {
    for (const bool dense : {false, true}) {
        ImageSplitDocument document;
        const auto source = image(dense ? QSize(5001, 300) : QSize(400, 240));
        document.setImage(source);
        ImageSplitSettings settings;
        settings.direction = ImageSplitDirection::Grid;
        settings.columns = dense ? 5001 : 3;
        settings.rows = 2;
        settings.overlapWidth = 7;
        require(document.setSettings(settings), "overlay damage fixture must be valid");
        ImageSplitPreviewCanvas canvas(&document);
        canvas.resize(900, 640);
        canvas.show();
        pump();
        CanvasPaintObserver observer;
        canvas.installEventFilter(&observer);
        for (const auto* locale : {"en_US", "zh_CN", "zh_TW"}) {
            QTranslator translator;
            require(translator.load(
                        QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                    "overlay damage tests must load the application translations");
            QApplication::installTranslator(&translator);
            QEvent language(QEvent::LanguageChange);
            QApplication::sendEvent(&canvas, &language);
            for (const bool clipped : {false, true}) {
                canvas.fitToCanvas();
                if (clipped) {
                    const auto viewport = canvasViewport(canvas);
                    const QPointF start(450, 320);
                    const auto end = start + viewport.topLeft() - QPointF(10, 10) - canvas.offset();
                    mouse(&canvas, QEvent::MouseButtonPress, start, Qt::MiddleButton,
                          Qt::MiddleButton);
                    mouse(&canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::MiddleButton);
                    mouse(&canvas, QEvent::MouseButtonRelease, end, Qt::MiddleButton, Qt::NoButton);
                }
                for (const qreal scale : {1.0, 1.5, 2.0}) {
                    QEvent leave(QEvent::Leave);
                    QApplication::sendEvent(&canvas, &leave);
                    pump();
                    const std::array points{canvas.imageToViewport({dense ? 1234.0 : 133.0, 80}),
                                            rulerPoint(canvas, true, dense ? 1457 : 77),
                                            rulerPoint(canvas, true, dense ? 1789 : 173),
                                            rulerPoint(canvas, false, 93), QPointF(1, 1)};
                    for (const auto& point : points) {
                        auto incremental = renderCanvas(canvas, scale);
                        observer.reset();
                        mouse(&canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
                        pump();
                        const auto damage = observer.damage;
                        require(!damage.isEmpty(),
                                "changing an overlay must schedule damaged pixels");
                        QPainter painter(&incremental);
                        canvas.render(&painter, damage.boundingRect().topLeft(), damage);
                        painter.end();
                        require(incremental == renderCanvas(canvas, scale),
                                "incremental overlays must match fresh renders across DPI, "
                                "languages, and clipped rulers");
                    }
                }
            }
            QApplication::removeTranslator(&translator);
        }
    }
}

void focusedDraftTeardown() {
    const std::array<const char*, 4> controls{"imageSplitCount", "imageSplitRows",
                                              "imageSplitColumns", "imageSplitSize"};
    for (const auto* name : controls) {
        for (const bool validDraft : {false, true}) {
            ImageSplitDocument document;
            document.setImage(image({1001, 800}));
            auto editor = std::make_unique<ImageSplitEditorWidget>(&document);
            show(*editor);
            if (QByteArray(name) == "imageSplitRows" || QByteArray(name) == "imageSplitColumns")
                child<AdSegmented>(*editor, "imageSplitDirection")
                    ->setCurrentValue(QStringLiteral("grid"));
            else if (QByteArray(name) == "imageSplitSize")
                child<AdSegmented>(*editor, "imageSplitType")
                    ->setCurrentValue(QStringLiteral("precise"));
            auto* input = child<AdInputNumber>(*editor, name);
            auto* text = input->findChild<QLineEdit*>();
            require(text, "teardown fixture must expose the numeric text editor");
            text->setFocus();
            pump();
            require(text->hasFocus(), "teardown fixture must destroy a focused numeric editor");
            text->selectAll();
            QKeyEvent key(QEvent::KeyPress, validDraft ? Qt::Key_6 : Qt::Key_0, Qt::NoModifier,
                          validDraft ? QStringLiteral("6") : QStringLiteral("0"));
            QApplication::sendEvent(text, &key);
            require(editor->configurationValid() == validDraft,
                    "teardown fixture must retain the entered draft validation state");
            const auto before = document.state();
            editor.reset();
            pump();
            require(document.state() == before,
                    "destroying a focused pending draft must preserve the surviving document");
        }
    }
}

void defaultsConditionalControlsAndActions() {
    ImageSplitDocument document;
    document.setImage(image({1200, 900}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    require(editor.canvas()->toolTip().isEmpty() &&
                !editor.canvas()->accessibleDescription().isEmpty(),
            "the preview canvas must keep accessible instructions without a tooltip");
    auto* form = child<AdForm>(editor, "imageSplitSettingsForm");
    require(
        !form->autoFillBackground(),
        "the settings form must inherit its modal background after attaching to the scroll area");
    auto* direction = child<AdSegmented>(editor, "imageSplitDirection");
    auto* type = child<AdSegmented>(editor, "imageSplitType");
    auto* count = child<AdInputNumber>(editor, "imageSplitCount");
    const auto options = type->options();
    require(options.size() == 3 && options[0].value == QStringLiteral("smart") &&
                options[1].value == QStringLiteral("equal") &&
                options[2].value == QStringLiteral("precise"),
            "split types place Smart first, followed by Equal and Precise");
    require(direction->currentValue() == QStringLiteral("vertical") &&
                type->currentValue() == QStringLiteral("smart") && count->value() == 2 &&
                editor.configurationValid() && document.layout().outputCount() == 2,
            "opening an image must show vertical Smart splitting into two images");
    require(!form->field(QStringLiteral("type"))->itemHidden() &&
                !form->field(QStringLiteral("count"))->itemHidden() &&
                form->field(QStringLiteral("rows"))->itemHidden() &&
                form->field(QStringLiteral("size"))->itemHidden(),
            "Smart splitting must show the same count controls as Equal splitting");
    const auto smartState = document.state();
    for (const auto* locale : {"en_US", "zh_CN", "zh_TW"}) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                "Smart labels use complete application catalogs");
        QApplication::installTranslator(&translator);
        QEvent language(QEvent::LanguageChange);
        QApplication::sendEvent(&editor, &language);
        const auto smart = QCoreApplication::translate("ImageSplitEditorWidget", "Smart split");
        require(type->optionLabel(0) == smart && type->currentValue() == QStringLiteral("smart") &&
                    document.state() == smartState &&
                    (QString::fromLatin1(locale) == QStringLiteral("en_US") ||
                     smart != QStringLiteral("Smart split")),
                "Smart label retranslates without changing selection or history");
        for (int option = 0; option < 3; ++option)
            require(type->rect().contains(type->optionRect(option)),
                    "all three split types fit the selector");
        QApplication::removeTranslator(&translator);
        QApplication::sendEvent(&editor, &language);
    }
    direction->setCurrentValue(QStringLiteral("grid"));
    require(form->field(QStringLiteral("type"))->itemHidden() &&
                form->field(QStringLiteral("count"))->itemHidden() &&
                !form->field(QStringLiteral("rows"))->itemHidden() &&
                !form->field(QStringLiteral("columns"))->itemHidden() &&
                document.layout().outputCount() == 4,
            "grid must hide split type and display the default two by two controls");
    pump();
    const auto rows = form->field(QStringLiteral("rows"))->geometry();
    const auto columns = form->field(QStringLiteral("columns"))->geometry();
    require(rows.top() == columns.top() && rows.right() < columns.left() &&
                std::abs(rows.width() - columns.width()) <= 1,
            "grid rows and columns must share one row with equal-width columns");
    const auto* rowsInput = child<AdInputNumber>(editor, "imageSplitRows");
    const auto* columnsInput = child<AdInputNumber>(editor, "imageSplitColumns");
    require(rowsInput->mapTo(form, QPoint{}).y() == columnsInput->mapTo(form, QPoint{}).y() &&
                rowsInput->width() == rows.width() && columnsInput->width() == columns.width(),
            "grid number inputs must align and fill their respective columns");
    child<AdInputNumber>(editor, "imageSplitColumns")->setValue(3);
    require(document.settings().rows == 2 && document.settings().columns == 3 &&
                document.layout().outputCount() == 6 &&
                document.layout().regionAt(3).top() == 450 &&
                document.layout().regionAt(3).left() == 0,
            "a rectangular grid must retain row-major output ordering");
    direction->setCurrentValue(QStringLiteral("horizontal"));
    type->setCurrentValue(QStringLiteral("precise"));
    require(!form->field(QStringLiteral("size"))->itemHidden() &&
                form->field(QStringLiteral("count"))->itemHidden() &&
                child<AdInputNumber>(editor, "imageSplitSize")->exactValue() ==
                    QStringLiteral("50") &&
                child<AdInputNumber>(editor, "imageSplitSize")->accessibleName() ==
                    QStringLiteral("Height per image"),
            "horizontal precise splitting must show a height and default fifty percent");
    const std::array<const char*, 4> names{"imageSplitUndo", "imageSplitRedo", "imageSplitFit",
                                           "imageSplitReload"};
    const std::array<const char*, 4> labels{"Undo", "Redo", "Fit to canvas", "Reload"};
    auto* toolbar = child<QWidget>(editor, "imageSplitCanvasToolbar");
    require(toolbar->parentWidget() == editor.canvas() && toolbar->isVisible() &&
                editor.canvas()->rect().contains(toolbar->geometry()),
            "canvas actions must float inside the preview canvas");
    int lastX = -1;
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto* button = child<AdButton>(editor, names[i]);
        require(button->buttonStyle() == AdButton::ButtonStyle::Text &&
                    button->sizeClass() == AdButton::SizeClass::Small &&
                    button->size() == QSize(26, 26) && button->text().isEmpty() &&
                    button->iconRef().isValid() &&
                    button->toolTip() == QString::fromLatin1(labels[i]) &&
                    button->accessibleName() == button->toolTip() &&
                    button->focusPolicy() == Qt::StrongFocus && button->parentWidget() == toolbar &&
                    button->x() > lastX,
                "floating actions must keep their order, compact icons and translated labels");
        lastX = button->x();
    }
}

class SizeLabelTranslator final : public QTranslator {
  public:
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QByteArray(context) == "ImageSplitEditorWidget") {
            if (QByteArray(source) == "Width per image")
                return QStringLiteral("每张图片宽度");
            if (QByteArray(source) == "Height per image")
                return QStringLiteral("每张图片高度");
        }
        return {};
    }
};

void preciseSizeUsesSharedFormLabel() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto original = themes.config();
    ImageSplitDocument document;
    document.setImage(image({1200, 900}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    auto* form = child<AdForm>(editor, "imageSplitSettingsForm");
    auto* type = child<AdSegmented>(editor, "imageSplitType");
    auto* direction = child<AdSegmented>(editor, "imageSplitDirection");
    auto* size = child<AdInputNumber>(editor, "imageSplitSize");
    auto* unit = child<AdSegmented>(editor, "imageSplitUnit");
    auto* field = form->field(QStringLiteral("size"));
    auto* label = field->findChild<QLabel*>(QStringLiteral("ad-form-item-label"));
    auto* reference = form->field(QStringLiteral("count"))
                          ->findChild<QLabel*>(QStringLiteral("ad-form-item-label"));
    require(field->controlWidget() == size && field->labelAccessoryWidget() == unit && label &&
                reference && label->buddy() == size,
            "precise image size must use the shared form field with a label and unit accessory");
    type->setCurrentValue(QStringLiteral("precise"));
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(theme);
        for (const auto& value : {QStringLiteral("vertical"), QStringLiteral("horizontal")}) {
            direction->setCurrentValue(value);
            pump();
            const auto expected = value == QStringLiteral("vertical")
                                      ? QStringLiteral("Width per image")
                                      : QStringLiteral("Height per image");
            require(field->label() == expected && label->text() == expected &&
                        size->accessibleName() == expected && label->font() == reference->font() &&
                        label->palette().color(QPalette::WindowText) ==
                            reference->palette().color(QPalette::WindowText),
                    "width and height labels must use the shared form typography and theme color");
            require(label->isVisible() && unit->isVisible() &&
                        label->parentWidget() == unit->parentWidget() &&
                        label->geometry().right() < unit->geometry().left() &&
                        unit->mapTo(field, QPoint(0, unit->height())).y() <=
                            size->mapTo(field, QPoint{}).y() &&
                        size->width() == field->width(),
                    "the unit selector must stay beside the label above a full-width number input");
        }
    }
    SizeLabelTranslator translator;
    const auto state = document.state();
    QApplication::installTranslator(&translator);
    QEvent language(QEvent::LanguageChange);
    QApplication::sendEvent(&editor, &language);
    require(field->label() == QStringLiteral("每张图片高度") && label->text() == field->label() &&
                size->accessibleName() == field->label() && document.state() == state,
            "runtime language changes must refresh the shared size label and accessible name");
    direction->setCurrentValue(QStringLiteral("vertical"));
    require(field->label() == QStringLiteral("每张图片宽度") && label->text() == field->label() &&
                size->accessibleName() == field->label(),
            "changing direction must update the translated size label and accessible name");
    QApplication::removeTranslator(&translator);
    QApplication::sendEvent(&editor, &language);
    type->setCurrentValue(QStringLiteral("equal"));
    require(!unit->isVisible(), "equal splitting must hide the precise size unit selector");
    themes.setConfig(original);
}

void floatingToolbarFollowsCanvas() {
    ImageSplitDocument document;
    document.setImage(image({1200, 900}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    auto* canvas = editor.canvas();
    auto* toolbar = child<QWidget>(editor, "imageSplitCanvasToolbar");
    const auto state = document.state();
    for (const QSize size : {QSize(1100, 720), QSize(900, 600), QSize(540, 240)}) {
        editor.resize(size);
        pump();
        require(canvas->rect().contains(toolbar->geometry()) &&
                    std::abs(QRectF(toolbar->geometry()).center().x() -
                             QRectF(canvas->rect()).center().x()) <= .5 &&
                    canvas->height() - toolbar->geometry().bottom() - 1 == 16,
                "floating toolbar must stay centered with a bottom inset when resized");
        const auto geometry = toolbar->geometry();
        canvas->fitToCanvas();
        const auto fittedZoom = canvas->zoom();
        const auto fittedOffset = canvas->offset();
        wheel(canvas, {100, 100}, 120);
        mouse(canvas, QEvent::MouseButtonPress, {80, 80}, Qt::MiddleButton, Qt::MiddleButton);
        mouse(canvas, QEvent::MouseMove, {100, 90}, Qt::NoButton, Qt::MiddleButton);
        mouse(canvas, QEvent::MouseButtonRelease, {100, 90}, Qt::MiddleButton, Qt::NoButton);
        pump();
        require(toolbar->geometry() == geometry,
                "zooming and panning must not move the floating toolbar");
        click(child<AdButton>(editor, "imageSplitFit"));
        require(qFuzzyCompare(canvas->zoom(), fittedZoom) &&
                    (canvas->offset() - fittedOffset).manhattanLength() < .001 &&
                    document.state() == state,
                "floating fit action must restore the viewport without editing the image");
        mouse(toolbar, QEvent::MouseButtonPress, {1, 1}, Qt::MiddleButton, Qt::MiddleButton);
        mouse(toolbar, QEvent::MouseMove, {6, 1}, Qt::NoButton, Qt::MiddleButton);
        mouse(toolbar, QEvent::MouseButtonRelease, {6, 1}, Qt::MiddleButton, Qt::NoButton);
        require(canvas->offset() == fittedOffset && document.state() == state,
                "toolbar background gestures must not reach the image canvas");
    }
}

void overlapControlValidationAndHistory() {
    ImageSplitDocument document;
    document.setImage(image({400, 240}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    auto* overlap = child<AdInputNumber>(editor, "imageSplitOverlapWidth");
    auto* form = child<AdForm>(editor, "imageSplitSettingsForm");
    auto* field = form->field(QStringLiteral("overlapWidth"));
    require(overlap->value() == 0 && overlap->suffixText() == QStringLiteral(" px") &&
                field->label() == QStringLiteral("Overlap width") &&
                overlap->accessibleName() == field->label() && !overlap->toolTip().isEmpty() &&
                overlap->accessibleDescription() == overlap->toolTip() &&
                form->items().indexOf(field) ==
                    form->items().indexOf(form->field(QStringLiteral("type"))) + 1,
            "overlap defaults to zero pixels immediately beneath split type with accessibility");
    require(document.createLine(true, 30), "add a manual guide before changing overlap");
    const auto manual = document.state();
    overlap->setValue(10);
    require(document.settings().overlapWidth == 10 && document.state().manuallyEdited &&
                document.state().verticalLines == manual.verticalLines,
            "changing the overlap control preserves manually edited guides");
    click(child<AdButton>(editor, "imageSplitUndo"));
    require(document.state() == manual && overlap->value() == 0,
            "undo restores the overlap control and original manual layout");
    click(child<AdButton>(editor, "imageSplitRedo"));
    require(overlap->value() == 10 && document.layout().overlapWidth == 10,
            "redo restores overlapping crops and the control");
    const auto committed = document.state();
    overlap->clear();
    require(!editor.configurationValid() && !field->errorMessages().isEmpty() &&
                !editor.commitPendingSettings() && document.state() == committed,
            "blank overlap drafts show inline errors without changing the layout");
    for (const double invalid : {-1.0, 2.5, double(std::numeric_limits<int>::max()) + 1}) {
        overlap->setValue(invalid);
        require(!editor.configurationValid() && !field->errorMessages().isEmpty() &&
                    !editor.commitPendingSettings() && document.state() == committed,
                "negative, fractional and overflowing overlap drafts are rejected");
    }
    overlap->setValue(std::numeric_limits<int>::max());
    require(editor.configurationValid() && document.layout().regionAt(0) == document.image().rect(),
            "maximum integer overlap is accepted and clipped to the image");
    overlap->setValue(0);
    require(editor.configurationValid() && field->errorMessages().isEmpty(),
            "zero overlap is valid and clears draft errors");
    child<AdSegmented>(editor, "imageSplitDirection")->setCurrentValue(QStringLiteral("grid"));
    require(overlap->isVisible() && !form->field(QStringLiteral("type"))->isVisible(),
            "grid mode keeps overlap visible when split type is hidden");
    editor.setLoading(true);
    require(!overlap->isEnabled(), "loading disables overlap editing");
    editor.setLoading(false);
    editor.setExportBusy(true);
    require(!overlap->isEnabled(), "exporting disables overlap editing");
    editor.setExportBusy(false);
    require(overlap->isEnabled(), "overlap editing resumes after busy operations");
    for (const auto* locale : {"en_US", "zh_CN", "zh_TW"}) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                "overlap translations must be available");
        QApplication::installTranslator(&translator);
        QEvent language(QEvent::LanguageChange);
        QApplication::sendEvent(&editor, &language);
        overlap->clear();
        const auto label = QCoreApplication::translate("ImageSplitEditorWidget", "Overlap width");
        require(field->label() == label && overlap->accessibleName() == label &&
                    (QByteArray(locale) == "en_US" || label != QStringLiteral("Overlap width")) &&
                    overlap->toolTip() ==
                        QCoreApplication::translate(
                            "ImageSplitEditorWidget",
                            "Each image extends this many pixels past each split line") &&
                    field->errorMessages().contains(QCoreApplication::translate(
                        "ImageSplitEditorWidget", "Enter a non-negative integer")),
                "overlap labels, descriptions and validation retranslate at runtime");
        overlap->setValue(10);
        QApplication::removeTranslator(&translator);
        QApplication::sendEvent(&editor, &language);
    }
    document.setImage(image({400, 240}));
    require(overlap->value() == 0 && document.layout().overlapWidth == 0 && !document.canUndo(),
            "replacement resets overlap without creating history");
}

void overlapBandsFollowGeometry() {
    ImageSplitDocument document;
    QImage source({400, 240}, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::white);
    document.setImage(source);
    auto settings = document.settings();
    settings.direction = ImageSplitDirection::Grid;
    settings.overlapWidth = 10;
    require(document.setSettings(settings), "configure grid overlap");
    ImageSplitPreviewCanvas canvas(&document);
    canvas.resize(900, 640);
    canvas.show();
    pump();
    for (const qreal scale : {1.0, 1.5, 2.0}) {
        const auto output = renderCanvas(canvas, scale);
        const auto pixel = [&](QPointF point) {
            point = canvas.imageToViewport(point) * scale;
            return output.pixelColor(qFloor(point.x()), qFloor(point.y()));
        };
        require(pixel({180, 80}) == QColor(Qt::white) && pixel({220, 80}) == QColor(Qt::white),
                "overlap shading stops at the expanded crop boundaries");
        const auto shaded = pixel({195, 80});
        require(shaded != QColor(Qt::white) && shaded == pixel({205, 80}) &&
                    shaded == pixel({80, 115}) && shaded == pixel({195, 115}),
                "both sides and grid intersections share one translucent fill");
        require(std::abs(shaded.red() - 217) <= 1 && std::abs(shaded.green() - 234) <= 1 &&
                    shaded.blue() == 255,
                "overlap uses the guide blue at twenty percent opacity");
    }
    require(document.beginLineDrag(true, 0) && document.moveDraggedLine(250),
            "move an overlapping guide");
    document.endLineDrag();
    const auto moved = renderCanvas(canvas, 1);
    const auto oldBand = canvas.imageToViewport({195, 80});
    const auto newBand = canvas.imageToViewport({245, 80});
    require(moved.pixelColor(qFloor(oldBand.x()), qFloor(oldBand.y())) == QColor(Qt::white) &&
                moved.pixelColor(qFloor(newBand.x()), qFloor(newBand.y())) != QColor(Qt::white),
            "moving a guide erases old shading and paints its new overlap");
    document.undo();
    settings.overlapWidth = 0;
    require(document.setSettings(settings), "disable overlap");
    const auto plain = renderCanvas(canvas, 1);
    require(plain.pixelColor(qFloor(oldBand.x()), qFloor(oldBand.y())) == QColor(Qt::white),
            "zero overlap removes shading");
    settings.overlapWidth = std::numeric_limits<int>::max();
    require(document.setSettings(settings), "oversized overlap remains renderable");
    const auto full = renderCanvas(canvas, 1);
    const auto corner = canvas.imageToViewport({20, 80});
    require(full.pixelColor(qFloor(corner.x()), qFloor(corner.y())) != QColor(Qt::white) &&
                full.pixelColor(5, 5) == plain.pixelColor(5, 5) && document.image() == source,
            "oversized bands clip to the image and never change source pixels");
}

void formValidationUnitsAndUndoableRegeneration() {
    ImageSplitDocument document;
    document.setImage(image({1001, 800}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    auto* count = child<AdInputNumber>(editor, "imageSplitCount");
    auto* form = child<AdForm>(editor, "imageSplitSettingsForm");
    count->clear();
    require(!editor.configurationValid() &&
                !form->field(QStringLiteral("count"))->errorMessages().isEmpty() &&
                document.settings().count == 2,
            "empty draft input must show an immediate inline error and preserve document settings");
    count->setValue(2.5);
    require(!editor.configurationValid() && document.settings().count == 2,
            "fractional image counts must be rejected instead of rounded");
    count->setValue(3);
    require(editor.configurationValid() && document.settings().count == 3,
            "correcting a draft must restore validity and commit the new count");
    document.createLine(true, 100);
    const auto manual = document.state();
    require(manual.manuallyEdited &&
                !editor.findChild<QLabel*>(QStringLiteral("imageSplitManualStatus")),
            "manual split line edits must not add a status message to the settings form");
    QMetaObject::invokeMethod(count, "editingFinished", Qt::DirectConnection);
    require(document.state() == manual,
            "finishing an unchanged numeric field must preserve manual split lines");
    count->setValue(4);
    require(!document.state().manuallyEdited && document.layout().outputCount() == 4,
            "a parameter change must replace all manually edited boundaries");
    click(child<AdButton>(editor, "imageSplitUndo"));
    require(document.state() == manual &&
                child<AdInputNumber>(editor, "imageSplitCount")->value() == 3,
            "undo must restore both manual lines and their form settings");
    click(child<AdButton>(editor, "imageSplitRedo"));
    require(document.settings().count == 4 && !document.state().manuallyEdited,
            "redo must restore parameter-generated boundaries");
    child<AdSegmented>(editor, "imageSplitType")->setCurrentValue(QStringLiteral("precise"));
    auto* size = child<AdInputNumber>(editor, "imageSplitSize");
    size->setExactValue(QStringLiteral("25"));
    const auto percentageLines = document.layout().verticalLines;
    require(percentageLines.size() == 3 && percentageLines.at(0) == 250 &&
                percentageLines.at(1) == 501 && percentageLines.at(2) == 751,
            "percentage edits must regenerate boundaries at rounded successive percentages");
    child<AdSegmented>(editor, "imageSplitUnit")->setCurrentValue(QStringLiteral("pixels"));
    require(document.settings().pixels == 250 && document.layout().outputCount() == 5,
            "switching to pixels must convert with half-up rounding and retain a remainder");
    size->setValue(300);
    child<AdSegmented>(editor, "imageSplitUnit")->setCurrentValue(QStringLiteral("percentage"));
    require(document.settings().percentage == QStringLiteral("29.97"),
            "switching to percentage must retain two decimal places");
    size->setExactValue(QStringLiteral("4.99"));
    require(!editor.configurationValid() &&
                document.settings().percentage == QStringLiteral("29.97"),
            "percentages below the chosen five-percent minimum must be invalid drafts");
    size->setExactValue(QStringLiteral("100"));
    require(editor.configurationValid() && document.layout().outputCount() == 1,
            "one hundred percent must produce exactly one output");
}

void unitConversionDoesNotClampInvalidResults() {
    ImageSplitDocument document;
    document.setImage(image({20, 10}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    child<AdSegmented>(editor, "imageSplitType")->setCurrentValue(QStringLiteral("precise"));
    auto* unit = child<AdSegmented>(editor, "imageSplitUnit");
    auto* size = child<AdInputNumber>(editor, "imageSplitSize");
    const auto beforeSmallConversion = document.state();
    unit->setCurrentValue(QStringLiteral("pixels"));
    require(unit->currentValue() == QStringLiteral("pixels") && size->value() == 10 &&
                !editor.configurationValid() && document.state() == beforeSmallConversion,
            "a pixel conversion below sixteen must remain visible and invalid without clamping or "
            "losing geometry");
    size->setValue(16);
    require(editor.configurationValid() && document.settings().unit == ImageSplitUnit::Pixels,
            "correcting a converted draft must commit its unit and size together");
    document.undo();
    require(document.state() == beforeSmallConversion &&
                unit->currentValue() == QStringLiteral("percentage"),
            "undoing a corrected unit conversion must restore the original unit and geometry");
    document.setImage(image({100, 80}));
    child<AdSegmented>(editor, "imageSplitType")->setCurrentValue(QStringLiteral("precise"));
    unit->setCurrentValue(QStringLiteral("pixels"));
    size->setValue(300);
    const auto beforeLargeConversion = document.state();
    unit->setCurrentValue(QStringLiteral("percentage"));
    require(size->exactValue() == QStringLiteral("300") && !editor.configurationValid() &&
                document.state() == beforeLargeConversion,
            "a pixel interval larger than the image must convert above one hundred percent without "
            "clamping");
    unit->setCurrentValue(QStringLiteral("pixels"));
    require(size->value() == 300 && editor.configurationValid() &&
                document.state() == beforeLargeConversion,
            "switching back from an invalid percentage must recover the unchanged pixel interval");
    document.setImage(image({1001, 800}));
    child<AdSegmented>(editor, "imageSplitType")->setCurrentValue(QStringLiteral("precise"));
    unit->setCurrentValue(QStringLiteral("pixels"));
    require(document.settings().pixels == 501,
            "percentage-to-pixel conversion must round exact half pixels upward");
}

void percentageDraftsKeepExactEnteredPrecision() {
    ImageSplitDocument document;
    document.setImage(image({1200, 900}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    child<AdSegmented>(editor, "imageSplitType")->setCurrentValue(QStringLiteral("precise"));
    auto* size = child<AdInputNumber>(editor, "imageSplitSize");
    auto* unit = child<AdSegmented>(editor, "imageSplitUnit");
    size->setExactValue(QStringLiteral("5.125"));
    require(editor.configurationValid() &&
                document.settings().percentage == QStringLiteral("5.125") &&
                document.layout().verticalLines.first() == 62,
            "entered percentages beyond two decimal places must remain exact and valid");
    unit->setCurrentValue(QStringLiteral("pixels"));
    require(document.settings().pixels == 62 && editor.configurationValid(),
            "exact percentage conversion must round half pixels upward");
    unit->setCurrentValue(QStringLiteral("percentage"));
    require(document.settings().percentage == QStringLiteral("5.17"),
            "pixel-to-percentage conversion must still display two decimal places");
    const auto belowHalf = QStringLiteral("5.1249999999999999999999999999999999999999999999999");
    size->setExactValue(belowHalf);
    require(editor.configurationValid() && document.settings().percentage == belowHalf &&
                document.layout().verticalLines.first() == 61,
            "a decimal just below a half pixel must not be rounded through floating point");
    unit->setCurrentValue(QStringLiteral("pixels"));
    require(document.settings().pixels == 61 && editor.configurationValid(),
            "unit conversion must use all entered percentage digits when rounding boundaries");
}

void transformedRulersAndManualGestures() {
    ImageSplitDocument document;
    document.setImage(image({1200, 900}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    auto* canvas = editor.canvas();
    const auto source = document.image();
    const QPointF anchor{250, 220};
    const auto anchoredPixel = canvas->viewportToImage(anchor);
    wheel(canvas, anchor, 240);
    require((canvas->viewportToImage(anchor) - anchoredPixel).manhattanLength() < .001,
            "wheel zoom must preserve the image coordinate under the pointer");
    mouse(canvas, QEvent::MouseButtonPress, {200, 150}, Qt::MiddleButton, Qt::MiddleButton);
    mouse(canvas, QEvent::MouseMove, {220, 180}, Qt::NoButton, Qt::MiddleButton);
    mouse(canvas, QEvent::MouseButtonRelease, {220, 180}, Qt::MiddleButton, Qt::NoButton);
    click(canvas, rulerPoint(*canvas, true, 300));
    require(document.layout().verticalLines.contains(300),
            "top ruler clicks must create image-pixel coordinates synchronized with zoom and pan");
    const auto beforeBlocked = document.state();
    click(canvas, rulerPoint(*canvas, false, 200));
    require(document.state() == beforeBlocked,
            "the left ruler must not create horizontal lines in vertical mode");
    const auto start = canvas->imageToViewport({300, 400});
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    for (int coordinate : {310, 320, 350})
        mouse(canvas, QEvent::MouseMove, canvas->imageToViewport({qreal(coordinate), 400}),
              Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, canvas->imageToViewport({350, 400}), Qt::LeftButton,
          Qt::NoButton);
    require(document.layout().verticalLines.contains(350),
            "dragging must move the selected split line");
    document.undo();
    require(document.layout().verticalLines.contains(300),
            "one undo must reverse the entire drag gesture");
    document.redo();
    const auto beforeCancel = document.state();
    mouse(canvas, QEvent::MouseButtonPress, canvas->imageToViewport({350, 400}), Qt::LeftButton,
          Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, canvas->imageToViewport({380, 400}), Qt::NoButton,
          Qt::LeftButton);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &escape);
    mouse(canvas, QEvent::MouseButtonRelease, canvas->imageToViewport({380, 400}), Qt::LeftButton,
          Qt::NoButton);
    require(document.state() == beforeCancel,
            "Escape must restore a provisional drag without history");
    click(canvas, canvas->imageToViewport({350, 400}));
    QKeyEvent deletion(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(canvas, &deletion);
    require(!document.layout().verticalLines.contains(350), "Delete must remove the selected line");
    document.undo();
    require(document.layout().verticalLines.contains(350), "line deletion must be undoable");
    const auto viewportZoom = canvas->zoom();
    const auto viewportOffset = canvas->offset();
    auto settings = document.settings();
    settings.count = 4;
    document.setSettings(settings);
    require(canvas->zoom() == viewportZoom && canvas->offset() == viewportOffset,
            "parameter regeneration must preserve the current viewport");
    canvas->fitToCanvas();
    const auto countBeforeViewport = document.state();
    wheel(canvas, {200, 150}, -120);
    canvas->fitToCanvas();
    require(document.state() == countBeforeViewport && document.image() == source,
            "viewport operations must preserve document state and source pixels");
    child<AdSegmented>(editor, "imageSplitDirection")->setCurrentValue(QStringLiteral("grid"));
    click(canvas, rulerPoint(*canvas, false, 200));
    require(document.layout().horizontalLines.contains(200),
            "grid must permit horizontal ruler edits");
}

void splitLineSelectionFollowsCrossing() {
    for (const bool vertical : {true, false}) {
        ImageSplitDocument document;
        document.setImage(image({400, 400}));
        auto settings = document.settings();
        settings.direction = ImageSplitDirection::Grid;
        settings.rows = 4;
        settings.columns = 4;
        require(document.setSettings(settings), "canvas crossing fixture settings apply");
        ImageSplitPreviewCanvas canvas(&document);
        canvas.resize(900, 700);
        canvas.show();
        pump();
        wheel(&canvas, canvas.imageToViewport({200, 200}), 120);
        mouse(&canvas, QEvent::MouseButtonPress, {80, 80}, Qt::MiddleButton, Qt::MiddleButton);
        mouse(&canvas, QEvent::MouseMove, {95, 92}, Qt::NoButton, Qt::MiddleButton);
        mouse(&canvas, QEvent::MouseButtonRelease, {95, 92}, Qt::MiddleButton, Qt::NoButton);
        const auto point = [&](int coordinate) {
            return canvas.imageToViewport(vertical ? QPointF(coordinate, 150)
                                                   : QPointF(150, coordinate));
        };
        const auto lines = [&] {
            const auto layout = document.layout();
            return vertical ? layout.verticalLines : layout.horizontalLines;
        };
        const auto original = document.state();
        mouse(&canvas, QEvent::MouseMove, point(100), Qt::NoButton, Qt::NoButton);
        mouse(&canvas, QEvent::MouseButtonPress, point(100), Qt::LeftButton, Qt::LeftButton);
        for (const int coordinate : {350, 50, 200, 250, 350})
            mouse(&canvas, QEvent::MouseMove, point(coordinate), Qt::NoButton, Qt::LeftButton);
        mouse(&canvas, QEvent::MouseButtonRelease, point(350), Qt::LeftButton, Qt::NoButton);
        require(lines() == QVector<int>{200, 300, 350},
                "zoomed and panned canvas drags can cross several lines in both directions");
        const auto moved = document.state();
        QKeyEvent deletion(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &deletion);
        require(lines() == QVector<int>{200, 300},
                "Delete follows the dragged line after it crosses other lines");
        document.undo();
        require(document.state() == moved, "undo restores the selected crossed line");
        document.undo();
        require(document.state() == original, "one undo reverses the canvas crossing gesture");
        document.redo();
        mouse(&canvas, QEvent::MouseButtonPress, point(350), Qt::LeftButton, Qt::LeftButton);
        mouse(&canvas, QEvent::MouseMove, point(50), Qt::NoButton, Qt::LeftButton);
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &escape);
        mouse(&canvas, QEvent::MouseButtonRelease, point(50), Qt::LeftButton, Qt::NoButton);
        require(document.state() == moved,
                "Escape restores crossed lines without changing history");
        QApplication::sendEvent(&canvas, &deletion);
        require(lines() == QVector<int>{200, 300},
                "Escape restores selection to the original dragged line before Delete");
    }
}

void replacementResetBusyAndDrops() {
    ImageSplitDocument document;
    document.setImage(image({1200, 900}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    document.createLine(true, 300);
    child<AdInputNumber>(editor, "imageSplitCount")->clear();
    document.setImage(image({800, 600}));
    require(editor.configurationValid() && !document.canUndo() && !document.canRedo() &&
                child<AdInputNumber>(editor, "imageSplitCount")->value() == 2 &&
                document.layout().outputCount() == 2 &&
                !child<AdButton>(editor, "imageSplitUndo")->isEnabled() &&
                !child<AdButton>(editor, "imageSplitRedo")->isEnabled(),
            "a successful image replacement must reset form drafts and history");
    int reloads = 0;
    QObject::connect(&editor, &ImageSplitEditorWidget::reloadRequested, &editor,
                     [&] { ++reloads; });
    click(child<AdButton>(editor, "imageSplitReload"));
    require(reloads == 1, "Reload must request file selection");
    QStringList received;
    QObject::connect(&editor, &ImageSplitEditorWidget::imageFilesDropped, &editor,
                     [&](const QStringList& paths) { received = paths; });
    QMimeData multiple;
    multiple.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/split-one.png")),
                      QUrl::fromLocalFile(QStringLiteral("C:/split-two.png"))});
    const auto beforeDrop = document.state();
    QDragEnterEvent enter(QPointF(100, 100), Qt::CopyAction, &multiple, Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(&editor, &enter);
    QDropEvent drop({100, 100}, Qt::CopyAction, &multiple, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&editor, &drop);
    require(received.size() == 2 && document.state() == beforeDrop,
            "multi-file drops must preserve all files for owner validation without changing edits");
    received.clear();
    QMimeData mixed;
    mixed.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/split-one.png")),
                   QUrl(QStringLiteral("https://example.com/image.png"))});
    QDragEnterEvent rejected(QPointF(100, 100), Qt::CopyAction, &mixed, Qt::LeftButton,
                             Qt::NoModifier);
    QApplication::sendEvent(&editor, &rejected);
    require(!rejected.isAccepted() && received.isEmpty(),
            "a mixed local and remote URL payload must never be filtered to one replacement");
    editor.setExportBusy(true);
    require(!child<AdButton>(editor, "imageSplitReload")->isEnabled() &&
                !child<AdSegmented>(editor, "imageSplitDirection")->isEnabled(),
            "busy export must prevent duplicate reloads and form changes");
    QDragEnterEvent busyEnter(QPointF(100, 100), Qt::CopyAction, &multiple, Qt::LeftButton,
                              Qt::NoModifier);
    QApplication::sendEvent(&editor, &busyEnter);
    require(!busyEnter.isAccepted(), "busy work must reject incoming image drops");
    editor.setExportBusy(false);
    require(child<AdButton>(editor, "imageSplitReload")->isEnabled(),
            "ending busy work must restore controls");
}

void dragFeedbackUsesUploadSurface() {
    ImageSplitDocument document;
    document.setImage(image({800, 600}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    const auto state = document.state();
    auto* hint = child<QWidget>(editor, "imageSplitDropHint");
    auto* title = child<QLabel>(editor, "imageSplitDropTitle");
    auto* description = child<QLabel>(editor, "imageSplitDropDescription");
    require(hint->isHidden() && hint->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                hint->accessibleName() == title->text() &&
                hint->accessibleDescription() == description->text(),
            "upload feedback must start hidden with accessible labels and pass-through input");
    QMimeData files;
    files.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/replacement.png"))});
    const auto enter = [&](QWidget* target, Qt::DropActions actions = Qt::CopyAction) {
        QDragEnterEvent event(QRectF(target->rect()).center(), actions, &files, Qt::LeftButton,
                              Qt::NoModifier);
        QApplication::sendEvent(target, &event);
        return event.isAccepted() && event.dropAction() == Qt::CopyAction;
    };
    const auto leave = [&] {
        QDragLeaveEvent event;
        QApplication::sendEvent(&editor, &event);
        require(event.isAccepted() && hint->isHidden(), "drag leave must clear upload feedback");
    };
    for (auto* target :
         {static_cast<QWidget*>(editor.canvas()), child<QWidget>(editor, "imageSplitCanvasToolbar"),
          child<QWidget>(editor, "imageSplitSettingsScroll")}) {
        require(enter(target) && hint->isVisible() &&
                    hint->geometry() == editor.canvas()->geometry(),
                "drags over the canvas, actions and settings must highlight only the preview");
        leave();
    }
    require(enter(&editor), "image files must activate upload feedback");
    editor.resize(900, 600);
    pump();
    require(hint->geometry() == editor.canvas()->geometry(),
            "upload feedback must follow the resized canvas");
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalTheme = themes.config();
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(theme);
        pump();
        const auto tokens = themes.resolveTheme(hint);
        require(title->font().pixelSize() == qRound(tokens.fontSizeLG) &&
                    description->font().pixelSize() == qRound(tokens.fontSize) &&
                    title->palette().color(QPalette::WindowText) == tokens.colorText &&
                    description->palette().color(QPalette::WindowText) == tokens.colorTextSecondary,
                "upload feedback must use Stitch Image's title and description typography");
        for (const qreal scale : {1.0, 1.5, 2.0}) {
            QImage pixels(QSize(qRound(hint->width() * scale), qRound(hint->height() * scale)),
                          QImage::Format_ARGB32_Premultiplied);
            pixels.setDevicePixelRatio(scale);
            pixels.fill(Qt::transparent);
            QPainter painter(&pixels);
            hint->render(&painter);
            painter.end();
            require(pixels.pixelColor(pixels.width() / 2, qRound(12 * scale)) ==
                            tokens.colorPrimaryBg &&
                        pixels.pixelColor(0, 0) == tokens.colorBgContainer,
                    "upload feedback must share Stitch Image's tinted surface and rounded corners "
                    "in both themes at every DPI scale");
            bool hasPrimaryIcon = false;
            const auto icon = hint->findChildren<QWidget*>(QString{}, Qt::FindDirectChildrenOnly);
            for (auto* widget : icon) {
                if (widget->size() != QSize(48, 48))
                    continue;
                const auto bounds = widget->geometry();
                for (int y = qRound(bounds.top() * scale);
                     y < qRound((bounds.bottom() + 1) * scale); ++y)
                    for (int x = qRound(bounds.left() * scale);
                         x < qRound((bounds.right() + 1) * scale); ++x)
                        hasPrimaryIcon |= pixels.pixelColor(x, y) == tokens.colorPrimary;
            }
            require(hasPrimaryIcon, "upload feedback must include the primary-colored inbox icon");
        }
    }
    themes.setConfig(originalTheme);
    for (const auto* language : {"en_US", "zh_CN", "zh_TW"}) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(language))),
                "upload feedback translations must be available in every language");
        QApplication::installTranslator(&translator);
        QEvent change(QEvent::LanguageChange);
        QApplication::sendEvent(&editor, &change);
        require(hint->isVisible() &&
                    title->text() == QCoreApplication::translate(
                                         "ImageSplitEditorWidget",
                                         "Drop one image to replace the current image") &&
                    description->text() ==
                        QCoreApplication::translate("ImageSplitEditorWidget",
                                                    "Drop one image file at a time") &&
                    hint->accessibleName() == title->text() &&
                    hint->accessibleDescription() == description->text(),
                "visible upload feedback and accessibility must retranslate together");
        QApplication::removeTranslator(&translator);
    }
    require(document.state() == state && !document.canUndo(),
            "drag feedback, resizing and language changes must preserve image edits");
    QDragMoveEvent rejected(QPointF(editor.rect().center()), Qt::MoveAction, &files, Qt::LeftButton,
                            Qt::NoModifier);
    QApplication::sendEvent(&editor, &rejected);
    require(!rejected.isAccepted() && hint->isHidden(),
            "a drag without copy permission must clear feedback");
    leave();
    require(!enter(&editor, Qt::MoveAction) && hint->isHidden(),
            "a move-only drag must not activate upload feedback");
    require(enter(&editor), "copy drags must recover after a rejected drag");
    int drops = 0;
    QObject::connect(&editor, &ImageSplitEditorWidget::imageFilesDropped, &editor,
                     [&](const QStringList&) { ++drops; });
    QDropEvent rejectedDrop(editor.rect().center(), Qt::MoveAction, &files, Qt::LeftButton,
                            Qt::NoModifier);
    QApplication::sendEvent(&editor, &rejectedDrop);
    require(!rejectedDrop.isAccepted() && hint->isHidden() && drops == 0,
            "a move-only drop must not replace an image or leave stale feedback");
    require(enter(&editor), "copy drags must recover after a rejected drop");
    editor.setLoading(true);
    require(hint->isHidden() && !enter(&editor), "loading must clear and block upload feedback");
    editor.setLoading(false);
    require(enter(&editor), "upload feedback must recover after loading");
    editor.setExportBusy(true);
    require(hint->isHidden() && !enter(&editor), "export must clear and block upload feedback");
    editor.setExportBusy(false);
    require(enter(&editor), "upload feedback must recover after export");
    editor.hide();
    require(hint->isHidden(), "hiding the editor must clear upload feedback");
}

void invalidDraftsSurviveFocusAndCanceledLoading() {
    ImageSplitDocument document;
    document.setImage(image({1001, 800}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    auto* count = child<AdInputNumber>(editor, "imageSplitCount");
    auto* form = child<AdForm>(editor, "imageSplitSettingsForm");
    count->setFocus();
    count->setValue(0);
    const auto countState = document.state();
    editor.canvas()->setFocus();
    pump();
    editor.setLoading(true);
    editor.setLoading(false);
    require(count->displayText() == QStringLiteral("0") && !editor.configurationValid() &&
                !form->field(QStringLiteral("count"))->errorMessages().isEmpty() &&
                document.state() == countState,
            "an invalid zero count must survive focus loss and canceled replacement loading");
    child<AdSegmented>(editor, "imageSplitType")->setCurrentValue(QStringLiteral("precise"));
    auto* size = child<AdInputNumber>(editor, "imageSplitSize");
    document.createLine(true, 100);
    const auto percentageState = document.state();
    size->setFocus();
    size->setExactValue(QStringLiteral("4.99"));
    editor.canvas()->setFocus();
    pump();
    editor.setLoading(true);
    editor.setLoading(false);
    require(size->displayText() == QStringLiteral("4.99") && !editor.configurationValid() &&
                !form->field(QStringLiteral("size"))->errorMessages().isEmpty() &&
                document.state() == percentageState && document.canUndo(),
            "an invalid precise percentage must retain its exact text, errors, geometry and "
            "history after canceled loading");
    child<AdSegmented>(editor, "imageSplitUnit")->setCurrentValue(QStringLiteral("pixels"));
    require(size->value() == 50 && editor.configurationValid() && document.settings().pixels == 50,
            "changing units must convert the visible invalid draft rather than a stale committed "
            "percentage");
}

void invalidDirectionDoesNotEditThePreviousAxis() {
    ImageSplitDocument document;
    document.setImage(image({1200, 1}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    auto* direction = child<AdSegmented>(editor, "imageSplitDirection");
    direction->setCurrentValue(QStringLiteral("horizontal"));
    require(!editor.configurationValid() &&
                direction->currentValue() == QStringLiteral("horizontal") &&
                document.settings().direction == ImageSplitDirection::Vertical,
            "an invalid direction draft must remain displayed while preserving valid geometry");
    const auto previous = document.state();
    click(editor.canvas(), rulerPoint(*editor.canvas(), true, 300));
    require(
        document.state() == previous,
        "an invalid displayed horizontal direction must not allow edits to the old vertical axis");
    child<AdInputNumber>(editor, "imageSplitCount")->setValue(1);
    require(editor.configurationValid() &&
                document.settings().direction == ImageSplitDirection::Horizontal,
            "correcting the count must commit the displayed direction and count together");
}

void submissionCommitsValidPendingDraftsOnly() {
    ImageSplitDocument document;
    document.setImage(image({1001, 800}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    auto* count = child<AdInputNumber>(editor, "imageSplitCount");
    auto* text = count->findChild<QLineEdit*>();
    require(text != nullptr, "number control must expose its text editor");
    text->setFocus();
    text->selectAll();
    QKeyEvent three(QEvent::KeyPress, Qt::Key_3, Qt::NoModifier, QStringLiteral("3"));
    QApplication::sendEvent(text, &three);
    require(editor.configurationValid() && count->displayText() == QStringLiteral("3") &&
                document.settings().count == 2 && !document.canUndo(),
            "valid keyboard drafts must wait for a commit without changing document history");
    require(editor.commitPendingSettings() && document.settings().count == 3 &&
                document.layout().outputCount() == 3,
            "submission must commit the visible valid draft before using output geometry");
    document.undo();
    require(document.settings().count == 2 && !document.canUndo() && document.canRedo(),
            "submitting one pending draft must create exactly one undo operation");
    document.redo();
    const auto beforeInvalid = document.state();
    text->setFocus();
    text->selectAll();
    QKeyEvent zero(QEvent::KeyPress, Qt::Key_0, Qt::NoModifier, QStringLiteral("0"));
    QApplication::sendEvent(text, &zero);
    require(!editor.commitPendingSettings() && !editor.configurationValid() &&
                document.state() == beforeInvalid,
            "submission must reject invalid pending drafts without changing output geometry");
}

void submissionRequiresTheRequestedSettings() {
    ImageSplitDocument document;
    document.setImage(image({1001, 800}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    auto* count = child<AdInputNumber>(editor, "imageSplitCount");
    auto* text = count->findChild<QLineEdit*>();
    require(text != nullptr, "number control must expose its text editor");
    const auto typeThree = [&] {
        text->setFocus();
        text->selectAll();
        QKeyEvent three(QEvent::KeyPress, Qt::Key_3, Qt::NoModifier, QStringLiteral("3"));
        QApplication::sendEvent(text, &three);
    };
    editor.setSettingsCommitter([&](const ImageSplitSettings&) { editor.setCalculating(true); });
    typeThree();
    require(!editor.commitPendingSettings() && editor.calculating() && editor.configurationValid(),
            "valid input remains actionable while its geometry is still pending");
    editor.setCalculating(false);
    editor.restoreCommittedSettings();
    editor.setCalculating(true);
    require(!editor.commitPendingSettings(),
            "even matching settings cannot be submitted while calculation is pending");
    editor.setCalculating(false);
    editor.setSettingsCommitter(
        [&](const ImageSplitSettings&) { editor.restoreCommittedSettings(); });
    typeThree();
    require(!editor.commitPendingSettings() && editor.configurationValid() &&
                document.settings().count == 2 && count->value() == 2 && !document.canUndo(),
            "a synchronous commit failure cannot submit the restored previous settings");
    editor.setSettingsCommitter({});
}

void focusedDraftsAreDiscardedOnDestruction() {
    for (const auto* name :
         {"imageSplitCount", "imageSplitRows", "imageSplitColumns", "imageSplitSize"}) {
        for (const auto* draft : {"75", "0"}) {
            ImageSplitDocument document;
            document.setImage(image({1001, 800}));
            auto settings = document.settings();
            if (QString::fromLatin1(name) == QStringLiteral("imageSplitRows") ||
                QString::fromLatin1(name) == QStringLiteral("imageSplitColumns"))
                settings.direction = ImageSplitDirection::Grid;
            if (QString::fromLatin1(name) == QStringLiteral("imageSplitSize"))
                settings.type = ImageSplitType::Precise;
            document.setSettings(settings);
            const auto state = document.state();
            auto editor = std::make_unique<ImageSplitEditorWidget>(&document);
            show(*editor);
            auto* input = child<AdInputNumber>(*editor, name);
            auto* text = input->findChild<QLineEdit*>();
            require(text != nullptr, "number control must expose its text editor");
            text->setFocus();
            pump();
            require(text->hasFocus(), "the pending draft must retain focus during teardown");
            text->selectAll();
            for (const auto character : QString::fromLatin1(draft)) {
                QKeyEvent key(QEvent::KeyPress, character.unicode(), Qt::NoModifier,
                              QString(character));
                QApplication::sendEvent(text, &key);
            }
            require(input->displayText() == QString::fromLatin1(draft) && document.state() == state,
                    "focused keyboard drafts must stay uncommitted before teardown");
            editor.reset();
            pump();
            require(document.state() == state,
                    "destroying a focused split editor must discard pending drafts safely");
        }
    }
}

void zoomedCanvasUsesFullResolutionPixels() {
    ImageSplitDocument document;
    QImage source({4096, 512}, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::red);
    source.setPixelColor(2048, 256, Qt::blue);
    QImage preview({2048, 256}, QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::green);
    document.setImage(source, preview);
    ImageSplitPreviewCanvas canvas(&document);
    canvas.resize(500, 400);
    canvas.show();
    pump();
    const auto pixelCenter = canvas.imageToViewport({2048.5, 256.5});
    wheel(&canvas, pixelCenter, 3600);
    require(canvas.zoom() > 2, "fixture must magnify individual source pixels");
    QImage output(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    canvas.render(&painter);
    painter.end();
    const auto position = canvas.imageToViewport({2048.5, 256.5});
    const QColor color = output.pixelColor(static_cast<int>(std::floor(position.x())),
                                           static_cast<int>(std::floor(position.y())));
    require(
        color == QColor(Qt::blue) && document.image() == source,
        "zoomed previews must draw full-resolution source pixels while preserving the original");
}

void sequenceNumbersUsePdfBadgeStyle() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalTheme = themes.config();
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(theme);
        ImageSplitDocument document;
        QImage source({1000, 1000}, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::white);
        document.setImage(source);
        auto settings = document.settings();
        settings.direction = ImageSplitDirection::Grid;
        settings.rows = 10;
        settings.columns = 10;
        settings.overlapWidth = 3;
        require(document.setSettings(settings), "badge fixture must produce one hundred results");
        ImageSplitPreviewCanvas canvas(&document);
        canvas.resize(1120, 1080);
        canvas.show();
        pump();
        const auto state = document.state();
        for (const int fontSize : {13, 17}) {
            auto font = canvas.font();
            font.setPixelSize(fontSize);
            canvas.setFont(font);
            pump();
            for (const qreal scale : {1.0, 1.25, 1.5, 2.0}) {
                const auto output = renderCanvas(canvas, scale);
                for (const int index : {0, 9, 99}) {
                    const auto label = QString::number(index + 1);
                    const QSize size(
                        std::max(24, canvas.fontMetrics().horizontalAdvance(label) + 12), 22);
                    const auto origin =
                        canvas.imageToViewport(document.layout().baseRegionAt(index).topLeft()) +
                        QPointF(4, 4);
                    // Render the existing PDF page label independently as the visual reference.
                    QLabel reference;
                    reference.setFont(canvas.font());
                    reference.setAlignment(Qt::AlignCenter);
                    reference.setText(label);
                    reference.setStyleSheet(QStringLiteral(
                        "color: white; background-color: rgba(0, 0, 0, 130); border-radius: 4px;"));
                    reference.resize(size);
                    QImage expected(output.size(), QImage::Format_ARGB32_Premultiplied);
                    expected.setDevicePixelRatio(scale);
                    expected.fill(Qt::white);
                    QPainter painter(&expected);
                    painter.translate(origin);
                    reference.render(&painter, {}, {}, QWidget::DrawChildren);
                    painter.end();
                    const QRect bounds(qFloor(origin.x() * scale), qFloor(origin.y() * scale),
                                       qCeil(size.width() * scale), qCeil(size.height() * scale));
                    int different = 0;
                    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
                        for (int x = bounds.left(); x <= bounds.right(); ++x) {
                            const auto actual = output.pixelColor(x, y);
                            const auto wanted = expected.pixelColor(x, y);
                            if (std::abs(actual.red() - wanted.red()) > 8 ||
                                std::abs(actual.green() - wanted.green()) > 8 ||
                                std::abs(actual.blue() - wanted.blue()) > 8)
                                ++different;
                        }
                    }
                    const bool matches = different < bounds.width() * bounds.height() / 10;
                    if (!matches) {
                        std::cerr << "Badge " << label.toStdString() << ", font " << fontSize
                                  << ", scale " << scale << ": " << different << " of "
                                  << bounds.width() * bounds.height() << " pixels differ\n";
                        const auto directory =
                            qEnvironmentVariable("SNOW_TEST_IMAGE_SPLIT_CANVAS_ARTIFACT_DIR");
                        if (!directory.isEmpty()) {
                            output.copy(bounds).save(QDir(directory).filePath("badge-actual.png"));
                            expected.copy(bounds).save(QDir(directory).filePath("badge-pdf.png"));
                        }
                    }
                    require(matches,
                            "result ordinals must match PDF badge size, corners, opacity and font "
                            "in both themes at every DPI");
                    const auto fill = output.pixelColor(qFloor((origin.x() + 5) * scale),
                                                        qFloor((origin.y() + 2) * scale));
                    require(fill == QColor(125, 125, 125),
                            "sequence badges must retain the PDF translucent black background");
                }
            }
        }
        require(document.state() == state && document.image() == source,
                "sequence number styling must preserve split geometry and source pixels");
    }
    themes.setConfig(originalTheme);
}

void denseGuidesDoNotHideLaterReadableRegions() {
    ImageSplitDocument document;
    QImage source(QSize(5001, 300), QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::red);
    document.setImage(source);
    auto settings = document.settings();
    settings.count = 5001;
    require(document.setSettings(settings), "dense fixture must generate one-pixel columns");
    while (document.layout().verticalLines.size() > 4001)
        require(document.removeLine(true, 4001), "dense fixture must merge its trailing columns");
    ImageSplitPreviewCanvas canvas(&document);
    canvas.resize(900, 400);
    canvas.show();
    pump();
    const auto state = document.state();
    QImage output(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    canvas.render(&painter);
    painter.end();
    const auto readableRegion = document.layout().regionAt(4001);
    const auto badge = canvas.imageToViewport(readableRegion.topLeft()) + QPointF(9, 9);
    require(output.pixelColor(static_cast<int>(std::floor(badge.x())),
                              static_cast<int>(std::floor(badge.y()))) != QColor(Qt::red),
            "a readable ordinal after four thousand thin columns must remain visible");
    require(document.state() == state && document.image() == source,
            "dense preview culling must preserve split geometry and source pixels");
}

void elongatedImagesKeepViewportAndLineInteractionsUsable() {
    constexpr int count = 1000000;
    ImageSplitDocument document;
    QImage source(QSize(count, 1), QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::red);
    QImage preview(QSize(2048, 1), QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::red);
    document.setImage(source, preview);
    auto settings = document.settings();
    settings.count = count;
    settings.overlapWidth = 1;
    require(document.setSettings(settings), "elongated fixture must allow one-pixel sections");
    ImageSplitEditorWidget editor(&document);
    show(editor);
    auto* canvas = editor.canvas();
    const auto original = document.state();
    const auto paint = [&] {
        QImage output(canvas->size(), QImage::Format_ARGB32_Premultiplied);
        output.fill(Qt::transparent);
        QPainter painter(&output);
        canvas->render(&painter);
    };
    paint();
    const auto anchor = canvas->imageToViewport({count / 2.0, .5});
    const auto anchoredPixel = canvas->viewportToImage(anchor);
    wheel(canvas, anchor, 240);
    require((canvas->viewportToImage(anchor) - anchoredPixel).manhattanLength() < .001,
            "extreme aspect ratios must preserve image-pixel zoom anchoring");
    mouse(canvas, QEvent::MouseButtonPress, anchor, Qt::MiddleButton, Qt::MiddleButton);
    mouse(canvas, QEvent::MouseMove, anchor + QPointF(10, 0), Qt::NoButton, Qt::MiddleButton);
    mouse(canvas, QEvent::MouseButtonRelease, anchor + QPointF(10, 0), Qt::MiddleButton,
          Qt::NoButton);
    paint();
    require(document.state() == original,
            "painting, zooming and panning a million-section image must preserve split geometry");
    click(canvas, canvas->imageToViewport({count / 2.0, .5}));
    QKeyEvent deletion(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(canvas, &deletion);
    require(document.layout().outputCount() == count - 1,
            "hit testing must allow selecting and deleting a guide in a dense elongated image");
    document.undo();
    require(document.state() == original && document.layout().outputCount() == count &&
                document.image() == source,
            "dense line deletion must be undoable without altering full-resolution pixels");
}

class SplitTranslator final : public QTranslator {
  public:
    QString translate(const char* context, const char* source, const char*, int) const override {
        if (QByteArray(context) == "ImageSplitEditorWidget" && QByteArray(source) == "Reload")
            return QStringLiteral("重新加载");
        if (QByteArray(context) == "ImageSplitPreviewCanvas" &&
            QByteArray(source) == "Image splitting preview")
            return QStringLiteral("分割预览");
        return {};
    }
};
void fittedPreviewReservesOnlyRulerWidth() {
    ImageSplitDocument document;
    ImageSplitPreviewCanvas canvas(&document);
    canvas.resize(900, 1200);
    canvas.show();
    pump();
    for (const QSize size : {QSize(1200, 300), QSize(1200, 900)}) {
        document.setImage(image(size));
        const auto state = document.state();
        for (const auto* locale : {"en_US", "zh_CN", "zh_TW"}) {
            QTranslator translator;
            require(translator.load(
                        QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                    "preview spacing tests must load the application translations");
            QApplication::installTranslator(&translator);
            QEvent language(QEvent::LanguageChange);
            QApplication::sendEvent(&canvas, &language);
            canvas.fitToCanvas();
            const auto topLeft = canvas.imageToViewport({0, 0});
            const auto bottomRight =
                canvas.imageToViewport({qreal(size.width()), qreal(size.height())});
            require(std::abs(topLeft.x() - 38 - 24) < .001 &&
                        std::abs(canvas.width() - bottomRight.x() - 24) < .001,
                    "fitting must leave equal padding around the image and its left ruler, "
                    "without reserving an extra coordinate-hint gutter");
            require(document.state() == state,
                    "fitting and translating the preview must preserve the split document");
            QApplication::removeTranslator(&translator);
            QApplication::sendEvent(&canvas, &language);
        }
    }
}

void canvasBackgroundAndRulersFollowImage() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto original = themes.config();
    const QColor background(201, 183, 165);
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(theme);
        QWidget host;
        host.resize(900, 640);
        host.setAutoFillBackground(true);
        auto palette = host.palette();
        palette.setColor(QPalette::Window, background);
        host.setPalette(palette);
        ImageSplitDocument document;
        QImage source({400, 240}, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::black);
        document.setImage(source);
        auto settings = document.settings();
        settings.direction = ImageSplitDirection::Grid;
        document.setSettings(settings);
        ImageSplitPreviewCanvas canvas(&document, &host);
        canvas.resize(host.size());
        host.show();
        pump();
        const auto verifyRulers = [&] {
            const auto bounds =
                QRectF(canvas.offset(), QSizeF(document.image().size()) * canvas.zoom())
                    .intersected(canvasViewport(canvas));
            require(!bounds.isEmpty(), "ruler fixture must leave part of the image visible");
            for (const qreal scale : {1.0, 1.25, 1.5, 1.75, 2.0}) {
                const auto output = renderCanvas(canvas, scale);
                const auto pixel = [&](QPointF point) {
                    return output.pixelColor(qFloor(point.x() * scale), qFloor(point.y() * scale));
                };
                require(pixel({5, 5}) == background &&
                            pixel({canvas.width() - 5.0, canvas.height() - 5.0}) == background,
                        "canvas whitespace must inherit the host background in both themes");
                require(pixel({bounds.center().x(), bounds.top() - 2}) != background &&
                            pixel({bounds.left() - 2, bounds.center().y()}) != background,
                        "both rulers must draw directly against the visible image edges");
                if (bounds.top() > 60)
                    require(pixel({bounds.center().x(), bounds.top() - 30}) == background,
                            "the top ruler must occupy only the strip beside the image");
                if (bounds.left() > 80)
                    require(pixel({bounds.left() - 40, bounds.center().y()}) == background,
                            "the left ruler must occupy only the strip beside the image");
                if (bounds.right() < canvas.width() - 40)
                    require(pixel({bounds.right() + 10, bounds.top() - 10}) == background,
                            "rulers must stop at the visible image extent");
            }
        };
        verifyRulers();
        const auto initial = document.state();
        click(&canvas, {canvas.imageToViewport({113, 0}).x(), 12});
        require(document.state() == initial,
                "empty space at the widget edge must no longer act as an image ruler");
        click(&canvas, rulerPoint(canvas, true, 113));
        click(&canvas, rulerPoint(canvas, false, 73));
        require(document.layout().verticalLines.contains(113) &&
                    document.layout().horizontalLines.contains(73),
                "image-attached rulers must add split lines on both axes");
        const auto beforePan = document.state();
        const auto pan = [&](QPointF delta) {
            mouse(&canvas, QEvent::MouseButtonPress, {80, 80}, Qt::MiddleButton, Qt::MiddleButton);
            mouse(&canvas, QEvent::MouseMove, QPointF(80, 80) + delta, Qt::NoButton,
                  Qt::MiddleButton);
            mouse(&canvas, QEvent::MouseButtonRelease, QPointF(80, 80) + delta, Qt::MiddleButton,
                  Qt::NoButton);
        };
        wheel(&canvas, canvas.imageToViewport({200, 120}), 240);
        pan({35, 27});
        verifyRulers();
        pan(QPointF(10, 10) - canvas.offset());
        verifyRulers();
        const auto topPoint = rulerPoint(canvas, true, 173);
        mouse(&canvas, QEvent::MouseMove, topPoint, Qt::NoButton, Qt::NoButton);
        require(canvas.cursor().shape() == Qt::CrossCursor,
                "a clipped image must retain an interactive ruler along its visible edge");
        require(document.state() == beforePan && document.image() == source,
                "moving the attached rulers must preserve split coordinates and image pixels");
        click(&canvas, topPoint);
        require(document.layout().verticalLines.contains(173),
                "clipped rulers must still map clicks to the original image coordinates");
        pan({1000, 1000});
        const auto invisible = document.state();
        mouse(&canvas, QEvent::MouseMove, {100, 12}, Qt::NoButton, Qt::NoButton);
        require(canvas.cursor().shape() != Qt::CrossCursor,
                "rulers must disappear when the image is entirely outside the viewport");
        click(&canvas, {100, 12});
        const auto output = renderCanvas(canvas, 1.5);
        require(output.pixelColor(output.width() / 2, output.height() / 2) == background &&
                    document.state() == invisible,
                "an offscreen image must leave only the inherited canvas background");
    }
    themes.setConfig(original);
}

void splitPositionBadgesFollowRulers() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto original = themes.config();
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(theme);
        for (const bool vertical : {true, false}) {
            ImageSplitDocument document;
            QImage source({400, 240}, QImage::Format_ARGB32_Premultiplied);
            source.fill(Qt::black);
            document.setImage(source);
            auto settings = document.settings();
            settings.direction =
                vertical ? ImageSplitDirection::Vertical : ImageSplitDirection::Horizontal;
            settings.count = 2;
            document.setSettings(settings);
            ImageSplitPreviewCanvas canvas(&document);
            canvas.resize(900, 640);
            canvas.show();
            pump();
            const auto pan = [&](QPointF delta) {
                mouse(&canvas, QEvent::MouseButtonPress, {80, 80}, Qt::MiddleButton,
                      Qt::MiddleButton);
                mouse(&canvas, QEvent::MouseMove, QPointF(80, 80) + delta, Qt::NoButton,
                      Qt::MiddleButton);
                mouse(&canvas, QEvent::MouseButtonRelease, QPointF(80, 80) + delta,
                      Qt::MiddleButton, Qt::NoButton);
            };
            const auto badgeBounds = [&](const QImage& output) {
                const auto primary = themes.resolveTheme(&canvas).colorPrimary.rgba();
                QRect bounds;
                for (int y = 0; y < output.height(); ++y) {
                    const auto* pixels = reinterpret_cast<const QRgb*>(output.constScanLine(y));
                    for (int x = 0; x < output.width(); ++x) {
                        if (pixels[x] == primary)
                            bounds = bounds.united(QRect(x, y, 1, 1));
                    }
                }
                const qreal scale = output.devicePixelRatio();
                return QRectF(bounds.x() / scale, bounds.y() / scale, bounds.width() / scale,
                              bounds.height() / scale);
            };
            const auto verifyBadge = [&](int coordinate) {
                const auto imageBounds =
                    QRectF(canvas.offset(), QSizeF(document.image().size()) * canvas.zoom())
                        .intersected(canvasViewport(canvas));
                const auto ruler = vertical ? QRectF(imageBounds.left(), imageBounds.top() - 28,
                                                     imageBounds.width(), 28)
                                            : QRectF(imageBounds.left() - 38, imageBounds.top(), 38,
                                                     imageBounds.height());
                const auto center = rulerPoint(canvas, vertical, coordinate);
                for (const qreal scale : {1.0, 1.25, 1.5, 1.75, 2.0}) {
                    const auto bounds = badgeBounds(renderCanvas(canvas, scale));
                    require(!bounds.isEmpty() && QRectF(canvas.rect()).contains(bounds) &&
                                ruler.intersects(bounds),
                            "split position badges must remain visible beside their ruler");
                    const qreal gap = vertical ? imageBounds.top() - bounds.bottom()
                                               : imageBounds.left() - bounds.right();
                    if (vertical || bounds.width() + 3 <= imageBounds.left()) {
                        require(std::abs(gap - 3) <= 1 / scale,
                                "coordinate badges must keep a three-pixel image gap when space "
                                "allows");
                    } else {
                        require(std::abs(bounds.left()) <= 1 / scale,
                                "a wide Y hint must clamp to the canvas edge without widening "
                                "the preview gutter");
                    }
                    require(std::abs(vertical ? bounds.center().x() - center.x()
                                              : bounds.center().y() - center.y()) <= 1 / scale,
                            "ruler position badges must align with the split line at every DPI");
                    QFont badgeFont = canvas.font();
                    badgeFont.setPixelSize(12);
                    const auto label =
                        QCoreApplication::translate("ImageSplitPreviewCanvas",
                                                    vertical ? "X: %1 px" : "Y: %1 px")
                            .arg(coordinate);
                    require(bounds.width() > bounds.height() &&
                                std::abs(bounds.width() -
                                         (QFontMetrics(badgeFont).horizontalAdvance(label) + 12)) <=
                                    2 / scale,
                            "both axes must show full-width horizontal coordinate hints");
                }
            };
            for (const auto* locale : {"en_US", "zh_CN", "zh_TW"}) {
                QTranslator translator;
                require(
                    translator.load(
                        QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(locale))),
                    "coordinate badge tests must load the application translations");
                QApplication::installTranslator(&translator);
                QEvent language(QEvent::LanguageChange);
                QApplication::sendEvent(&canvas, &language);
                canvas.resize(900, 640);
                document.setImage(source);
                document.setSettings(settings);
                canvas.fitToCanvas();
                for (const bool transformed : {false, true}) {
                    if (transformed) {
                        wheel(&canvas, canvas.imageToViewport({200, 120}), 240);
                        pan(QPointF(10, 10) - canvas.offset());
                    }
                    const int coordinate = vertical ? 200 : 120;
                    const auto line = canvas.imageToViewport(vertical ? QPointF(coordinate, 80)
                                                                      : QPointF(80, coordinate));
                    click(&canvas, canvas.imageToViewport({160, 80}));
                    const auto before = document.state();
                    const bool couldUndo = document.canUndo();
                    const bool couldRedo = document.canRedo();
                    mouse(&canvas, QEvent::MouseMove, rulerPoint(canvas, vertical, coordinate - 37),
                          Qt::NoButton, Qt::NoButton);
                    verifyBadge(coordinate - 37);
                    QEvent hoverLeave(QEvent::Leave);
                    QApplication::sendEvent(&canvas, &hoverLeave);
                    require(
                        badgeBounds(renderCanvas(canvas, 1.5)).isEmpty(),
                        "leaving a ruler must clear its coordinate hint without selecting a line");
                    mouse(&canvas, QEvent::MouseMove, rulerPoint(canvas, vertical, coordinate - 37),
                          Qt::NoButton, Qt::NoButton);
                    canvas.setEditingEnabled(false);
                    require(badgeBounds(renderCanvas(canvas, 1.5)).isEmpty(),
                            "disabled editing must clear the ruler coordinate hint");
                    canvas.setEditingEnabled(true);
                    require(badgeBounds(renderCanvas(canvas, 1.5)).isEmpty(),
                            "re-enabling editing must not restore a stale coordinate hint");
                    mouse(&canvas, QEvent::MouseMove, line, Qt::NoButton, Qt::NoButton);
                    verifyBadge(coordinate);
                    require(document.state() == before && document.canUndo() == couldUndo &&
                                document.canRedo() == couldRedo,
                            "showing a ruler badge must preserve split settings and history");
                    click(&canvas, line);
                    QEvent leave(QEvent::Leave);
                    QApplication::sendEvent(&canvas, &leave);
                    verifyBadge(coordinate);
                    mouse(&canvas, QEvent::MouseMove, rulerPoint(canvas, vertical, coordinate + 37),
                          Qt::NoButton, Qt::NoButton);
                    verifyBadge(coordinate + 37);
                    QApplication::sendEvent(&canvas, &leave);
                    verifyBadge(coordinate);
                    mouse(&canvas, QEvent::MouseButtonPress, line, Qt::LeftButton, Qt::LeftButton);
                    const int moved = coordinate + 20;
                    const auto destination =
                        canvas.imageToViewport(vertical ? QPointF(moved, 80) : QPointF(80, moved));
                    mouse(&canvas, QEvent::MouseMove, destination, Qt::NoButton, Qt::LeftButton);
                    verifyBadge(moved);
                    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                    QApplication::sendEvent(&canvas, &escape);
                    mouse(&canvas, QEvent::MouseButtonRelease, line, Qt::LeftButton, Qt::NoButton);
                    require(document.state() == before && document.canUndo() == couldUndo &&
                                document.canRedo() == couldRedo,
                            "canceling a drag must restore the split coordinate without history");
                    verifyBadge(coordinate);
                }
                const auto center = rulerPoint(canvas, vertical, vertical ? 200 : 120);
                pan(vertical ? QPointF(canvasViewport(canvas).left() - center.x() - 60, 0)
                             : QPointF(0, 28 - center.y() - 60));
                require(badgeBounds(renderCanvas(canvas, 1.5)).isEmpty(),
                        "selected split lines outside the visible ruler must not show a position "
                        "badge");
                if (!vertical) {
                    QImage tallSource({32, 120000}, QImage::Format_ARGB32_Premultiplied);
                    tallSource.fill(Qt::black);
                    document.setImage(tallSource);
                    document.setSettings(settings);
                    canvas.resize(180, 200);
                    canvas.fitToCanvas();
                    constexpr int coordinate = 60000;
                    mouse(&canvas, QEvent::MouseMove, rulerPoint(canvas, false, coordinate),
                          Qt::NoButton, Qt::NoButton);
                    verifyBadge(coordinate);
                    wheel(&canvas, canvas.imageToViewport({16, coordinate}), 7200);
                    pan({10 - canvas.offset().x(), 0});
                    mouse(&canvas, QEvent::MouseMove, rulerPoint(canvas, false, coordinate),
                          Qt::NoButton, Qt::NoButton);
                    verifyBadge(coordinate);
                }
                QApplication::removeTranslator(&translator);
                QApplication::sendEvent(&canvas, &language);
            }
        }
    }
    themes.setConfig(original);
}

void rulerHoverPreviewsSplitLines() {
    ImageSplitDocument document;
    QImage source({400, 240}, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::black);
    document.setImage(source);
    auto settings = document.settings();
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 1;
    settings.columns = 1;
    settings.count = 1;
    document.setSettings(settings);
    ImageSplitPreviewCanvas canvas(&document);
    canvas.resize(900, 640);
    canvas.show();
    pump();
    const auto hover = [&](QPointF point) {
        mouse(&canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
    };
    const auto hasGuide = [&](const QImage& output, bool vertical, int coordinate) {
        const qreal scale = output.devicePixelRatio();
        const auto bounds = QRectF(canvas.offset(), QSizeF(source.size()) * canvas.zoom())
                                .intersected(canvasViewport(canvas));
        const auto point =
            canvas.imageToViewport(vertical ? QPointF(coordinate, 0) : QPointF(0, coordinate));
        const qreal cross = vertical ? point.x() : point.y();
        const qreal first = (vertical ? bounds.top() : bounds.left()) + 35;
        const qreal last = (vertical ? bounds.bottom() : bounds.right()) - 5;
        qreal coverage = 0;
        for (int along = qCeil(first * scale); along < qFloor(last * scale); ++along) {
            for (int across = qFloor((cross - 3) * scale); across < qCeil((cross + 3) * scale);
                 ++across) {
                const auto color =
                    vertical ? output.pixelColor(across, along) : output.pixelColor(along, across);
                coverage += static_cast<qreal>(color.blueF());
            }
        }
        return coverage > 10 * scale;
    };
    for (const bool transformed : {false, true}) {
        if (transformed) {
            wheel(&canvas, canvas.imageToViewport({200, 120}), 240);
            const auto delta = QPointF(10, 10) - canvas.offset();
            mouse(&canvas, QEvent::MouseButtonPress, {80, 80}, Qt::MiddleButton, Qt::MiddleButton);
            mouse(&canvas, QEvent::MouseMove, QPointF(80, 80) + delta, Qt::NoButton,
                  Qt::MiddleButton);
            mouse(&canvas, QEvent::MouseButtonRelease, QPointF(80, 80) + delta, Qt::MiddleButton,
                  Qt::NoButton);
        }
        const auto before = document.state();
        const bool couldUndo = document.canUndo();
        const bool couldRedo = document.canRedo();
        for (const bool vertical : {true, false}) {
            const int first = vertical ? 113 : 73;
            const int second = vertical ? 173 : 93;
            auto point = rulerPoint(canvas, vertical, first);
            // Preview and click must agree on the rounded image-pixel boundary.
            point += vertical ? QPointF(.25 * canvas.zoom(), 0) : QPointF(0, .25 * canvas.zoom());
            hover(point);
            for (const qreal scale : {1.0, 1.5, 2.0}) {
                const auto output = renderCanvas(canvas, scale);
                require(hasGuide(output, vertical, first),
                        "ruler hover must preview a split line across the image at every DPI");
                require(!hasGuide(output, vertical, second),
                        "the hover preview must appear only at the pointer's image coordinate");
            }
            hover(rulerPoint(canvas, vertical, second));
            auto output = renderCanvas(canvas, 1.5);
            require(!hasGuide(output, vertical, first) && hasGuide(output, vertical, second),
                    "moving along a ruler must move the temporary split line in real time");
            hover(canvas.imageToViewport({300, 180}));
            require(!hasGuide(renderCanvas(canvas, 1.5), vertical, second),
                    "moving off a ruler must remove its split preview");
            hover(point);
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(&canvas, &leave);
            require(!hasGuide(renderCanvas(canvas, 1.5), vertical, first),
                    "leaving the canvas must remove the ruler split preview");
            hover(point);
            canvas.setEditingEnabled(false);
            require(!hasGuide(renderCanvas(canvas, 1.5), vertical, first),
                    "disabled editing must clear the ruler split preview");
            canvas.setEditingEnabled(true);
            require(!hasGuide(renderCanvas(canvas, 1.5), vertical, first),
                    "re-enabling editing must not restore a stale ruler preview");
        }
        require(document.state() == before && document.canUndo() == couldUndo &&
                    document.canRedo() == couldRedo && document.image() == source,
                "ruler hover must preserve split settings, output regions, history and pixels");
    }
    for (const auto direction : {ImageSplitDirection::Vertical, ImageSplitDirection::Horizontal}) {
        settings.direction = direction;
        document.setSettings(settings);
        const bool vertical = direction == ImageSplitDirection::Vertical;
        hover(rulerPoint(canvas, !vertical, vertical ? 73 : 113));
        require(!hasGuide(renderCanvas(canvas, 1.5), !vertical, vertical ? 73 : 113),
                "the ruler for a blocked split direction must not show a preview");
        hover(rulerPoint(canvas, vertical, vertical ? 113 : 73));
        require(hasGuide(renderCanvas(canvas, 1.5), vertical, vertical ? 113 : 73),
                "the active split direction must retain its ruler preview");
    }
    settings.direction = ImageSplitDirection::Grid;
    document.setSettings(settings);
    canvas.fitToCanvas();
    for (const bool vertical : {true, false}) {
        for (const int boundary : {0, vertical ? source.width() : source.height()}) {
            hover(rulerPoint(canvas, vertical, boundary));
            const auto withHover = renderCanvas(canvas, 1.5);
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(&canvas, &leave);
            require(withHover == renderCanvas(canvas, 1.5),
                    "image edges must not preview split lines that cannot be created");
        }
    }
    const auto point = rulerPoint(canvas, true, 113) + QPointF(.25 * canvas.zoom(), 0);
    hover(point);
    click(&canvas, point);
    require(document.layout().verticalLines == QVector<int>{113},
            "clicking a ruler must commit exactly the previewed split coordinate");
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&canvas, &leave);
    require(hasGuide(renderCanvas(canvas, 1.5), true, 113),
            "a committed split line must remain after the hover preview clears");
    document.undo();
    require(document.layout().verticalLines.isEmpty() &&
                !hasGuide(renderCanvas(canvas, 1.5), true, 113),
            "one undo must remove a ruler click without any hover history entries");
    hover(rulerPoint(canvas, true, 173));
    document.setImage(source.copy());
    const auto replaced = renderCanvas(canvas, 1.5);
    QApplication::sendEvent(&canvas, &leave);
    require(replaced == renderCanvas(canvas, 1.5),
            "replacing the image must discard the previous ruler hover preview");
}

void splitGuidesKeepLogicalThicknessAndEngineDashes() {
    enum class GuideState {
        Idle,
        Hovered,
        RulerHovered,
        Selected,
        SelectedHovered,
        SelectedRulerHovered
    };
    ImageSplitDocument document;
    QImage source({400, 240}, QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::black);
    document.setImage(source);
    auto settings = document.settings();
    settings.direction = ImageSplitDirection::Grid;
    document.setSettings(settings);
    ImageSplitPreviewCanvas canvas(&document);
    canvas.resize(900, 640);
    canvas.show();
    pump();
    for (const int zoomSteps : {0, 240, -480}) {
        wheel(&canvas, canvas.imageToViewport({200, 120}), zoomSteps);
        // Exercise subpixel line placement independently of canvas zoom and output DPI.
        mouse(&canvas, QEvent::MouseButtonPress, {80, 80}, Qt::MiddleButton, Qt::MiddleButton);
        mouse(&canvas, QEvent::MouseMove, {80.3, 80.4}, Qt::NoButton, Qt::MiddleButton);
        mouse(&canvas, QEvent::MouseButtonRelease, {80.3, 80.4}, Qt::MiddleButton, Qt::NoButton);
        for (const bool vertical : {true, false}) {
            for (const auto state :
                 {GuideState::Idle, GuideState::Hovered, GuideState::RulerHovered,
                  GuideState::Selected, GuideState::SelectedHovered,
                  GuideState::SelectedRulerHovered}) {
                // Settings changes clear both selection and hover without moving the viewport.
                auto reset = document.settings();
                reset.count = reset.count == 2 ? 3 : 2;
                document.setSettings(reset);
                const QPointF onLine =
                    canvas.imageToViewport(vertical ? QPointF(200, 100) : QPointF(100, 120));
                const bool selected = state == GuideState::Selected ||
                                      state == GuideState::SelectedHovered ||
                                      state == GuideState::SelectedRulerHovered;
                if (selected)
                    click(&canvas, onLine);
                QEvent leave(QEvent::Leave);
                QApplication::sendEvent(&canvas, &leave);
                if (state == GuideState::Hovered || state == GuideState::SelectedHovered)
                    mouse(&canvas, QEvent::MouseMove, onLine, Qt::NoButton, Qt::NoButton);
                if (state == GuideState::RulerHovered ||
                    state == GuideState::SelectedRulerHovered) {
                    mouse(&canvas, QEvent::MouseMove,
                          rulerPoint(canvas, vertical, vertical ? 200 : 120), Qt::NoButton,
                          Qt::NoButton);
                }
                for (const qreal scale : {1.0, 1.25, 1.5, 1.75, 2.0}) {
                    const auto output = renderCanvas(canvas, scale);
                    constexpr qreal width = 1.0;
                    const auto origin = canvas.offset();
                    const qreal cross = vertical ? onLine.x() : onLine.y();
                    const qreal start = vertical ? origin.y() : origin.x();
                    // Isolate one guide from badges, intersections, image borders and rulers.
                    const qreal first = start + 40 * canvas.zoom();
                    const qreal last = start + 90 * canvas.zoom();
                    qreal maximum = 0;
                    qreal minimum = 10;
                    int dashSamples = 0;
                    int gapSamples = 0;
                    for (int along = qCeil(first * scale); along < qFloor(last * scale); ++along) {
                        qreal coverage = 0;
                        for (int across = qFloor((cross - 3) * scale);
                             across <= qCeil((cross + 3) * scale); ++across) {
                            const auto color = vertical ? output.pixelColor(across, along)
                                                        : output.pixelColor(along, across);
                            coverage += static_cast<qreal>(color.blueF());
                        }
                        coverage /= scale;
                        minimum = std::min(minimum, coverage);
                        maximum = std::max(maximum, coverage);
                        if (state != GuideState::Idle) {
                            require(
                                std::abs(coverage - 1.0) < .08,
                                "hovered, selected and ruler preview split lines must stay solid "
                                "and one logical pixel wide");
                            continue;
                        }
                        const qreal phase = std::fmod((along + .5) / scale - start, width * 8);
                        // Flat 5:3 dashes leave a full-width middle and a fully clear gap.
                        if (phase > width && phase < width * 4) {
                            require(
                                std::abs(coverage - width) < .08,
                                "split dashes must retain their logical width at fractional DPI");
                            ++dashSamples;
                        } else if (phase > width * 6 && phase < width * 7) {
                            require(coverage < .04,
                                    "idle guides must keep clear engine-style dash gaps");
                            ++gapSamples;
                        }
                    }
                    if (state == GuideState::Idle)
                        require(dashSamples > 0 && gapSamples > 0 &&
                                    std::abs(maximum - width) < .08 && minimum < .04,
                                "both split axes must use fixed view widths and the engine's 5:3 "
                                "dashes");
                }
            }
        }
    }
}

void saveCanvasInspectionImages() {
    const auto directory = qEnvironmentVariable("SNOW_TEST_IMAGE_SPLIT_CANVAS_ARTIFACT_DIR");
    if (directory.isEmpty())
        return;
    require(QDir().mkpath(directory), "create split canvas inspection directory");
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto original = themes.config();
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(theme);
        ImageSplitDocument document;
        QImage source({800, 480}, QImage::Format_ARGB32_Premultiplied);
        QPainter imagePainter(&source);
        QLinearGradient gradient(0, 0, 800, 480);
        gradient.setColorAt(0, QColor(27, 69, 112));
        gradient.setColorAt(1, QColor(110, 165, 185));
        imagePainter.fillRect(source.rect(), gradient);
        imagePainter.end();
        document.setImage(source);
        auto settings = document.settings();
        settings.direction = ImageSplitDirection::Grid;
        settings.columns = 3;
        document.setSettings(settings);
        ImageSplitEditorWidget editor(&document);
        editor.setAutoFillBackground(true);
        auto palette = editor.palette();
        palette.setColor(QPalette::Window, themes.resolveTheme(&editor).colorBgElevated);
        editor.setPalette(palette);
        show(editor);
        const auto capture = [&](const char* name) {
            const auto filename =
                QStringLiteral("%1-%2.png")
                    .arg(theme == adqt::theme::ThemeScheme::Light ? QStringLiteral("light")
                                                                  : QStringLiteral("dark"),
                         QString::fromLatin1(name));
            require(renderCanvas(*editor.canvas(), 1.5).save(QDir(directory).filePath(filename)),
                    "save split canvas inspection image");
        };
        capture("fit");
        const auto line = editor.canvas()->imageToViewport({267, 120});
        click(editor.canvas(), line);
        mouse(editor.canvas(), QEvent::MouseMove, line, Qt::NoButton, Qt::NoButton);
        capture("selected");
        const auto horizontalLine = editor.canvas()->imageToViewport({400, 240});
        click(editor.canvas(), horizontalLine);
        mouse(editor.canvas(), QEvent::MouseMove, horizontalLine, Qt::NoButton, Qt::NoButton);
        capture("selected-horizontal");
        mouse(editor.canvas(), QEvent::MouseMove, rulerPoint(*editor.canvas(), false, 173),
              Qt::NoButton, Qt::NoButton);
        capture("ruler-hover-horizontal");
        wheel(editor.canvas(), editor.canvas()->imageToViewport({400, 240}), 360);
        capture("zoomed");
    }
    themes.setConfig(original);
}

void themesHighDpiAndLanguage() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto original = themes.config();
    ImageSplitDocument document;
    document.setImage(image({1001, 800}));
    ImageSplitEditorWidget editor(&document);
    show(editor);
    const auto state = document.state();
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(theme);
        for (const qreal scale : {1.0, 1.5, 2.0}) {
            QImage output(QSize(qRound(editor.canvas()->width() * scale),
                                qRound(editor.canvas()->height() * scale)),
                          QImage::Format_ARGB32_Premultiplied);
            output.setDevicePixelRatio(scale);
            output.fill(Qt::transparent);
            QPainter painter(&output);
            editor.canvas()->render(&painter);
            require(!output.isNull() && output.pixelColor(5, 5).alpha() == 255,
                    "rulers must render at fractional and integer high-DPI scales");
            auto* toolbar = child<QWidget>(editor, "imageSplitCanvasToolbar");
            QImage surface(
                QSize(qRound(toolbar->width() * scale), qRound(toolbar->height() * scale)),
                QImage::Format_ARGB32_Premultiplied);
            surface.setDevicePixelRatio(scale);
            surface.fill(Qt::transparent);
            QPainter surfacePainter(&surface);
            toolbar->render(&surfacePainter, {}, {}, QWidget::DrawWindowBackground);
            surfacePainter.end();
            const auto tokens = themes.resolveTheme(toolbar);
            require(surface.pixelColor(surface.width() / 2, surface.height() / 2) ==
                        tokens.colorBgContainer,
                    "floating toolbar must use the collage surface token in both themes and DPI "
                    "scales");
        }
    }
    themes.setConfig(original);
    SplitTranslator translator;
    QApplication::installTranslator(&translator);
    QEvent language(QEvent::LanguageChange);
    QApplication::sendEvent(&editor, &language);
    auto* reload = child<AdButton>(editor, "imageSplitReload");
    require(reload->text().isEmpty() && reload->toolTip() == QStringLiteral("重新加载") &&
                reload->accessibleName() == reload->toolTip() &&
                editor.canvas()->accessibleName() == QStringLiteral("分割预览") &&
                editor.canvas()->toolTip().isEmpty() && document.state() == state &&
                !document.canUndo(),
            "runtime language changes must refresh controls without changing settings or history");
    QApplication::removeTranslator(&translator);
    QApplication::sendEvent(&editor, &language);
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    try {
        if (application.arguments().contains(QStringLiteral("--submission-only"))) {
            submissionCommitsValidPendingDraftsOnly();
            submissionRequiresTheRequestedSettings();
            std::cout << "Image split submission tests passed\n";
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--overlap-only"))) {
            overlapControlValidationAndHistory();
            overlapBandsFollowGeometry();
            std::cout << "Image split overlap editor tests passed\n";
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--empty-state-only"))) {
            emptyImageShowsUploadSurface();
            std::cout << "Image split empty state tests passed\n";
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--teardown-only"))) {
            focusedDraftTeardown();
            focusedDraftsAreDiscardedOnDestruction();
            std::cout << "Image split teardown tests passed\n";
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--canvas-only"))) {
            unchangedHoverDoesNotRepaint();
            overlayDamageMatchesFreshRendering();
            saveCanvasInspectionImages();
            fittedPreviewReservesOnlyRulerWidth();
            canvasBackgroundAndRulersFollowImage();
            splitPositionBadgesFollowRulers();
            rulerHoverPreviewsSplitLines();
            splitGuidesKeepLogicalThicknessAndEngineDashes();
            transformedRulersAndManualGestures();
            splitLineSelectionFollowsCrossing();
            zoomedCanvasUsesFullResolutionPixels();
            sequenceNumbersUsePdfBadgeStyle();
            denseGuidesDoNotHideLaterReadableRegions();
            elongatedImagesKeepViewportAndLineInteractionsUsable();
            themesHighDpiAndLanguage();
            std::cout << "Image split canvas tests passed\n";
            return 0;
        }
        focusedDraftTeardown();
        focusedDraftsAreDiscardedOnDestruction();
        emptyImageShowsUploadSurface();
        unchangedHoverDoesNotRepaint();
        overlayDamageMatchesFreshRendering();
        defaultsConditionalControlsAndActions();
        overlapControlValidationAndHistory();
        overlapBandsFollowGeometry();
        preciseSizeUsesSharedFormLabel();
        floatingToolbarFollowsCanvas();
        formValidationUnitsAndUndoableRegeneration();
        unitConversionDoesNotClampInvalidResults();
        percentageDraftsKeepExactEnteredPrecision();
        transformedRulersAndManualGestures();
        splitLineSelectionFollowsCrossing();
        replacementResetBusyAndDrops();
        dragFeedbackUsesUploadSurface();
        invalidDraftsSurviveFocusAndCanceledLoading();
        invalidDirectionDoesNotEditThePreviousAxis();
        submissionCommitsValidPendingDraftsOnly();
        submissionRequiresTheRequestedSettings();
        zoomedCanvasUsesFullResolutionPixels();
        sequenceNumbersUsePdfBadgeStyle();
        denseGuidesDoNotHideLaterReadableRegions();
        elongatedImagesKeepViewportAndLineInteractionsUsable();
        fittedPreviewReservesOnlyRulerWidth();
        canvasBackgroundAndRulersFollowImage();
        splitPositionBadgesFollowRulers();
        rulerHoverPreviewsSplitLines();
        splitGuidesKeepLogicalThicknessAndEngineDashes();
        themesHighDpiAndLanguage();
        std::cout << "Image split editor tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
