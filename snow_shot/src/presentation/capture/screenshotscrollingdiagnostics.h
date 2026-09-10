#pragma once

#include "snow_shot/diagnostics/diagnostics.h"

#include <QRect>

#include <chrono>

namespace snow_shot::capture_detail {
inline void logScrollingEvent(const char* event, quint64 generation, QJsonObject fields = {},
                              QtMsgType level = QtInfoMsg) {
    fields.insert(QStringLiteral("operation"), QString::number(generation));
    snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.scrolling"),
                                     QString::fromLatin1(event), fields, level);
}

inline QString scrollingRect(const QRect& rect) {
    return QStringLiteral("%1,%2 %3x%4")
        .arg(rect.x())
        .arg(rect.y())
        .arg(rect.width())
        .arg(rect.height());
}

// Owned only by the stream consumer. Reporting never changes capture cadence or recovery.
struct ScrollingCaptureDiagnostics {
    using Clock = std::chrono::steady_clock;
    Clock::time_point started = Clock::now();
    Clock::time_point nextReport = started + std::chrono::seconds(5);
    qint64 received = 0;
    qint64 accepted = 0;
    qint64 timeouts = 0;
    qint64 duplicates = 0;
    qint64 invalid = 0;
    qint64 mailboxDropped = 0;
    qint64 poolUnavailable = 0;
    qint64 droppedEvents = 0;

    bool reportDue(Clock::time_point now) {
        if (now < nextReport)
            return false;
        nextReport = now + std::chrono::seconds(30);
        return true;
    }

    QJsonObject fields(Clock::time_point now = Clock::now()) const {
        return {{QStringLiteral("duration_ms"),
                 static_cast<qint64>(
                     std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count())},
                {QStringLiteral("frames_received"), received},
                {QStringLiteral("frames_accepted"), accepted},
                {QStringLiteral("receive_timeouts"), timeouts},
                {QStringLiteral("duplicate_frames"), duplicates},
                {QStringLiteral("invalid_frames"), invalid},
                {QStringLiteral("mailbox_dropped"), mailboxDropped},
                {QStringLiteral("pool_unavailable"), poolUnavailable},
                {QStringLiteral("dropped_events"), droppedEvents}};
    }
};
} // namespace snow_shot::capture_detail
