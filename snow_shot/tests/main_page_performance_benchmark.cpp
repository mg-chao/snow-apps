#include "snow_shot/presentation/components/aboutpagewidget.h"
#include "snow_shot/app/edition.h"
#include "snow_shot/presentation/components/screenshothistorypagewidget.h"
#include "snow_shot/presentation/components/pinnedwindowmanagementpagewidget.h"
#include "snow_shot/presentation/components/translationpagewidget.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimeZone>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace storage = snow_shot::storage;

namespace {

void drain() {
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}
double ms(const QElapsedTimer& timer) {
    return static_cast<double>(timer.nsecsElapsed()) / 1e6;
}
QJsonObject distribution(QVector<double> values) {
    std::sort(values.begin(), values.end());
    return {{QStringLiteral("median"), values.at(values.size() / 2)},
            {QStringLiteral("p95"), values.at((values.size() - 1) * 95 / 100)}};
}

class HistorySource final : public ScreenshotHistoryPageDataSource {
  public:
    QVector<storage::CaptureHistoryRecord> data;
    QVector<storage::CaptureHistoryRecord> records() const override {
        return data;
    }
    std::optional<storage::CaptureHistoryAssetSet>
    displayAssets(const storage::CaptureHistoryRecord&) const override {
        return {};
    }
    bool supportsAsyncDisplayAssets() const override {
        // Measure page construction independently of filesystem reads and image decoding.
        return true;
    }
    void remove(const QString&) override {}
    bool requestRemoveMany(const QVector<QString>&) override {
        return false;
    }
    bool requestClear() override {
        return false;
    }
};
class PinnedSource final : public PinnedWindowManagementDataSource {
  public:
    QVector<storage::PinnedWindowSummary> data;
    QVector<storage::PinnedWindowSummary> records() const override {
        return data;
    }
    QVector<storage::PinnedWindowGroup> groups() const override {
        return {};
    }
    void requestPreview(const QString&, quint64, const QSize&) override {}
    void requestFullImage(const QString&, quint64) override {}
    void showRecord(const QString&) override {}
    void removeRecords(const QVector<QString>&) override {}
};

QJsonObject measure(const QString& name, int samples, const std::function<QWidget*()>& factory) {
    QVector<double> constructors, displays;
    int widgets = 0;
    for (int sample = -1; sample < samples; ++sample) {
        QElapsedTimer timer;
        timer.start();
        std::unique_ptr<QWidget> page(factory());
        const double constructed = ms(timer);
        page->resize(880, 760);
        page->show();
        drain();
        const double displayed = ms(timer);
        widgets = static_cast<int>(page->findChildren<QWidget*>().size());
        page.reset();
        drain();
        if (sample >= 0) {
            constructors.push_back(constructed);
            displays.push_back(displayed);
        }
    }
    QJsonObject row{{QStringLiteral("page"), name},
                    {QStringLiteral("construction_ms"), distribution(constructors)},
                    {QStringLiteral("first_display_ms"), distribution(displays)},
                    {QStringLiteral("widgets"), widgets}};
    std::cout << QJsonDocument(row).toJson(QJsonDocument::Compact).constData() << '\n';
    return row;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    const QDir fonts(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts"));
    if (QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("segoeui.ttf"))) < 0 ||
        QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("msyh.ttc"))) < 0)
        return 2;
#endif
    const auto args = app.arguments();
    const int outputIndex = args.indexOf(QStringLiteral("--output"));
    const int samplesIndex = args.indexOf(QStringLiteral("--samples"));
    const int samples = samplesIndex >= 0 ? args.value(samplesIndex + 1).toInt() : 11;
    if (samples < 3 || outputIndex < 0 || args.value(outputIndex + 1).isEmpty()) {
        std::cerr << "Usage: main page benchmark --output result.json [--samples 11]\n";
        return EXIT_FAILURE;
    }
    QTemporaryDir temporary;
    auto& store = storage::ApplicationStorage::instance();
    if (!temporary.isValid() ||
        !store.initialize({temporary.path(), temporary.path(), 60000}).success)
        return 3;
    snow_shot::presentation::LanguageManager::instance().initialize();
    snow_shot::presentation::styles::ThemeManager::instance().initialize(app);
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    // Keep the catalog pending locally so network latency cannot enter the measurement.
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost))
        return 4;
    SnowShotApiClient client(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
#endif
    drain();
    QJsonArray pages;
    pages.append(measure(QStringLiteral("about"), samples, [] { return new AboutPageWidget; }));
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    pages.append(measure(QStringLiteral("translation-pending-local-catalog"), samples,
                         [&] { return new TranslationPageWidget(nullptr, &client); }));
#endif
    pages.append(measure(QStringLiteral("screenshot-history-empty"), samples,
                         [] { return new ScreenshotHistoryPageWidget; }));
    pages.append(measure(QStringLiteral("pinned-management-empty"), samples,
                         [] { return new PinnedWindowManagementPageWidget; }));
    for (int count : {100, 10000}) {
        HistorySource history;
        PinnedSource pinned;
        for (int i = 0; i < count; ++i) {
            const auto id = QString::number(i);
            const auto date = QDateTime::fromSecsSinceEpoch(1791043200 - i, QTimeZone::UTC);
            storage::CaptureHistoryRecord record;
            record.id = id;
            record.createdUtc = date;
            record.result = storage::CaptureHistoryResultRecord{QSize(1920, 1080), 100000};
            history.data.push_back(record);
            storage::PinnedWindowSummary summary;
            summary.id = id;
            summary.createdUtc = date;
            summary.updatedUtc = date;
            summary.activitySequence = static_cast<quint64>(count - i);
            pinned.data.push_back(summary);
        }
        pages.append(measure(QStringLiteral("screenshot-history-synthetic-%1").arg(count), samples,
                             [&] { return new ScreenshotHistoryPageWidget(&history, nullptr); }));
        pages.append(
            measure(QStringLiteral("pinned-management-synthetic-%1").arg(count), samples,
                    [&] { return new PinnedWindowManagementPageWidget(&pinned, nullptr); }));
    }
    const QJsonObject report{
        {QStringLiteral("samples"), samples},
        {QStringLiteral("viewport"), QStringLiteral("880x760")},
        {QStringLiteral("platform"), QApplication::platformName()},
        {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
        {QStringLiteral("notes"),
         QStringLiteral("Warm process, fresh widgets; synthetic sources exclude repository reads "
                        "and thumbnail completion; local translation request remains pending.")},
        {QStringLiteral("pages"), pages}};
    QFile file(args.value(outputIndex + 1));
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(report).toJson()) < 0)
        return 5;
    store.shutdown();
    return 0;
}
