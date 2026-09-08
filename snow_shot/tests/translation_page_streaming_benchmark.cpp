#include "translation_test_support.h"

#include "snow_shot/presentation/components/translationpagewidget.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/translationpagecontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/input_text_edit.h"

#include <QApplication>
#include <QTemporaryDir>
#include <QTextDocument>

using namespace translation_tests;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(directory.isValid() &&
                storage.initialize({directory.path(), directory.path(), 60000}).success,
            "initialize isolated benchmark storage");
    snow_shot::presentation::styles::ThemeManager::instance().initialize(app);
    for (bool paragraphs : {false, true}) {
        Server server;
        SnowShotApiClient client(server.url());
        TranslationPageWidget page(nullptr, &client, 0);
        page.resize(900, 650);
        page.show();
        auto* controller = page.findChild<snow_shot::presentation::TranslationPageController*>();
        auto* result =
            page.findChild<adqt::widgets::AdTextEdit*>(QStringLiteral("translationResultText"));
        page.setSourceText(QString(5000, u'x'));
        waitUntil([&] { return server.streams.size() == 1; }, "start benchmark translation");
        const QString initial = paragraphs ? QStringLiteral("Translated paragraph.\n").repeated(200)
                                           : QString(4500, u'x');
        server.delta(0, initial);
        waitUntil([&] { return result->toPlainText() == initial; }, "render initial long output");
        int changes = 0;
        QObject::connect(result->document(), &QTextDocument::contentsChanged, &page,
                         [&] { ++changes; });
        QByteArray burst;
        constexpr int tokens = 512;
        for (int index = 0; index < tokens; ++index) {
            burst += "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n";
        }
        QElapsedTimer timer;
        timer.start();
        server.send(0, burst);
        server.finish(0);
        waitUntil([&] { return !controller->translating(); }, "complete benchmark stream");
        require(result->toPlainText() == initial + QString(tokens, u'x'), "retain all output");
        std::cout << (paragraphs ? "200 paragraphs" : "4500-character paragraph") << ": "
                  << timer.nsecsElapsed() / 1000000.0 << " ms, " << changes
                  << " document changes for " << tokens << " tokens\n";
    }
    storage.shutdown();
    return 0;
}
