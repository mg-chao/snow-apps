#pragma once

#include <QFile>
#include <QString>

namespace snow_shot::platform {

inline QByteArray minizipPath(const QString& path) {
#ifdef Q_OS_WIN
    // Minizip-ng decodes UTF-8 before calling the wide Windows filesystem APIs.
    // QFile::encodeName() uses the ANSI code page here, which is a different contract.
    return path.toUtf8();
#else
    // Preserve Qt's native filename encoding, including macOS normalization.
    return QFile::encodeName(path);
#endif
}

} // namespace snow_shot::platform
