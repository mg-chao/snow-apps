#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTEDITINGSESSION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTEDITINGSESSION_H

#include <QString>
#include <QTextDocument>
#include <QVector>

class ScreenshotOcrTextEditingSession final {
  public:
    explicit ScreenshotOcrTextEditingSession(QString originalText);

    [[nodiscard]] const QString& originalText() const;
    [[nodiscard]] QString text() const;
    [[nodiscard]] QTextDocument* document();
    [[nodiscard]] const QTextDocument* document() const;

    // Replaces the complete draft as one text-history step.
    bool replaceText(const QString& text);
    bool reset();
    bool setFormatting(const QString& value);
    bool setPunctuation(const QString& value);
    void clearTransforms();
    [[nodiscard]] const QString& formatting() const;
    [[nodiscard]] const QString& punctuation() const;
    void establishBaseline(const QString& text);
    void establishHistory(const QString& text);
    void replaceTextWithoutHistory(const QString& text);
    void recordCurrentText();
    void undo();
    void redo();
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;

  private:
    void applyText(const QString& text);
    [[nodiscard]] QString transformedText() const;

    QString m_originalText;
    QString m_transformBaseline;
    QString m_formatting;
    QString m_punctuation;
    QTextDocument m_document;
    QVector<QString> m_history;
    int m_historyIndex = 0;
    bool m_applying = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTEDITINGSESSION_H
