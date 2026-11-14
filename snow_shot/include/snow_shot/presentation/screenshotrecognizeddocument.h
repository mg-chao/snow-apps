#pragma once
#include "snow_shot/app/edition.h"
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>
class QTextDocument;
class QMimeData;

struct ScreenshotDocumentFormula {
    QString id;
    QString source;
    bool display = false;
};

struct ScreenshotDocumentClipboard {
    QString html;
    QString text;
};

// Value-only, implicitly shared snapshot. Canonical HTML contains ordered rich-text blocks,
// lists and merged tables; all resource names resolve exclusively through the owned asset map.
// Builders finish formula rasters before publishing shared_ptr<const ...> to a session.
struct ScreenshotRecognizedDocument {
    static constexpr qint64 maximumImageBytes = 64LL * 1024 * 1024;
    QString html;
    QHash<QString, QImage> images;
    QVector<ScreenshotDocumentFormula> formulas;
    std::optional<ScreenshotDocumentClipboard> preparedClipboard;
    [[nodiscard]] static std::shared_ptr<ScreenshotRecognizedDocument>
    parse(const QJsonObject& raw, const QImage& original, QString* error,
          const std::function<bool()>& cancelled = {});
    enum class FormulaFallback { Placeholder, Source };
    [[nodiscard]] std::unique_ptr<QTextDocument>
    createTextDocument(bool layout = true,
                       FormulaFallback fallback = FormulaFallback::Placeholder) const;
    void updateTextDocumentImages(QTextDocument& document, FormulaFallback fallback) const;
    [[nodiscard]] QString portableHtml(const std::function<bool()>& cancelled = {}) const;
    [[nodiscard]] QString plainText() const;
    [[nodiscard]] ScreenshotDocumentClipboard
    clipboardContent(const std::function<bool()>& cancelled = {}) const;
    [[nodiscard]] QSet<QString> markdownImageIds() const;
    [[nodiscard]] QString markdown(const QString& imageDirectory) const;
    [[nodiscard]] QString latex(const QString& imageDirectory) const;
    [[nodiscard]] qint64 retainedBytes() const;
};
