#include "cloud_upload_test_support.h"
#include "snow_shot/app/edition.h"
#include "snow_shot/network/s3uploadprotocol.h"
#include "snow_shot/presentation/screenshotclouduploadservice.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/storage/configurationarchive.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/presentation/components/clouduploadsettingswidget.h"
#include "snow_shot/presentation/components/formfields.h"
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
#include "snow_shot/presentation/components/texttranslationsettingswidget.h"
#endif
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/button.h"
#include "widgets/modal.h"
#include "widgets/input_line_edit.h"
#include "widgets/combo_box.h"
#include <QApplication>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QLayout>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QSemaphore>
#include <QScopeGuard>
#include <QThread>
#include <cstring>
#include <cstdlib>
#include "snow_shot/presentation/screenshotrecognitionimage.h"

using namespace snow_shot;
using namespace translation_tests;
namespace {
using namespace cloud_upload_tests;
void settingsContracts() {
    const QString key = QStringLiteral("cloud_upload/configuration");
    auto profile = configuration();
    profile.sessionToken = QStringLiteral("session-secret");
    CloudUploadSettings settings{{profile}, profile.id};
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("settings.json"));
    {
        storage::ConfigurationStore store(path, true, true, 0);
        require(store.setValue(key, cloudUploadSettingsToJson(settings)),
                "accept cloud settings in this edition");
        auto duplicate = profile;
        duplicate.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        duplicate.name = profile.name.toUpper();
        require(!store.setValue(key, cloudUploadSettingsToJson({{profile, duplicate}, profile.id})),
                "reject duplicate names atomically");
        require(store.flushNow().success, "flush cloud settings");
    }
    storage::ConfigurationStore reopened(path, true, true, 0);
    require(cloudUploadSettingsFromJson(reopened.value(key)) == settings,
            "cloud settings roundtrip");
    for (const auto& endpoint :
         {QStringLiteral("https://user:pass@host"), QStringLiteral("https://host?a=1"),
          QStringLiteral("https://host#x"), QStringLiteral("ftp://host")})
        require(!validCloudUploadUrl(endpoint), "invalid cloud endpoint rejected");
    auto virtualInvalid = profile;
    virtualInvalid.addressingStyle = QStringLiteral("virtual");
    virtualInvalid.bucket = QStringLiteral("invalid@host");
    require(!cloudUploadConfigurationValid(virtualInvalid),
            "reject invalid virtual-host bucket names");
    auto missing = profile;
    missing.secretAccessKey.clear();
    require(cloudUploadConfigurationValid(missing) && !cloudUploadConfigurationUsable(missing),
            "redacted credentials retained but cannot upload");
    require(
        cloudUploadSettingsFromJson(cloudUploadSettingsToJson({{missing}, missing.id})).selected(),
        "load redacted selected profile without dropping it");
    auto broken = cloudUploadSettingsToJson(settings);
    auto profiles = broken.value(QStringLiteral("configurations")).toArray();
    profiles.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("bad")}});
    broken.insert(QStringLiteral("configurations"), profiles);
    bool valid = true;
    require(cloudUploadSettingsFromJson(broken, &valid) == settings && !valid,
            "salvage valid cloud profiles");
    const QMap<QString, QJsonValue> snapshot{{key, cloudUploadSettingsToJson(settings)}};
    const auto archive = dir.filePath(QStringLiteral("redacted.zip"));
    require(storage::ConfigurationArchive::write(archive, snapshot, 3, true).isEmpty(),
            "export redacted cloud settings");
    auto imported = storage::ConfigurationArchive::read(archive);
    require(imported.isValid(), "read cloud archive");
    auto redacted = cloudUploadSettingsFromJson(imported.values[key]);
    require(redacted.selected() && redacted.selected()->accessKeyId.isEmpty() &&
                redacted.selected()->secretAccessKey.isEmpty() &&
                redacted.selected()->sessionToken.isEmpty(),
            "redact cloud credentials");
    imported.preserveOmittedCredentials(snapshot);
    require(cloudUploadSettingsFromJson(imported.values[key]) == settings,
            "restore credentials only for same destination");
    imported = storage::ConfigurationArchive::read(archive);
    auto changed = settings;
    changed.configurations[0].bucket = QStringLiteral("other");
    imported.preserveOmittedCredentials({{key, cloudUploadSettingsToJson(changed)}});
    require(cloudUploadSettingsFromJson(imported.values[key]).selected()->secretAccessKey.isEmpty(),
            "destination change cannot borrow credentials");
    for (const auto* scope : {"screenshot", "pin_to_screen"}) {
        const auto shortcut = storage::ConfigurationSchema::defaultValue(
            QString::fromLatin1(scope) + QStringLiteral("_shortcuts/upload_to_cloud"));
        const auto bindings = shortcuts::shortcutBindingsFromJson(shortcut, false);
        require(bindings.size() == 1 && bindings.first().portableText == QStringLiteral("Ctrl+U"),
                "upload shortcut defaults to Ctrl+U");
    }
    for (const auto kind : {storage::ScreenshotToolbarLayoutKind::ActionTools,
                            storage::ScreenshotToolbarLayoutKind::PinnedActionTools}) {
        storage::ScreenshotToolbarLayout legacy;
        legacy.positions = {{QStringLiteral("quick-save"), QStringLiteral("save-as-file")}};
        const auto migrated = presentation::toolbar_layout::normalizedLayout(legacy, kind);
        require(migrated.positions.first().first() == QStringLiteral("upload-to-cloud"),
                "cloud appended to popover by reversed storage order");
        const auto stack = presentation::toolbar_layout::stackPresentation(
            migrated.positions.first(), [](const QString&) { return true; });
        require(stack.entryItemId() == QStringLiteral("save-as-file") &&
                    stack.popoverItemIds.last() == QStringLiteral("upload-to-cloud"),
                "retain Save primary and append upload option");
        legacy.positions.clear();
        legacy.hidden = {QStringLiteral("save-as-file")};
        require(presentation::toolbar_layout::normalizedLayout(legacy, kind)
                    .hidden.contains(QStringLiteral("upload-to-cloud")),
                "inherit hidden save group");
        legacy.positions = {{QStringLiteral("upload-to-cloud")}, {QStringLiteral("save-as-file")}};
        legacy.hidden.clear();
        require(presentation::toolbar_layout::normalizedLayout(legacy, kind).positions.first() ==
                    legacy.positions.first(),
                "preserve custom upload placement");
    }
}
void signingContracts() {
    auto config = configuration(QStringLiteral("https://s3.amazonaws.com"));
    config.bucket = QStringLiteral("examplebucket");
    config.addressingStyle = QStringLiteral("virtual");
    config.accessKeyId = QStringLiteral("AKIAIOSFODNN7EXAMPLE");
    config.secretAccessKey = QStringLiteral("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
    const auto request = s3_upload::signedPut(
        config, QStringLiteral("test$file.text"),
        "44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072", {},
        QDateTime::fromString(QStringLiteral("2013-05-24T00:00:00Z"), Qt::ISODate),
        {{"date", "Fri, 24 May 2013 00:00:00 GMT"}, {"x-amz-storage-class", "REDUCED_REDUNDANCY"}});
    require(
        request.rawHeader("Authorization")
            .endsWith("Signature=98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd"),
        "match published AWS S3 PUT signature vector");
    config.sessionToken = QStringLiteral("temporary-token");
    config.endpoint = QStringLiteral("https://host.test:8443/api");
    config.addressingStyle = QStringLiteral("path");
    const auto signedToken =
        s3_upload::signedPut(config, QString::fromUtf8("folder/中文 +%#?.png"), "hash", "image/png",
                             QDateTime::currentDateTimeUtc());
    require(signedToken.rawHeader("host") == "host.test:8443" &&
                signedToken.rawHeader("x-amz-security-token") == "temporary-token",
            "sign host port and session token");
    require(signedToken.url().toEncoded().contains("%20%2B%25%23%3F"),
            "encode reserved characters without form encoding");
    config.endpoint = QStringLiteral("https://host.test/a%2Fb/has+space");
    require(s3_upload::objectUrl(config, QStringLiteral("a.png"))
                .toEncoded()
                .contains("/a%2Fb/has%2Bspace/"),
            "preserve encoded endpoint separators and encode signing path punctuation");
    config.endpoint = QStringLiteral("https://[::1]:8443/root");
    require(s3_upload::signedPut(config, QStringLiteral("a.png"), "hash", "image/png",
                                 QDateTime::currentDateTimeUtc())
                    .rawHeader("host") == "[::1]:8443",
            "sign IPv6 host and port");
    config.endpoint = QString::fromUtf8("https://例子.test/root");
    require(s3_upload::signedPut(config, QStringLiteral("a.png"), "hash", "image/png",
                                 QDateTime::currentDateTimeUtc())
                    .rawHeader("host") == QUrl::toAce(QUrl(config.endpoint).host()),
            "sign IDN host in wire encoding");
    config.publicBaseUrl = QStringLiteral("https://cdn.test/assets/");
    require(s3_upload::objectUrl(config, QStringLiteral("folder/a.png"), true).toEncoded() ==
                "https://cdn.test/assets/folder/a.png",
            "public base represents bucket root");
}
void preparedSourcesAndCancellation() {
    QImage background(48, 32, QImage::Format_RGBA8888);
    background.fill(QColor(24, 70, 130));
    ScreenshotRecognitionImageSnapshot recognition;
    recognition.image = background;
    recognition.canvasRect = QRectF(background.rect());
    recognition.filteredImage = background;
    recognition.filteredImage.fill(QColor(200, 30, 50));
    recognition.filteredCanvasRect = recognition.canvasRect;
    QObject receiver;
    S3Server server;
    auto* job = ScreenshotCloudUploadService::upload(
        std::make_shared<ScreenshotExportArtifact>(
            ScreenshotExportSource::fromRecognitionImage(recognition)),
        configuration(server.url()), {}, &receiver);
    bool done = false;
    QObject::connect(job, &ScreenshotCloudUploadJob::finished, &receiver, [&](const auto& result) {
        require(result.status == ScreenshotCloudUploadResult::Status::Succeeded,
                "upload recognition export source");
        done = true;
    });
    waitUntil([&] { return done; }, "finish recognition source upload");
    require(QImage::fromData(server.body).pixelColor(2, 2) ==
                recognition.filteredImage.pixelColor(2, 2),
            "upload applicable recognition overlay pixels");

    // Hold a file encoder after its temporary directory has been created.
    // Cancellation must keep that directory alive until the worker unwinds.
    auto entered = std::make_shared<QSemaphore>();
    auto release = std::make_shared<QSemaphore>();
    const auto unblock = qScopeGuard([&] { release->release(); });
    ScreenshotImageRowSource rows;
    rows.size = background.size();
    rows.readRows = [background, entered, release](int first, int count, qsizetype stride,
                                                   uchar* target, qsizetype) {
        entered->release();
        release->acquire();
        for (int i = 0; i < count; ++i)
            std::memcpy(target + stride * i, background.constScanLine(first + i),
                        static_cast<size_t>(background.width()) * 4);
        return true;
    };
    auto artifact = std::make_shared<ScreenshotExportArtifact>(
        ScreenshotExportSource::fromProducer({}, [rows](std::function<bool()> cancel) mutable {
            rows.cancellationRequested = std::move(cancel);
            return rows;
        }));
    ScreenshotCloudUploadOptions options;
    options.format = ScreenshotImageFileFormat::Bmp;
    QPointer<ScreenshotCloudUploadJob> pending =
        ScreenshotCloudUploadService::upload(artifact, configuration(), options, &receiver);
    waitUntil([&] { return entered->available() > 0; }, "start background file encoding");
    const QString directory = QFileInfo(pending->preparedPath()).absolutePath();
    pending->cancel();
    artifact.reset();
    flushEvents();
    require(!pending && QDir(directory).exists(),
            "cancelled encoder retains temporary directory while running");
    release->release(100);
    waitUntil([&] { return !QDir(directory).exists(); },
              "clean temporary directory after cancelled encoder unwinds");

    auto* destroyedReceiver = new QObject;
    S3Server held;
    held.hold = true;
    auto* abandoned = ScreenshotCloudUploadService::upload(
        std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(background)),
        configuration(held.url()), {}, destroyedReceiver);
    bool stale = false;
    QObject::connect(abandoned, &ScreenshotCloudUploadJob::finished, &receiver,
                     [&](const auto&) { stale = true; });
    waitUntil([&] { return held.count == 1; }, "upload owned by source receiver");
    const auto path = abandoned->preparedPath();
    delete destroyedReceiver;
    flushEvents();
    require(!stale && !QFileInfo::exists(path),
            "destroyed source suppresses callback and cleans payload");
    auto* closingReceiver = new QObject;
    bool closed = false;
    auto* closingJob = ScreenshotCloudUploadService::upload(
        std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(background)),
        configuration(), {}, closingReceiver, [&](const ScreenshotCloudUploadResult& result) {
            require(result.status == ScreenshotCloudUploadResult::Status::Cancelled,
                    "coordinator completion receives cancellation");
            closed = true;
            delete closingReceiver;
        });
    closingJob->cancel();
    flushEvents();
    require(closed, "completion may close its source receiver safely");

    auto* progressReceiver = new QObject;
    QPointer<ScreenshotCloudUploadJob> progressJob = ScreenshotCloudUploadService::upload(
        std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(background)),
        configuration(), {}, progressReceiver);
    QString progressPath;
    QObject::connect(progressJob, &ScreenshotCloudUploadJob::progress, &receiver, [&](int) {
        progressPath = progressJob->preparedPath();
        delete progressReceiver;
    });
    waitUntil([&] { return !progressJob; }, "progress callback may close its source receiver");
    waitUntil([&] { return !QFileInfo::exists(progressPath); },
              "clean payload after source closes during preparation progress");
}
void uploadContracts() {
    QImage image(23, 19, QImage::Format_RGBA8888);
    image.fill(QColor(24, 70, 130));
    for (const auto format : {ScreenshotImageFileFormat::Png, ScreenshotImageFileFormat::Jpeg,
                              ScreenshotImageFileFormat::Webp, ScreenshotImageFileFormat::Jxl,
                              ScreenshotImageFileFormat::Avif, ScreenshotImageFileFormat::Bmp,
                              ScreenshotImageFileFormat::Pdf}) {
        S3Server server;
        QObject receiver;
        auto config = configuration(server.url());
        config.keyPrefix = QString::fromUtf8("screenshots/中文 +%");
        config.publicBaseUrl = QStringLiteral("https://cdn.test/");
        ScreenshotCloudUploadOptions options;
        options.format = format;
        auto* job = ScreenshotCloudUploadService::upload(
            std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(image)),
            config, options, &receiver);
        bool done = false;
        ScreenshotCloudUploadResult result;
        QString path;
        QObject::connect(job, &ScreenshotCloudUploadJob::progress, &receiver,
                         [job, &path](int) { path = job->preparedPath(); });
        QObject::connect(job, &ScreenshotCloudUploadJob::finished, &receiver,
                         [&](const auto& value) {
                             result = value;
                             done = true;
                         });
        waitUntil([&] { return done; }, "complete encoded cloud upload");
        require(result.status == ScreenshotCloudUploadResult::Status::Succeeded &&
                    server.count == 1,
                "upload succeeds once");
        require(server.headers.startsWith("PUT /root/images/screenshots/") &&
                    !server.headers.toLower().contains("x-amz-acl"),
                "PUT existing bucket without changing access policy");
        const auto mime =
            format == ScreenshotImageFileFormat::Pdf ? QByteArray("application/pdf")
            : format == ScreenshotImageFileFormat::Jpeg
                ? QByteArray("image/jpeg")
                : QByteArray("image/") + ScreenshotImageFileService::extension(format).toLatin1();
        require(server.headers.toLower().contains("content-type: " + mime),
                "send encoded image MIME type");
        require(server.headers.contains(
                    QCryptographicHash::hash(server.body, QCryptographicHash::Sha256).toHex()),
                "sign exact uploaded bytes");
        require(
            result.url.host() == QStringLiteral("cdn.test") &&
                result.url.path().endsWith(u'.' + ScreenshotImageFileService::extension(format)),
            "copyable permanent link has correct format");
        if (format == ScreenshotImageFileFormat::Png)
            require(QImage::fromData(server.body).pixelColor(2, 2) == image.pixelColor(2, 2),
                    "upload prepared image pixels");
        if (format == ScreenshotImageFileFormat::Pdf)
            require(server.body.startsWith("%PDF"), "upload encoded PDF");
        flushEvents();
        require(!path.isEmpty() && !QFileInfo::exists(path),
                "remove temporary file after successful upload");
    }
    for (const int status : {403, 307}) {
        S3Server server;
        server.status = status;
        QObject receiver;
        auto* job = ScreenshotCloudUploadService::upload(
            std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(image)),
            configuration(server.url()), {}, &receiver);
        bool done = false;
        QObject::connect(job, &ScreenshotCloudUploadJob::finished, &receiver,
                         [&](const auto& result) {
                             require(result.status == ScreenshotCloudUploadResult::Status::Failed &&
                                         !result.error.contains(QStringLiteral("secret")),
                                     "safe failure for denied upload or redirect");
                             done = true;
                         });
        waitUntil([&] { return done; }, "complete failed upload");
        require(server.count == 1, "no retry or redirect");
    }
    for (const bool timeout : {false, true}) {
        S3Server server;
        server.hold = true;
        QObject receiver;
        ScreenshotCloudUploadOptions options;
        options.transferTimeoutMs = timeout ? 50 : 60000;
        QImage payloadImage = image;
        if (!timeout) {
            payloadImage = QImage(769, 515, QImage::Format_RGBA8888);
            payloadImage.fill(QColor(24, 70, 130));
            options.format = ScreenshotImageFileFormat::Bmp;
        }
        auto artifact = std::make_shared<ScreenshotExportArtifact>(
            ScreenshotExportSource::fromImage(std::move(payloadImage)));
        const std::weak_ptr<ScreenshotExportArtifact> preparedArtifact = artifact;
        auto* job = ScreenshotCloudUploadService::upload(
            std::move(artifact), configuration(server.url()), options, &receiver);
        bool done = false;
        QString path;
        QObject::connect(
            job, &ScreenshotCloudUploadJob::finished, &receiver, [&](const auto& result) {
                require(result.status == (timeout ? ScreenshotCloudUploadResult::Status::Failed
                                                  : ScreenshotCloudUploadResult::Status::Cancelled),
                        "timeout and cancellation have distinct terminal states");
                done = true;
            });
        waitUntil([&] { return server.count == 1; }, "start pending upload");
        require(preparedArtifact.expired(),
                "release source pixels before network transfer finishes");
        if (!timeout) {
            require(
                server.body.size() > 1024 * 1024 &&
                    server.headers.contains(
                        QCryptographicHash::hash(server.body, QCryptographicHash::Sha256).toHex()),
                "stream and sign a payload spanning multiple hash chunks");
        }
        path = job->preparedPath();
        if (!timeout)
            job->cancel();
        waitUntil([&] { return done; }, "terminate pending upload");
        flushEvents();
        require(!QFileInfo::exists(path), "remove cancelled upload file");
    }
    {
        QObject receiver;
        QPointer<ScreenshotCloudUploadJob> job = ScreenshotCloudUploadService::upload(
            std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(image)),
            configuration(), {}, &receiver);
        bool cancelled = false;
        QObject::connect(
            job, &ScreenshotCloudUploadJob::finished, &receiver, [&](const auto& result) {
                cancelled = result.status == ScreenshotCloudUploadResult::Status::Cancelled;
            });
        job->cancel();
        flushEvents();
        require(cancelled && !job, "cancel before preparation starts");
    }
}
void widgetContracts(QApplication& app) {
    QTemporaryDir directory;
    auto& store = storage::ApplicationStorage::instance();
    const auto shutdown = qScopeGuard([&] { store.shutdown(); });
    require(store.initialize({directory.path(), directory.path(), 0}).success,
            "initialize cloud settings storage");
    presentation::styles::ThemeManager::instance().initialize(app);
    presentation::LanguageManager::instance().initialize();
    require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("en_US")),
            "English settings");
    presentation::GlobalShortcutManager shortcuts;
    presentation::settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = presentation::settings::buildBuiltInSettingsRegistry();
    presentation::settings::SettingsRuntimeSession session(registry, backend);
    CloudUploadSettingsWidget widget(session);
    const auto scheme = presentation::styles::ThemeManager::instance().themeColorScheme();
    widget.applyTheme(scheme);
    widget.resize(760, 800);
    widget.show();
    flushEvents();
    const auto previewIndex = app.arguments().indexOf(QStringLiteral("--preview-dir"));
    const auto savePreview = [&](const QString& name) {
        if (previewIndex < 0 || previewIndex + 1 >= app.arguments().size())
            return;
        QDir output(app.arguments()[previewIndex + 1]);
        require(output.mkpath(QStringLiteral(".")), "create cloud settings preview directory");
        const QSize previousSize = widget.size();
        widget.resize(previousSize.width(), widget.sizeHint().height());
        flushEvents();
        require(widget.grab().save(output.filePath(name)), "save cloud settings preview");
        widget.resize(previousSize);
        flushEvents();
    };
    auto* title = widget.findChild<QLabel*>(QStringLiteral("cloudUploadConfigurationsTitle"));
    require(title && title->text() == QStringLiteral("Upload Configurations") &&
                title->font().pixelSize() == scheme.metricAlias.fontSize &&
                title->font().weight() == QFont::DemiBold,
            "cloud configurations have the same subtitle style as translation configurations");
    auto* empty = widget.findChild<QLabel*>(QStringLiteral("cloudUploadConfigurationsEmpty"));
    require(empty && empty->font().pixelSize() == scheme.metricAlias.fontSize &&
                empty->palette().color(QPalette::WindowText) == scheme.map.colorText,
            "cloud empty state uses the settings typography and text color");
#if SNOW_SHOT_ENABLE_TEXT_TRANSLATION
    TextTranslationSettingsWidget translationWidget(session);
    translationWidget.applyTheme(scheme);
    auto* translationEmpty =
        translationWidget.findChild<QLabel*>(QStringLiteral("textTranslationConfigurationsEmpty"));
    require(empty && translationEmpty && empty->font() == translationEmpty->font() &&
                empty->palette().color(QPalette::WindowText) ==
                    translationEmpty->palette().color(QPalette::WindowText),
            "cloud and translation empty states use the same typography and text color");
#endif
    auto* defaultRow =
        widget.findChild<QWidget*>(QStringLiteral("settings-item-cloudUploadDefault"));
    auto* destination =
        widget.findChild<adqt::widgets::AdComboBox*>(QStringLiteral("cloudUploadDefault"));
    require(defaultRow && destination, "default destination uses a standard settings row");
    QLabel* defaultTitle = nullptr;
    QLabel* defaultDescription = nullptr;
    const auto descriptionSource = "Choose the configuration used for cloud uploads";
    for (auto* label : defaultRow->findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("Default destination"))
            defaultTitle = label;
        if (label->text() == QString::fromUtf8(descriptionSource))
            defaultDescription = label;
    }
    require(defaultDescription && defaultDescription->isVisible() &&
                defaultDescription->font().pixelSize() == scheme.metricAlias.fontSize &&
                defaultDescription->palette().color(QPalette::WindowText) ==
                    scheme.map.colorTextSecondary,
            "default upload description uses the shared settings description style");
    require(defaultTitle && defaultTitle->font().pixelSize() == scheme.metricAlias.fontSizeLG &&
                defaultTitle->font().weight() == QFont::Medium &&
                defaultTitle->mapTo(&widget, QPoint()).x() <
                    destination->mapTo(&widget, QPoint()).x() &&
                std::abs(defaultTitle->parentWidget()
                             ->mapTo(&widget, defaultTitle->parentWidget()->rect().center())
                             .y() -
                         destination->mapTo(&widget, destination->rect().center()).y()) <= 1 &&
                destination->width() == 230,
            "default destination aligns its title and fixed-width control horizontally");
    for (const auto& language : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        require(presentation::LanguageManager::instance().setLanguage(language),
                "translate cloud settings subtitle");
        flushEvents();
        require(defaultDescription->text() == QCoreApplication::translate(
                                                  "CloudUploadSettingsWidget", descriptionSource) &&
                    defaultDescription->isVisible(),
                "default upload description retranslates in place");
        require(title->text() == (language == QStringLiteral("zh_CN")
                                      ? QString::fromUtf8("上传配置")
                                      : QString::fromUtf8("上傳配置")),
                "cloud settings subtitle translates on language change");
        require(widget.findChild<QLabel*>(QStringLiteral("cloudUploadConfigurationsEmpty"))
                        ->font()
                        .pixelSize() == scheme.metricAlias.fontSize,
                "retranslated cloud empty state preserves the shared font size");
    }
    savePreview(QStringLiteral("empty-zh-TW.png"));
    require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("en_US")),
            "restore cloud settings language");
    flushEvents();
    auto* add = widget.findChild<adqt::widgets::AdButton*>(QStringLiteral("cloudUploadAdd"));
    require(add && session.cloudUploadSettings().configurations.isEmpty(),
            "cloud settings initial state");
    adqt::widgets::AdButton currentFocus(&widget);
    currentFocus.show();
    add->click();
    flushEvents();
    auto* cancelled =
        widget.findChild<adqt::widgets::AdModal*>(QStringLiteral("cloudUploadEditor"));
    require(cancelled, "open cloud editor for dismissal regression");
    currentFocus.setFocus(Qt::MouseFocusReason);
    require(currentFocus.hasFocus(), "establish focus before dismissing cloud editor");
    cancelled->reject();
    flushEvents();
    require(currentFocus.hasFocus(), "cloud editor dismissal must not refocus Add");
    add->click();
    flushEvents();
    auto* editor = widget.findChild<adqt::widgets::AdModal*>(QStringLiteral("cloudUploadEditor"));
    require(editor, "open cloud editor");
    const auto assertEditorColumns = [](adqt::widgets::AdModal* modal) {
        flushEvents();
        auto* body = modal->contentWidget();
        body->layout()->activate();
        QMap<QString, QWidget*> views;
        for (auto* field :
             body->findChildren<presentation::components::form_fields::FormField*>()) {
            views.insert(field->metadata().id, field->viewWidget());
            auto* mark =
                field->item()->findChild<QLabel*>(QStringLiteral("ad-form-item-required-mark"));
            require(mark && mark->isVisible() == field->item()->required() &&
                        (!field->item()->required() || mark->text() == QStringLiteral("*")),
                    "cloud form shows Ant Design required marks only for required fields");
        }
        const QStringList ids{QStringLiteral("configurationName"),
                              QStringLiteral("endpoint"),
                              QStringLiteral("region"),
                              QStringLiteral("bucket"),
                              QStringLiteral("accessKeyId"),
                              QStringLiteral("secretAccessKey"),
                              QStringLiteral("sessionToken"),
                              QStringLiteral("keyPrefix"),
                              QStringLiteral("publicBaseUrl"),
                              QStringLiteral("addressingStyle"),
                              QStringLiteral("uploadProtocol")};
        require(views.size() == ids.size(), "cloud editor retains all eleven form fields");
        for (qsizetype index = 0; index + 1 < ids.size(); index += 2) {
            auto* left = views.value(ids[index]);
            auto* right = views.value(ids[index + 1]);
            require(left && right && left->isVisible() && right->isVisible() &&
                        left->y() == right->y() && right->x() > left->geometry().right() &&
                        std::abs(left->width() - right->width()) <= 1,
                    "cloud upload modal arranges its form fields in two equal columns");
            if (index > 0)
                require(left->y() > views.value(ids[index - 2])->geometry().bottom(),
                        "cloud form field pairs occupy successive rows");
        }
        auto* protocol = views.value(ids.last());
        require(protocol && protocol->x() == views.value(ids.first())->x() &&
                    protocol->y() >
                        views.value(QStringLiteral("publicBaseUrl"))->geometry().bottom(),
                "last cloud form field starts the next row in the left column");
    };
    assertEditorColumns(editor);
    editor->acceptButton()->click();
    flushEvents();
    require(session.cloudUploadSettings().configurations.isEmpty() && editor->isOpen(),
            "required fields reject empty cloud profile");
    assertEditorColumns(editor);
    require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("zh_CN")),
            "translate existing cloud validation feedback");
    flushEvents();
    const auto labels = editor->contentWidget()->findChildren<QLabel*>();
    const auto translatedRequired =
        QCoreApplication::translate("CloudUploadSettingsWidget", "This field is required.");
    require(
        std::any_of(labels.cbegin(), labels.cend(),
                    [&](const auto* label) { return label->text().contains(translatedRequired); }),
        "cloud validation feedback retranslates in place");
    require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("en_US")),
            "restore cloud validation language");
    flushEvents();
    const auto fill = [editor](const QString& id, const QString& text) {
        auto* input = editor->contentWidget()->findChild<adqt::widgets::AdLineEdit*>(id);
        require(input, "shared cloud editor field");
        input->setText(text);
    };
    fill(QStringLiteral("configurationName"), QStringLiteral("Main"));
    fill(QStringLiteral("endpoint"), QStringLiteral("https://s3.example.test"));
    fill(QStringLiteral("bucket"), QStringLiteral("images"));
    fill(QStringLiteral("accessKeyId"), QStringLiteral("ACCESS"));
    fill(QStringLiteral("secretAccessKey"), QStringLiteral("secret"));
    require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("zh_CN")),
            "retranslate open cloud editor");
    flushEvents();
    assertEditorColumns(editor);
    if (previewIndex >= 0 && previewIndex + 1 < app.arguments().size())
        require(editor->contentWidget()->window()->grab().save(
                    QDir(app.arguments()[previewIndex + 1])
                        .filePath(QStringLiteral("upload-editor-zh-CN.png"))),
                "save two-column cloud upload modal preview");
    require(editor->windowTitle() == QString::fromUtf8("添加云上传配置") &&
                editor->contentWidget()
                        ->findChild<adqt::widgets::AdLineEdit*>(QStringLiteral("endpoint"))
                        ->text() == QStringLiteral("https://s3.example.test"),
            "language change preserves cloud form values");
    require(session.cloudUploadSettings().configurations.isEmpty() && editor->isOpen(),
            "retranslation validates without saving a usable draft");
    require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("en_US")),
            "restore cloud editor language");
    flushEvents();
    editor->acceptButton()->click();
    waitUntil([&] { return session.cloudUploadSettings().configurations.size() == 1; },
              "save usable configuration");
    const auto original = session.cloudUploadSettings().configurations.first();
    require(session.cloudUploadSettings().defaultId == original.id,
            "first usable destination selected");
    flushEvents();
    auto* copy = widget.findChild<adqt::widgets::AdButton*>(QStringLiteral("copy:") + original.id);
    require(copy, "copy cloud configuration action");
    QPointer<adqt::widgets::AdButton> originalCopy(copy);
    widget.activateWindow();
    flushEvents();
    copy->setFocus();
    require(QApplication::focusWidget() == copy, "focus the cloud action before synchronization");
    for (const auto phase : {presentation::settings::SettingsWritePhase::Pending,
                             presentation::settings::SettingsWritePhase::Clean}) {
        auto state = session.state(QStringLiteral("screenshot-output.cloud-upload"));
        state.phase = phase;
        state.busy = phase == presentation::settings::SettingsWritePhase::Pending;
        state.dirty = state.busy;
        emit session.fieldChanged(QStringLiteral("screenshot-output.cloud-upload"), state);
        flushEvents();
        require(originalCopy && QApplication::focusWidget() == originalCopy,
                "unchanged cloud persistence notifications preserve actions and focus");
    }
    for (const auto& defaultId : {QString(), original.id}) {
        auto selection = session.cloudUploadSettings();
        selection.defaultId = defaultId;
        require(session.applyCloudUploadSettings(selection), "change cloud default destination");
        flushEvents();
        require(originalCopy && QApplication::focusWidget() == originalCopy &&
                    destination->currentValue().toString() == defaultId,
                "changing cloud destination synchronizes selection without recreating rows");
    }
    copy->click();
    waitUntil([&] { return session.cloudUploadSettings().configurations.size() == 2; },
              "copy saves distinct identity");
    require(session.cloudUploadSettings().configurations.last().id != original.id &&
                session.cloudUploadSettings().defaultId == original.id,
            "copy preserves selected destination");
    flushEvents();
    require(widget.findChild<QWidget*>(QStringLiteral("cloudUploadRow:") +
                                       session.cloudUploadSettings().configurations.last().id),
            "a changed cloud list renders its new configuration");
    auto* edit = widget.findChild<adqt::widgets::AdButton*>(QStringLiteral("edit:") + original.id);
    edit->click();
    flushEvents();
    editor = widget.findChild<adqt::widgets::AdModal*>(QStringLiteral("cloudUploadEditor"));
    require(editor, "edit cloud configuration");
    assertEditorColumns(editor);
    editor->contentWidget()
        ->findChild<adqt::widgets::AdLineEdit*>(QStringLiteral("configurationName"))
        ->setText(QStringLiteral("Renamed"));
    editor->acceptButton()->click();
    waitUntil(
        [&] {
            return session.cloudUploadSettings().configurations.first().name ==
                   QStringLiteral("Renamed");
        },
        "rename profile");
    require(session.cloudUploadSettings().defaultId == original.id,
            "editing keeps identity selection");
    flushEvents();
    widget.findChild<adqt::widgets::AdButton*>(QStringLiteral("delete:") + original.id)->click();
    flushEvents();
    auto* remove =
        widget.findChild<adqt::widgets::AdModal*>(QStringLiteral("cloudUploadDeleteModal"));
    require(remove, "delete cloud confirmation");
    currentFocus.setFocus(Qt::MouseFocusReason);
    remove->reject();
    flushEvents();
    require(currentFocus.hasFocus(), "cloud delete dismissal must not refocus Add");
    widget.findChild<adqt::widgets::AdButton*>(QStringLiteral("delete:") + original.id)->click();
    flushEvents();
    remove = widget.findChild<adqt::widgets::AdModal*>(QStringLiteral("cloudUploadDeleteModal"));
    require(remove, "reopen cloud delete confirmation");
    remove->acceptButton()->click();
    waitUntil([&] { return session.cloudUploadSettings().configurations.size() == 1; },
              "delete selected destination");
    require(session.cloudUploadSettings().defaultId.isEmpty() &&
                !ScreenshotCloudUploadService::configuration(),
            "deleting default disables upload without changing destination");
    auto gridValues = session.cloudUploadSettings();
    for (int index = 0; index < 2; ++index) {
        auto value = gridValues.configurations.first();
        value.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        value.name =
            QStringLiteral("A long upload destination name ").repeated(5) + QString::number(index);
        gridValues.configurations.push_back(value);
    }
    require(session.applyCloudUploadSettings(gridValues), "populate three upload configurations");
    flushEvents();
    const auto assertList = [&] {
        flushEvents();
        widget.layout()->activate();
        auto* first = widget.findChild<QWidget*>(QStringLiteral("cloudUploadRow:") +
                                                 gridValues.configurations[0].id);
        auto* second = widget.findChild<QWidget*>(QStringLiteral("cloudUploadRow:") +
                                                  gridValues.configurations[1].id);
        auto* third = widget.findChild<QWidget*>(QStringLiteral("cloudUploadRow:") +
                                                 gridValues.configurations[2].id);
        require(first && second && third && first->x() == second->x() && third->x() == first->x() &&
                    second->y() > first->geometry().bottom() &&
                    third->y() > second->geometry().bottom() && first->width() == second->width() &&
                    second->width() == third->width(),
                "upload configurations retain a single full-width column");
        for (auto* row : {first, second, third})
            for (auto* button : row->findChildren<adqt::widgets::AdButton*>())
                require(row->rect().contains(button->geometry()),
                        "long configuration names keep every action inside its row");
    };
    assertList();
    destination->setCurrentValue(gridValues.configurations[1].id);
    flushEvents();
    require(session.cloudUploadSettings().defaultId == gridValues.configurations[1].id,
            "standard default destination row persists selection");
    widget.resize(600, 800);
    flushEvents();
    assertList();
    require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("zh_CN")),
            "translate populated cloud settings list");
    flushEvents();
    assertList();
    savePreview(QStringLiteral("configurations-zh-CN.png"));
    if (previewIndex >= 0 && previewIndex + 1 < app.arguments().size()) {
        SettingsPageWidget page(registry, QStringLiteral("files-history"), session);
        page.resize(880, 900);
        page.show();
        page.reveal({page.pageId(), QStringLiteral("screenshots"),
                     QStringLiteral("screenshot-output.cloud-upload")});
        flushEvents();
        flushEvents();
        require(page.findChild<CloudUploadSettingsWidget*>(),
                "render cloud settings in export page");
        require(page.grab().save(QDir(app.arguments()[previewIndex + 1])
                                     .filePath(QStringLiteral("export-page-zh-CN.png"))),
                "save cloud settings page preview");
    }
    require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("en_US")),
            "restore populated cloud settings language");
    require(session.reset(presentation::settings::SettingsSectionReset::ScreenshotOutput),
            "reset Image Export");
    waitUntil([&] { return session.cloudUploadSettings().configurations.isEmpty(); },
              "reset removes cloud configuration");
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    static_cast<void>(
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")));
    static_cast<void>(
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")));
#endif
    if (!app.arguments().contains(QStringLiteral("--settings-only"))) {
        settingsContracts();
        signingContracts();
        uploadContracts();
        preparedSourcesAndCancellation();
    }
    widgetContracts(app);
    std::cout << "Cloud upload tests passed\n";
    return 0;
}
