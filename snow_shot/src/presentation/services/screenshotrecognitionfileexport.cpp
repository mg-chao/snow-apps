#include "snow_shot/presentation/screenshotrecognitionfileexport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QTemporaryFile>

namespace {
QString message(const char* source) {
    return QCoreApplication::translate("ScreenshotRecognitionFileExport", source);
}

QString reserveTemporaryPath(const QString& directory) {
    QTemporaryFile temporary(QDir(directory).filePath(QStringLiteral(".snow-shot-XXXXXX")));
    temporary.setAutoRemove(false);
    if (!temporary.open())
        return {};
    const QString path = temporary.fileName();
    temporary.close();
    return path;
}

QStringList existingPaths(const QStringList& paths) {
    QStringList existing;
    for (const QString& path : paths) {
        if (QFileInfo::exists(path))
            existing.push_back(path);
    }
    return existing;
}
} // namespace

QString ScreenshotRecognitionFileExport::extension(ScreenshotRecognitionFileKind kind) {
    switch (kind) {
    case ScreenshotRecognitionFileKind::Html:
        return QStringLiteral("html");
    case ScreenshotRecognitionFileKind::Markdown:
        return QStringLiteral("md");
    case ScreenshotRecognitionFileKind::Qr:
        return QStringLiteral("txt");
    }
    return {};
}

QString ScreenshotRecognitionFileExport::dialogFilter(ScreenshotRecognitionFileKind kind) {
    switch (kind) {
    case ScreenshotRecognitionFileKind::Html:
        return message(
            QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "HTML document (*.html)"));
    case ScreenshotRecognitionFileKind::Markdown:
        return message(
            QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Markdown document (*.md)"));
    case ScreenshotRecognitionFileKind::Qr:
        return message(
            QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Text document (*.txt)"));
    }
    return {};
}

QString ScreenshotRecognitionFileExport::normalizedPath(const QString& path,
                                                        ScreenshotRecognitionFileKind kind) {
    QString normalized = QDir::cleanPath(path.trimmed());
    if (path.trimmed().isEmpty())
        return {};
    const QString fileName = QFileInfo(normalized).fileName();
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot > 0 && dot + 1 < fileName.size())
        normalized.chop(fileName.size() - dot);
    else if (normalized.endsWith(QLatin1Char('.')))
        normalized.chop(1);
    return normalized + QLatin1Char('.') + extension(kind);
}

QStringList ScreenshotRecognitionFileExport::outputPaths(const QString& primaryPath,
                                                         ScreenshotRecognitionFileKind kind) {
    if (primaryPath.isEmpty())
        return {};
    QStringList paths{primaryPath};
    if (kind != ScreenshotRecognitionFileKind::Qr) {
        QString companion = primaryPath;
        companion.chop(extension(kind).size());
        companion += QStringLiteral("txt");
        paths.push_back(companion);
    }
    return paths;
}

bool ScreenshotRecognitionFileExport::confirmOverwrite(QWidget* owner, const QStringList& paths) {
    const QStringList existing = existingPaths(paths);
    if (existing.isEmpty())
        return true;
    return QMessageBox::question(owner,
                                 message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                                           "Replace existing files?")),
                                 message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                                           "Replace the existing file(s)?\n%1"))
                                     .arg(existing.join(QLatin1Char('\n'))),
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) == QMessageBox::Yes;
}

ScreenshotRecognitionFileSaveResult
ScreenshotRecognitionFileExport::saveToPath(const ScreenshotRecognitionFileSnapshot& snapshot,
                                            const QString& path, bool allowOverwrite) {
    if (snapshot.source.isEmpty())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "No recognition text is available to save"))};
    const QString primary = normalizedPath(path, snapshot.kind);
    if (primary.isEmpty())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "No output file was selected"))};
    const QStringList paths = outputPaths(primary, snapshot.kind);
    const QString directory = QFileInfo(primary).absolutePath();
    if (!QDir().mkpath(directory))
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The output directory could not be created"))};
    for (const QString& output : paths) {
        const QFileInfo information(output);
        if (information.exists() && !information.isFile())
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An existing output file could not be replaced"))};
    }
    if (!allowOverwrite && !existingPaths(paths).isEmpty())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "An output file already exists"))};

    const QByteArray bytes = snapshot.source.toUtf8();
    QStringList staged;
    QStringList backups(paths.size());
    QStringList published;
    auto rollback = [&]() {
        for (const QString& publishedPath : published)
            QFile::remove(publishedPath);
        for (qsizetype index = 0; index < backups.size(); ++index) {
            if (!backups.at(index).isEmpty())
                QFile::rename(backups.at(index), paths.at(index));
        }
        for (const QString& stagedPath : staged)
            QFile::remove(stagedPath);
    };
    for (qsizetype index = 0; index < paths.size(); ++index) {
        const QString temporary = reserveTemporaryPath(directory);
        if (temporary.isEmpty()) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "The text file could not be staged"))};
        }
        staged.push_back(temporary);
        QFile file(temporary);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(bytes) != bytes.size() || !file.flush()) {
            const QString error = file.errorString();
            file.close();
            rollback();
            return {{}, error};
        }
        file.close();
    }
    if (!allowOverwrite && !existingPaths(paths).isEmpty()) {
        rollback();
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "An output file already exists"))};
    }
    for (qsizetype index = 0; index < paths.size(); ++index) {
        if (!QFileInfo::exists(paths.at(index)))
            continue;
        if (!allowOverwrite) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An output file already exists"))};
        }
        if (!QFileInfo(paths.at(index)).isFile()) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An existing output file could not be replaced"))};
        }
        const QString backup = reserveTemporaryPath(directory);
        if (backup.isEmpty() || !QFile::remove(backup) || !QFile::rename(paths.at(index), backup)) {
            if (!backup.isEmpty())
                QFile::remove(backup);
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An existing output file could not be replaced"))};
        }
        backups[index] = backup;
    }
    for (qsizetype index = 0; index < paths.size(); ++index) {
        if (!QFile::rename(staged.at(index), paths.at(index))) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "The text file could not be published"))};
        }
        published.push_back(paths.at(index));
    }
    for (const QString& backup : backups) {
        if (!backup.isEmpty())
            QFile::remove(backup);
    }
    return {primary, {}};
}

ScreenshotRecognitionFileSaveResult
ScreenshotRecognitionFileExport::quickSave(const ScreenshotRecognitionFileSnapshot& snapshot,
                                           const QString& directory, const QString& baseName) {
    if (snapshot.source.isEmpty())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "No recognition text is available to save"))};
    if (directory.trimmed().isEmpty())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The save directory is not configured"))};
    if (baseName.isEmpty() || baseName.contains(QLatin1Char('/')) ||
        baseName.contains(QLatin1Char('\\')))
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The screenshot filename format is invalid"))};
    const QString target = QDir::cleanPath(directory.trimmed());
    if (!QDir().mkpath(target))
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The output directory could not be created"))};
    for (int suffix = 0; suffix < 1000000; ++suffix) {
        const QString candidateBase =
            suffix == 0 ? baseName : QStringLiteral("%1_%2").arg(baseName).arg(suffix);
        const QString path =
            QDir(target).filePath(candidateBase + QLatin1Char('.') + extension(snapshot.kind));
        if (existingPaths(outputPaths(path, snapshot.kind)).isEmpty()) {
            const auto result = saveToPath(snapshot, path);
            if (result.error != message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                                          "An output file already exists")))
                return result;
        }
    }
    return {{},
            message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                      "No available output filename was found"))};
}
