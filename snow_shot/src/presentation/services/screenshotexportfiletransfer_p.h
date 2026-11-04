// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFile>
#include <QScopeGuard>
#include <functional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace screenshot_export_file_transfer {
// Source is a completed private staging file. Linking preserves it for companion rollback and
// collision retries; removing staging later leaves the published inode intact. Never overwrite.
inline bool publishExclusive(const QString& source, const QString& destination,
                             const std::function<bool()>& cancelled, QString* error,
                             bool tryHardLink = true) {
    const auto stopped = [&] { return cancelled && cancelled(); };
    if (stopped())
        return false;
    if (tryHardLink) {
#ifdef Q_OS_WIN
        if (CreateHardLinkW(reinterpret_cast<LPCWSTR>(destination.utf16()),
                            reinterpret_cast<LPCWSTR>(source.utf16()), nullptr))
            return true;
#else
        if (::link(QFile::encodeName(source).constData(),
                   QFile::encodeName(destination).constData()) == 0)
            return true;
#endif
    }
    // Cross-volume and filesystems without hard links use bounded, cancellable copying.
    QFile input(source);
    QFile output(destination);
    if (!input.open(QIODevice::ReadOnly) ||
        !output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (error)
            *error = input.isOpen() ? output.errorString() : input.errorString();
        return false;
    }
    bool published = false;
    const auto rollback = qScopeGuard([&] {
        output.close();
        if (!published)
            output.remove();
    });
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        if (stopped())
            return false;
        const auto size = input.read(buffer.data(), buffer.size());
        if (size < 0) {
            if (error)
                *error = input.errorString();
            return false;
        }
        if (size == 0)
            break;
        if (output.write(buffer.constData(), size) != size) {
            if (error)
                *error = output.errorString();
            return false;
        }
    }
    if (stopped())
        return false;
    if (!output.flush()) {
        if (error)
            *error = output.errorString();
        return false;
    }
    output.setPermissions(input.permissions());
    published = true;
    return true;
}
} // namespace screenshot_export_file_transfer
