#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

namespace snow_shot::update {

// Shared with protocol.rs. Count compact UTF-8 JSON, including escaping, rather than
// paths or characters. The remaining frame capacity belongs to the protocol envelope.
inline constexpr qsizetype kMaximumRelaunchArgumentsBytes = 24 * 1024;
inline constexpr qsizetype kMaximumUpdateFrameBytes = 64 * 1024;

[[nodiscard]] inline bool relaunchIntentFits(const QStringList& arguments,
                                             const QString& executable) {
    const auto json =
        QJsonDocument(QJsonArray::fromStringList(arguments)).toJson(QJsonDocument::Compact);
    if (json.size() > kMaximumRelaunchArgumentsBytes)
        return false;

    // Windows quotes argv for CreateProcess. Quoting every operand is a conservative
    // upper bound, including escaped trailing backslashes and the terminating NUL.
    qsizetype commandUnits = executable.size() + 3;
    for (const auto& argument : arguments) {
        commandUnits += argument.size() + 3;
        qsizetype backslashes = 0;
        for (const auto character : argument) {
            if (character == u'\\') {
                ++backslashes;
            } else {
                if (character == u'"')
                    commandUnits += backslashes + 1;
                backslashes = 0;
            }
        }
        commandUnits += backslashes;
    }
    return commandUnits <= 32767;
}

} // namespace snow_shot::update
