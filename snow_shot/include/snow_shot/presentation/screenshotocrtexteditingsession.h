#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTEDITINGSESSION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTEDITINGSESSION_H

#include <QString>
#include <QTextDocument>
#include <QVector>

class ScreenshotOcrTextEditingSession final {
  public:
    explicit ScreenshotOcrTextEditingSession(QString originalText, bool preserveSource = false);
    ~ScreenshotOcrTextEditingSession();

    [[nodiscard]] const QString& originalText() const;
    [[nodiscard]] QString text() const;
    [[nodiscard]] QTextDocument* document();

    // Replaces the complete draft as one text-history step.
    bool replaceText(const QString& text);
    bool reset();
    bool applyInitialTransforms(const QString& formatting, const QString& punctuation,
                                const QString& smartText = {});
    bool setFormatting(const QString& value, const QString& smartText = {});
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
    void updatePreservedSourceMapping();
    void synchronizePreservedSource();
    void synchronizePreservedSource(int position, int charsRemoved, int charsAdded);
    [[nodiscard]] QString transformedText() const;

    QString m_originalText;
    QString m_transformBaseline;
    QString m_smartText;
    QString m_formatting;
    QString m_punctuation;
    QString m_preservedSource;
    QString m_documentRawText;
    QVector<qsizetype> m_documentSourceOffsets;
    QTextDocument m_document;
    QMetaObject::Connection m_sourceChangeConnection;
    QMetaObject::Connection m_documentChangeConnection;
    QVector<QString> m_history;
    int m_historyIndex = 0;
    bool m_applying = false;
    bool m_preserveSource = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTEDITINGSESSION_H
