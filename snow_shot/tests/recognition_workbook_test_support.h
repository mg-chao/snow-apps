#ifndef SNOW_SHOT_TESTS_RECOGNITION_WORKBOOK_TEST_SUPPORT_H
#define SNOW_SHOT_TESTS_RECOGNITION_WORKBOOK_TEST_SUPPORT_H

#include <QFile>
#include <QMap>
#include <QScopeGuard>
#include <QXmlStreamReader>
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
#include <stdexcept>

namespace recognition_workbook_test {
inline void check(bool condition) {
    if (!condition)
        throw std::runtime_error("invalid Excel workbook package");
}

inline QMap<QString, QByteArray> parts(const QString& path, bool xmlOnly = true) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error((QStringLiteral("Cannot read workbook: ") + path +
                                  QStringLiteral(" ") + file.errorString())
                                     .toStdString());
    const QByteArray bytes = file.readAll();
    void* reader = mz_zip_reader_create();
    check(reader != nullptr);
    const auto guard = qScopeGuard([&] { mz_zip_reader_delete(&reader); });
    check(mz_zip_reader_open_buffer(reader, reinterpret_cast<const uint8_t*>(bytes.constData()),
                                    static_cast<int32_t>(bytes.size()), 0) == MZ_OK);
    QMap<QString, QByteArray> result;
    int32_t status = mz_zip_reader_goto_first_entry(reader);
    while (status == MZ_OK) {
        mz_zip_file* info = nullptr;
        check(mz_zip_reader_entry_get_info(reader, &info) == MZ_OK && info);
        const QString name = QString::fromUtf8(info->filename);
        const int32_t length = mz_zip_reader_entry_save_buffer_length(reader);
        check(length > 0);
        QByteArray xml(length, Qt::Uninitialized);
        check(mz_zip_reader_entry_save_buffer(reader, xml.data(), length) == MZ_OK);
        if (xmlOnly || name.endsWith(QStringLiteral(".xml")) ||
            name.endsWith(QStringLiteral(".rels"))) {
            QXmlStreamReader parser(xml);
            while (!parser.atEnd())
                parser.readNext();
            check(!parser.hasError());
        }
        check(!result.contains(name));
        result.insert(name, xml);
        status = mz_zip_reader_goto_next_entry(reader);
    }
    check(status == MZ_END_OF_LIST);
    return result;
}

struct Sheet {
    QMap<QString, QString> cells;
    QMap<QString, QString> styles;
    QStringList merges;
    QString dimension;
    QMap<int, double> columnWidths;
};

inline Sheet sheet(const QMap<QString, QByteArray>& package) {
    check(package.contains(QStringLiteral("xl/worksheets/sheet1.xml")));
    QXmlStreamReader xml(package.value(QStringLiteral("xl/worksheets/sheet1.xml")));
    Sheet result;
    QString reference;
    bool sawSheetData = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;
        if (xml.name() == u"dimension")
            result.dimension = xml.attributes().value(u"ref").toString();
        if (xml.name() == u"sheetData")
            sawSheetData = true;
        if (xml.name() == u"col") {
            check(!sawSheetData && xml.attributes().value(u"customWidth") == u"1");
            const int first = xml.attributes().value(u"min").toInt();
            const int last = xml.attributes().value(u"max").toInt();
            const double width = xml.attributes().value(u"width").toDouble();
            check(first > 0 && last >= first && width > 0.0 && width <= 255.0);
            for (int column = first; column <= last; ++column)
                result.columnWidths.insert(column, width);
        }
        if (xml.name() == u"mergeCell")
            result.merges.append(xml.attributes().value(u"ref").toString());
        if (xml.name() == u"c") {
            reference = xml.attributes().value(u"r").toString();
            check(xml.attributes().value(u"t") == u"inlineStr");
            result.styles.insert(reference, xml.attributes().value(u"s").toString());
        }
        if (xml.name() == u"t") {
            check(xml.attributes().value(u"http://www.w3.org/XML/1998/namespace", u"space") ==
                  u"preserve");
            result.cells.insert(reference, xml.readElementText());
        }
        check(xml.name() != u"f");
    }
    check(!xml.hasError());
    return result;
}
} // namespace recognition_workbook_test

#endif
