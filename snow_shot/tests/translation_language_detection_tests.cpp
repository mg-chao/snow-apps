#include "snow_shot/translation/translationlanguagedetector.h"
#include "snow_shot/translation/translationservice.h"
#include "translation_test_support.h"

#include <array>
#include <limits>

using namespace snow_shot::translation;
using translation_tests::require;

namespace {
void targetSelectionUsesOnlyMatchingLanguageAndConfidence() {
    TranslationPreferences preferences{
        QStringLiteral("ja"), QStringLiteral("en"), {}, QStringLiteral("fr")};
    const auto target = [&](const QString& language, double confidence) {
        return resolveTranslationTarget(preferences,
                                        DetectedTranslationLanguage{language, confidence});
    };
    require(target(QStringLiteral("eng"), 0.6) == QStringLiteral("fr"),
            "confidence equal to the threshold switches targets");
    require(target(QStringLiteral("eng"), 0.599999) == QStringLiteral("en"),
            "confidence below the threshold retains the primary target");
    require(target(QStringLiteral("fra"), 1.0) == QStringLiteral("en") &&
                target(QStringLiteral("epo"), 1.0) == QStringLiteral("en") &&
                resolveTranslationTarget(preferences, std::nullopt) == QStringLiteral("en"),
            "different, unsupported, and unknown languages retain the primary target");
    require(target(QStringLiteral("eng"), std::numeric_limits<double>::quiet_NaN()) ==
                    QStringLiteral("en") &&
                target(QStringLiteral("eng"), std::numeric_limits<double>::infinity()) ==
                    QStringLiteral("en"),
            "invalid confidence cannot trigger a switch");
    for (const int threshold : {30, 60, 100}) {
        preferences.languageDetectionConfidenceThreshold = threshold;
        const double boundary = threshold / 100.0;
        require(target(QStringLiteral("eng"), boundary) == QStringLiteral("fr") &&
                    target(QStringLiteral("eng"), boundary - 0.000001) == QStringLiteral("en"),
                "configured minimum, default, and maximum boundaries are inclusive");
    }
    preferences.secondaryTargetLanguage = preferences.targetLanguage;
    require(target(QStringLiteral("eng"), 1.0) == preferences.targetLanguage,
            "identical targets stay unchanged");
    preferences.secondaryTargetLanguage.clear();
    require(target(QStringLiteral("eng"), 1.0) == preferences.targetLanguage,
            "an absent secondary target stays on the primary");
}

void everySupportedTargetMapsToTheDetectedLanguage() {
    static const std::array<std::pair<const char*, const char*>, 16> mappings{{
        {"ar", "ara"},
        {"de", "deu"},
        {"en", "eng"},
        {"es", "spa"},
        {"fr", "fra"},
        {"hi", "hin"},
        {"it", "ita"},
        {"ja", "jpn"},
        {"ko", "kor"},
        {"pt", "por"},
        {"ru", "rus"},
        {"th", "tha"},
        {"tr", "tur"},
        {"vi", "vie"},
        {"zh-Hans", "cmn"},
        {"zh-Hant", "cmn"},
    }};
    for (const auto& [target, language] : mappings) {
        TranslationPreferences preferences{
            QStringLiteral("auto"), QString::fromLatin1(target), {}, QStringLiteral("fr")};
        if (preferences.targetLanguage == preferences.secondaryTargetLanguage)
            preferences.secondaryTargetLanguage = QStringLiteral("en");
        require(resolveTranslationTarget(
                    preferences, DetectedTranslationLanguage{QString::fromLatin1(language), 1.0}) ==
                    preferences.secondaryTargetLanguage,
                "every configured target matches its ISO 639-3 detection code");
        for (const int threshold : {30, 60, 100}) {
            preferences.languageDetectionConfidenceThreshold = threshold;
            const double boundary = threshold / 100.0;
            require(resolveTranslationTarget(
                        preferences,
                        DetectedTranslationLanguage{QString::fromLatin1(language), boundary}) ==
                            preferences.secondaryTargetLanguage &&
                        resolveTranslationTarget(
                            preferences, DetectedTranslationLanguage{QString::fromLatin1(language),
                                                                     boundary - 0.000001}) ==
                            preferences.targetLanguage,
                    "every language respects configured routing confidence boundaries");
        }
    }
    TranslationPreferences preferences{
        QStringLiteral("auto"), QStringLiteral("zh-Hans"), {}, QStringLiteral("zh-Hant")};
    require(resolveTranslationTarget(preferences,
                                     DetectedTranslationLanguage{QStringLiteral("cmn"), 1.0}) ==
                QStringLiteral("zh-Hant"),
            "Chinese language matching preserves the chosen output script");
}

void rustAdapterDetectsTheCompleteText() {
    const auto english = detectTranslationLanguage(QStringLiteral(
        "This application translates the complete input text into the selected language.\n"
        "All paragraphs are analyzed together to choose one translation target."));
    require(english && english->languageCode == QStringLiteral("eng") && english->confidence >= 0 &&
                english->confidence <= 1,
            "Rust adapter detects a complete English input and preserves raw confidence");
    for (const auto& text : {QStringLiteral("这是一个用于识别整个输入文本语言的完整中文段落。"),
                             QStringLiteral("這是一個用於識別整個輸入文字語言的完整中文段落。")}) {
        const auto chinese = detectTranslationLanguage(text);
        require(chinese && chinese->languageCode == QStringLiteral("cmn"),
                "both Chinese scripts are detected as Mandarin");
    }
    const std::array<std::pair<QString, QString>, 3> samples{{
        {QStringLiteral("hin"), QStringLiteral("यह अनुप्रयोग पूरे पाठ का अनुवाद चुनी गई भाषा में करता है। "
                                               "सभी अनुच्छेदों का एक साथ विश्लेषण किया जाता है।")},
        {QStringLiteral("tha"), QStringLiteral("แอปพลิเคชันนี้แปลข้อความทั้งหมดเป็นภาษาที่เลือก "
                                               "ทุกย่อหน้าจะได้รับการวิเคราะห์ร่วมกันเพื่อเลือกภาษาเป้าหมาย")},
        {QStringLiteral("vie"),
         QStringLiteral("Ứng dụng này dịch toàn bộ văn bản sang ngôn ngữ đã chọn. "
                        "Tất cả các đoạn văn được phân tích cùng nhau để chọn ngôn ngữ đích.")},
    }};
    for (const auto& [language, text] : samples) {
        const auto detection = detectTranslationLanguage(text);
        require(detection && detection->languageCode == language && detection->confidence >= 0 &&
                    detection->confidence <= 1,
                "Rust adapter detects Hindi, Thai, and Vietnamese input");
    }
    require(!detectTranslationLanguage({}) &&
                !detectTranslationLanguage(QStringLiteral("12345 \n !@#$%")),
            "empty and nonlinguistic inputs have no detection");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    targetSelectionUsesOnlyMatchingLanguageAndConfidence();
    everySupportedTargetMapsToTheDetectedLanguage();
    rustAdapterDetectsTheCompleteText();
    return 0;
}
