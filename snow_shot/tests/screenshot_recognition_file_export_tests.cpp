#include "snow_shot/presentation/screenshotrecognitionfileexport.h"

#include <QApplication>
#include <QAbstractButton>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>

#include <stdexcept>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "saved text file is unreadable");
    return file.readAll();
}

void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "text fixture cannot be written");
    require(file.write(bytes) == bytes.size(), "text fixture write failed");
}

void savesConversionPairsWithExactSource() {
    QTemporaryDir directory;
    require(directory.isValid(), "conversion save directory unavailable");
    for (const auto kind :
         {ScreenshotRecognitionFileKind::Html, ScreenshotRecognitionFileKind::Markdown}) {
        const QString source = kind == ScreenshotRecognitionFileKind::Html
                                   ? QStringLiteral("<h1>雪</h1>\n<p>Partial")
                                   : QStringLiteral("# 雪\n\n**Partial");
        const QString input = directory.filePath(QStringLiteral("result.other"));
        const auto saved = ScreenshotRecognitionFileExport::saveToPath({kind, source}, input, true);
        const QString primary = directory.filePath(
            QStringLiteral("result.") + ScreenshotRecognitionFileExport::extension(kind));
        require(saved.succeeded() && saved.path == primary,
                "conversion primary extension must match its source format");
        require(read(primary) == source.toUtf8() &&
                    read(directory.filePath(QStringLiteral("result.txt"))) == source.toUtf8(),
                "conversion primary and companion must preserve exact UTF-8 source");
    }
}

void savesQrValuesAsOneTextFile() {
    QTemporaryDir directory;
    require(directory.isValid(), "QR save directory unavailable");
    const QString source = QStringList{QStringLiteral("first"), QStringLiteral("雪"),
                                       QStringLiteral("https://example.test")}
                               .join(QLatin1Char('\n'));
    const auto saved = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Qr, source},
        directory.filePath(QStringLiteral("codes.html")));
    require(saved.succeeded() && QFileInfo(saved.path).suffix() == QStringLiteral("txt") &&
                read(saved.path) == source.toUtf8(),
            "QR values must retain their order in one UTF-8 text file");
    require(!QFileInfo::exists(directory.filePath(QStringLiteral("codes.html"))),
            "QR save must not produce an image or conversion file");
}

void quickSaveAvoidsEitherPairCollision() {
    QTemporaryDir directory;
    require(directory.isValid(), "quick save directory unavailable");
    write(directory.filePath(QStringLiteral("Auto.txt")), QByteArrayLiteral("keep"));
    const auto first = ScreenshotRecognitionFileExport::quickSave(
        {ScreenshotRecognitionFileKind::Markdown, QStringLiteral("# partial")}, directory.path(),
        QStringLiteral("Auto"));
    require(first.succeeded() && first.path.endsWith(QStringLiteral("Auto_1.md")) &&
                read(directory.filePath(QStringLiteral("Auto.txt"))) == QByteArrayLiteral("keep") &&
                read(directory.filePath(QStringLiteral("Auto_1.txt"))) ==
                    QByteArrayLiteral("# partial"),
            "a companion collision must move the entire quick-save pair to a new base name");
    const auto second = ScreenshotRecognitionFileExport::quickSave(
        {ScreenshotRecognitionFileKind::Markdown, QStringLiteral("# next")}, directory.path(),
        QStringLiteral("Auto"));
    require(second.succeeded() && second.path.endsWith(QStringLiteral("Auto_2.md")),
            "quick save must preserve existing primary files too");
}

void emptyAndFailedPairLeaveNoPartialOutput() {
    QTemporaryDir directory;
    require(directory.isValid(), "failure save directory unavailable");
    const QString primary = directory.filePath(QStringLiteral("result.html"));
    const QString companion = directory.filePath(QStringLiteral("result.txt"));
    const auto empty = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Html, {}}, primary);
    require(!empty.succeeded() && !QFileInfo::exists(primary),
            "an empty recognition result must not create a file");
    write(primary, QByteArrayLiteral("original"));
    require(QDir().mkpath(companion), "failure companion directory unavailable");
    const auto failed = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Html, QStringLiteral("replacement")}, primary, true);
    require(!failed.succeeded() && read(primary) == QByteArrayLiteral("original") &&
                QFileInfo(companion).isDir(),
            "a pair publication failure must preserve prior outputs");
}

void manualOverwriteConfirmsAndReplacesBothFiles() {
    QTemporaryDir directory;
    require(directory.isValid(), "overwrite directory unavailable");
    const QString primary = directory.filePath(QStringLiteral("result.md"));
    const QString companion = directory.filePath(QStringLiteral("result.txt"));
    write(primary, QByteArrayLiteral("old primary"));
    write(companion, QByteArrayLiteral("old companion"));
    const QStringList paths = ScreenshotRecognitionFileExport::outputPaths(
        primary, ScreenshotRecognitionFileKind::Markdown);
    const auto confirm = [&](QMessageBox::StandardButton answer) {
        QTimer timer;
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, &timer, [answer]() {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible())
                    box->button(answer)->click();
            }
        });
        timer.start();
        const bool accepted = ScreenshotRecognitionFileExport::confirmOverwrite(nullptr, paths);
        timer.stop();
        return accepted;
    };
    require(!confirm(QMessageBox::No) && read(primary) == QByteArrayLiteral("old primary") &&
                read(companion) == QByteArrayLiteral("old companion"),
            "declining overwrite must preserve both existing files");
    require(confirm(QMessageBox::Yes), "manual overwrite requires explicit confirmation");
    const auto saved = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Markdown, QStringLiteral("# new")}, primary, true);
    require(saved.succeeded() && read(primary) == QByteArrayLiteral("# new") &&
                read(companion) == QByteArrayLiteral("# new"),
            "confirmed overwrite must replace both members from one source snapshot");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        savesConversionPairsWithExactSource();
        savesQrValuesAsOneTextFile();
        quickSaveAvoidsEitherPairCollision();
        emptyAndFailedPairLeaveNoPartialOutput();
        manualOverwriteConfirmsAndReplacesBothFiles();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
