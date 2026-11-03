// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/collagepdfexport.h"

#include "collageresourcebudget_p.h"
#include "collagetransformedrows_p.h"
#include "snow_shot/presentation/screenshotpdfexport.h"
#include "snowimageqtcodec.h"
#include "snow_shot/storage/exportfilename.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace snow_shot::presentation {
namespace {
[[maybe_unused]] constexpr const char* labels[] = {
    QT_TRANSLATE_NOOP("CollagePdfExport", "Export was canceled."),
    QT_TRANSLATE_NOOP("CollagePdfExport", "The PDF settings or images are invalid."),
    QT_TRANSLATE_NOOP("CollagePdfExport", "No output destination was selected."),
    QT_TRANSLATE_NOOP("CollagePdfExport", "The output folder could not be created."),
    QT_TRANSLATE_NOOP("CollagePdfExport",
                      "The selected PDF already exists. Choose another filename."),
    QT_TRANSLATE_NOOP("CollagePdfExport", "Page %1 could not be exported: %2"),
    QT_TRANSLATE_NOOP("CollagePdfExport", "The PDF could not be saved: %1"),
    QT_TRANSLATE_NOOP("CollagePdfExport", "The image could not be prepared for PDF export."),
    QT_TRANSLATE_NOOP("CollagePdfExport", "There is not enough memory to export the PDF."),
};

void setError(CollagePdfExportResult& result, const char* source, QStringList arguments = {}) {
    result.errorSource = QString::fromUtf8(source);
    result.errorArguments = std::move(arguments);
    result.error = result.errorMessage();
}

QString preparationError() {
    return QCoreApplication::translate("CollagePdfExport",
                                       "The image could not be prepared for PDF export.");
}

QString safeImageName(QString label) {
    // Labels can be display names or absolute paths from either supported platform.
    label = label.mid(std::max(label.lastIndexOf(u'/'), label.lastIndexOf(u'\\')) + 1);
    return storage::sanitizeExportFileName(QFileInfo(label).completeBaseName(), u"Image",
                                           {storage::ExportFileNameControls::C0AndC1, 120});
}

struct RowLifetime final {
    // Retire the reader and its rasters before these reservations are released.
    std::shared_ptr<collage_detail::ResourceReservation> raster;
    std::shared_ptr<collage_detail::ResourceReservation> working;
    std::shared_ptr<void> original;
    ScreenshotImageRowSource source;
};

ScreenshotImageRowSource pageRows(const CollageItem& item, const std::function<bool()>& cancelled,
                                  QString* error) {
    const QSize size = transformedSize(item);
    if (size.isEmpty()) {
        if (error)
            *error = preparationError();
        return {};
    }
    const bool streamed = collage_detail::streamableTransform(item);
    const bool materialized = !item.transform.isIdentity() && !streamed;
    const qint64 nativeDepth = std::max(4, item.image.depth() / 8);
    const qint64 transformedBytes = qint64(size.width()) * size.height() * nativeDepth;
    if (transformedBytes > collage_detail::maximumRasterBytes) {
        if (error)
            *error = preparationError();
        return {};
    }
    auto owner = std::make_shared<RowLifetime>();
    owner->original = item.resourceOwner;
    const qint64 unownedBytes = item.resourceOwner ? 0 : item.image.sizeInBytes();
    owner->raster =
        collage_detail::reserve(unownedBytes + (materialized ? transformedBytes : 0), error);
    if (!owner->raster)
        return {};
    const qint64 workingBytes =
        streamed
            ? collage_detail::transformCacheBytes(item) +
                  collage_detail::transformWorkingBytes(item)
            : qint64{10} * 1024 * 1024 +
                  (materialized ? std::max(qint64(item.image.sizeInBytes()), transformedBytes) : 0);
    owner->working = collage_detail::reserveWorking(workingBytes, error);
    if (!owner->working)
        return {};
    if (cancelled && cancelled())
        return {};
    owner->source = streamed
                        ? collage_detail::transformedRowSource(item, cancelled)
                        : image_codec::srgbRowSource(
                              materialized ? snowCanvasTransformImage(item.image, item.transform,
                                                                      Qt::FastTransformation)
                                           : item.image);
    if (!owner->source.isValid()) {
        if (error)
            *error = preparationError();
        return {};
    }
    if (streamed && !item.image.hasAlphaChannel())
        owner->source.isKnownOpaque = true;
    ScreenshotImageRowSource rows;
    rows.size = owner->source.size;
    rows.isKnownOpaque = true;
    rows.cancellationRequested = cancelled;
    if (owner->source.isKnownOpaque)
        rows.backingImage = owner->source.backingImage;
    rows.readRows = [owner, cancelled](int first, int count, qsizetype stride, uchar* target,
                                       qsizetype capacity) {
        if ((cancelled && cancelled()) ||
            !owner->source.readRows(first, count, stride, target, capacity))
            return false;
        if (owner->source.isKnownOpaque)
            return !cancelled || !cancelled();
        for (int row = 0; row < count; ++row) {
            if (cancelled && cancelled())
                return false;
            uchar* pixels = target + qsizetype(row) * stride;
            for (int column = 0; column < owner->source.size.width(); ++column, pixels += 4) {
                if ((column & 4095) == 0 && cancelled && cancelled())
                    return false;
                const unsigned alpha = pixels[3];
                if (alpha != 255) {
                    for (int channel = 0; channel < 3; ++channel)
                        pixels[channel] = uchar(
                            (unsigned(pixels[channel]) * alpha + 255 * (255 - alpha) + 127) / 255);
                    pixels[3] = 255;
                }
            }
        }
        return !cancelled || !cancelled();
    };
    rows.reserveResources = [](qint64 retained, qint64 scratch,
                               QString* admissionError) -> std::shared_ptr<void> {
        if (retained < 0 || scratch < 0 || retained > collage_detail::maximumWorkingBytes ||
            scratch > collage_detail::maximumWorkingBytes - retained) {
            if (admissionError)
                *admissionError = preparationError();
            return {};
        }
        return collage_detail::reserveWorking(retained + scratch, admissionError);
    };
    return rows;
}

struct Publication final {
    bool success = false;
    bool conflict = false;
    QString error;
};

Publication publishFile(const QString& source, const QString& target) {
#ifdef Q_OS_WIN
    if (MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                    reinterpret_cast<LPCWSTR>(target.utf16()), MOVEFILE_WRITE_THROUGH))
        return {true, false, {}};
    const DWORD error = GetLastError();
    wchar_t message[2048]{};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, error, 0, message, 2048, nullptr);
    return {false, error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS,
            length ? QString::fromWCharArray(message, static_cast<int>(length)).trimmed()
                   : QString::number(error)};
#else
    const QByteArray original = QFile::encodeName(source);
    const QByteArray destination = QFile::encodeName(target);
#ifdef Q_OS_MACOS
    if (renamex_np(original.constData(), destination.constData(), RENAME_EXCL) == 0)
        return {true, false, {}};
    if (errno != ENOTSUP)
        return {false, errno == EEXIST, QString::fromLocal8Bit(std::strerror(errno))};
#endif
    // Some filesystem engines fall back from no-replace rename to ordinary rename. Avoid that
    // race: an exclusive hard link publishes complete bytes, then retires the staging pathname.
    if (::link(original.constData(), destination.constData()) != 0)
        return {false, errno == EEXIST, QString::fromLocal8Bit(std::strerror(errno))};
    if (::unlink(original.constData()) == 0)
        return {true, false, {}};
    const int error = errno;
    static_cast<void>(::unlink(destination.constData()));
    return {false, false, QString::fromLocal8Bit(std::strerror(error))};
#endif
}

QString publishUnique(const QString& stagingPath, const QDir& output, const QString& base,
                      const std::function<bool()>& cancelled, QString* error) {
    for (qint64 suffix = 1; suffix <= std::numeric_limits<int>::max(); ++suffix) {
        if (cancelled && cancelled())
            return {};
        const QString name = suffix == 1 ? base + QStringLiteral(".pdf")
                                         : QStringLiteral("%1_%2.pdf").arg(base).arg(suffix);
        const QString path = output.filePath(name);
        const auto publication = publishFile(stagingPath, path);
        if (publication.success)
            return path;
        if (!publication.conflict) {
            if (error)
                *error = publication.error;
            return {};
        }
    }
    if (error)
        *error = preparationError();
    return {};
}
} // namespace

bool CollagePdfExportResult::succeeded() const {
    return !cancelled && error.isEmpty() && totalCount > 0 && completedPages.size() == totalCount &&
           !paths.isEmpty();
}

QString CollagePdfExportResult::errorMessage() const {
    if (errorSource.isEmpty())
        return error;
    const auto source = errorSource.toUtf8();
    QString message = QCoreApplication::translate("CollagePdfExport", source.constData());
    for (const auto& argument : errorArguments)
        message = message.arg(argument);
    return message;
}

CollagePdfExportResult
exportCollagePdf(const QVector<CollageItem>& items, const CollagePdfSettings& settings,
                 const CollagePdfDestination& destination, std::function<bool()> cancelled,
                 std::function<void(const CollagePdfExportProgress&)> progress) {
    CollagePdfExportResult result;
    const qsizetype pageCount = collagePdfPageCount(items.size(), settings);
    result.totalCount = pageCount;
    const auto stopped = [&] { return cancelled && cancelled(); };
    const auto stop = [&] {
        result.cancelled = true;
        setError(result, "Export was canceled.");
    };
    if (stopped()) {
        stop();
        return result;
    }
    if (items.isEmpty() || !validateCollagePdfSettings(settings).isEmpty() ||
        std::any_of(items.cbegin(), items.cend(), [](const CollageItem& item) {
            return item.image.isNull() || transformedSize(item).isEmpty();
        })) {
        setError(result, "The PDF settings or images are invalid.");
        return result;
    }
    if (destination.path.trimmed().isEmpty()) {
        setError(result, "No output destination was selected.");
        return result;
    }
    const auto reportProgress = [&](qsizetype page, qreal fraction) {
        if (progress)
            progress({std::min(page, pageCount), pageCount, std::min(page + 1, pageCount),
                      std::clamp(fraction, qreal{0}, qreal{1})});
    };
    qsizetype activePage = 0;
    const screenshot_pdf::PageProducer produce = [&](qsizetype pageIndex,
                                                     screenshot_pdf::Page* page, QString* error) {
        activePage = pageIndex;
        const auto geometry = calculateCollagePdfComposedPage(items, settings, pageIndex);
        if (!geometry.isValid()) {
            if (error)
                *error = geometry.error;
            return false;
        }
        reportProgress(pageIndex, 0);
        constexpr qreal pointsPerMillimeter = 72.0 / 25.4;
        page->geometry.pagePoints = geometry.pageMillimeters * pointsPerMillimeter;
        const auto points = [](const QRectF& rect) {
            return QRectF(rect.topLeft() * pointsPerMillimeter, rect.size() * pointsPerMillimeter);
        };
        qsizetype occupied = 0;
        for (const auto& cell : geometry.cells)
            occupied += cell.itemIndex >= 0 ? 1 : 0;
        qsizetype prepared = 0;
        for (const auto& cell : geometry.cells) {
            if (cell.itemIndex < 0)
                continue;
            if (stopped())
                return false;
            const auto rows = pageRows(items[cell.itemIndex], cancelled, error);
            if (!rows.isValid())
                return false;
            auto payload = screenshot_pdf::prepare(rows, 100, error, true, [&](qreal fraction) {
                reportProgress(pageIndex, 0.85 * (qreal(prepared) + fraction) / qreal(occupied));
            });
            if (!payload)
                return false;
            page->layers.append(
                {std::move(payload), points(cell.imageMillimeters), points(cell.clipMillimeters)});
            ++prepared;
        }
        return !page->layers.isEmpty();
    };
    const auto write = [&](QIODevice* output, qsizetype first, qsizetype count, QString* error) {
        return screenshot_pdf::writeDocument(
            count,
            [&](qsizetype local, screenshot_pdf::Page* page, QString* pageError) {
                return produce(first + local, page, pageError);
            },
            output, {}, error, cancelled,
            [&](qsizetype local, qreal fraction) {
                reportProgress(first + local, 0.85 + 0.15 * fraction);
            });
    };
    const auto failure = [&](const QString& error) {
        if (stopped())
            stop();
        else
            setError(
                result, "Page %1 could not be exported: %2",
                {QString::number(activePage + 1), error.isEmpty() ? preparationError() : error});
    };
    try {
        result.completedPages.reserve(pageCount);
        result.paths.reserve(settings.exportMode == CollagePdfExportMode::Combined ? 1 : pageCount);
        auto writerWorking = collage_detail::reserveWorking(32 * 1024 * 1024, &result.error);
        if (!writerWorking)
            return result;
        if (settings.exportMode == CollagePdfExportMode::Combined) {
            QString path = QFileInfo(destination.path).absoluteFilePath();
            const bool hasPdfSuffix = path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive);
            // Permission belongs to the exact selected filename. Adding an extension changes
            // that destination and must never authorize replacement of another existing file.
            const bool overwriteAllowed = destination.overwriteAuthorized && hasPdfSuffix;
            if (!hasPdfSuffix)
                path += QStringLiteral(".pdf");
            if (QFileInfo::exists(path) && !overwriteAllowed) {
                setError(result, "The selected PDF already exists. Choose another filename.");
                return result;
            }
            const QDir directory(QFileInfo(path).absolutePath());
            if (!QDir().mkpath(directory.absolutePath())) {
                setError(result, "The output folder could not be created.");
                return result;
            }
            QString error;
            if (overwriteAllowed) {
                QSaveFile file(path);
                file.setDirectWriteFallback(false);
                if (!file.open(QIODevice::WriteOnly)) {
                    setError(result, "The PDF could not be saved: %1", {file.errorString()});
                    return result;
                }
                if (!write(&file, 0, pageCount, &error) || stopped()) {
                    file.cancelWriting();
                    failure(error);
                    return result;
                }
                if (!file.commit()) {
                    setError(result, "The PDF could not be saved: %1", {file.errorString()});
                    return result;
                }
            } else {
                // QTemporaryFile::close() retains its native handle on Windows and blocks
                // publishFile's rename. A regular QFile releases that handle; the private
                // directory owns cleanup and keeps staging on the destination filesystem.
                QTemporaryDir staging(directory.filePath(QStringLiteral(".snow-shot-pdf-XXXXXX")));
                if (!staging.isValid()) {
                    setError(result, "The PDF could not be saved: %1", {staging.errorString()});
                    return result;
                }
                QFile file(staging.filePath(QStringLiteral("document.pdf")));
                if (!file.open(QIODevice::WriteOnly)) {
                    setError(result, "The PDF could not be saved: %1", {file.errorString()});
                    return result;
                }
                if (!write(&file, 0, pageCount, &error) || !file.flush() || stopped()) {
                    failure(error.isEmpty() ? file.errorString() : error);
                    return result;
                }
                file.close();
                const auto publication = publishFile(file.fileName(), path);
                if (!publication.success) {
                    setError(result, "The PDF could not be saved: %1", {publication.error});
                    return result;
                }
            }
            result.paths.append(path);
            for (qsizetype index = 0; index < pageCount; ++index)
                result.completedPages.append(index + 1);
        } else {
            const QDir directory(QFileInfo(destination.path).absoluteFilePath());
            if (!QDir().mkpath(directory.absolutePath()) || !QFileInfo(directory.path()).isDir()) {
                setError(result, "The output folder could not be created.");
                return result;
            }
            const int digits = std::max(3, static_cast<int>(QString::number(pageCount).size()));
            for (qsizetype index = 0; index < pageCount; ++index) {
                activePage = index;
                if (stopped()) {
                    stop();
                    return result;
                }
                QTemporaryDir staging(directory.filePath(QStringLiteral(".snow-shot-pdf-XXXXXX")));
                if (!staging.isValid()) {
                    failure(staging.errorString());
                    return result;
                }
                QFile file(staging.filePath(QStringLiteral("document.pdf")));
                if (!file.open(QIODevice::WriteOnly)) {
                    failure(file.errorString());
                    return result;
                }
                QString error;
                if (!write(&file, index, 1, &error) || !file.flush() || stopped()) {
                    failure(error.isEmpty() ? file.errorString() : error);
                    return result;
                }
                file.close();
                const QString base =
                    QStringLiteral("%1_%2")
                        .arg(index + 1, digits, 10, u'0')
                        .arg(settings.stitchingMethod == CollagePdfStitchingMethod::TemplateBased
                                 ? QStringLiteral("page")
                                 : safeImageName(items[index].label));
                const QString path =
                    publishUnique(file.fileName(), directory, base, cancelled, &error);
                if (path.isEmpty()) {
                    failure(error);
                    return result;
                }
                result.completedPages.append(index + 1);
                result.paths.append(path);
                if (progress)
                    progress({result.completedPages.size(), pageCount, index + 1, 0});
            }
        }
        if (progress)
            progress({pageCount, pageCount, pageCount, 1});
    } catch (const std::bad_alloc&) {
        setError(result, "There is not enough memory to export the PDF.");
    }
    return result;
}
} // namespace snow_shot::presentation
