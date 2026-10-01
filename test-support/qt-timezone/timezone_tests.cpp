#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QTimeZone>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main(int argc, char** argv) {
    const QCoreApplication application(argc, argv);
    const auto utc =
        QDateTime::fromString(QStringLiteral("2024-01-15T12:34:56.789Z"), Qt::ISODateWithMs);
    require(utc.isValid() && utc.offsetFromUtc() == 0, "UTC ISO timestamps must parse");
    require(utc.toString(Qt::ISODateWithMs) == QStringLiteral("2024-01-15T12:34:56.789Z"),
            "UTC storage timestamps must round-trip with milliseconds");
    require(utc.toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz'Z'")) ==
                QStringLiteral("20240115T123456789Z"),
            "storage filename date tokens must remain unchanged");

    const QTimeZone shanghai(QByteArrayLiteral("Asia/Shanghai"));
    require(shanghai.isValid(), "the platform must resolve IANA time zones");
    const auto china = utc.toTimeZone(shanghai);
    require(china.offsetFromUtc() == 8 * 60 * 60 && china.time().hour() == 20,
            "named-zone conversion must retain UTC offsets and local wall time");
    require(china.toUTC() == utc, "named-zone conversion must preserve the instant");
    require(china.toString(Qt::ISODateWithMs) == QStringLiteral("2024-01-15T20:34:56.789+08:00"),
            "ISO output must preserve the explicit time-zone offset");

    const QTimeZone newYork(QByteArrayLiteral("America/New_York"));
    const auto summerUtc =
        QDateTime::fromString(QStringLiteral("2024-07-15T12:34:56Z"), Qt::ISODate);
    require(newYork.isValid(), "the platform must resolve zones with daylight saving");
    require(utc.toTimeZone(newYork).offsetFromUtc() == -5 * 60 * 60 &&
                summerUtc.toTimeZone(newYork).offsetFromUtc() == -4 * 60 * 60,
            "historical daylight-saving offsets must remain supported");
    const auto springUtc =
        QDateTime::fromString(QStringLiteral("2024-03-10T07:00:00Z"), Qt::ISODate);
    const auto autumnUtc =
        QDateTime::fromString(QStringLiteral("2024-11-03T06:00:00Z"), Qt::ISODate);
    require(newYork.hasTransitions(), "the native backend must retain transition information");
    require(newYork.nextTransition(utc).atUtc == springUtc &&
                newYork.nextTransition(summerUtc).atUtc == autumnUtc,
            "native daylight-saving transition instants must remain unchanged");
    require(springUtc.addSecs(-1).toTimeZone(newYork).offsetFromUtc() == -5 * 60 * 60 &&
                springUtc.toTimeZone(newYork).offsetFromUtc() == -4 * 60 * 60,
            "conversion must select the correct side of a daylight-saving boundary");
#ifdef Q_OS_WIN
    require(shanghai.abbreviation(utc) == QStringLiteral("UTC+08:00") &&
                shanghai.offsetData(utc).abbreviation == QStringLiteral("UTC+08:00"),
            "Shanghai abbreviation and transition data must use the UTC+08:00 fallback");
    require(newYork.abbreviation(utc) == QStringLiteral("UTC-05:00") &&
                newYork.offsetData(utc).abbreviation == QStringLiteral("UTC-05:00") &&
                newYork.abbreviation(summerUtc) == QStringLiteral("UTC-04:00") &&
                newYork.offsetData(summerUtc).abbreviation == QStringLiteral("UTC-04:00"),
            "New York abbreviations must distinguish standard and daylight offsets");
    require(china.toString(QStringLiteral("t")) == QStringLiteral("UTC+08:00"),
            "date formatting must retain the accurate Shanghai abbreviation fallback");
#endif

    const auto local = utc.toLocalTime();
    require(local.isValid() && local.toUTC() == utc,
            "system-local conversion must preserve the timestamp");
    for (const auto locale :
         {QLocale(QLocale::English, QLocale::UnitedStates),
          QLocale(QLocale::Chinese, QLocale::China), QLocale(QLocale::Chinese, QLocale::Taiwan)}) {
        require(locale.toString(china, QStringLiteral("yyyy-MM-dd HH:mm:ss")) ==
                    QStringLiteral("2024-01-15 20:34:56"),
                "numeric dates used by the application must preserve locale formatting");
        require(!china.toString(QStringLiteral("t")).isEmpty(),
                "time-zone abbreviations must remain available without localized display names");
        for (const auto nameType :
             {QTimeZone::ShortName, QTimeZone::LongName, QTimeZone::OffsetName}) {
            require(!shanghai.displayName(utc, nameType, locale).isEmpty(),
                    "time-zone display names must provide an accurate fallback in every locale");
            require(!newYork.displayName(QTimeZone::StandardTime, nameType, locale).isEmpty() &&
                        !newYork.displayName(QTimeZone::DaylightTime, nameType, locale).isEmpty(),
                    "standard and daylight display names must remain available");
        }
#ifdef Q_OS_WIN
        for (const auto nameType : {QTimeZone::ShortName, QTimeZone::OffsetName}) {
            require(shanghai.displayName(utc, nameType, locale) == QStringLiteral("UTC+08:00"),
                    "Shanghai short and offset names must retain UTC+08:00 in every locale");
            require(newYork.displayName(utc, nameType, locale) == QStringLiteral("UTC-05:00") &&
                        newYork.displayName(summerUtc, nameType, locale) ==
                            QStringLiteral("UTC-04:00"),
                    "New York names must retain the instant's standard or daylight offset");
            require(newYork.displayName(QTimeZone::StandardTime, nameType, locale) ==
                            QStringLiteral("UTC-05:00") &&
                        newYork.displayName(QTimeZone::DaylightTime, nameType, locale) ==
                            QStringLiteral("UTC-04:00"),
                    "time-type names must retain accurate New York fallback offsets");
        }
#endif
    }
    std::cout << "Qt time-zone contract tests passed.\n";
    return EXIT_SUCCESS;
}
