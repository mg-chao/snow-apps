#include "snow_shot/globalmouseactivationkeys.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/ocrtextoptions.h"
#include "snow_shot/proxyconfiguration.h"
#include "snow_shot/selectiontransitionanimation.h"
#include "snow_shot/storage/floatingtoolbarsettings.h"
#include "snow_shot/app/edition.h"
#include "snow_shot/clouduploadconfiguration.h"
#if SNOW_SHOT_ENABLE_API_CONFIGURATION
#include "snow_shot/serverconfiguration.h"
#include "snow_shot/customaimodelconfiguration.h"
#include "snow_shot/texttranslationconfiguration.h"
#endif

#include "snow_shot/app/editionfeatures.h"

#include "snow_shot/storage/capturehistorytypes.h"
#include "snow_shot/storage/persistedselectioncodec.h"
#include "snow_shot/storage/recordingareahistory.h"
#include "snow_shot/shortcuts/shortcutbinding.h"
#include "snow_shot/shortcuts/menuaccesskey.h"
#include "snow_shot/shortcuts/screenshotregiontypeshortcut.h"
#include "snow_draw_engine.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QHash>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <type_traits>
#include <cmath>
#include <limits>

namespace snow_shot::storage {
namespace {
// Descriptors contain only immutable storage and scalar metadata. QStrings are
// materialized with the same static backing representation as QStringLiteral.
struct SchemaString {
    const char16_t* data = nullptr;
    quint16 size = 0;

    constexpr SchemaString() = default;
    constexpr SchemaString(const char16_t* text, quint16 length) : data(text), size(length) {}
    template <std::size_t N>
    consteval SchemaString(const char16_t (&text)[N])
        : data(text), size(static_cast<quint16>(N - 1)) {
        static_assert(N - 1 <= std::numeric_limits<quint16>::max());
    }
};

QString schemaString(SchemaString value) {
    return value.data ? QString::fromRawData(value.data, value.size) : QString();
}

// Keep one loop even when whole-program optimization sees literal arguments.
Q_NEVER_INLINE QStringList schemaStrings(const SchemaString* values, std::size_t count) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(count));
    for (std::size_t i = 0; i < count; ++i)
        result.push_back(schemaString(values[i]));
    return result;
}

template <std::size_t N> QStringList schemaStrings(const SchemaString (&values)[N]) {
    return schemaStrings(values, N);
}

template <std::size_t N, std::size_t M>
consteval auto joinedSchemaStrings(const SchemaString (&first)[N],
                                   const SchemaString (&second)[M]) {
    std::array<SchemaString, N + M> result;
    std::copy_n(first, N, result.begin());
    std::copy_n(second, M, result.begin() + N);
    return result;
}

template <std::size_t N, std::size_t M, std::size_t K>
consteval auto joinedSchemaStrings(const SchemaString (&first)[N], const SchemaString (&second)[M],
                                   const SchemaString (&third)[K]) {
    std::array<SchemaString, N + M + K> result;
    std::copy_n(first, N, result.begin());
    std::copy_n(second, M, result.begin() + N);
    std::copy_n(third, K, result.begin() + N + M);
    return result;
}

struct SchemaPosition {
    const SchemaString* items;
    std::size_t size;
};

Q_NEVER_INLINE QVector<QStringList> schemaPositions(const SchemaPosition* positions,
                                                    std::size_t count) {
    QVector<QStringList> result;
    result.reserve(static_cast<qsizetype>(count));
    for (std::size_t i = 0; i < count; ++i)
        result.push_back(schemaStrings(positions[i].items, positions[i].size));
    return result;
}

enum class SchemaDefaultKind : quint8 {
    Boolean,
    Integer,
    Number,
    String,
    EmptyArray,
    EmptyObject,
    StringArray,
    StringObject,
    Factory
};
enum class SchemaDefaultFactory : quint8 {
    CloudUpload,
    FloatingToolbar,
    MoviesDirectory,
    PicturesDirectory,
    DrawingToolbar,
    PinnedToolbar,
    ActionToolbar,
    RecordingToolbar,
    NextSelectionType,
    PreviousSelectionType
};
struct SchemaProperty {
    SchemaString key;
    SchemaString value;
};
union SchemaDefaultPayload {
    bool boolean;
    int integer;
    double number;
    const char16_t* string;
    const SchemaString* strings;
    const SchemaProperty* properties;
    SchemaDefaultFactory factory;

    constexpr explicit SchemaDefaultPayload(bool value) : boolean(value) {}
    constexpr explicit SchemaDefaultPayload(int value) : integer(value) {}
    constexpr explicit SchemaDefaultPayload(double value) : number(value) {}
    constexpr explicit SchemaDefaultPayload(const char16_t* value) : string(value) {}
    constexpr explicit SchemaDefaultPayload(const SchemaString* value) : strings(value) {}
    constexpr explicit SchemaDefaultPayload(const SchemaProperty* value) : properties(value) {}
    constexpr explicit SchemaDefaultPayload(SchemaDefaultFactory value) : factory(value) {}
};
struct SchemaDefault {
    SchemaDefaultPayload payload;
    quint16 size = 0;
    SchemaDefaultKind kind;
};

constexpr SchemaDefault booleanDefault(bool value) {
    return {SchemaDefaultPayload(value), 0, SchemaDefaultKind::Boolean};
}
constexpr SchemaDefault integerDefault(int value) {
    return {SchemaDefaultPayload(value), 0, SchemaDefaultKind::Integer};
}
constexpr SchemaDefault numberDefault(double value) {
    return {SchemaDefaultPayload(value), 0, SchemaDefaultKind::Number};
}
constexpr SchemaDefault stringDefault(SchemaString value) {
    return {SchemaDefaultPayload(value.data), value.size, SchemaDefaultKind::String};
}
constexpr SchemaDefault emptyArrayDefault() {
    return {SchemaDefaultPayload(0), 0, SchemaDefaultKind::EmptyArray};
}
constexpr SchemaDefault emptyObjectDefault() {
    return {SchemaDefaultPayload(0), 0, SchemaDefaultKind::EmptyObject};
}
template <std::size_t N> consteval SchemaDefault stringsDefault(const SchemaString (&values)[N]) {
    static_assert(N <= std::numeric_limits<quint16>::max());
    return {SchemaDefaultPayload(values), static_cast<quint16>(N), SchemaDefaultKind::StringArray};
}
template <std::size_t N> consteval SchemaDefault objectDefault(const SchemaProperty (&values)[N]) {
    static_assert(N <= std::numeric_limits<quint16>::max());
    return {SchemaDefaultPayload(values), static_cast<quint16>(N), SchemaDefaultKind::StringObject};
}
constexpr SchemaDefault factoryDefault(SchemaDefaultFactory factory) {
    return {SchemaDefaultPayload(factory), 0, SchemaDefaultKind::Factory};
}

enum class SchemaAllowedProvider : quint8 {
    Literal,
    LastDrawingTools,
    SkinPositions,
    OcrFormatting,
    OcrPunctuation
};
struct SchemaAllowedValues {
    const SchemaString* values = nullptr;
    std::size_t size = 0;
    SchemaAllowedProvider provider = SchemaAllowedProvider::Literal;
};

struct SchemaEntryDescriptor {
    const char16_t* key;
    SchemaDefaultPayload payload;
    const ConfigurationIntegerRange* integerRange;
    const SchemaAllowedValues* allowed;
    quint16 keySize;
    quint16 defaultSize;
    SchemaDefaultKind defaultKind;
    quint8 valueKind;
    qint8 maximumListItems;

    template <std::size_t N>
    consteval SchemaEntryDescriptor(const char16_t (&name)[N], SchemaDefault value,
                                    ConfigurationValueKind kind,
                                    const ConfigurationIntegerRange* range = nullptr,
                                    const SchemaAllowedValues* allowedValues = nullptr,
                                    int maximumItems = -1)
        : key(name), payload(value.payload), integerRange(range), allowed(allowedValues),
          keySize(static_cast<quint16>(N - 1)), defaultSize(value.size), defaultKind(value.kind),
          valueKind(static_cast<quint8>(kind)), maximumListItems(static_cast<qint8>(maximumItems)) {
        static_assert(N - 1 <= std::numeric_limits<quint16>::max());
        if (maximumItems < -1 || maximumItems > std::numeric_limits<qint8>::max())
            throw "schema list limit exceeds its descriptor storage";
    }
};
static_assert(std::is_trivially_copyable_v<SchemaEntryDescriptor>);
static_assert(std::is_trivially_destructible_v<SchemaEntryDescriptor>);
static_assert(sizeof(SchemaEntryDescriptor) <= 40);

QStringList editionActionIds(QStringList ids) {
    ids.removeIf([](const QString& id) { return !app::editionActionToolAvailable(id); });
    return ids;
}
QVector<QStringList> editionActionPositions(QVector<QStringList> positions) {
    for (QStringList& position : positions) {
        position.removeIf([](const QString& id) {
            return !app::editionActionToolAvailable(id) ||
                   (app::edition::isMini && id == QStringLiteral("text-recognition"));
        });
    }
    positions.removeIf([](const QStringList& position) { return position.isEmpty(); });
    return positions;
}
constexpr SchemaString kDrawingToolIdsData[] = {
    u"shape",     u"arrow",       u"line",      u"bracket",      u"angle", u"distance",
    u"free-draw", u"highlighter", u"spotlight", u"magnifier",    u"text",  u"serial-number",
    u"filter",    u"eraser",      u"watermark", u"speech-bubble"};

constexpr SchemaString kDrawingPrefix[] = {u"select", u"select-separator"};
constexpr SchemaString kDrawingSuffix[] = {u"separator", u"undo", u"redo"};
constexpr SchemaString kDrawingEmpty[] = {u""};
constexpr auto kDrawingToolbarIdsData =
    joinedSchemaStrings(kDrawingPrefix, kDrawingToolIdsData, kDrawingSuffix);
constexpr auto kLastDrawingToolIdsData = joinedSchemaStrings(kDrawingEmpty, kDrawingToolIdsData);
const QStringList kDrawingToolbarItemIds =
    schemaStrings(kDrawingToolbarIdsData.data(), kDrawingToolbarIdsData.size());
const QStringList kLastDrawingToolIds =
    schemaStrings(kLastDrawingToolIdsData.data(), kLastDrawingToolIdsData.size());
constexpr SchemaString kSkinPositionsData[] = {u"top_left",    u"top_center",    u"top_right",
                                               u"center_left", u"center",        u"center_right",
                                               u"bottom_left", u"bottom_center", u"bottom_right"};
const QStringList kSkinPositions = schemaStrings(kSkinPositionsData);

constexpr SchemaString kActionToolbarItemIdsData[] = {u"barcode-recognition",
                                                      u"table-recognition",
                                                      u"document-recognition",
                                                      u"convert-to-markdown",
                                                      u"latex-recognition",
                                                      u"convert-to-html",
                                                      u"record-screen",
                                                      u"pin-to-screen",
                                                      u"pin-window-to-screen",
                                                      u"text-recognition",
                                                      u"text-translation",
                                                      u"scrolling-screenshot",
                                                      u"quick-save",
                                                      u"print",
                                                      u"airdrop",
                                                      u"save-as-file",
                                                      u"upload-to-cloud",
                                                      u"separator",
                                                      u"cancel",
                                                      u"copy"};
const QStringList kActionToolbarItemIds =
    editionActionIds(schemaStrings(kActionToolbarItemIdsData));

QJsonArray jsonArray(const QStringList& values) {
    QJsonArray result;
    for (const QString& value : values) {
        result.push_back(value);
    }
    return result;
}

QJsonArray jsonArray(const QVector<QStringList>& values) {
    QJsonArray result;
    for (const QStringList& value : values) {
        result.push_back(jsonArray(value));
    }
    return result;
}

constexpr SchemaString defaultDrawingToolbarPositionsGroup0[] = {u"select"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup1[] = {u"select-separator"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup2[] = {u"shape"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup3[] = {u"bracket", u"angle", u"distance",
                                                                 u"line", u"arrow"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup4[] = {u"free-draw"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup5[] = {u"magnifier", u"spotlight",
                                                                 u"highlighter"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup6[] = {u"text"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup7[] = {u"speech-bubble",
                                                                 u"serial-number"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup8[] = {u"filter"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup9[] = {u"eraser"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup10[] = {u"watermark"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup11[] = {u"separator"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup12[] = {u"undo"};
constexpr SchemaString defaultDrawingToolbarPositionsGroup13[] = {u"redo"};
constexpr SchemaPosition defaultDrawingToolbarPositionsData[] = {
    {defaultDrawingToolbarPositionsGroup0, std::size(defaultDrawingToolbarPositionsGroup0)},
    {defaultDrawingToolbarPositionsGroup1, std::size(defaultDrawingToolbarPositionsGroup1)},
    {defaultDrawingToolbarPositionsGroup2, std::size(defaultDrawingToolbarPositionsGroup2)},
    {defaultDrawingToolbarPositionsGroup3, std::size(defaultDrawingToolbarPositionsGroup3)},
    {defaultDrawingToolbarPositionsGroup4, std::size(defaultDrawingToolbarPositionsGroup4)},
    {defaultDrawingToolbarPositionsGroup5, std::size(defaultDrawingToolbarPositionsGroup5)},
    {defaultDrawingToolbarPositionsGroup6, std::size(defaultDrawingToolbarPositionsGroup6)},
    {defaultDrawingToolbarPositionsGroup7, std::size(defaultDrawingToolbarPositionsGroup7)},
    {defaultDrawingToolbarPositionsGroup8, std::size(defaultDrawingToolbarPositionsGroup8)},
    {defaultDrawingToolbarPositionsGroup9, std::size(defaultDrawingToolbarPositionsGroup9)},
    {defaultDrawingToolbarPositionsGroup10, std::size(defaultDrawingToolbarPositionsGroup10)},
    {defaultDrawingToolbarPositionsGroup11, std::size(defaultDrawingToolbarPositionsGroup11)},
    {defaultDrawingToolbarPositionsGroup12, std::size(defaultDrawingToolbarPositionsGroup12)},
    {defaultDrawingToolbarPositionsGroup13, std::size(defaultDrawingToolbarPositionsGroup13)}};
QVector<QStringList> defaultDrawingToolbarPositions() {
    return schemaPositions(defaultDrawingToolbarPositionsData,
                           std::size(defaultDrawingToolbarPositionsData));
}

constexpr SchemaString defaultActionToolbarPositionsGroup0[] = {
    u"convert-to-html",     u"convert-to-markdown",  u"latex-recognition",
    u"barcode-recognition", u"document-recognition", u"table-recognition"};
constexpr SchemaString defaultActionToolbarPositionsGroup1[] = {u"record-screen"};
constexpr SchemaString defaultActionToolbarPositionsGroup2[] = {u"pin-window-to-screen",
                                                                u"pin-to-screen"};
constexpr SchemaString defaultActionToolbarPositionsGroup3[] = {u"text-recognition"};
constexpr SchemaString defaultActionToolbarPositionsGroup4[] = {u"text-translation"};
constexpr SchemaString defaultActionToolbarPositionsGroup5[] = {u"scrolling-screenshot"};
constexpr SchemaString defaultActionToolbarPositionsGroup6[] = {
    u"upload-to-cloud", u"airdrop", u"print", u"quick-save", u"save-as-file"};
constexpr SchemaString defaultActionToolbarPositionsGroup7[] = {u"separator"};
constexpr SchemaString defaultActionToolbarPositionsGroup8[] = {u"cancel"};
constexpr SchemaString defaultActionToolbarPositionsGroup9[] = {u"copy"};
constexpr SchemaPosition defaultActionToolbarPositionsData[] = {
    {defaultActionToolbarPositionsGroup0, std::size(defaultActionToolbarPositionsGroup0)},
    {defaultActionToolbarPositionsGroup1, std::size(defaultActionToolbarPositionsGroup1)},
    {defaultActionToolbarPositionsGroup2, std::size(defaultActionToolbarPositionsGroup2)},
    {defaultActionToolbarPositionsGroup3, std::size(defaultActionToolbarPositionsGroup3)},
    {defaultActionToolbarPositionsGroup4, std::size(defaultActionToolbarPositionsGroup4)},
    {defaultActionToolbarPositionsGroup5, std::size(defaultActionToolbarPositionsGroup5)},
    {defaultActionToolbarPositionsGroup6, std::size(defaultActionToolbarPositionsGroup6)},
    {defaultActionToolbarPositionsGroup7, std::size(defaultActionToolbarPositionsGroup7)},
    {defaultActionToolbarPositionsGroup8, std::size(defaultActionToolbarPositionsGroup8)},
    {defaultActionToolbarPositionsGroup9, std::size(defaultActionToolbarPositionsGroup9)}};
QVector<QStringList> defaultActionToolbarPositions() {
    return editionActionPositions(schemaPositions(defaultActionToolbarPositionsData,
                                                  std::size(defaultActionToolbarPositionsData)));
}

constexpr SchemaString kPinnedActionToolbarItemIdsData[] = {u"barcode-recognition",
                                                            u"table-recognition",
                                                            u"document-recognition",
                                                            u"convert-to-markdown",
                                                            u"latex-recognition",
                                                            u"convert-to-html",
                                                            u"text-recognition",
                                                            u"text-translation",
                                                            u"separator",
                                                            u"print",
                                                            u"airdrop",
                                                            u"quick-save",
                                                            u"save-as-file",
                                                            u"upload-to-cloud",
                                                            u"copy",
                                                            u"confirm",
                                                            u"confirm-separator"};
const QStringList kPinnedActionToolbarItemIds =
    editionActionIds(schemaStrings(kPinnedActionToolbarItemIdsData));

constexpr SchemaString defaultPinnedActionToolbarPositionsGroup0[] = {
    u"convert-to-html",     u"convert-to-markdown",  u"latex-recognition",
    u"barcode-recognition", u"document-recognition", u"table-recognition"};
constexpr SchemaString defaultPinnedActionToolbarPositionsGroup1[] = {u"text-recognition"};
constexpr SchemaString defaultPinnedActionToolbarPositionsGroup2[] = {u"text-translation"};
constexpr SchemaString defaultPinnedActionToolbarPositionsGroup3[] = {u"separator"};
constexpr SchemaString defaultPinnedActionToolbarPositionsGroup4[] = {
    u"upload-to-cloud", u"airdrop", u"print", u"quick-save", u"save-as-file"};
constexpr SchemaString defaultPinnedActionToolbarPositionsGroup5[] = {u"copy"};
constexpr SchemaString defaultPinnedActionToolbarPositionsGroup6[] = {u"confirm-separator"};
constexpr SchemaString defaultPinnedActionToolbarPositionsGroup7[] = {u"confirm"};
constexpr SchemaPosition defaultPinnedActionToolbarPositionsData[] = {
    {defaultPinnedActionToolbarPositionsGroup0,
     std::size(defaultPinnedActionToolbarPositionsGroup0)},
    {defaultPinnedActionToolbarPositionsGroup1,
     std::size(defaultPinnedActionToolbarPositionsGroup1)},
    {defaultPinnedActionToolbarPositionsGroup2,
     std::size(defaultPinnedActionToolbarPositionsGroup2)},
    {defaultPinnedActionToolbarPositionsGroup3,
     std::size(defaultPinnedActionToolbarPositionsGroup3)},
    {defaultPinnedActionToolbarPositionsGroup4,
     std::size(defaultPinnedActionToolbarPositionsGroup4)},
    {defaultPinnedActionToolbarPositionsGroup5,
     std::size(defaultPinnedActionToolbarPositionsGroup5)},
    {defaultPinnedActionToolbarPositionsGroup6,
     std::size(defaultPinnedActionToolbarPositionsGroup6)},
    {defaultPinnedActionToolbarPositionsGroup7,
     std::size(defaultPinnedActionToolbarPositionsGroup7)}};
QVector<QStringList> defaultPinnedActionToolbarPositions() {
    return editionActionPositions(
        schemaPositions(defaultPinnedActionToolbarPositionsData,
                        std::size(defaultPinnedActionToolbarPositionsData)));
}

QJsonObject defaultToolbarLayout(const QVector<QStringList>& positions, bool actionTools = false) {
    return {{QStringLiteral("positions"), jsonArray(positions)},
            {QStringLiteral("hidden"), actionTools && app::edition::isMini
                                           ? QJsonArray{QStringLiteral("text-recognition")}
                                           : QJsonArray()}};
}

constexpr SchemaString kRecordingActionToolbarItemIdsData[] = {
    u"start-stop",  u"pause-resume", u"duration", u"microphone", u"system-audio", u"separator",
    u"open-folder", u"close",        u"trim",     u"save",       u"copy"};
const QStringList kRecordingActionToolbarItemIds =
    schemaStrings(kRecordingActionToolbarItemIdsData);

QVector<QStringList> defaultRecordingActionToolbarPositions() {
    QVector<QStringList> positions;
    positions.reserve(kRecordingActionToolbarItemIds.size());
    for (const QString& id : kRecordingActionToolbarItemIds) {
        positions.push_back({id});
    }
    return positions;
}

QString defaultOutputDirectory(QStandardPaths::StandardLocation primary) {
    QString root = QStandardPaths::writableLocation(primary);
    if (root.isEmpty()) {
        root = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    return root;
}

constexpr SchemaString kSchemaStrings0[] = {u"#F5222DFF", u"#52C41AFF", u"#1677FFFF", u"#FADB14FF",
                                            u"#000000FF"};
constexpr SchemaString kSchemaStrings1[] = {u"#FFFFFF00", u"#FFCCC7FF", u"#D9F7BEFF", u"#BAE0FFFF",
                                            u"#FFF1B8FF"};
constexpr SchemaString kHighlightColorPresets[] = {u"#FF7875FF", u"#D3F261FF", u"#69B1FFFF",
                                                   u"#FFF566FF", u"#B37FEBFF"};
constexpr SchemaString kSchemaStrings2[] = {u"Ctrl+U"};
constexpr ConfigurationIntegerRange kSchemaRange0{10, 100, 1};
constexpr SchemaString kSchemaStrings3[] = {u"icon", u"toolbar"};
constexpr SchemaAllowedValues kSchemaAllowed0 = {kSchemaStrings3, std::size(kSchemaStrings3)};
constexpr ConfigurationIntegerRange kSchemaRange1{5, 5, 1};
constexpr SchemaString kSchemaStrings4[] = {u"system", u"light", u"dark"};
constexpr SchemaAllowedValues kSchemaAllowed1 = {kSchemaStrings4, std::size(kSchemaStrings4)};
constexpr ConfigurationIntegerRange kSchemaRange2{12, 32, 1};
constexpr SchemaAllowedValues kSchemaAllowed2 = {nullptr, 0, SchemaAllowedProvider::SkinPositions};
constexpr SchemaString kSchemaStrings5[] = {u"overlay", u"contain"};
constexpr SchemaAllowedValues kSchemaAllowed3 = {kSchemaStrings5, std::size(kSchemaStrings5)};
constexpr ConfigurationIntegerRange kSchemaRange3{0, 100, 1};
constexpr SchemaString kSchemaStrings6[] = {u"normal", u"above_normal", u"high", u"real_time"};
constexpr SchemaAllowedValues kSchemaAllowed4 = {kSchemaStrings6, std::size(kSchemaStrings6)};
constexpr SchemaString kSchemaStrings7[] = {u"user_interactive", u"user_initiated", u"default",
                                            u"utility", u"background"};
constexpr SchemaAllowedValues kSchemaAllowed5 = {kSchemaStrings7, std::size(kSchemaStrings7)};
constexpr SchemaString kSchemaStrings8[] = {u"smart_control", u"disabled"};
constexpr SchemaAllowedValues kSchemaAllowed6 = {kSchemaStrings8, std::size(kSchemaStrings8)};
constexpr SchemaString kSchemaStrings9[] = {u"images_only", u"image_files", u"disabled"};
constexpr SchemaAllowedValues kSchemaAllowed7 = {kSchemaStrings9, std::size(kSchemaStrings9)};
constexpr SchemaString kSchemaStrings10[] = {u"manual", u"check"};
constexpr SchemaAllowedValues kSchemaAllowed8 = {kSchemaStrings10, std::size(kSchemaStrings10)};
constexpr SchemaString kSchemaStrings11[] = {u"manual", u"check", u"download", u"next_launch"};
[[maybe_unused]] constexpr SchemaAllowedValues kSchemaAllowed9 = {kSchemaStrings11,
                                                                  std::size(kSchemaStrings11)};
constexpr SchemaString kSchemaStrings12[] = {u"none", u"system", u"manual"};
constexpr SchemaAllowedValues kSchemaAllowed10 = {kSchemaStrings12, std::size(kSchemaStrings12)};
constexpr SchemaString kSchemaStrings13[] = {u"extra_small", u"small", u"medium"};
constexpr SchemaAllowedValues kSchemaAllowed11 = {kSchemaStrings13, std::size(kSchemaStrings13)};
constexpr SchemaString kSchemaStrings14[] = {u"max", u"min"};
constexpr SchemaAllowedValues kSchemaAllowed12 = {kSchemaStrings14, std::size(kSchemaStrings14)};
constexpr SchemaString kSchemaStrings15[] = {u"accuracy_first", u"speed_first"};
constexpr SchemaAllowedValues kSchemaAllowed13 = {kSchemaStrings15, std::size(kSchemaStrings15)};
constexpr SchemaString kSchemaStrings16[] = {u"auto", u"ar", u"de", u"en",      u"es",     u"fr",
                                             u"hi",   u"it", u"ja", u"ko",      u"pt",     u"ru",
                                             u"th",   u"tr", u"vi", u"zh-Hans", u"zh-Hant"};
[[maybe_unused]] constexpr SchemaAllowedValues kSchemaAllowed14 = {kSchemaStrings16,
                                                                   std::size(kSchemaStrings16)};
constexpr SchemaString kSchemaStrings17[] = {u"ar", u"de", u"en",      u"es",     u"fr", u"hi",
                                             u"it", u"ja", u"ko",      u"pt",     u"ru", u"th",
                                             u"tr", u"vi", u"zh-Hans", u"zh-Hant"};
[[maybe_unused]] constexpr SchemaAllowedValues kSchemaAllowed15 = {kSchemaStrings17,
                                                                   std::size(kSchemaStrings17)};
[[maybe_unused]] constexpr ConfigurationIntegerRange kSchemaRange4{30, 100};
constexpr SchemaString kSchemaStrings18[] = {u"smart_merge", u"original"};
[[maybe_unused]] constexpr SchemaAllowedValues kSchemaAllowed16 = {kSchemaStrings18,
                                                                   std::size(kSchemaStrings18)};
constexpr SchemaAllowedValues kSchemaAllowed17 = {nullptr, 0, SchemaAllowedProvider::OcrFormatting};
constexpr SchemaAllowedValues kSchemaAllowed18 = {nullptr, 0,
                                                  SchemaAllowedProvider::OcrPunctuation};
constexpr SchemaString kSchemaStrings19[] = {u"blur", u"background_fill"};
constexpr SchemaAllowedValues kSchemaAllowed19 = {kSchemaStrings19, std::size(kSchemaStrings19)};
constexpr SchemaString kSchemaStrings20[] = {u"F1"};
constexpr SchemaString kSchemaStrings21[] = {u"Ctrl+F1"};
constexpr SchemaString kSchemaStrings22[] = {u"F3"};
constexpr SchemaString kSchemaStrings23[] = {u"Ctrl+F3"};
constexpr SchemaString kSchemaStrings24[] = {u"all_windows", u"game_windows"};
constexpr SchemaAllowedValues kSchemaAllowed20 = {kSchemaStrings24, std::size(kSchemaStrings24)};
[[maybe_unused]] constexpr SchemaProperty kSchemaProperties0[] = {{u"activation_key", u"windows"},
                                                                  {u"mouse_button", u"left_drag"}};
[[maybe_unused]] constexpr SchemaProperty kSchemaProperties1[] = {{u"activation_key", u"windows"},
                                                                  {u"mouse_button", u"wheel_drag"}};
[[maybe_unused]] constexpr SchemaProperty kSchemaProperties2[] = {{u"activation_key", u"windows"},
                                                                  {u"mouse_button", u"right_drag"}};
constexpr ConfigurationIntegerRange kSchemaRange5{-24, 24, 1};
constexpr SchemaString kSchemaStrings25[] = {u"4k", u"2k", u"1080p", u"720p", u"480p"};
constexpr SchemaAllowedValues kSchemaAllowed21 = {kSchemaStrings25, std::size(kSchemaStrings25)};
constexpr SchemaString kSchemaStrings26[] = {u"1080p", u"720p", u"480p"};
constexpr SchemaAllowedValues kSchemaAllowed22 = {kSchemaStrings26, std::size(kSchemaStrings26)};
constexpr SchemaString kSchemaStrings27[] = {u"mp4", u"gif", u"apng", u"webp"};
constexpr SchemaAllowedValues kSchemaAllowed23 = {kSchemaStrings27, std::size(kSchemaStrings27)};
constexpr ConfigurationIntegerRange kSchemaRange6{100, 2000, 100};
constexpr ConfigurationIntegerRange kSchemaRange7{32, 128, 8};
constexpr ConfigurationIntegerRange kSchemaRange8{0, 7, 1};
constexpr ConfigurationIntegerRange kSchemaRange9{50, 500, 1};
constexpr ConfigurationIntegerRange kSchemaRange10{0, 32768, 1};
constexpr SchemaString kSchemaStrings28[] = {u"square", u"circle"};
constexpr SchemaAllowedValues kSchemaAllowed24 = {kSchemaStrings28, std::size(kSchemaStrings28)};
constexpr SchemaString kSchemaStrings29[] = {u"progress_bar", u"playback_time"};
constexpr SchemaAllowedValues kSchemaAllowed25 = {kSchemaStrings29, std::size(kSchemaStrings29)};
constexpr ConfigurationIntegerRange kSchemaRange11{8, 128, 1};
constexpr SchemaString kSchemaStrings30[] = {u"h264_hw", u"h265_hw", u"h264", u"h265"};
constexpr SchemaAllowedValues kSchemaAllowed26 = {kSchemaStrings30, std::size(kSchemaStrings30)};
constexpr SchemaString kSchemaStrings31[] = {u"ultrafast", u"veryfast", u"medium", u"veryslow",
                                             u"placebo"};
constexpr SchemaAllowedValues kSchemaAllowed27 = {kSchemaStrings31, std::size(kSchemaStrings31)};
constexpr SchemaString kSchemaStrings32[] = {u"dxgi", u"wgc", u"gdi"};
constexpr SchemaAllowedValues kSchemaAllowed28 = {kSchemaStrings32, std::size(kSchemaStrings32)};
constexpr ConfigurationIntegerRange kSchemaRange12{0, 10, 1};
constexpr SchemaString kSchemaStrings33[] = {u"free-draw", u"pen-filter", u"text",
                                             u"speech-bubble"};
constexpr SchemaString kSchemaStrings34[] = {
    u"shape",         u"arrow",     u"line",          u"bracket",
    u"angle",         u"distance",  u"free-draw",     u"rectangle-highlight",
    u"pen-highlight", u"spotlight", u"magnifier",     u"rectangle-filter",
    u"pen-filter",    u"text",      u"serial-number", u"speech-bubble"};
constexpr SchemaAllowedValues kSchemaAllowed29 = {kSchemaStrings34, std::size(kSchemaStrings34)};
constexpr SchemaString kSchemaStrings35[] = {u"free-draw", u"pen-filter"};
constexpr SchemaString kSchemaStrings36[] = {
    u"shape",         u"arrow",        u"line",          u"bracket",
    u"angle",         u"distance",     u"free-draw",     u"rectangle-highlight",
    u"pen-highlight", u"spotlight",    u"magnifier",     u"rectangle-filter",
    u"pen-filter",    u"text",         u"serial-number", u"eraser",
    u"watermark",     u"speech-bubble"};
constexpr SchemaAllowedValues kSchemaAllowed30 = {kSchemaStrings36, std::size(kSchemaStrings36)};
constexpr SchemaProperty kSchemaProperties3[] = {{u"text", {}}, {u"template_value", {}}};
constexpr SchemaString kSchemaStrings37[] = {u"V"};
constexpr SchemaString kSchemaStrings38[] = {u"1"};
constexpr SchemaString kSchemaStrings39[] = {u"2"};
constexpr SchemaString kSchemaStrings40[] = {u"3", u"P"};
constexpr SchemaString kSchemaStrings41[] = {u"4", u"H"};
constexpr SchemaString kSchemaStrings42[] = {u"5", u"T"};
constexpr SchemaString kSchemaStrings43[] = {u"6", u"N"};
constexpr SchemaString kSchemaStrings44[] = {u"7", u"F"};
constexpr SchemaString kSchemaStrings45[] = {u"8", u"E"};
constexpr SchemaString kSchemaStrings46[] = {u"Ctrl+Shift+Z"};
constexpr SchemaString kSchemaStrings47[] = {u"9"};
constexpr SchemaString kSchemaStrings48[] = {u"M", u"Ctrl+E"};
constexpr SchemaString kSchemaStrings49[] = {u"W", u"Up"};
constexpr SchemaString kSchemaStrings50[] = {u"S", u"Down"};
constexpr SchemaString kSchemaStrings51[] = {u"A", u"Left"};
constexpr SchemaString kSchemaStrings52[] = {u"D", u"Right"};
constexpr SchemaString kSchemaStrings53[] = {u"Space"};
constexpr SchemaString kSchemaStrings54[] = {u"Shift"};
constexpr SchemaString kSchemaStrings55[] = {u"Q"};
constexpr SchemaString kSchemaStrings56[] = {u"Tab"};
constexpr SchemaString kSchemaStrings57[] = {u","};
constexpr SchemaString kSchemaStrings58[] = {u"."};
constexpr SchemaString kSchemaStrings59[] = {u"R"};
constexpr SchemaString kSchemaStrings60[] = {u"Shift+R"};
constexpr SchemaString kSchemaStrings61[] = {u"`"};
constexpr SchemaString kSchemaStrings62[] = {u"Alt+R"};
constexpr SchemaString kSchemaStrings63[] = {u"Shift+P"};
constexpr SchemaString kSchemaStrings64[] = {u"Alt"};
constexpr SchemaString kSchemaStrings65[] = {u"C"};
[[maybe_unused]] constexpr SchemaString kSchemaStrings66[] = {u"Ctrl+X"};
[[maybe_unused]] constexpr SchemaString kSchemaStrings67[] = {u"Ctrl+Q"};
constexpr SchemaString kSchemaStrings68[] = {u"Ctrl+R"};
constexpr SchemaString kSchemaStrings69[] = {u"Ctrl+D"};
[[maybe_unused]] constexpr SchemaString kSchemaStrings70[] = {u"Ctrl+T"};
constexpr SchemaString kSchemaStrings71[] = {u"L"};
constexpr SchemaString kSchemaStrings72[] = {u"Ctrl+Shift+S"};
constexpr SchemaString kSchemaStrings73[] = {u"Ctrl+P"};
constexpr SchemaString kSchemaStrings74[] = {u"Ctrl+S"};
constexpr SchemaString kSchemaStrings75[] = {u"Ctrl+F"};
constexpr SchemaString kSchemaStrings76[] = {u"Alt+W"};
constexpr SchemaString kSchemaStrings77[] = {u"Esc"};
constexpr SchemaString kSchemaStrings78[] = {u"Ctrl+C", u"Return"};
constexpr SchemaString kSchemaStrings79[] = {u"Ctrl+Z"};
constexpr SchemaString kSchemaStrings80[] = {u"Ctrl+Y"};
constexpr SchemaString kSchemaStrings81[] = {u"Ctrl+E"};
constexpr SchemaString kSchemaStrings82[] = {u"Ctrl+C"};
constexpr SchemaString kSchemaStrings83[] = {u"Ctrl+Shift+C"};
constexpr SchemaString kSchemaStrings84[] = {u"Ctrl+Alt+A"};
constexpr SchemaString kSchemaStrings85[] = {u"Ctrl+Alt+J"};
constexpr SchemaString kSchemaStrings86[] = {u"Ctrl+Alt+C"};
constexpr SchemaString kSchemaStrings87[] = {u"Ctrl+Alt+S"};
constexpr SchemaString kSchemaStrings88[] = {u"M"};
constexpr SchemaString kSchemaStrings89[] = {u"H"};
constexpr SchemaString kSchemaStrings90[] = {u"Ctrl+M"};
constexpr SchemaString kSchemaStrings91[] = {u"T"};
constexpr SchemaString kSchemaStrings92[] = {u"B"};
constexpr SchemaString kSchemaStrings93[] = {u"Y"};
constexpr SchemaString kSchemaStrings94[] = {u"Shift+Esc"};
constexpr SchemaString kSchemaStrings95[] = {u"]"};
constexpr SchemaString kSchemaStrings96[] = {u"["};
constexpr SchemaString kSchemaStrings97[] = {u"3"};
constexpr SchemaString kSchemaStrings98[] = {u"4"};
constexpr SchemaString kSchemaStrings99[] = {u"0"};
constexpr SchemaString kSchemaStrings100[] = {u"arrow", u"line"};
constexpr SchemaAllowedValues kSchemaAllowed31 = {kSchemaStrings100, std::size(kSchemaStrings100)};
constexpr SchemaString kSchemaStrings101[] = {u"pen_highlight", u"highlight", u"spotlight"};
constexpr SchemaAllowedValues kSchemaAllowed32 = {kSchemaStrings101, std::size(kSchemaStrings101)};
constexpr SchemaString kSchemaStrings102[] = {u"table", u"qr"};
[[maybe_unused]] constexpr SchemaAllowedValues kSchemaAllowed33 = {kSchemaStrings102,
                                                                   std::size(kSchemaStrings102)};
constexpr SchemaString kSchemaStrings103[] = {u"rectangle-filter", u"pen-filter", u"auto-filter"};
constexpr SchemaAllowedValues kSchemaAllowed34 = {kSchemaStrings103, std::size(kSchemaStrings103)};
constexpr SchemaString kSchemaStrings104[] = {u"eraser", u"rectangle-eraser", u"brush-eraser"};
constexpr SchemaAllowedValues kSchemaAllowed35 = {kSchemaStrings104, std::size(kSchemaStrings104)};
constexpr SchemaString kSchemaStrings105[] = {u"rectangle-highlight", u"pen-highlight"};
constexpr SchemaAllowedValues kSchemaAllowed36 = {kSchemaStrings105, std::size(kSchemaStrings105)};
constexpr SchemaAllowedValues kSchemaAllowed37 = {nullptr, 0,
                                                  SchemaAllowedProvider::LastDrawingTools};
constexpr SchemaString kSchemaStrings106[] = {u"small", u"normal"};
constexpr SchemaAllowedValues kSchemaAllowed38 = {kSchemaStrings106, std::size(kSchemaStrings106)};
constexpr ConfigurationIntegerRange kSchemaRange13{0, 72, 1};
constexpr SchemaString kSchemaStrings107[] = {u"physical_pixels", u"logical_pixels"};
constexpr SchemaAllowedValues kSchemaAllowed39 = {kSchemaStrings107, std::size(kSchemaStrings107)};
constexpr SchemaString kSchemaStrings108[] = {u"hide_outside_selection", u"always_show",
                                              u"always_hide"};
constexpr SchemaAllowedValues kSchemaAllowed40 = {kSchemaStrings108, std::size(kSchemaStrings108)};
constexpr SchemaString kSchemaStrings109[] = {u"global", u"relative"};
constexpr SchemaAllowedValues kSchemaAllowed41 = {kSchemaStrings109, std::size(kSchemaStrings109)};
constexpr SchemaString kSchemaStrings110[] = {u"hex", u"hex_without_hash", u"rgb", u"hsl"};
constexpr SchemaAllowedValues kSchemaAllowed42 = {kSchemaStrings110, std::size(kSchemaStrings110)};
constexpr ConfigurationIntegerRange kSchemaRange14{100, 2000, 1};
constexpr ConfigurationIntegerRange kSchemaRange15{8, 96, 1};
constexpr SchemaString kSchemaStrings111[] = {u"mouse_position", u"top_left",     u"top_right",
                                              u"bottom_left",    u"bottom_right", u"center"};
constexpr SchemaAllowedValues kSchemaAllowed43 = {kSchemaStrings111, std::size(kSchemaStrings111)};
constexpr ConfigurationIntegerRange kSchemaRange16{1, 20, 1};
constexpr SchemaString kSchemaStrings112[] = {
    u"none",  u"reset_zoom", u"thumbnail_mode", u"hide_to_top", u"show_text_recognition_results",
    u"close", u"destroy"};
constexpr SchemaAllowedValues kSchemaAllowed44 = {kSchemaStrings112, std::size(kSchemaStrings112)};
constexpr SchemaString kSchemaStrings113[] = {u"only_when_displayed", u"always"};
constexpr SchemaAllowedValues kSchemaAllowed45 = {kSchemaStrings113, std::size(kSchemaStrings113)};
constexpr SchemaString kSchemaStrings114[] = {u"none", u"shake_window",
                                              u"restore_last_closed_window", u"repeat_action"};
constexpr SchemaAllowedValues kSchemaAllowed46 = {kSchemaStrings114, std::size(kSchemaStrings114)};
constexpr SchemaString kSchemaStrings115[] = {u"default",      u"light",      u"dark",
                                              u"snow-default", u"snow-light", u"snow-dark"};
constexpr SchemaAllowedValues kSchemaAllowed47 = {kSchemaStrings115, std::size(kSchemaStrings115)};
constexpr SchemaString kSchemaStrings116[] = {u"inherit",      u"default",    u"light",    u"dark",
                                              u"snow-default", u"snow-light", u"snow-dark"};
constexpr SchemaAllowedValues kSchemaAllowed48 = {kSchemaStrings116, std::size(kSchemaStrings116)};
constexpr SchemaString kSchemaStrings117[] = {u"none",
                                              u"screenshot",
                                              u"show_main_window",
                                              u"screenshot_copy",
                                              u"screenshot_fixed",
                                              u"open_capture_history",
                                              u"open_pin_to_screen_management",
                                              u"open_function_settings"};
constexpr SchemaAllowedValues kSchemaAllowed49 = {kSchemaStrings117, std::size(kSchemaStrings117)};
constexpr SchemaString kSchemaStrings118[] = {u"quick.screenshot",
                                              u"quick.screenshot-delay",
                                              u"quick.screenshot-fixed",
                                              u"quick.screenshot-ocr",
                                              u"quick.screenshot-copy",
                                              u"quick.pin-clipboard-content",
                                              u"quick.restore-last-closed-windows",
                                              u"quick.screen-record",
                                              u"quick.toggle-global-hotkeys",
                                              u"tray.window-grouping",
                                              u"tray.show-main-window",
                                              u"tray.exit"};
constexpr SchemaString kSchemaStrings119[] = {u"quick.screenshot",
                                              u"quick.screenshot-delay",
                                              u"quick.screenshot-fixed",
                                              u"quick.screenshot-ocr",
                                              u"quick.screenshot-ocr-copy",
                                              u"quick.screenshot-translation",
                                              u"quick.screenshot-scrolling",
                                              u"quick.screenshot-copy",
                                              u"quick.screenshot-save",
                                              u"quick.screenshot-quick-save",
                                              u"quick.screenshot-full-screen",
                                              u"quick.screenshot-focused-window",
                                              u"quick.pin-clipboard-content",
                                              u"quick.pin-selected-files",
                                              u"quick.restore-last-closed-windows",
                                              u"quick.hide-show-all-windows",
                                              u"quick.open-pin-to-screen-management",
                                              u"quick.screen-record",
                                              u"quick.screen-record-copy",
                                              u"quick.open-screen-recording-folder",
                                              u"quick.open-capture-history",
                                              u"quick.global-canvas",
                                              u"quick.stitch-selected-images",
                                              u"quick.split-selected-images",
                                              u"quick.edit-selected-images",
                                              u"quick.translate-selected-text",
                                              u"quick.toggle-window-under-cursor-always-on-top",
                                              u"quick.toggle-global-hotkeys",
                                              u"quick.toggle-disable-on-focused-fullscreen-window",
                                              u"tray.window-grouping",
                                              u"tray.show-main-window",
                                              u"tray.restart-app",
                                              u"tray.exit"};
[[maybe_unused]] constexpr SchemaAllowedValues kSchemaAllowed50 = {kSchemaStrings119,
                                                                   std::size(kSchemaStrings119)};
constexpr SchemaString kSchemaStrings120[] = {u"quick.screenshot",
                                              u"quick.screenshot-delay",
                                              u"quick.screenshot-fixed",
                                              u"quick.screenshot-ocr",
                                              u"quick.screenshot-ocr-copy",
                                              u"quick.screenshot-translation",
                                              u"quick.screenshot-scrolling",
                                              u"quick.screenshot-copy",
                                              u"quick.screenshot-save",
                                              u"quick.screenshot-quick-save",
                                              u"quick.screenshot-full-screen",
                                              u"quick.screenshot-focused-window",
                                              u"quick.pin-clipboard-content",
                                              u"quick.pin-selected-files",
                                              u"quick.restore-last-closed-windows",
                                              u"quick.hide-show-all-windows",
                                              u"quick.open-pin-to-screen-management",
                                              u"quick.screen-record",
                                              u"quick.screen-record-copy",
                                              u"quick.open-screen-recording-folder",
                                              u"quick.open-capture-history",
                                              u"quick.global-canvas",
                                              u"quick.stitch-selected-images",
                                              u"quick.split-selected-images",
                                              u"quick.edit-selected-images",
                                              u"quick.translate-selected-text",
                                              u"quick.toggle-global-hotkeys",
                                              u"quick.toggle-disable-on-focused-fullscreen-window",
                                              u"tray.window-grouping",
                                              u"tray.show-main-window",
                                              u"tray.restart-app",
                                              u"tray.exit"};
constexpr SchemaAllowedValues kSchemaAllowed51 = {kSchemaStrings120, std::size(kSchemaStrings120)};
constexpr SchemaString kSchemaStrings121[] = {u"rectangle", u"polyline", u"curve", u"freehand"};
constexpr SchemaAllowedValues kSchemaAllowed52 = {kSchemaStrings121, std::size(kSchemaStrings121)};
constexpr SchemaString kSchemaStrings122[] = {u"window_sub_element", u"window", u"disabled"};
constexpr SchemaAllowedValues kSchemaAllowed53 = {kSchemaStrings122, std::size(kSchemaStrings122)};
constexpr ConfigurationIntegerRange kSchemaRange17{0, ScreenshotLayoutCompaction::maximumSpacing,
                                                   1};
constexpr ConfigurationIntegerRange kSchemaRange18{0, 256, 1};
constexpr SchemaString kSchemaStrings123[] = {u"shadow", u"border"};
constexpr SchemaAllowedValues kSchemaAllowed54 = {kSchemaStrings123, std::size(kSchemaStrings123)};
constexpr ConfigurationIntegerRange kSchemaRange19{0, 64, 1};
constexpr SchemaString kSchemaStrings124[] = {u"free", u"1:1", u"3:2", u"4:3",
                                              u"16:9", u"2:3", u"3:4", u"9:16"};
constexpr SchemaAllowedValues kSchemaAllowed55 = {kSchemaStrings124, std::size(kSchemaStrings124)};
constexpr ConfigurationIntegerRange kSchemaRange20{128, 1000, 1};
constexpr ConfigurationIntegerRange kSchemaRange21{1, 10, 1};
constexpr SchemaString kSchemaStrings125[] = {u"no_action",
                                              u"copy_text",
                                              u"copy_text_and_end_screenshot",
                                              u"quick_copy_text",
                                              u"quick_copy_text_and_end_screenshot",
                                              u"enable_edit_mode"};
constexpr SchemaAllowedValues kSchemaAllowed56 = {kSchemaStrings125, std::size(kSchemaStrings125)};
constexpr SchemaString kSchemaStrings126[] = {u"copy", u"save",     u"quick_save", u"pin",
                                              u"ocr",  u"ocr_copy", u"cancel",     u"none"};
constexpr SchemaAllowedValues kSchemaAllowed57 = {kSchemaStrings126, std::size(kSchemaStrings126)};
constexpr SchemaString kHiDpiImageScalingValues[] = {u"disabled", u"auto", u"125", u"150",
                                                     u"175",      u"200",  u"225", u"250",
                                                     u"275",      u"300",  u"325", u"350"};
constexpr SchemaAllowedValues kHiDpiImageScalingAllowed = {kHiDpiImageScalingValues,
                                                           std::size(kHiDpiImageScalingValues)};

constexpr SchemaString kSchemaStrings127[] = {u"follow_mouse_movement", u"follow_mouse_position"};
constexpr SchemaAllowedValues kSchemaAllowed58 = {kSchemaStrings127, std::size(kSchemaStrings127)};
constexpr SchemaString kSchemaStrings128[] = {
    u"copy_screenshot",        u"copy_scrolling_screenshot", u"copy_pinned_image",
    u"capture_focused_window", u"capture_current_monitor",   u"pin_to_screen"};
constexpr SchemaAllowedValues kSchemaAllowed59 = {kSchemaStrings128, std::size(kSchemaStrings128)};
constexpr SchemaString kSchemaStrings129[] = {u"png", u"jpeg", u"bmp",  u"webp",
                                              u"jxl", u"avif", u"tiff", u"pdf"};
constexpr SchemaAllowedValues kSchemaAllowed60 = {kSchemaStrings129, std::size(kSchemaStrings129)};
constexpr SchemaString kSchemaStrings130[] = {u"system", u"snow_shot"};
constexpr SchemaAllowedValues kSchemaAllowed61 = {kSchemaStrings130, std::size(kSchemaStrings130)};
constexpr SchemaString kSchemaStrings131[] = {u"vertical", u"horizontal", u"custom"};
constexpr SchemaAllowedValues kSchemaAllowed62 = {kSchemaStrings131, std::size(kSchemaStrings131)};
constexpr ConfigurationIntegerRange kSchemaRange22{1, std::numeric_limits<int>::max(), 1};
constexpr SchemaString kSchemaStrings132[] = {u"original", u"uniform"};
constexpr SchemaAllowedValues kSchemaAllowed63 = {kSchemaStrings132, std::size(kSchemaStrings132)};
constexpr ConfigurationIntegerRange kSchemaRange23{0, std::numeric_limits<int>::max(), 1};
constexpr SchemaString kSchemaStrings133[] = {u"page_based", u"template_based"};
constexpr SchemaAllowedValues kSchemaAllowed64 = {kSchemaStrings133, std::size(kSchemaStrings133)};
constexpr SchemaString kSchemaStrings134[] = {u"two_images",   u"id_card",      u"three_images",
                                              u"four_images",  u"five_images",  u"six_images",
                                              u"seven_images", u"eight_images", u"nine_images"};
constexpr SchemaAllowedValues kSchemaAllowed65 = {kSchemaStrings134, std::size(kSchemaStrings134)};
constexpr SchemaString kSchemaStrings135[] = {u"a3",     u"a4",    u"a5",     u"b5",
                                              u"letter", u"legal", u"custom", u"image_size"};
constexpr SchemaAllowedValues kSchemaAllowed66 = {kSchemaStrings135, std::size(kSchemaStrings135)};
constexpr SchemaString kSchemaStrings136[] = {u"portrait", u"landscape"};
constexpr SchemaAllowedValues kSchemaAllowed67 = {kSchemaStrings136, std::size(kSchemaStrings136)};
constexpr SchemaString kSchemaStrings137[] = {u"combined", u"per_page"};
constexpr SchemaAllowedValues kSchemaAllowed68 = {kSchemaStrings137, std::size(kSchemaStrings137)};
constexpr SchemaString kSchemaStrings138[] = {u"low", u"medium", u"high"};
constexpr SchemaAllowedValues kSchemaAllowed69 = {kSchemaStrings138, std::size(kSchemaStrings138)};
constexpr SchemaString kSchemaStrings139[] = {u"image_size", u"a4_portrait", u"a4_landscape"};
constexpr SchemaAllowedValues kSchemaAllowed70 = {kSchemaStrings139, std::size(kSchemaStrings139)};
constexpr SchemaString kSchemaStrings140[] = {u"auto", u"dxgi", u"wgc", u"gdi"};
constexpr SchemaAllowedValues kSchemaAllowed71 = {kSchemaStrings140, std::size(kSchemaStrings140)};
constexpr SchemaString kSchemaStrings141[] = {u"msaa", u"uia"};
constexpr SchemaAllowedValues kSchemaAllowed72 = {kSchemaStrings141, std::size(kSchemaStrings141)};
constexpr ConfigurationIntegerRange kSchemaRange24{CaptureHistoryPolicy::MinimumRetentionDays,
                                                   CaptureHistoryPolicy::MaximumRetentionDays, 1};
constexpr ConfigurationIntegerRange kSchemaRange25{CaptureHistoryPolicy::MinimumEntries,
                                                   CaptureHistoryPolicy::MaximumEntries, 1};
constexpr ConfigurationIntegerRange kSchemaRange26{CaptureHistoryPolicy::MinimumDiskMiB,
                                                   CaptureHistoryPolicy::MaximumDiskMiB, 1};

constexpr SchemaEntryDescriptor kEntryDescriptors[] = {
    {u"screenshot/stroke_color_presets", stringsDefault(kSchemaStrings0),
     ConfigurationValueKind::StringList},
    {u"screenshot/fill_color_presets", stringsDefault(kSchemaStrings1),
     ConfigurationValueKind::StringList},
    {u"screenshot/highlight_color_presets", stringsDefault(kHighlightColorPresets),
     ConfigurationValueKind::StringList},
    {u"cloud_upload/configuration", factoryDefault(SchemaDefaultFactory::CloudUpload),
     ConfigurationValueKind::Structured},
    {u"screenshot_shortcuts/upload_to_cloud", stringsDefault(kSchemaStrings2),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/upload_to_cloud", stringsDefault(kSchemaStrings2),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"floating_toolbar/enabled", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"floating_toolbar/opacity", integerDefault(50), ConfigurationValueKind::Integer,
     &kSchemaRange0},
    {u"floating_toolbar/hide_in_fullscreen", booleanDefault(true), ConfigurationValueKind::Boolean},
    {u"floating_toolbar/hide_during_capture", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"floating_toolbar/mode", stringDefault(u"icon"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed0},
    {u"floating_toolbar/placement", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"floating_toolbar/layout", factoryDefault(SchemaDefaultFactory::FloatingToolbar),
     ConfigurationValueKind::Structured},
#if SNOW_SHOT_ENABLE_API_CONFIGURATION
    {u"api_configuration/server_url", stringDefault({}), ConfigurationValueKind::String},
#endif
#if SNOW_SHOT_ENABLE_API_CONFIGURATION
    {u"api_configuration/text_translation", emptyArrayDefault(),
     ConfigurationValueKind::Structured},
#endif
#if SNOW_SHOT_ENABLE_API_CONFIGURATION
    {u"api_configuration/custom_models", emptyArrayDefault(), ConfigurationValueKind::Structured},
#endif
    {u"storage/schema_version", integerDefault(5), ConfigurationValueKind::Integer, &kSchemaRange1},
    {u"interface/theme_mode", stringDefault(u"system"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed1},
    {u"interface/theme_primary_color", stringDefault(u"#1677FFFF"), ConfigurationValueKind::String},
    {u"interface/app_font", stringDefault(u""), ConfigurationValueKind::String},
    {u"interface/app_font_size", integerDefault(14), ConfigurationValueKind::Integer,
     &kSchemaRange2},
    {u"interface/language", stringDefault(u"system"), ConfigurationValueKind::String},
    {u"interface/skin_path", stringDefault({}), ConfigurationValueKind::String},
    {u"interface/skin_position", stringDefault(u"center"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed2},
    {u"interface/toolbar_skin_path", stringDefault({}), ConfigurationValueKind::String},
    {u"interface/toolbar_skin_position", stringDefault(u"center"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed2},
    {u"interface/tray_menu_skin_path", stringDefault({}), ConfigurationValueKind::String},
    {u"interface/tray_menu_skin_position", stringDefault(u"center"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed2},
    {u"interface/skin_display_mode", stringDefault(u"overlay"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed3},
    {u"interface/skin_opacity", integerDefault(100), ConfigurationValueKind::Integer,
     &kSchemaRange3},
    {u"interface/skin_blur_level", integerDefault(0), ConfigurationValueKind::Integer,
     &kSchemaRange3},
    {u"interface/skin_mask_opacity", integerDefault(80), ConfigurationValueKind::Integer,
     &kSchemaRange3},
    {u"system/application_priority", stringDefault(u"above_normal"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed4},
    {u"system/application_qos", stringDefault(u"user_interactive"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed5},
    {u"system/memory_optimization_policy", stringDefault(u"smart_control"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed6},
    {u"system/auto_start_at_boot", booleanDefault(true), ConfigurationValueKind::Boolean},
    {u"system/context_menu_mode", stringDefault(u"image_files"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed7},
    {u"system/annotation_mode_when_opening_for_editing", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"system/launch_as_administrator", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"mcp/enabled", booleanDefault(false), ConfigurationValueKind::Boolean},
#ifdef Q_OS_MACOS
    {u"updates/mode", stringDefault(u"check"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed8},
#else
    {u"updates/mode", stringDefault(u"download"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed9},
#endif
    {u"network/proxy", stringDefault(u"none"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed10},
    {u"network/http_proxy", stringDefault({}), ConfigurationValueKind::String},
    {u"text_recognition/resident_process", booleanDefault(!app::edition::isMini),
     ConfigurationValueKind::Boolean},
    {u"text_recognition/model_hot_start", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"text_recognition/hardware_acceleration", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"text_recognition/model_type", stringDefault(u"small"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed11},
    {u"text_recognition/detector_resize_policy", stringDefault(u"max"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed12},
    {u"text_recognition/text_detection_processing", stringDefault(u"accuracy_first"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed13},
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    {u"screenshot_translation/source_language", stringDefault(u"auto"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed14},
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    {u"screenshot_translation/target_language", stringDefault({}), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed15},
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    {u"screenshot_translation/secondary_target_language", stringDefault(u"en"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed15},
    {u"screenshot_translation/language_detection_confidence_threshold", integerDefault(60),
     ConfigurationValueKind::Integer, &kSchemaRange4},
    {u"screenshot_translation/model", stringDefault(u"free-translation"),
     ConfigurationValueKind::String},
#endif
#if SNOW_SHOT_ENABLE_IMAGE_CONVERSION
    {u"screenshot_conversion/vision_model", stringDefault({}), ConfigurationValueKind::String},
#endif
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    {u"screenshot_table/model", stringDefault(u"dedicated"), ConfigurationValueKind::String},
#endif
#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    {u"screenshot_latex/model", stringDefault(u"dedicated"), ConfigurationValueKind::String},
#endif
#if SNOW_SHOT_ENABLE_EXTENDED_FEATURES
    {u"extended_features/standalone_translation_window", booleanDefault(false),
     ConfigurationValueKind::Boolean},
#endif
#if SNOW_SHOT_ENABLE_EXTENDED_FEATURES
    {u"extended_features/jump_to_translation_page", booleanDefault(false),
     ConfigurationValueKind::Boolean},
#endif
#if SNOW_SHOT_ENABLE_EXTENDED_FEATURES
    {u"extended_features/translation_page_enabled", booleanDefault(false),
     ConfigurationValueKind::Boolean},
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    {u"screenshot_translation/original_image_translation", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screenshot_translation/copy_translation_results_as_image", booleanDefault(false),
     ConfigurationValueKind::Boolean},
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    {u"screenshot_translation/layout_processing", stringDefault(u"smart_merge"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed16},
#endif
    {u"interface/sidebar_collapsed", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"interface/screenshot_history_page_size", integerDefault(10),
     ConfigurationValueKind::Integer},
    {u"interface/pinned_window_management_page_size", integerDefault(10),
     ConfigurationValueKind::Integer},
    {u"interface/main_window_geometry", emptyObjectDefault(), ConfigurationValueKind::Structured},
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    {u"interface/translation_window_geometry", emptyObjectDefault(),
     ConfigurationValueKind::Structured},
    {u"interface/translation_window_size", emptyObjectDefault(),
     ConfigurationValueKind::Structured},
#endif
    {u"text_recognition/save_recognition_result_as_image", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"text_recognition/show_original_image_preview", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"text_recognition/default_formatting", stringDefault(u"none"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed17},
    {u"text_recognition/default_punctuation", stringDefault(u"none"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed18},
    {u"text_recognition/fill_style", stringDefault(u"background_fill"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed19},
    {u"text_recognition/show_text_highlighting", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"global_shortcuts/screenshot", stringsDefault(kSchemaStrings20),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/screenshot_delay", emptyArrayDefault(), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
    {u"global_shortcuts/screenshot_fixed", emptyArrayDefault(), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
    {u"global_shortcuts/screenshot_ocr", emptyArrayDefault(), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
    {u"global_shortcuts/screenshot_ocr_copy", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    {u"global_shortcuts/screenshot_translation", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
#endif
    {u"global_shortcuts/screenshot_scrolling", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/screenshot_copy", stringsDefault(kSchemaStrings21),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/screenshot_save", emptyArrayDefault(), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
    {u"global_shortcuts/screenshot_quick_save", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/screenshot_full_screen", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/screenshot_focused_window", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/screen_record", emptyArrayDefault(), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
    {u"global_shortcuts/screen_record_copy", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/open_screen_recording_folder", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/open_capture_history", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/open_pin_to_screen_management", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/switch_window_group", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/global_canvas", emptyArrayDefault(), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
    {u"global_shortcuts/stitch_selected_images", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/split_selected_images", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/edit_selected_images", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/open_settings", emptyArrayDefault(), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    {u"global_shortcuts/translate_selected_text", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
#endif
#ifdef Q_OS_WIN
    {u"global_shortcuts/toggle_window_under_cursor_always_on_top", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
#endif
    {u"global_shortcuts/pin_clipboard_content", stringsDefault(kSchemaStrings22),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/pin_selected_files", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/restore_last_closed_windows", stringsDefault(kSchemaStrings23),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/hide_show_all_windows", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/toggle_global_hotkeys", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/toggle_disable_on_focused_fullscreen_window", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"global_shortcuts/disable_on_focused_fullscreen_window", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"global_shortcuts/fullscreen_window_filter", stringDefault(u"game_windows"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed20},

#ifdef Q_OS_MACOS
    {u"global_mouse/screenshot_copy", emptyObjectDefault(), ConfigurationValueKind::Structured},
#else
    {u"global_mouse/screenshot_copy", objectDefault(kSchemaProperties0),
     ConfigurationValueKind::Structured},
#endif

#ifdef Q_OS_MACOS
    {u"global_mouse/screenshot_fixed", emptyObjectDefault(), ConfigurationValueKind::Structured},
#else
    {u"global_mouse/screenshot_fixed", objectDefault(kSchemaProperties1),
     ConfigurationValueKind::Structured},
#endif

#ifdef Q_OS_MACOS
    {u"global_mouse/screenshot_ocr", emptyObjectDefault(), ConfigurationValueKind::Structured},
#else
    {u"global_mouse/screenshot_ocr", objectDefault(kSchemaProperties2),
     ConfigurationValueKind::Structured},
#endif

#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    {u"global_mouse/screenshot_translation", emptyObjectDefault(),
     ConfigurationValueKind::Structured},
#endif
    {u"global_mouse/screenshot_ocr_copy", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"global_mouse/screenshot_quick_save", emptyObjectDefault(),
     ConfigurationValueKind::Structured},
    {u"global_mouse/screenshot_save", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"global_mouse/screen_recording", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"screen_recording/area_history", emptyArrayDefault(), ConfigurationValueKind::Structured},
    {u"screen_recording/enable_microphone", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"screen_recording/enable_system_audio", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/microphone_gain_db", integerDefault(0), ConfigurationValueKind::Integer,
     &kSchemaRange5},
    {u"screen_recording/system_audio_gain_db", integerDefault(0), ConfigurationValueKind::Integer,
     &kSchemaRange5},
    {u"screen_recording/separate_audio_tracks", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/clarity", stringDefault(u"1080p"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed21},
    {u"screen_recording/frame_rate", integerDefault(30), ConfigurationValueKind::Integer},
    {u"screen_recording/animated_image_clarity", stringDefault(u"720p"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed22},
    {u"screen_recording/animated_image_frame_rate", integerDefault(10),
     ConfigurationValueKind::Integer},
    {u"screen_recording/loop_animated_images", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/output_format", stringDefault(u"mp4"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed23},
    {u"screen_recording/mouse_trail_color", stringDefault(u"#00000000"),
     ConfigurationValueKind::String},
    {u"screen_recording/mouse_click_color", stringDefault(u"#00000000"),
     ConfigurationValueKind::String},
    {u"screen_recording/mouse_trail_duration_ms", integerDefault(500),
     ConfigurationValueKind::Integer, &kSchemaRange6},
    {u"screen_recording/keyboard_size", integerDefault(64), ConfigurationValueKind::Integer,
     &kSchemaRange7},
    {u"screen_recording/keyboard_background_color", stringDefault(u"#000000CC"),
     ConfigurationValueKind::String},
    {u"screen_recording/keyboard_foreground_color", stringDefault(u"#FFFFFFFF"),
     ConfigurationValueKind::String},
    {u"screen_recording/show_keyboard", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"screen_recording/record_number_keys", booleanDefault(true), ConfigurationValueKind::Boolean},
    {u"screen_recording/record_letters_and_symbols", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/record_control_keys", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/preserve_keyboard_shortcuts", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/follow_mouse", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"screen_recording/follow_mouse_smooth_movement", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/follow_mouse_reduce_jitter", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/enable_webcam", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"screen_recording/webcam_orientation", integerDefault(0), ConfigurationValueKind::Integer,
     &kSchemaRange8},
    {u"screen_recording/webcam_size", integerDefault(150), ConfigurationValueKind::Integer,
     &kSchemaRange9},
    {u"screen_recording/webcam_offset_left", integerDefault(48), ConfigurationValueKind::Integer,
     &kSchemaRange10},
    {u"screen_recording/webcam_offset_bottom", integerDefault(48), ConfigurationValueKind::Integer,
     &kSchemaRange10},
    {u"screen_recording/webcam_shape", stringDefault(u"square"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed24},
    {u"screen_recording/post_processing_enabled", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/post_processing_effect", stringDefault(u"progress_bar"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed25},
    {u"screen_recording/progress_bar_color", stringDefault(u"#1677FFFF"),
     ConfigurationValueKind::String},
    {u"screen_recording/show_cursor", booleanDefault(true), ConfigurationValueKind::Boolean},
    {u"screen_recording/mouse_highlight_enabled", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/record_mouse_clicks", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/record_mouse_scrolling", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/mouse_highlight_color", stringDefault(u"#FFFF0080"),
     ConfigurationValueKind::String},
    {u"screen_recording/mouse_highlight_radius", integerDefault(41),
     ConfigurationValueKind::Integer, &kSchemaRange11},
    {u"screen_recording/encoder", stringDefault(u"h264"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed26},
    {u"screen_recording/video_quality", integerDefault(80), ConfigurationValueKind::Integer,
     &kSchemaRange3},
    {u"screen_recording/encoding_preset", stringDefault(u"veryfast"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed27},

#ifdef Q_OS_WIN
    {u"screen_recording/api_mode", stringDefault(u"wgc"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed28},
#else
    {u"screen_recording/api_mode", stringDefault(u"dxgi"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed28},
#endif

    {u"screen_recording/capture_toolbar_in_recording", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/auto_hide_toolbar", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"screen_recording/show_status_prompts", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/auto_exit_after_recording_ends", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/notify_after_export_completes", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/delete_files_on_recording_close", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screen_recording/start_delay_seconds", integerDefault(0), ConfigurationValueKind::Integer,
     &kSchemaRange12},
    {u"screen_recording/video_save_directory",
     factoryDefault(SchemaDefaultFactory::MoviesDirectory), ConfigurationValueKind::String},
    {u"screen_recording/video_filename_format",
     stringDefault(u"SnowShot_Video_{YYYY-MM-DD_HH-mm-ss}"), ConfigurationValueKind::String},
    {u"drawing/automatically_select_newly_created_elements", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"drawing/automatic_selection_excluded_tools", stringsDefault(kSchemaStrings33),
     ConfigurationValueKind::StringList, nullptr, &kSchemaAllowed29},
    {u"drawing/quick_selection_disabled_tools", stringsDefault(kSchemaStrings35),
     ConfigurationValueKind::StringList, nullptr, &kSchemaAllowed30},
    {u"drawing/delete_text_after_cursor", booleanDefault(true), ConfigurationValueKind::Boolean},
    {u"drawing/remember_last_used_tool", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"drawing/sync_tool_colors", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"drawing/shared_tool_color", stringDefault(u""), ConfigurationValueKind::String},
    {u"drawing/always_show_first_toolbar_group_button", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"drawing/shape_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/speech_bubble_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/arrow_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/distance_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/angle_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/bracket_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/magnifier_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/line_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/free_draw_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/rectangle_highlight_style", emptyObjectDefault(),
     ConfigurationValueKind::Structured},
    {u"drawing/pen_highlight_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/rectangle_filter_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/pen_filter_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/brush_eraser_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/text_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/serial_number_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/watermark_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/watermark_content", objectDefault(kSchemaProperties3),
     ConfigurationValueKind::Structured},
    {u"drawing/spotlight_style", emptyObjectDefault(), ConfigurationValueKind::Structured},
    {u"drawing/watermark_templates", emptyArrayDefault(), ConfigurationValueKind::Structured},
    {u"drawing/draw_templates", emptyArrayDefault(), ConfigurationValueKind::Structured},
    {u"drawing_shortcuts/select", stringsDefault(kSchemaStrings37),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"drawing_shortcuts/shape", stringsDefault(kSchemaStrings38),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"drawing_shortcuts/speech_bubble", emptyArrayDefault(), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
    {u"drawing_shortcuts/arrow", stringsDefault(kSchemaStrings39),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"drawing_shortcuts/distance", emptyArrayDefault(), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
    {u"drawing_shortcuts/angle", emptyArrayDefault(), ConfigurationValueKind::StringList, nullptr,
     nullptr, 2},
    {u"drawing_shortcuts/bracket", emptyArrayDefault(), ConfigurationValueKind::StringList, nullptr,
     nullptr, 2},
    {u"drawing_shortcuts/line", emptyArrayDefault(), ConfigurationValueKind::StringList, nullptr,
     nullptr, 2},
    {u"drawing_shortcuts/magnifier", emptyArrayDefault(), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
    {u"drawing_shortcuts/spotlight", emptyArrayDefault(), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
    {u"drawing_shortcuts/brush", stringsDefault(kSchemaStrings40),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"drawing_shortcuts/highlight", stringsDefault(kSchemaStrings41),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"drawing_shortcuts/text", stringsDefault(kSchemaStrings42),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"drawing_shortcuts/serial_number", stringsDefault(kSchemaStrings43),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"drawing_shortcuts/filter", stringsDefault(kSchemaStrings44),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"drawing_shortcuts/eraser", stringsDefault(kSchemaStrings45),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"drawing_shortcuts/reset_canvas", stringsDefault(kSchemaStrings46),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"drawing_shortcuts/watermark", stringsDefault(kSchemaStrings47),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/move_tool", stringsDefault(kSchemaStrings48),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/move_cursor_up", stringsDefault(kSchemaStrings49),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/move_cursor_down", stringsDefault(kSchemaStrings50),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/move_cursor_left", stringsDefault(kSchemaStrings51),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/move_cursor_right", stringsDefault(kSchemaStrings52),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/move_entire_selection", stringsDefault(kSchemaStrings53),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/keep_selection_width_and_height_consistent",
     stringsDefault(kSchemaStrings54), ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/selection_aspect_ratio_snap", stringsDefault(kSchemaStrings55),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/switch_selection_between_window_and_window_sub_element",
     stringsDefault(kSchemaStrings56), ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/next_selection_type",
     factoryDefault(SchemaDefaultFactory::NextSelectionType), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
    {u"screenshot_shortcuts/previous_selection_type",
     factoryDefault(SchemaDefaultFactory::PreviousSelectionType),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/previous_screenshot_history", stringsDefault(kSchemaStrings57),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/next_screenshot_history", stringsDefault(kSchemaStrings58),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/select_previously_selected_area", stringsDefault(kSchemaStrings59),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/select_next_previously_selected_area", stringsDefault(kSchemaStrings60),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/duplicate_latest_region", stringsDefault(kSchemaStrings59),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/toggle_cursor_visibility", stringsDefault(kSchemaStrings61),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/recapture", stringsDefault(kSchemaStrings62),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/toggle_coordinate_mode", stringsDefault(kSchemaStrings63),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/toggle_guides", stringsDefault(kSchemaStrings64),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/copy_color", stringsDefault(kSchemaStrings65),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    {u"screenshot_shortcuts/table_recognition", stringsDefault(kSchemaStrings66),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
#endif
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    {u"screenshot_shortcuts/qr_code_recognition", stringsDefault(kSchemaStrings67),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
#endif
    {u"screenshot_shortcuts/video_recording", stringsDefault(kSchemaStrings68),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/text_recognition", stringsDefault(kSchemaStrings69),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    {u"screenshot_shortcuts/text_translation", stringsDefault(kSchemaStrings70),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
#endif
    {u"screenshot_shortcuts/scrolling_screenshot", stringsDefault(kSchemaStrings71),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/quick_save", stringsDefault(kSchemaStrings72),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/print", stringsDefault(kSchemaStrings73),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/save_as_file", stringsDefault(kSchemaStrings74),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/pin_to_screen", stringsDefault(kSchemaStrings75),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/pin_window_to_screen", stringsDefault(kSchemaStrings76),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/cancel_screenshot", stringsDefault(kSchemaStrings77),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/copy_to_clipboard", stringsDefault(kSchemaStrings78),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/undo", stringsDefault(kSchemaStrings79),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_shortcuts/redo", stringsDefault(kSchemaStrings80),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"context_menu_shortcuts/edit_with_snow_shot", emptyArrayDefault(),
     ConfigurationValueKind::ShortcutList, nullptr, nullptr, 1},
    {u"screen_recording_shortcuts/previous_recording_area", stringsDefault(kSchemaStrings59),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screen_recording_shortcuts/next_recording_area", stringsDefault(kSchemaStrings60),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screen_recording_shortcuts/export", stringsDefault(kSchemaStrings81),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screen_recording_shortcuts/toggle_recording", stringsDefault(kSchemaStrings74),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screen_recording_shortcuts/copy_to_clipboard", stringsDefault(kSchemaStrings82),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screen_recording_shortcuts/end_recording", stringsDefault(kSchemaStrings77),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/copy_to_clipboard", stringsDefault(kSchemaStrings82),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/copy_original_content", stringsDefault(kSchemaStrings83),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/print", stringsDefault(kSchemaStrings73),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/batch_select_all", stringsDefault(kSchemaStrings84),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/batch_image_stitching", stringsDefault(kSchemaStrings85),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/batch_copy_files_to_clipboard", stringsDefault(kSchemaStrings86),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/batch_save_as_file", stringsDefault(kSchemaStrings87),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/save_as_file", stringsDefault(kSchemaStrings74),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/show_text_recognition_results", stringsDefault(kSchemaStrings69),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/drawing_mode", stringsDefault(kSchemaStrings53),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/toggle_playback", stringsDefault(kSchemaStrings53),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/next_frame", emptyArrayDefault(), ConfigurationValueKind::StringList,
     nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/previous_frame", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/decrease_playback_speed", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/increase_playback_speed", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/reset_playback_speed", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/resize_window", stringsDefault(kSchemaStrings88),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/hide_to_top", stringsDefault(kSchemaStrings89),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/toggle_click_through", stringsDefault(kSchemaStrings90),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/toggle_lock", stringsDefault(kSchemaStrings71),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/always_on_top", stringsDefault(kSchemaStrings91),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/show_border", stringsDefault(kSchemaStrings92),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/show_shadow", stringsDefault(kSchemaStrings93),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/thumbnail_mode", stringsDefault(kSchemaStrings59),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/close_window", stringsDefault(kSchemaStrings77),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/destroy_window", stringsDefault(kSchemaStrings94),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/increase_opacity", stringsDefault(kSchemaStrings95),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/decrease_opacity", stringsDefault(kSchemaStrings96),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/increase_scale", stringsDefault(kSchemaStrings58),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/decrease_scale", stringsDefault(kSchemaStrings57),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/rotate_clockwise", stringsDefault(kSchemaStrings38),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/rotate_counterclockwise", stringsDefault(kSchemaStrings39),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/flip_horizontal", stringsDefault(kSchemaStrings97),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/flip_vertical", stringsDefault(kSchemaStrings98),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/reset_transform", stringsDefault(kSchemaStrings99),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/move_cursor_up", stringsDefault(kSchemaStrings49),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/move_cursor_down", stringsDefault(kSchemaStrings50),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/move_cursor_left", stringsDefault(kSchemaStrings51),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"pin_to_screen_shortcuts/move_cursor_right", stringsDefault(kSchemaStrings52),
     ConfigurationValueKind::StringList, nullptr, nullptr, 2},
    {u"screenshot_toolbar/arrow_line_tool", stringDefault(u"arrow"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed31},
    {u"screenshot_toolbar/highlight_tool", stringDefault(u"pen_highlight"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed32},
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_QR_RECOGNITION
    {u"screenshot_toolbar/table_qr_tool", stringDefault(u"table"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed33},
#endif
    {u"screenshot_toolbar/last_filter_tool", stringDefault(u"pen-filter"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed34},
    {u"screenshot_toolbar/last_eraser_tool", stringDefault(u"eraser"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed35},
    {u"screenshot_toolbar/last_highlight_tool", stringDefault(u"pen-highlight"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed36},
    {u"screenshot_toolbar/last_drawing_tool", stringDefault({}), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed37},
    {u"screenshot_toolbar/layout", factoryDefault(SchemaDefaultFactory::DrawingToolbar),
     ConfigurationValueKind::Structured},
    {u"pin_to_screen/action_tools_layout", factoryDefault(SchemaDefaultFactory::PinnedToolbar),
     ConfigurationValueKind::Structured},
    {u"screenshot_toolbar/action_tools_layout", factoryDefault(SchemaDefaultFactory::ActionToolbar),
     ConfigurationValueKind::Structured},
    {u"screen_recording/action_tools_layout",
     factoryDefault(SchemaDefaultFactory::RecordingToolbar), ConfigurationValueKind::Structured},
    {u"screenshot_ui/toolbar_size", stringDefault(u"normal"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed38},
    {u"screenshot_ui/selection_transition_animation",
     integerDefault(static_cast<int>(SelectionTransitionAnimation::Soft)),
     ConfigurationValueKind::Integer, &kSchemaRange13},

#ifdef Q_OS_MACOS
    {u"screenshot_ui/selection_display_unit", stringDefault(u"logical_pixels"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed39},
#else
    {u"screenshot_ui/selection_display_unit", stringDefault(u"physical_pixels"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed39},
#endif

    {u"screenshot_ui/selection_toolbar_hidden", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screenshot_ui/color_picker_display_mode", stringDefault(u"hide_outside_selection"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed40},
    {u"screenshot_ui/color_picker_coordinate_mode", stringDefault(u"global"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed41},
    {u"screenshot_ui/color_picker_format", stringDefault(u"hex"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed42},
    {u"screenshot_ui/selection_border_color", stringDefault(u"#4096FFFF"),
     ConfigurationValueKind::String},
    {u"screenshot_ui/selection_mask_color", stringDefault(u"#00000080"),
     ConfigurationValueKind::String},
    {u"screenshot_ui/shortcut_hint_opacity", integerDefault(100), ConfigurationValueKind::Integer,
     &kSchemaRange3},
    {u"screenshot_ui/area_type_hint_enabled", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screenshot_ui/show_guides_by_default", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screenshot_ui/cursor_guide_line_color", stringDefault(u"#000000FF"),
     ConfigurationValueKind::String},
    {u"screenshot_ui/selection_center_guide_line_color", stringDefault(u"#4096FFFF"),
     ConfigurationValueKind::String},
    {u"screenshot_ui/monitor_center_guide_line_color", stringDefault(u"#FF0000FF"),
     ConfigurationValueKind::String},
    {u"screenshot_ui/color_picker_center_guide_line_color", stringDefault(u"#00000000"),
     ConfigurationValueKind::String},
    {u"screenshot_ui/show_edit_selection_toolbar", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"pin_to_screen/text_maximum_width", integerDefault(1024), ConfigurationValueKind::Integer,
     &kSchemaRange14},
    {u"pin_to_screen/text_font_family", stringDefault({}), ConfigurationValueKind::String},
    {u"pin_to_screen/text_font_size", integerDefault(16), ConfigurationValueKind::Integer,
     &kSchemaRange15},
    {u"pin_to_screen/text_color", stringDefault(u"system"), ConfigurationValueKind::String},
    {u"pin_to_screen/text_background_color", stringDefault(u"system"),
     ConfigurationValueKind::String},
    {u"pin_to_screen/show_border_by_default", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"pin_to_screen/show_shadow_by_default", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"pin_to_screen/shadow_color", stringDefault(u"#BFBFBFFF"), ConfigurationValueKind::String},
    {u"pin_to_screen/shadow_active_color", stringDefault(u"#69B1FFFF"),
     ConfigurationValueKind::String},
    {u"pin_to_screen/locked_shadow_color", stringDefault(u"#FAAD14FF"),
     ConfigurationValueKind::String},
    {u"pin_to_screen/border_color", stringDefault(u"#DBDBDBFF"), ConfigurationValueKind::String},
    {u"pin_to_screen/border_active_color", stringDefault(u"#69B1FFFF"),
     ConfigurationValueKind::String},
    {u"pin_to_screen/locked_border_color", stringDefault(u"#FAAD14FF"),
     ConfigurationValueKind::String},
    {u"pin_to_screen/mouse_wheel_zoom_mode", stringDefault(u"mouse_position"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed43},
    {u"pin_to_screen/mouse_wheel_opacity_step", integerDefault(5), ConfigurationValueKind::Integer,
     &kSchemaRange16},
    {u"pin_to_screen/mouse_wheel_zoom_step", integerDefault(10), ConfigurationValueKind::Integer,
     &kSchemaRange16},
    {u"pin_to_screen/double_click_action", stringDefault(u"thumbnail_mode"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed44},
    {u"pin_to_screen/middle_mouse_button_action", stringDefault(u"reset_zoom"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed44},
    {u"pin_to_screen/show_window_buttons", booleanDefault(true), ConfigurationValueKind::Boolean},
    {u"pin_to_screen/shake_window_to_hide_others", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"pin_to_screen/exit_annotation_mode_before_closing", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"pin_to_screen/confirm_before_closing_window", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"pin_to_screen/confirm_before_destroying_window", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"pin_to_screen/automatic_text_recognition", booleanDefault(!app::edition::isMini),
     ConfigurationValueKind::Boolean},
    {u"pin_to_screen/text_selection_on_recognition_results", stringDefault(u"always"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed45},
    {u"pin_to_screen/quick_annotation_for_selected_text", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"pin_to_screen/quick_text_annotation_styles", emptyObjectDefault(),
     ConfigurationValueKind::Structured},
    {u"pin_to_screen/duplicate_content_action", stringDefault(u"shake_window"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed46},
    {u"pin_to_screen/auto_resize_window", booleanDefault(true), ConfigurationValueKind::Boolean},
    {u"tray/enabled", booleanDefault(true), ConfigurationValueKind::Boolean},
    {u"tray/use_system_icon_color", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"tray/icon", stringDefault(u"default"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed47},
    {u"tray/custom_icon", stringDefault({}), ConfigurationValueKind::String},
    {u"tray/dark_icon", stringDefault(u"inherit"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed48},
    {u"tray/dark_custom_icon", stringDefault({}), ConfigurationValueKind::String},
    {u"tray/left_click_action", stringDefault(u"screenshot"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed49},
    {u"tray/middle_click_action", stringDefault(u"screenshot_fixed"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed49},

#ifdef Q_OS_WIN
    {u"tray/menu_options", stringsDefault(kSchemaStrings118), ConfigurationValueKind::StringList,
     nullptr, &kSchemaAllowed50},
#else
    {u"tray/menu_options", stringsDefault(kSchemaStrings118), ConfigurationValueKind::StringList,
     nullptr, &kSchemaAllowed51},
#endif

    {u"screenshot_selection/region_type", stringDefault(u"rectangle"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed52},
    {u"screenshot_selection/smart_selection", stringDefault(u"window_sub_element"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed53},
    {u"screenshot_selection/background_opacity_percent", integerDefault(0),
     ConfigurationValueKind::Integer, &kSchemaRange3},
    {u"screenshot_selection/layout_compaction_enabled", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screenshot_selection/layout_compaction_spacing", integerDefault(0),
     ConfigurationValueKind::Integer, &kSchemaRange17},
    {u"screenshot_selection/corner_radius", integerDefault(0), ConfigurationValueKind::Integer,
     &kSchemaRange18},
    {u"screenshot_selection/border_type", stringDefault(u"shadow"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed54},
    {u"screenshot_selection/border_width", integerDefault(0), ConfigurationValueKind::Integer,
     &kSchemaRange19},
    {u"screenshot_selection/border_color", stringDefault(u"#FFFFFFFF"),
     ConfigurationValueKind::String},
    {u"screenshot_selection/shadow_width", integerDefault(0), ConfigurationValueKind::Integer,
     &kSchemaRange19},
    {u"screenshot_selection/shadow_color", stringDefault(u"#333333FF"),
     ConfigurationValueKind::String},
    {u"screenshot_selection/aspect_ratio", stringDefault(u"free"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed55},
    {u"screenshot_selection/lock_aspect_ratio", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screenshot/scrolling_auto_scroll_interval_ms", integerDefault(200),
     ConfigurationValueKind::Integer, &kSchemaRange20},
    {u"screenshot/capture_cursor", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"screenshot/capture_ui_in_scrolling_screenshot", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screenshot/shutter_sound_notification", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screenshot/screenshot_sound_notification", booleanDefault(false),
     ConfigurationValueKind::Boolean},
#if SNOW_SHOT_ENABLE_QR_RECOGNITION
    {u"screenshot/auto_recognize_qr_code", booleanDefault(true), ConfigurationValueKind::Boolean},
#endif
    {u"screenshot/confirm_before_exiting_via_shortcut", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screenshot/restore_original_screen_colors", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screenshot/delay_seconds", integerDefault(3), ConfigurationValueKind::Integer,
     &kSchemaRange21},
    {u"screenshot/auto_execute_after_text_recognition", stringDefault(u"no_action"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed56},
    {u"screenshot/double_click_action", stringDefault(u"copy"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed57},
    {u"screenshot/middle_mouse_button_action", stringDefault(u"pin"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed57},
    {u"screenshot/quick_selection_modification", booleanDefault(true),
     ConfigurationValueKind::Boolean},
    {u"screenshot/hidpi_image_scaling", stringDefault(u"disabled"), ConfigurationValueKind::String,
     nullptr, &kHiDpiImageScalingAllowed},
    {u"screenshot/selection_resize_mode", stringDefault(u"follow_mouse_movement"),
     ConfigurationValueKind::String, nullptr, &kSchemaAllowed58},
    {u"screenshot/auto_save_after_execution_actions", emptyArrayDefault(),
     ConfigurationValueKind::StringList, nullptr, &kSchemaAllowed59},
    {u"screenshot/copy_image_file_to_clipboard", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screenshot/copy_after_pinning_to_screen", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screenshot/keep_annotations_editable_after_pinning_to_screen", booleanDefault(false),
     ConfigurationValueKind::Boolean},
    {u"screenshot/show_save_result", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"screenshot/image_save_directory", factoryDefault(SchemaDefaultFactory::PicturesDirectory),
     ConfigurationValueKind::String},
    {u"screenshot/last_manual_save_directory", stringDefault({}), ConfigurationValueKind::String},
    {u"screenshot/last_manual_save_format", stringDefault(u"png"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed60},
    {u"screenshot/save_as_file_dialog", stringDefault(u"system"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed61},
    {u"screenshot/save_path_shortcuts", emptyArrayDefault(), ConfigurationValueKind::Structured},
    {u"collage/direction", stringDefault(u"vertical"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed62},
    {u"collage/rows", integerDefault(1), ConfigurationValueKind::Integer, &kSchemaRange22},
    {u"collage/columns", integerDefault(1), ConfigurationValueKind::Integer, &kSchemaRange22},
    {u"collage/custom_grid_initialized", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"collage/sizing_mode", stringDefault(u"uniform"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed63},
    {u"collage/border_px", integerDefault(0), ConfigurationValueKind::Integer, &kSchemaRange23},
    {u"collage/spacing_px", integerDefault(0), ConfigurationValueKind::Integer, &kSchemaRange23},
    {u"collage/background_color", stringDefault(u"#FFFFFFFF"), ConfigurationValueKind::String},
    {u"collage/pdf_stitching_method", stringDefault(u"page_based"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed64},
    {u"collage/pdf_template_type", stringDefault(u"two_images"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed65},
    {u"collage/pdf_image_spacing_mm", numberDefault(5.0), ConfigurationValueKind::Structured},
    {u"collage/pdf_paper_size", stringDefault(u"a4"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed66},
    {u"collage/pdf_orientation", stringDefault(u"portrait"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed67},
    // Decimal dimensions and margins use key-specific finite-number normalization below.
    {u"collage/pdf_custom_width_mm", numberDefault(210.0), ConfigurationValueKind::Structured},
    {u"collage/pdf_custom_height_mm", numberDefault(297.0), ConfigurationValueKind::Structured},
    {u"collage/pdf_margin_top_mm", numberDefault(10.0), ConfigurationValueKind::Structured},
    {u"collage/pdf_margin_bottom_mm", numberDefault(10.0), ConfigurationValueKind::Structured},
    {u"collage/pdf_margin_left_mm", numberDefault(10.0), ConfigurationValueKind::Structured},
    {u"collage/pdf_margin_right_mm", numberDefault(10.0), ConfigurationValueKind::Structured},
    {u"collage/pdf_export_mode", stringDefault(u"combined"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed68},
    {u"collage/pdf_image_quality", integerDefault(100), ConfigurationValueKind::Integer,
     &kSchemaRange3},
    {u"screenshot/manual_save_format_options", emptyObjectDefault(),
     ConfigurationValueKind::Structured},
    {u"screenshot/image_format", stringDefault(u"png"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed60},
    {u"screenshot/compression_level", stringDefault(u"medium"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed69},
    {u"screenshot/image_quality", integerDefault(100), ConfigurationValueKind::Integer,
     &kSchemaRange3},
    {u"screenshot/pdf_page_size", stringDefault(u"a4_portrait"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed70},
    {u"screenshot/api_mode", stringDefault(u"auto"), ConfigurationValueKind::String, nullptr,
     &kSchemaAllowed71},
    {u"screenshot/window_element_api", stringDefault(u"uia"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed72},
    {u"screenshot/manual_save_filename_format", stringDefault(u"SnowShot_{YYYY-MM-DD_HH-mm-ss}"),
     ConfigurationValueKind::String},
    {u"screenshot/auto_save_filename_format", stringDefault(u"SnowShot_{YYYY-MM-DD_HH-mm-ss}"),
     ConfigurationValueKind::String},
    {u"screenshot_selection/selection_rect_presets", emptyArrayDefault(),
     ConfigurationValueKind::Structured},
    {u"capture_history/enabled", booleanDefault(true), ConfigurationValueKind::Boolean},
    {u"capture_history/keep_permanently", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"capture_history/compression_level", stringDefault(u"medium"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed69},
    {u"capture_history/retention_days", integerDefault(7), ConfigurationValueKind::Integer,
     &kSchemaRange24},
    {u"capture_history/max_entries", integerDefault(100), ConfigurationValueKind::Integer,
     &kSchemaRange25},
    {u"capture_history/max_disk_mib", integerDefault(1024), ConfigurationValueKind::Integer,
     &kSchemaRange26},
    {u"pinned_history/enabled", booleanDefault(true), ConfigurationValueKind::Boolean},
    {u"pinned_history/keep_permanently", booleanDefault(false), ConfigurationValueKind::Boolean},
    {u"pinned_history/compression_level", stringDefault(u"medium"), ConfigurationValueKind::String,
     nullptr, &kSchemaAllowed69},
    {u"pinned_history/retention_days", integerDefault(7), ConfigurationValueKind::Integer,
     &kSchemaRange24},
    {u"pinned_history/max_entries", integerDefault(100), ConfigurationValueKind::Integer,
     &kSchemaRange25},
    {u"pinned_history/max_disk_mib", integerDefault(1024), ConfigurationValueKind::Integer,
     &kSchemaRange26},
};

QJsonValue materializeFactory(SchemaDefaultFactory factory) {
    switch (factory) {
    case SchemaDefaultFactory::CloudUpload:
        return cloudUploadSettingsToJson({});
    case SchemaDefaultFactory::FloatingToolbar:
        return QJsonObject{
            {QStringLiteral("positions"), jsonArray(defaultFloatingToolbarLayout().positions)},
            {QStringLiteral("hidden"), jsonArray(defaultFloatingToolbarLayout().hidden)}};
    case SchemaDefaultFactory::MoviesDirectory:
        return defaultOutputDirectory(QStandardPaths::MoviesLocation);
    case SchemaDefaultFactory::PicturesDirectory:
        return defaultOutputDirectory(QStandardPaths::PicturesLocation);
    case SchemaDefaultFactory::DrawingToolbar:
        return defaultToolbarLayout(defaultDrawingToolbarPositions());
    case SchemaDefaultFactory::PinnedToolbar:
        return defaultToolbarLayout(defaultPinnedActionToolbarPositions(), true);
    case SchemaDefaultFactory::ActionToolbar:
        return defaultToolbarLayout(defaultActionToolbarPositions(), true);
    case SchemaDefaultFactory::RecordingToolbar:
        return defaultToolbarLayout(defaultRecordingActionToolbarPositions());
    case SchemaDefaultFactory::NextSelectionType:
        return QJsonArray{
            QKeySequence(screenshotRegionTypeCycleKey()).toString(QKeySequence::PortableText)};
    case SchemaDefaultFactory::PreviousSelectionType:
        return QJsonArray{
            QKeySequence(screenshotRegionTypeCycleKey(true)).toString(QKeySequence::PortableText)};
    }
    Q_UNREACHABLE();
}

Q_NEVER_INLINE QJsonValue materializeDefault(const SchemaEntryDescriptor& entry) {
    switch (entry.defaultKind) {
    case SchemaDefaultKind::Boolean:
        return entry.payload.boolean;
    case SchemaDefaultKind::Integer:
        return entry.payload.integer;
    case SchemaDefaultKind::Number:
        return entry.payload.number;
    case SchemaDefaultKind::String:
        return schemaString({entry.payload.string, entry.defaultSize});
    case SchemaDefaultKind::EmptyArray:
        return QJsonArray();
    case SchemaDefaultKind::EmptyObject:
        return QJsonObject();
    case SchemaDefaultKind::StringArray: {
        QJsonArray result;
        for (quint16 i = 0; i < entry.defaultSize; ++i)
            result.push_back(schemaString(entry.payload.strings[i]));
        return result;
    }
    case SchemaDefaultKind::StringObject: {
        QJsonObject result;
        for (quint16 i = 0; i < entry.defaultSize; ++i) {
            const auto& property = entry.payload.properties[i];
            result.insert(schemaString(property.key), schemaString(property.value));
        }
        return result;
    }
    case SchemaDefaultKind::Factory:
        return materializeFactory(entry.payload.factory);
    }
    Q_UNREACHABLE();
}

Q_NEVER_INLINE QStringList materializeAllowed(const SchemaAllowedValues* allowed) {
    if (allowed == nullptr)
        return {};
    switch (allowed->provider) {
    case SchemaAllowedProvider::Literal:
        return schemaStrings(allowed->values, allowed->size);
    case SchemaAllowedProvider::LastDrawingTools:
        return kLastDrawingToolIds;
    case SchemaAllowedProvider::SkinPositions:
        return kSkinPositions;
    case SchemaAllowedProvider::OcrFormatting:
        return ocrTextOptionValues(ocrFormattingOptions());
    case SchemaAllowedProvider::OcrPunctuation:
        return ocrTextOptionValues(ocrPunctuationOptions());
    }
    Q_UNREACHABLE();
}

Q_NEVER_INLINE ConfigurationSchemaEntry materializeEntry(const SchemaEntryDescriptor& entry) {
    return {schemaString({entry.key, entry.keySize}),
            materializeDefault(entry),
            static_cast<ConfigurationValueKind>(entry.valueKind),
            entry.integerRange ? std::optional(*entry.integerRange) : std::nullopt,
            materializeAllowed(entry.allowed),
            entry.maximumListItems};
}

bool shortcutConfigurationKey(const QString& key) {
    return key.startsWith(QStringLiteral("global_shortcuts/")) ||
           key.startsWith(QStringLiteral("drawing_shortcuts/")) ||
           key.startsWith(QStringLiteral("screenshot_shortcuts/")) ||
           key.startsWith(QStringLiteral("screen_recording_shortcuts/")) ||
           key.startsWith(QStringLiteral("pin_to_screen_shortcuts/"));
}

QJsonArray shortcutDefaults(const QString& key, const QJsonValue& value) {
    return snow_shot::shortcuts::shortcutBindingsToJson(
        snow_shot::shortcuts::shortcutBindingsFromJson(
            value, true, -1, nullptr, nullptr,
            key == QStringLiteral("screenshot_shortcuts/toggle_guides"),
            key.startsWith(QStringLiteral("global_shortcuts/"))
                ? snow_shot::shortcuts::ShortcutPolicy::Native
                : snow_shot::shortcuts::ShortcutPolicy::Application));
}

#ifdef Q_OS_MACOS
QJsonArray macGlobalShortcutDefault(const QString& portableText, quint32 virtualKey) {
    snow_shot::shortcuts::ShortcutBinding binding{portableText};
    binding.physicalKeys.insert(snow_shot::shortcuts::ShortcutPlatform::MacOS, virtualKey);
    return snow_shot::shortcuts::shortcutBindingsToJson({binding});
}
#endif

QVector<ConfigurationSchemaEntry> buildEntries() {
    QVector<ConfigurationSchemaEntry> result;
    result.reserve(static_cast<qsizetype>(std::size(kEntryDescriptors)));
    for (const SchemaEntryDescriptor& descriptor : kEntryDescriptors)
        result.push_back(materializeEntry(descriptor));
    for (ConfigurationSchemaEntry& entry : result) {
        if (!shortcutConfigurationKey(entry.key) ||
            entry.valueKind != ConfigurationValueKind::StringList) {
            continue;
        }
        entry.valueKind = ConfigurationValueKind::ShortcutList;
        entry.defaultValue = shortcutDefaults(entry.key, entry.defaultValue);
    }
#ifdef Q_OS_MACOS
    const auto replaceDefault = [&result](const QString& key, const QJsonArray& value) {
        const auto found = std::find_if(result.begin(), result.end(),
                                        [&key](const auto& entry) { return entry.key == key; });
        if (found != result.end()) {
            found->defaultValue = value;
        }
    };
    // Portable Meta maps to the physical Control key on Apple platforms.
    replaceDefault(QStringLiteral("global_shortcuts/screenshot"),
                   macGlobalShortcutDefault(QStringLiteral("Meta+1"), 18));
    replaceDefault(QStringLiteral("global_shortcuts/screenshot_copy"),
                   macGlobalShortcutDefault(QStringLiteral("Meta+2"), 19));
    replaceDefault(QStringLiteral("global_shortcuts/restore_last_closed_windows"),
                   macGlobalShortcutDefault(QStringLiteral("Meta+Shift+3"), 20));
    replaceDefault(QStringLiteral("global_shortcuts/pin_clipboard_content"),
                   macGlobalShortcutDefault(QStringLiteral("Meta+3"), 20));
#endif
    result.removeIf([](const ConfigurationSchemaEntry& entry) {
        return !app::editionConfigurationKeyAvailable(entry.key);
    });
    if constexpr (!app::edition::textTranslation) {
        for (auto& entry : result) {
            if (entry.key == QStringLiteral("tray/menu_options")) {
                entry.allowedStringValues.removeAll(QStringLiteral("quick.screenshot-translation"));
                entry.allowedStringValues.removeAll(
                    QStringLiteral("quick.translate-selected-text"));
            }
        }
    }
    return result;
}

const QVector<ConfigurationSchemaEntry> kEntries = buildEntries();

const QHash<QString, int>& entryIndex() {
    static const QHash<QString, int> index = [] {
        QHash<QString, int> result;
        result.reserve(kEntries.size());
        for (int i = 0; i < kEntries.size(); ++i) {
            result.insert(kEntries.at(i).key, i);
        }
        return result;
    }();
    return index;
}

bool isInteger(const QJsonValue& value, int* result = nullptr) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble()) != value.toDouble()) {
        return false;
    }
    if (value.toDouble() < static_cast<double>(std::numeric_limits<int>::min()) ||
        value.toDouble() > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (result != nullptr) {
        *result = value.toInt();
    }
    return true;
}

ConfigurationNormalization exactType(const QJsonValue& value, QJsonValue::Type type) {
    return {value, value.type() == type, false};
}

ConfigurationNormalization normalizeIntegerRange(const QJsonValue& value, int minimum,
                                                 int maximum) {
    int integer = 0;
    if (!isInteger(value, &integer) || integer < minimum || integer > maximum) {
        return {};
    }
    return {integer, true, false};
}

ConfigurationNormalization normalizeTheme(const QJsonValue& value) {
    if (!value.isString()) {
        return {};
    }
    const QString normalized = value.toString().trimmed().toLower();
    if (normalized != QStringLiteral("system") && normalized != QStringLiteral("light") &&
        normalized != QStringLiteral("dark")) {
        return {};
    }
    return {normalized, true, normalized != value.toString()};
}

ConfigurationNormalization normalizeLanguage(const QJsonValue& value) {
    if (!value.isString()) {
        return {};
    }
    QString normalized = value.toString().trimmed();
    if (normalized.compare(QStringLiteral("system"), Qt::CaseInsensitive) == 0) {
        normalized = QStringLiteral("system");
    } else {
        normalized.replace(u'-', u'_');
        if (normalized.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0) {
            normalized = QStringLiteral("en_US");
        } else {
            static const QRegularExpression localePattern(
                QStringLiteral("^[A-Za-z]{2,3}(?:_[A-Za-z0-9]{2,8})*$"));
            if (!localePattern.match(normalized).hasMatch()) {
                return {};
            }
            const QLocale locale(normalized);
            if (locale.language() == QLocale::AnyLanguage || locale.name() == QStringLiteral("C")) {
                return {};
            }
            normalized = locale.name();
        }
    }
    return {normalized, true, normalized != value.toString()};
}

ConfigurationNormalization normalizeShortcuts(const QJsonValue& value, int maximumItems,
                                              bool allowModifierOnlyShift,
                                              bool allowModifierOnlyAlt,
                                              snow_shot::shortcuts::ShortcutPolicy policy) {
    bool changed = false;
    bool valid = false;
    const auto bindings = snow_shot::shortcuts::shortcutBindingsFromJson(
        value, allowModifierOnlyShift, maximumItems, &valid, &changed, allowModifierOnlyAlt,
        policy);
    return {snow_shot::shortcuts::shortcutBindingsToJson(bindings), valid, changed};
}

ConfigurationNormalization normalizeAllowedStringList(const ConfigurationSchemaEntry& schemaEntry,
                                                      const QJsonValue& value) {
    if (!value.isArray()) {
        return {};
    }
    QJsonArray normalized;
    QSet<QString> seen;
    bool changed = false;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isString()) {
            changed = true;
            continue;
        }
        const QString original = item.toString();
        const QString candidate = original.trimmed();
        const auto canonical = std::find_if(
            schemaEntry.allowedStringValues.cbegin(), schemaEntry.allowedStringValues.cend(),
            [&candidate](const QString& allowed) {
                return allowed.compare(candidate, Qt::CaseInsensitive) == 0;
            });
        if (canonical == schemaEntry.allowedStringValues.cend() || seen.contains(*canonical) ||
            (schemaEntry.maximumListItems >= 0 &&
             normalized.size() >= schemaEntry.maximumListItems)) {
            changed = true;
            continue;
        }
        seen.insert(*canonical);
        normalized.push_back(*canonical);
        changed = changed || *canonical != original;
    }
    return {normalized, true, changed};
}

ConfigurationNormalization normalizeTrayMenuOptions(const ConfigurationSchemaEntry& schemaEntry,
                                                    const QJsonValue& value) {
    if (!value.isArray()) {
        return {};
    }
    QJsonArray migrated;
    bool renamed = false;
    for (const QJsonValue& item : value.toArray()) {
        // Earlier builds exposed the hotkey switch through the dedicated tray
        // command "tray.disable-shortcut-functions"; rename stored copies to the
        // quick action so upgraders keep the entry instead of silently losing it.
        if (item.isString() &&
            item.toString().trimmed() == QStringLiteral("tray.disable-shortcut-functions")) {
            migrated.push_back(QStringLiteral("quick.toggle-global-hotkeys"));
            renamed = true;
            continue;
        }
        migrated.push_back(item);
    }
    ConfigurationNormalization normalized = normalizeAllowedStringList(schemaEntry, migrated);
    // The rename itself is a change even when the mapped list is otherwise
    // canonical, so the store rewrites the persisted document with the new id.
    normalized.changed = normalized.changed || renamed;
    return normalized;
}

ConfigurationNormalization normalizeAllowedInteger(const QJsonValue& value,
                                                   std::initializer_list<int> allowed) {
    int integer = 0;
    if (!isInteger(value, &integer) ||
        std::find(allowed.begin(), allowed.end(), integer) == allowed.end()) {
        return {};
    }
    return {integer, true, false};
}

ConfigurationNormalization normalizePresets(const QJsonValue& value) {
    if (!value.isArray()) {
        return {};
    }
    QJsonArray result;
    bool changed = false;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isObject()) {
            changed = true;
            continue;
        }
        const QJsonObject itemObject = item.toObject();
        const QString name = itemObject.value(QStringLiteral("name")).toString().trimmed();
        const PersistedSelectionNormalization normalized = normalizePersistedSelection(item);
        if (name.isEmpty() || !normalized.valid) {
            changed = true;
            continue;
        }
        QJsonObject normalizedSelection = persistedSelectionToJson(normalized.value);
        normalizedSelection.insert(QStringLiteral("name"), name);
        result.push_back(normalizedSelection);
        changed = changed || normalized.changed || normalizedSelection != itemObject;
    }
    return {result, true, changed};
}

ConfigurationNormalization normalizeWatermarkContent(const QJsonValue& value) {
    if (!value.isObject()) {
        return {};
    }
    const QJsonObject object = value.toObject();
    const QJsonObject result{
        {QStringLiteral("text"), object.value(QStringLiteral("text")).toString()},
        {QStringLiteral("template_value"),
         object.value(QStringLiteral("template_value")).toString()},
    };
    return {result, true, result != object};
}

ConfigurationNormalization normalizeWatermarkTemplates(const QJsonValue& value) {
    if (!value.isArray()) {
        return {};
    }
    QJsonArray result;
    bool changed = false;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isObject()) {
            changed = true;
            continue;
        }
        const QJsonObject object = item.toObject();
        const QJsonValue nameValue = object.value(QStringLiteral("name"));
        const QJsonValue templateValue = object.value(QStringLiteral("value"));
        if (!nameValue.isString() || !templateValue.isString()) {
            changed = true;
            continue;
        }
        const QString name = nameValue.toString().trimmed();
        const QString templateText = templateValue.toString();
        if (name.isEmpty() || templateText.trimmed().isEmpty()) {
            changed = true;
            continue;
        }
        const QJsonObject normalized{{QStringLiteral("name"), name},
                                     {QStringLiteral("value"), templateText}};
        result.push_back(normalized);
        changed = changed || normalized != object;
    }
    return {result, true, changed};
}

ConfigurationNormalization normalizeDrawTemplates(const QJsonValue& value) {
    // Use the linked engine's format contract for both explicit edits and loading repairs.
    const auto maximumSupportedTemplateVersion = snow_draw_template_schema_version();
    if (!value.isArray()) {
        return {};
    }
    QJsonArray result;
    bool changed = false;
    for (const QJsonValue& item : value.toArray()) {
        const QJsonObject object = item.toObject();
        const QString name = object.value(QStringLiteral("name")).toString().trimmed();
        const QString encoded = object.value(QStringLiteral("payload")).toString();
        const QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
        const QJsonObject payload = document.object();
        int version = 0;
        if (!item.isObject() || name.isEmpty() || bytes.isEmpty() ||
            bytes.size() > 16 * 1024 * 1024 || QString::fromLatin1(bytes.toBase64()) != encoded ||
            error.error != QJsonParseError::NoError ||
            !ConfigurationSchema::parseIntegerVersion(
                payload.value(QStringLiteral("schemaVersion")), &version) ||
            static_cast<quint32>(version) > maximumSupportedTemplateVersion ||
            payload.value(QStringLiteral("elements")).toArray().isEmpty() ||
            payload.value(QStringLiteral("selectedIds")).toArray().isEmpty()) {
            changed = true;
            continue;
        }
        const QJsonObject normalized{{QStringLiteral("name"), name},
                                     {QStringLiteral("payload"), encoded}};
        result.push_back(normalized);
        changed = changed || normalized != object;
    }
    return {result, true, changed};
}

bool isRgbaColorKey(const QString& key) {
    return key == QStringLiteral("collage/background_color") ||
           key == QStringLiteral("interface/theme_primary_color") ||
           key == QStringLiteral("screenshot_selection/shadow_color") ||
           key == QStringLiteral("screenshot_selection/border_color") ||
           key == QStringLiteral("screenshot_ui/selection_border_color") ||
           key == QStringLiteral("screenshot_ui/selection_mask_color") ||
           key == QStringLiteral("screenshot_ui/cursor_guide_line_color") ||
           key == QStringLiteral("screenshot_ui/selection_center_guide_line_color") ||
           key == QStringLiteral("screenshot_ui/monitor_center_guide_line_color") ||
           key == QStringLiteral("screenshot_ui/color_picker_center_guide_line_color") ||
           key == QStringLiteral("pin_to_screen/border_color") ||
           key == QStringLiteral("pin_to_screen/border_active_color") ||
           key == QStringLiteral("pin_to_screen/locked_border_color") ||
           key == QStringLiteral("pin_to_screen/shadow_color") ||
           key == QStringLiteral("pin_to_screen/shadow_active_color") ||
           key == QStringLiteral("pin_to_screen/locked_shadow_color") ||
           key == QStringLiteral("screen_recording/mouse_trail_color") ||
           key == QStringLiteral("screen_recording/mouse_click_color") ||
           key == QStringLiteral("screen_recording/mouse_highlight_color") ||
           key == QStringLiteral("screen_recording/progress_bar_color") ||
           key == QStringLiteral("screen_recording/keyboard_background_color") ||
           key == QStringLiteral("screen_recording/keyboard_foreground_color");
}

ConfigurationNormalization normalizeRgbaColor(const QJsonValue& value) {
    if (!value.isString()) {
        return {};
    }
    const QString original = value.toString();
    const QString normalized = original.trimmed().toUpper();
    static const QRegularExpression pattern(QStringLiteral("^#[0-9A-F]{8}$"));
    if (!pattern.match(normalized).hasMatch()) {
        return {};
    }
    return {normalized, true, normalized != original};
}

ConfigurationNormalization normalizeColorPresets(const QJsonValue& value) {
    if (!value.isArray() || value.toArray().size() != 5) {
        return {};
    }
    QJsonArray colors;
    for (const QJsonValue& item : value.toArray()) {
        const ConfigurationNormalization normalized = normalizeRgbaColor(item);
        if (!normalized.valid) {
            return {};
        }
        colors.push_back(normalized.value);
    }
    return {colors, true, colors != value.toArray()};
}

bool isFilenameFormatKey(const QString& key) {
    return key == QStringLiteral("screenshot/manual_save_filename_format") ||
           key == QStringLiteral("screenshot/auto_save_filename_format") ||
           key == QStringLiteral("screen_recording/video_filename_format");
}

ConfigurationNormalization normalizeFilenameFormat(const QJsonValue& value) {
    if (!value.isString()) {
        return {};
    }
    const QString original = value.toString();
    const QString normalized = original.trimmed();
    static const QRegularExpression invalidCharacters(QStringLiteral("[\\\\/:*?\"<>|]"));
    if (normalized.isEmpty() || invalidCharacters.match(normalized).hasMatch()) {
        return {};
    }
    return {normalized, true, normalized != original};
}

#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
ConfigurationNormalization normalizeTranslationLanguage(const ConfigurationSchemaEntry& schemaEntry,
                                                        const QJsonValue& value) {
    if (!value.isString()) {
        return {};
    }
    const QString original = value.toString();
    const QString trimmed = original.trimmed();
    const auto canonical =
        std::find_if(schemaEntry.allowedStringValues.cbegin(),
                     schemaEntry.allowedStringValues.cend(), [&trimmed](const QString& allowed) {
                         return allowed.compare(trimmed, Qt::CaseInsensitive) == 0;
                     });
    if (canonical == schemaEntry.allowedStringValues.cend()) {
        return {};
    }
    return {*canonical, true, *canonical != original};
}
#endif

ConfigurationNormalization
normalizeToolbarLayout(const QJsonValue& value, const QStringList& itemIds,
                       const QVector<QStringList>& defaultPositions,
                       bool migrateScreenshotLayout = false, const QStringList& defaultHidden = {},
                       bool legacyDefaults = true, const QStringList& standaloneItemIds = {}) {
    if (!value.isObject()) {
        return {};
    }
    const QJsonObject object = value.toObject();
    const QSet<QString> known(itemIds.cbegin(), itemIds.cend());
    QVector<QStringList> positions;
    QSet<QString> positioned;
    QStringList hidden;
    QSet<QString> hiddenSet;
    const auto appendPosition = [&positions, &positioned, &hiddenSet, &known,
                                 &standaloneItemIds](const QStringList& ids) {
        QStringList position;
        for (const QString& id : ids) {
            if (known.contains(id) && !positioned.contains(id) && !hiddenSet.contains(id)) {
                if (id == QStringLiteral("separator") || id == QStringLiteral("select-separator") ||
                    id == QStringLiteral("confirm-separator") || standaloneItemIds.contains(id)) {
                    if (!position.isEmpty()) {
                        positions.push_back(position);
                        position.clear();
                    }
                    positions.push_back({id});
                    positioned.insert(id);
                    continue;
                }
                position.push_back(id);
                positioned.insert(id);
            }
        }
        if (!position.isEmpty()) {
            positions.push_back(position);
        }
    };
    const auto appendHidden = [&hidden, &hiddenSet, &positioned, &known](const QStringList& ids) {
        for (const QString& id : ids) {
            if (known.contains(id) && !positioned.contains(id) && !hiddenSet.contains(id)) {
                hidden.push_back(id);
                hiddenSet.insert(id);
            }
        }
    };

    if (object.value(QStringLiteral("positions")).isArray()) {
        for (const QJsonValue& positionValue :
             object.value(QStringLiteral("positions")).toArray()) {
            if (!positionValue.isArray()) {
                continue;
            }
            QStringList ids;
            for (const QJsonValue& item : positionValue.toArray()) {
                if (item.isString()) {
                    ids.push_back(item.toString());
                }
            }
            appendPosition(ids);
        }
        if (object.value(QStringLiteral("hidden")).isArray()) {
            QStringList hiddenIds;
            for (const QJsonValue& item : object.value(QStringLiteral("hidden")).toArray()) {
                if (item.isString()) {
                    hiddenIds.push_back(item.toString());
                }
            }
            appendHidden(hiddenIds);
        }
    } else {
        return {};
    }

    appendHidden(defaultHidden);
    // Upgrade only the previous default drawing layout; custom stacks remain intact.
    auto previousDrawingDefaults = defaultPositions;
    QStringList missingAnnotations;
    for (const QString& id : {QStringLiteral("bracket"), QStringLiteral("angle"),
                              QStringLiteral("distance"), QStringLiteral("magnifier")}) {
        if (known.contains(id) && !positioned.contains(id) && !hiddenSet.contains(id)) {
            missingAnnotations.push_back(id);
            for (auto& position : previousDrawingDefaults)
                position.removeAll(id);
        }
    }
    const auto visibleDefaults = [&hidden](QVector<QStringList> defaults) {
        for (auto& position : defaults) {
            for (const QString& id : hidden)
                position.removeAll(id);
        }
        defaults.removeIf([](const QStringList& position) { return position.isEmpty(); });
        return defaults;
    };
    if (!missingAnnotations.isEmpty() && positions == visibleDefaults(previousDrawingDefaults)) {
        positions = visibleDefaults(defaultPositions);
        for (const QString& id : missingAnnotations)
            positioned.insert(id);
    }
    if (legacyDefaults && app::edition::isMini &&
        known.contains(QStringLiteral("text-recognition")) &&
        !positioned.contains(QStringLiteral("text-recognition")) &&
        !hiddenSet.contains(QStringLiteral("text-recognition"))) {
        appendHidden({QStringLiteral("text-recognition")});
    }

    if (known.contains(QStringLiteral("upload-to-cloud")) &&
        !positioned.contains(QStringLiteral("upload-to-cloud")) &&
        !hiddenSet.contains(QStringLiteral("upload-to-cloud"))) {
        for (QStringList& position : positions) {
            if (position.contains(QStringLiteral("save-as-file"))) {
                position.prepend(QStringLiteral("upload-to-cloud"));
                positioned.insert(QStringLiteral("upload-to-cloud"));
                break;
            }
        }
        if (!positioned.contains(QStringLiteral("upload-to-cloud")) &&
            hiddenSet.contains(QStringLiteral("save-as-file"))) {
            hidden.push_back(QStringLiteral("upload-to-cloud"));
            hiddenSet.insert(QStringLiteral("upload-to-cloud"));
        }
    }
    if (known.contains(QStringLiteral("quick-save")) &&
        !positioned.contains(QStringLiteral("quick-save")) &&
        !hiddenSet.contains(QStringLiteral("quick-save"))) {
        for (QStringList& position : positions) {
            const qsizetype saveIndex = position.indexOf(QStringLiteral("save-as-file"));
            if (saveIndex >= 0) {
                position.insert(saveIndex, QStringLiteral("quick-save"));
                positioned.insert(QStringLiteral("quick-save"));
                break;
            }
        }
        if (!positioned.contains(QStringLiteral("quick-save")) &&
            hiddenSet.contains(QStringLiteral("save-as-file"))) {
            hidden.push_back(QStringLiteral("quick-save"));
            hiddenSet.insert(QStringLiteral("quick-save"));
        }
    }
    if (known.contains(QStringLiteral("print")) && !positioned.contains(QStringLiteral("print")) &&
        !hiddenSet.contains(QStringLiteral("print"))) {
        for (QStringList& position : positions) {
            const qsizetype saveIndex = position.indexOf(QStringLiteral("save-as-file"));
            if (saveIndex >= 0) {
                const qsizetype quickIndex = position.indexOf(QStringLiteral("quick-save"));
                position.insert(quickIndex >= 0 ? std::min(quickIndex, saveIndex) : saveIndex,
                                QStringLiteral("print"));
                positioned.insert(QStringLiteral("print"));
                break;
            }
        }
        if (!positioned.contains(QStringLiteral("print")) &&
            hiddenSet.contains(QStringLiteral("save-as-file"))) {
            hidden.push_back(QStringLiteral("print"));
            hiddenSet.insert(QStringLiteral("print"));
        }
    }
    if (known.contains(QStringLiteral("pin-window-to-screen")) &&
        !positioned.contains(QStringLiteral("pin-window-to-screen")) &&
        !hiddenSet.contains(QStringLiteral("pin-window-to-screen"))) {
        for (QStringList& position : positions) {
            const qsizetype pin = position.indexOf(QStringLiteral("pin-to-screen"));
            if (pin >= 0) {
                position.insert(pin, QStringLiteral("pin-window-to-screen"));
                positioned.insert(QStringLiteral("pin-window-to-screen"));
                break;
            }
        }
        if (!positioned.contains(QStringLiteral("pin-window-to-screen")) &&
            hiddenSet.contains(QStringLiteral("pin-to-screen"))) {
            hidden.push_back(QStringLiteral("pin-window-to-screen"));
            hiddenSet.insert(QStringLiteral("pin-window-to-screen"));
        }
    }
    if (known.contains(QStringLiteral("airdrop")) &&
        !positioned.contains(QStringLiteral("airdrop")) &&
        !hiddenSet.contains(QStringLiteral("airdrop"))) {
        for (QStringList& position : positions) {
            const qsizetype save = position.indexOf(QStringLiteral("save-as-file"));
            if (save < 0)
                continue;
            const qsizetype print = position.indexOf(QStringLiteral("print"));
            const qsizetype quick = position.indexOf(QStringLiteral("quick-save"));
            position.insert(print >= 0   ? print
                            : quick >= 0 ? quick
                                         : save,
                            QStringLiteral("airdrop"));
            positioned.insert(QStringLiteral("airdrop"));
            break;
        }
        if (!positioned.contains(QStringLiteral("airdrop")) &&
            hiddenSet.contains(QStringLiteral("save-as-file"))) {
            hidden.push_back(QStringLiteral("airdrop"));
            hiddenSet.insert(QStringLiteral("airdrop"));
        }
    }
    if (migrateScreenshotLayout && !positions.isEmpty() &&
        known.contains(QStringLiteral("convert-to-markdown"))) {
        // Upgrade earlier defaults without changing custom placements.
        auto previousScreenshotDefault = defaultPositions;
        if (known.contains(QStringLiteral("cancel"))) {
            previousScreenshotDefault.removeIf([](const QStringList& position) {
                return position == QStringList{QStringLiteral("separator")} ||
                       position == QStringList{QStringLiteral("cancel")} ||
                       position == QStringList{QStringLiteral("copy")};
            });
        }
        auto previousDefault = previousScreenshotDefault;
        previousDefault[0] = {QStringLiteral("barcode-recognition"),
                              QStringLiteral("table-recognition")};
        previousDefault.insert(1, QStringList{QStringLiteral("convert-to-markdown")});
        previousDefault.insert(2, QStringList{QStringLiteral("convert-to-html")});
        auto previousGroupedDefault = previousScreenshotDefault;
        previousGroupedDefault[0] = {
            QStringLiteral("table-recognition"), QStringLiteral("barcode-recognition"),
            QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html")};
        if (hidden.isEmpty() &&
            (positions == previousDefault || positions == previousGroupedDefault)) {
            positions = defaultPositions;
            for (const QStringList& position : defaultPositions) {
                for (const QString& id : position) {
                    positioned.insert(id);
                }
            }
        }
        qsizetype recognitionPosition = -1;
        for (const QString& anchor :
             {QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition")}) {
            for (qsizetype index = 0; index < positions.size(); ++index) {
                if (positions.at(index).contains(anchor)) {
                    recognitionPosition = index;
                    break;
                }
            }
            if (recognitionPosition >= 0) {
                break;
            }
        }
        for (const QString& id :
             {QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html")}) {
            if (recognitionPosition >= 0 && !positioned.contains(id) && !hiddenSet.contains(id)) {
                positions[recognitionPosition].push_back(id);
                positioned.insert(id);
            }
        }
    }
    // Upgrade the previous default recognition group, preserving custom arrangements.
    for (QStringList& position : positions) {
        if (legacyDefaults && position == QStringList{QStringLiteral("convert-to-html"),
                                                      QStringLiteral("latex-recognition"),
                                                      QStringLiteral("convert-to-markdown"),
                                                      QStringLiteral("barcode-recognition"),
                                                      QStringLiteral("table-recognition")}) {
            position.swapItemsAt(1, 2);
        }
    }
    if (legacyDefaults && known.contains(QStringLiteral("latex-recognition")) &&
        !positioned.contains(QStringLiteral("latex-recognition")) &&
        !hiddenSet.contains(QStringLiteral("latex-recognition"))) {
        for (QStringList& position : positions) {
            if (position.contains(QStringLiteral("latex-recognition"))) {
                positioned.insert(QStringLiteral("latex-recognition"));
                break;
            }
            const auto index = position.indexOf(QStringLiteral("convert-to-markdown"));
            if (index >= 0) {
                // Popup buttons reverse the saved stack: insert after Markdown to appear to its
                // left.
                position.insert(index + 1, QStringLiteral("latex-recognition"));
                positioned.insert(QStringLiteral("latex-recognition"));
                break;
            }
        }
        if (!positioned.contains(QStringLiteral("latex-recognition")) &&
            hiddenSet.contains(QStringLiteral("convert-to-markdown"))) {
            hidden.push_back(QStringLiteral("latex-recognition"));
            hiddenSet.insert(QStringLiteral("latex-recognition"));
        }
    }
    if (known.contains(QStringLiteral("document-recognition")) &&
        !positioned.contains(QStringLiteral("document-recognition")) &&
        !hiddenSet.contains(QStringLiteral("document-recognition"))) {
        for (QStringList& position : positions) {
            const auto table = position.indexOf(QStringLiteral("table-recognition"));
            if (table >= 0) {
                position.insert(table, QStringLiteral("document-recognition"));
                positioned.insert(QStringLiteral("document-recognition"));
                break;
            }
        }
        if (!positioned.contains(QStringLiteral("document-recognition")) &&
            hiddenSet.contains(QStringLiteral("table-recognition"))) {
            hidden.push_back(QStringLiteral("document-recognition"));
            hiddenSet.insert(QStringLiteral("document-recognition"));
        }
    }
    // Preserve the former fixed prefix when upgrading saved annotation layouts.
    for (const QString& id : {QStringLiteral("select-separator"), QStringLiteral("select")}) {
        if (known.contains(id) && !positioned.contains(id) && !hiddenSet.contains(id)) {
            positions.prepend({id});
            positioned.insert(id);
        }
    }
    // New magnifiers join the highlight stack without moving any customized tools.
    const QString magnifier = QStringLiteral("magnifier");
    if (known.contains(magnifier) && !positioned.contains(magnifier) &&
        !hiddenSet.contains(magnifier)) {
        qsizetype anchor = -1;
        bool joined = false;
        for (qsizetype index = 0; index < positions.size(); ++index) {
            auto& position = positions[index];
            const bool highlight = position.contains(QStringLiteral("highlighter"));
            const bool spotlight = position.contains(QStringLiteral("spotlight"));
            if (highlight && spotlight) {
                position.prepend(magnifier);
                joined = true;
                break;
            }
            if (highlight || spotlight)
                anchor = index;
        }
        if (!joined) {
            qsizetype insertion = anchor + 1;
            if (anchor < 0) {
                insertion = positions.size();
                for (qsizetype index = 0; index < positions.size(); ++index) {
                    if (positions.at(index).contains(QStringLiteral("separator")) ||
                        positions.at(index).contains(QStringLiteral("undo")) ||
                        positions.at(index).contains(QStringLiteral("redo"))) {
                        insertion = index;
                        break;
                    }
                }
            }
            positions.insert(insertion, QStringList{magnifier});
        }
        positioned.insert(magnifier);
    }
    for (const QStringList& defaultPosition : defaultPositions) {
        QStringList missing;
        for (const QString& id : defaultPosition) {
            if (!positioned.contains(id) && !hiddenSet.contains(id)) {
                missing.push_back(id);
            }
        }
        appendPosition(missing);
    }

    const QJsonObject normalized{
        {QStringLiteral("positions"), jsonArray(positions)},
        {QStringLiteral("hidden"), jsonArray(hidden)},
    };
    return {normalized, true, normalized != object};
}

bool isGlobalMouseKey(const QString& key) {
    return key.startsWith(QStringLiteral("global_mouse/"));
}

ConfigurationNormalization normalizeGlobalMouseCombination(const QJsonValue& value) {
    if (!value.isObject()) {
        return {};
    }
    const QJsonObject object = value.toObject();
    if (object.isEmpty()) {
        return {QJsonObject(), true, false};
    }
    const QJsonValue activationValue = object.value(QStringLiteral("activation_key"));
    if (object.size() != 2 || (!activationValue.isString() && !activationValue.isArray()) ||
        !object.value(QStringLiteral("mouse_button")).isString()) {
        return {};
    }

    QStringList keys;
    if (activationValue.isString()) {
        keys.push_back(activationValue.toString().trimmed());
    } else {
        for (const QJsonValue& item : activationValue.toArray()) {
            if (!item.isString()) {
                return {};
            }
            keys.push_back(item.toString().trimmed());
        }
    }
    const QString mouseButton = object.value(QStringLiteral("mouse_button")).toString().trimmed();
    static const QStringList activationKeys = globalMouseActivationKeys();
    static const QSet<QString> mouseButtons{
        QStringLiteral("left_drag"), QStringLiteral("right_drag"), QStringLiteral("wheel_drag"),
        QStringLiteral("side_button_1_drag"), QStringLiteral("side_button_2_drag")};
    if (keys.isEmpty() || !mouseButtons.contains(mouseButton)) {
        return {};
    }
    for (const QString& key : keys) {
        if (!activationKeys.contains(key)) {
            return {};
        }
    }
    keys.removeDuplicates();
    keys.sort();

    const QJsonObject normalized{{QStringLiteral("activation_key"),
                                  keys.size() == 1 ? QJsonValue(keys.front())
                                                   : QJsonValue(QJsonArray::fromStringList(keys))},
                                 {QStringLiteral("mouse_button"), mouseButton}};
    return {normalized, true, normalized != object};
}

void insertPath(QJsonObject* root, const QString& path, const QJsonValue& value) {
    const QStringList parts = path.split(u'/');
    if (root == nullptr || parts.size() != 2) {
        return;
    }
    QJsonObject group = root->value(parts[0]).toObject();
    group.insert(parts[1], value);
    root->insert(parts[0], group);
}
} // namespace

const QVector<ConfigurationSchemaEntry>& ConfigurationSchema::entries() {
    return kEntries;
}

const ConfigurationSchemaEntry* ConfigurationSchema::entry(const QString& key) {
    if (!app::editionConfigurationKeyAvailable(key))
        return nullptr;
    const auto found = entryIndex().constFind(key);
    return found == entryIndex().cend() ? nullptr : &kEntries.at(found.value());
}

bool ConfigurationSchema::contains(const QString& key) {
    return entry(key) != nullptr;
}

QJsonValue ConfigurationSchema::defaultValue(const QString& key) {
    const ConfigurationSchemaEntry* found = entry(key);
    return found == nullptr ? QJsonValue() : found->defaultValue;
}

namespace {
ConfigurationNormalization normalizeManualSaveFormatOptions(const QJsonValue& value) {
    if (!value.isObject())
        return {};
    const QJsonObject source = value.toObject();
    QJsonObject result;
    const QSet<QString> qualityFormats{
        QStringLiteral("jpeg"), QStringLiteral("webp"), QStringLiteral("jxl"),
        QStringLiteral("avif"), QStringLiteral("pdf"),
    };
    const QSet<QString> compressionFormats{
        QStringLiteral("png"),  QStringLiteral("webp"), QStringLiteral("jxl"),
        QStringLiteral("avif"), QStringLiteral("tiff"),
    };
    const QSet<QString> compressionValues{
        QStringLiteral("low"),
        QStringLiteral("medium"),
        QStringLiteral("high"),
    };
    for (auto format = source.constBegin(); format != source.constEnd(); ++format) {
        if ((!qualityFormats.contains(format.key()) &&
             !compressionFormats.contains(format.key())) ||
            !format.value().isObject()) {
            continue;
        }
        const QJsonObject options = format.value().toObject();
        QJsonObject normalized;
        if (qualityFormats.contains(format.key())) {
            const QJsonValue quality = options.value(QStringLiteral("quality"));
            if (isInteger(quality))
                normalized.insert(QStringLiteral("quality"), qBound(0, quality.toInt(), 100));
        }
        if (compressionFormats.contains(format.key())) {
            const QString compression =
                options.value(QStringLiteral("compression_level")).toString().trimmed();
            if (compressionValues.contains(compression))
                normalized.insert(QStringLiteral("compression_level"), compression);
        }
        if (!normalized.isEmpty())
            result.insert(format.key(), normalized);
    }
    return {result, true, result != source};
}
} // namespace

ConfigurationNormalization ConfigurationSchema::normalize(const QString& key,
                                                          const QJsonValue& value) {
    const ConfigurationSchemaEntry* schemaEntry = entry(key);
    if (schemaEntry == nullptr) {
        return {};
    }
    if (key == QStringLiteral("context_menu_shortcuts/edit_with_snow_shot")) {
        if (!value.isArray() || value.toArray().size() > 1)
            return {};
        QJsonArray normalized;
        if (!value.toArray().isEmpty()) {
            bool valid = false;
            const auto binding =
                shortcuts::shortcutBindingFromJson(value.toArray().first(), false, &valid);
            const QString accessKey = shortcuts::canonicalMenuAccessKey(binding.portableText);
            if (!valid || accessKey.isEmpty())
                return {};
            normalized = shortcuts::shortcutBindingsToJson({shortcuts::ShortcutBinding{accessKey}});
        }
        return {normalized, true, normalized != value};
    }
    if (key.startsWith(QStringLiteral("collage/pdf_custom_")) ||
        key.startsWith(QStringLiteral("collage/pdf_margin_")) ||
        key == QStringLiteral("collage/pdf_image_spacing_mm")) {
        if (!value.isDouble() || !std::isfinite(value.toDouble()))
            return {};
        const bool customDimension = key.startsWith(QStringLiteral("collage/pdf_custom_"));
        if (customDimension ? value.toDouble() <= 0 || value.toDouble() > 381'000'000
                            : value.toDouble() < 0)
            return {};
        return {value, true, false};
    }
#ifdef Q_OS_MACOS
    if (key == u"updates/mode" &&
        (value.toString() == u"download" || value.toString() == u"next_launch")) {
        return {QStringLiteral("check"), true, true};
    }
#endif
    if (key == QStringLiteral("cloud_upload/configuration")) {
        bool valid = false;
        const auto values = cloudUploadSettingsFromJson(value, &valid);
        const auto normalized = cloudUploadSettingsToJson(values);
        return {normalized, valid, normalized != value};
    }
    if (key == QStringLiteral("network/http_proxy")) {
        const auto normalized = normalizedHttpProxyUrl(value.toString());
        return {normalized.value_or(QString()), value.isString() && normalized.has_value(),
                normalized.has_value() && *normalized != value};
    }
#if SNOW_SHOT_ENABLE_API_CONFIGURATION
    if (key == QStringLiteral("api_configuration/server_url")) {
        const auto normalized = normalizedServerUrl(value.toString());
        return {normalized.value_or(QString()), value.isString() && normalized.has_value(),
                normalized.has_value() && *normalized != value};
    }
    if (key == QStringLiteral("api_configuration/custom_models")) {
        bool valid = false;
        const auto models = customAiModelsFromJson(value, &valid);
        const auto normalized = customAiModelsToJson(models);
        return {normalized, valid, normalized != value};
    }
    if (key == QStringLiteral("api_configuration/text_translation")) {
        bool valid = false;
        const auto models = textTranslationConfigurationsFromJson(value, &valid);
        const auto normalized = textTranslationConfigurationsToJson(models);
        return {normalized, valid, normalized != value};
    }
#endif
    if (key == QStringLiteral("interface/theme_mode")) {
        return normalizeTheme(value);
    }
    if (key == QStringLiteral("interface/language")) {
        return normalizeLanguage(value);
    }
    if (key == QStringLiteral("screen_recording/area_history"))
        return normalizeRecordingAreaHistory(value);
    if (key == QStringLiteral("screenshot_selection/selection_rect_presets")) {
        return normalizePresets(value);
    }
    if (key == QStringLiteral("screenshot_selection/layout_compaction_spacing")) {
        // Migrate the previous spacing range without resetting an existing preference to zero.
        const auto legacy = normalizeIntegerRange(value, 0, 1000);
        if (!legacy.valid) {
            return {};
        }
        const int spacing =
            std::min(legacy.value.toInt(), ScreenshotLayoutCompaction::maximumSpacing);
        return {spacing, true, spacing != legacy.value.toInt()};
    }
    if (key == QStringLiteral("drawing/watermark_content")) {
        return normalizeWatermarkContent(value);
    }
    if (key == QStringLiteral("drawing/watermark_templates")) {
        return normalizeWatermarkTemplates(value);
    }
    if (key == QStringLiteral("drawing/draw_templates")) {
        return normalizeDrawTemplates(value);
    }
    if (key == QStringLiteral("screenshot/save_path_shortcuts")) {
        if (!value.isArray()) {
            return {};
        }
        QJsonArray result;
        QSet<QString> names;
        for (const auto& item : value.toArray()) {
            const auto object = item.toObject();
            const QString name = object.value(QStringLiteral("name")).toString().trimmed();
            const QString path = object.value(QStringLiteral("path")).toString().trimmed();
            if (name.isEmpty() || path.isEmpty() || names.contains(name.toCaseFolded())) {
                continue;
            }
            names.insert(name.toCaseFolded());
            result.append(
                QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("path"), path}});
        }
        return {result, true, result != value.toArray()};
    }
    if (key == QStringLiteral("screenshot/manual_save_format_options"))
        return normalizeManualSaveFormatOptions(value);
    if (key == QStringLiteral("floating_toolbar/placement")) {
        if (!value.isObject())
            return {};
        const auto input = value.toObject();
        if (input.isEmpty())
            return {input, true, false};
        const double x = input.value(QStringLiteral("x")).toDouble(-1);
        const double y = input.value(QStringLiteral("y")).toDouble(-1);
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1)
            return {};
        const QJsonObject result{
            {QStringLiteral("screen"), input.value(QStringLiteral("screen")).toString()},
            {QStringLiteral("x"), x},
            {QStringLiteral("y"), y}};
        return {result, true, result != input};
    }
    if (key == QStringLiteral("floating_toolbar/layout")) {
        return normalizeToolbarLayout(value, floatingToolbarItemIds(),
                                      defaultFloatingToolbarLayout().positions, false,
                                      defaultFloatingToolbarLayout().hidden, false);
    }
    if (key == QStringLiteral("screenshot_toolbar/layout")) {
        return normalizeToolbarLayout(value, kDrawingToolbarItemIds,
                                      defaultDrawingToolbarPositions());
    }
    if (key == QStringLiteral("pin_to_screen/action_tools_layout")) {
        return normalizeToolbarLayout(value, kPinnedActionToolbarItemIds,
                                      defaultPinnedActionToolbarPositions());
    }
    if (key == QStringLiteral("screenshot_toolbar/action_tools_layout")) {
        return normalizeToolbarLayout(value, kActionToolbarItemIds, defaultActionToolbarPositions(),
                                      true);
    }
    if (key == QStringLiteral("screen_recording/action_tools_layout")) {
        return normalizeToolbarLayout(value, kRecordingActionToolbarItemIds,
                                      defaultRecordingActionToolbarPositions(), false, {}, false,
                                      {QStringLiteral("duration")});
    }
    if (isGlobalMouseKey(key)) {
        return normalizeGlobalMouseCombination(value);
    }
    if (key == QStringLiteral("pin_to_screen/text_color") ||
        key == QStringLiteral("pin_to_screen/text_background_color")) {
        return value == QJsonValue(QStringLiteral("system"))
                   ? ConfigurationNormalization{value, true, false}
                   : normalizeRgbaColor(value);
    }
    if (key == QStringLiteral("pin_to_screen/text_font_family") &&
        (!value.isString() || value.toString().size() > 1024)) {
        return {};
    }
    if (key == QStringLiteral("drawing/shared_tool_color")) {
        return value == QJsonValue(QString()) ? ConfigurationNormalization{value, true, false}
                                              : normalizeRgbaColor(value);
    }
    if (isRgbaColorKey(key)) {
        return normalizeRgbaColor(value);
    }
    if (key == QStringLiteral("screenshot/stroke_color_presets") ||
        key == QStringLiteral("screenshot/fill_color_presets") ||
        key == QStringLiteral("screenshot/highlight_color_presets")) {
        return normalizeColorPresets(value);
    }
    if (isFilenameFormatKey(key)) {
        return normalizeFilenameFormat(value);
    }
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION || SNOW_SHOT_ENABLE_LATEX_RECOGNITION
    if (key == QStringLiteral("screenshot_table/model") ||
        key == QStringLiteral("screenshot_latex/model")) {
        if (!value.isString()) {
            return {};
        }
        const QString selection = value.toString().trimmed();
        if (selection.isEmpty() || selection.size() > 256 ||
            std::any_of(selection.cbegin(), selection.cend(), [](QChar character) {
                return character.isSpace() || character.category() == QChar::Other_Control;
            })) {
            return {};
        }
        return {selection, true, selection != value.toString()};
    }
#endif
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    if (key == QStringLiteral("screenshot_translation/source_language") ||
        key == QStringLiteral("screenshot_translation/target_language") ||
        key == QStringLiteral("screenshot_translation/secondary_target_language")) {
        if (key == QStringLiteral("screenshot_translation/target_language") && value.isString() &&
            value.toString().isEmpty())
            return {QString(), true, false};
        return normalizeTranslationLanguage(*schemaEntry, value);
    }
#endif
    if (key == QStringLiteral("drawing/quick_selection_disabled_tools") ||
        key == QStringLiteral("drawing/automatic_selection_excluded_tools")) {
        return normalizeAllowedStringList(*schemaEntry, value);
    }
    if (key == QStringLiteral("tray/menu_options")) {
        return normalizeTrayMenuOptions(*schemaEntry, value);
    }
    if (key == QStringLiteral("screen_recording/frame_rate")) {
        return normalizeAllowedInteger(value, {5, 10, 15, 24, 30, 60, 120, 83});
    }
    if (key == QStringLiteral("interface/screenshot_history_page_size") ||
        key == QStringLiteral("interface/pinned_window_management_page_size")) {
        return normalizeAllowedInteger(value, {10, 20, 50});
    }
    if (key == QStringLiteral("screenshot_ui/selection_transition_animation")) {
        if (value.isBool()) {
            return {static_cast<int>(value.toBool() ? SelectionTransitionAnimation::Soft
                                                    : SelectionTransitionAnimation::Disabled),
                    true, true};
        }
        return normalizeAllowedInteger(value, {0, 32, 72});
    }
    if (key == QStringLiteral("screen_recording/animated_image_frame_rate")) {
        return normalizeAllowedInteger(value, {5, 10, 15, 24});
    }
    switch (schemaEntry->valueKind) {
    case ConfigurationValueKind::Boolean:
        return exactType(value, QJsonValue::Bool);
    case ConfigurationValueKind::Integer:
        if (schemaEntry->integerRange.has_value()) {
            return normalizeIntegerRange(value, schemaEntry->integerRange->minimum,
                                         schemaEntry->integerRange->maximum);
        }
        return isInteger(value) ? ConfigurationNormalization{value, true, false}
                                : ConfigurationNormalization{};
    case ConfigurationValueKind::String: {
        if (!value.isString()) {
            return {};
        }
        const QString normalizedValue = value.toString().trimmed();
        if (!schemaEntry->allowedStringValues.isEmpty() &&
            !schemaEntry->allowedStringValues.contains(normalizedValue)) {
            return {};
        }
        return {normalizedValue, true, normalizedValue != value.toString()};
    }
    case ConfigurationValueKind::StringList:
        return normalizeAllowedStringList(*schemaEntry, value);
    case ConfigurationValueKind::ShortcutList:
        return normalizeShortcuts(value, schemaEntry->maximumListItems,
                                  allowsModifierOnlyShift(key),
                                  key == QStringLiteral("screenshot_shortcuts/toggle_guides"),
                                  key.startsWith(QStringLiteral("global_shortcuts/"))
                                      ? shortcuts::ShortcutPolicy::Native
                                      : shortcuts::ShortcutPolicy::Application);
    case ConfigurationValueKind::Structured:
        return exactType(value, QJsonValue::Object);
    }
    return {};
}

bool ConfigurationSchema::allowsModifierOnlyShift(const QString& key) {
    return key.startsWith(QStringLiteral("screenshot_shortcuts/")) ||
           key.startsWith(QStringLiteral("screen_recording_shortcuts/"));
}

bool ConfigurationSchema::parseIntegerVersion(const QJsonValue& value, int* version) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
        std::floor(value.toDouble()) != value.toDouble() || value.toDouble() < 1.0 ||
        value.toDouble() > static_cast<double>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (version != nullptr) {
        *version = value.toInt();
    }
    return true;
}

QMap<QString, QJsonValue>
ConfigurationSchema::migrateLegacyValues(QMap<QString, QJsonValue> values) {
    const QString smartSelectionKey = QStringLiteral("screenshot_selection/smart_selection");
    const QJsonValue legacyTarget =
        values.take(QStringLiteral("screenshot_selection/selection_target"));
    const QJsonValue legacySmartSelection = values.value(smartSelectionKey);
    if (legacySmartSelection.isBool() ||
        (!values.contains(smartSelectionKey) && legacyTarget.isString())) {
        values.insert(smartSelectionKey,
                      !legacySmartSelection.toBool(true) || legacyTarget == QStringLiteral("window")
                          ? QStringLiteral("window")
                          : QStringLiteral("window_sub_element"));
    }
    const QJsonValue fontPercentage =
        values.take(QStringLiteral("interface/app_font_size_percentage"));
    const QString fontSizeKey = QStringLiteral("interface/app_font_size");
    if (!values.contains(fontSizeKey) && fontPercentage.isDouble()) {
        const double percentage = fontPercentage.toDouble();
        if (std::isfinite(percentage) && std::floor(percentage) == percentage &&
            percentage >= 100 && percentage <= 200) {
            // The percentage setting scaled a 14 px base font size.
            values.insert(fontSizeKey, qRound(14 * percentage / 100.0));
        }
    }
    const QJsonValue legacy = values.take(QStringLiteral("screenshot/auto_save_after_copy"));
    const QString actionsKey = QStringLiteral("screenshot/auto_save_after_execution_actions");
    if (!values.contains(actionsKey) && legacy.isBool()) {
        QJsonArray actions;
        if (legacy.toBool()) {
            actions = {
                QStringLiteral("copy_screenshot"), QStringLiteral("copy_scrolling_screenshot"),
                QStringLiteral("copy_pinned_image"), QStringLiteral("capture_focused_window"),
                QStringLiteral("capture_current_monitor")};
        }
        values.insert(actionsKey, actions);
    }
    return values;
}

int ConfigurationSchema::currentVersion() {
    return defaultValue(QStringLiteral("storage/schema_version")).toInt();
}

QJsonObject ConfigurationSchema::completeDefaultDocument() {
    QJsonObject root;
    for (const ConfigurationSchemaEntry& entry : entries()) {
        insertPath(&root, entry.key, entry.defaultValue);
    }
    return root;
}
} // namespace snow_shot::storage
