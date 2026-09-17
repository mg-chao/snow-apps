#include "snow_shot/update/updatecontract.h"
#include "snow_shot/update/updateerrors.h"
#include "snow_shot/update/updatetransaction.h"
#include "snow_shot/update/updateservice.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
#include <Windows.h>
#include <bcrypt.h>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace snow_shot::update;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename F> void rejects(F&& action, const char* message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
}
QString hash(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QJsonObject descriptor(const QString& path, const QByteArray& bytes) {
    return {{"path", path}, {"size", bytes.size()}, {"sha256", hash(bytes)}};
}
void put(const QString& root, const QString& name, const QByteArray& bytes) {
    const auto path = QDir(root).filePath(name);
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create fixture path");
    writeAtomic(path, bytes);
}

struct Signer {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    QByteArray publicKeys;
    Signer() {
        require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) >= 0 &&
                    BCryptGenerateKeyPair(algorithm, &key, 3072, 0) >= 0 &&
                    BCryptFinalizeKeyPair(key, 0) >= 0,
                "generate isolated test signing key");
        ULONG length = 0;
        require(BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &length, 0) >= 0,
                "measure public key");
        QByteArray bytes(static_cast<qsizetype>(length), 0);
        require(BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                                reinterpret_cast<PUCHAR>(bytes.data()), length, &length, 0) >= 0,
                "export public key");
        const auto* header = reinterpret_cast<const BCRYPT_RSAKEY_BLOB*>(bytes.constData());
        const auto exponent = bytes.mid(sizeof(BCRYPT_RSAKEY_BLOB), header->cbPublicExp);
        const auto modulus =
            bytes.mid(sizeof(BCRYPT_RSAKEY_BLOB) + header->cbPublicExp, header->cbModulus);
        publicKeys =
            QJsonDocument(
                QJsonObject{{"keys", QJsonArray{QJsonObject{
                                         {"id", "test"},
                                         {"modulus", QString::fromLatin1(modulus.toBase64())},
                                         {"exponent", QString::fromLatin1(exponent.toBase64())}}}}})
                .toJson();
    }
    ~Signer() {
        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    QByteArray sign(const QJsonObject& payload) {
        const auto bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        BCRYPT_PSS_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM, 32};
        QByteArray signature(384, 0);
        ULONG length = 0;
        require(BCryptSignHash(key, &padding, reinterpret_cast<PUCHAR>(digest.data()),
                               static_cast<ULONG>(digest.size()),
                               reinterpret_cast<PUCHAR>(signature.data()), 384, &length,
                               BCRYPT_PAD_PSS) >= 0,
                "sign fixture release");
        return QJsonDocument(QJsonObject{{"schema", 1},
                                         {"keyId", "test"},
                                         {"payload", QString::fromLatin1(bytes.toBase64())},
                                         {"signature", QString::fromLatin1(signature.toBase64())}})
            .toJson(QJsonDocument::Compact);
    }
};

struct Fixture {
    QTemporaryDir temporary;
    QString root;
    QString archive;
    QJsonObject payload;
    QByteArray envelope;
    UpdateRelease release;
    Fixture(Signer& signer, QString variant = QStringLiteral("portable")) {
        require(temporary.isValid(), "temporary update fixture");
        root = temporary.filePath(QStringLiteral("installed app 安"));
        require(QDir().mkpath(root), "create install fixture");
        QJsonArray oldFiles{descriptor("bin/snow_shot.exe", "old executable"),
                            descriptor("bin/obsolete.txt", "old data")};
        put(root, "bin/snow_shot.exe", "old executable");
        put(root, "bin/obsolete.txt", "old data");
        put(root, "bin/portable/user.json", "precious settings");
        put(root, "bin/__data_directory", "portable");
        put(root, "user-file.txt", "preserve me");
        put(root, "snow-shot-installation.json",
            QJsonDocument(QJsonObject{{"schema", 1},
                                      {"version", "1.0.0-beta"},
                                      {"variant", variant},
                                      {"files", oldFiles}})
                .toJson());
        QMap<QString, QByteArray> files{{"bin/snow_shot.exe", "new executable"},
                                        {"bin/snow-shot-updater.exe", "new updater"},
                                        {"bin/new.txt", "new data"}};
        if (variant == u"portable") {
            files.insert("bin/__data_directory", "portable");
        }
        QJsonArray owned;
        for (auto it = files.begin(); it != files.end(); ++it) {
            owned.append(descriptor(it.key(), it.value()));
        }
        files.insert("snow-shot-installation.json",
                     QJsonDocument(QJsonObject{{"schema", 1},
                                               {"version", "1.0.0-beta.1"},
                                               {"variant", variant},
                                               {"files", owned}})
                         .toJson());
        QJsonArray inventory;
        for (auto it = files.begin(); it != files.end(); ++it) {
            inventory.append(descriptor(it.key(), it.value()));
        }
        archive = temporary.filePath("update.zip");
        void* writer = mz_zip_writer_create();
        require(mz_zip_writer_open_file(writer, QFile::encodeName(archive).constData(), 0, 0) ==
                    MZ_OK,
                "open fixture ZIP");
        for (auto it = files.begin(); it != files.end(); ++it) {
            mz_zip_file info{};
            info.filename = it.key().toUtf8().constData();
            info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
            require(mz_zip_writer_add_buffer(writer, const_cast<char*>(it.value().constData()),
                                             it.value().size(), &info) == MZ_OK,
                    "write fixture ZIP entry");
        }
        mz_zip_writer_close(writer);
        mz_zip_writer_delete(&writer);
        QJsonArray packages;
        for (const QString& v :
             {QStringLiteral("online"), QStringLiteral("offline"), QStringLiteral("portable")}) {
            const QStringList kinds =
                v == u"portable" ? QStringList{"portable"} : QStringList{"installer", "update"};
            for (const auto& kind : kinds) {
                const QString suffix = kind == u"installer" ? QStringLiteral(".exe")
                                       : kind == u"update"  ? QStringLiteral("-update.zip")
                                                            : QStringLiteral(".zip");
                QJsonObject package{{"variant", v},
                                    {"kind", kind},
                                    {"path", "setup/snow-shot_windows-x64-" + v + suffix},
                                    {"size", QFileInfo(archive).size()},
                                    {"sha256", sha256File(archive)}};
                if (kind != u"installer") {
                    package.insert("files", inventory);
                }
                packages.append(package);
            }
        }
        payload = {{"schema", 1},
                   {"version", "1.0.0-beta.1"},
                   {"publishedAt", "2026-09-10T00:00:00Z"},
                   {"platform", "windows-x64"},
                   {"packages", packages}};
        envelope = signer.sign(payload);
        release = verifyRelease(envelope, signer.publicKeys);
    }
    void preserved() const {
        require(readLimited(QDir(root).filePath("bin/portable/user.json")) == "precious settings",
                "preserve portable data");
        require(readLimited(QDir(root).filePath("user-file.txt")) == "preserve me",
                "preserve unknown user files");
    }
};

// The Rust contract is the single verifier; cross-check it against the
// in-process C++ signer and the strict SemVer and inventory rules.
void contractThroughFfi(Signer& signer) {
    Fixture fixture(signer);
    require(fixture.release.version == u"1.0.0-beta.1", "verify fixture through FFI");
    require(compareVersions("1.0.0-beta.1", "1.0.0-beta") > 0, "FFI SemVer ordering");
    require(compareVersions("1.0.0+build.1", "1.0.0+build.2") == 0, "FFI ignores build metadata");
    rejects([&] { compareVersions("1.0", "1.0.0"); }, "FFI rejects malformed versions");
    require(fixture.release.updatePackage(QStringLiteral("portable")).sha256 ==
                sha256File(fixture.archive),
            "FFI package selection");
    const auto identity = installationRecord(fixture.root);
    require(identity.variant == u"portable" && identity.version == u"1.0.0-beta",
            "FFI installation record");
    require(transactionPending(fixture.root) == false, "FFI pending marker");
    auto outer = QJsonDocument::fromJson(fixture.envelope).object();
    outer.insert("keyId", "attacker");
    rejects([&] { verifyRelease(QJsonDocument(outer).toJson(), signer.publicKeys); },
            "reject unknown key");
    outer = QJsonDocument::fromJson(fixture.envelope).object();
    auto bytes = QByteArray::fromBase64(outer.value("payload").toString().toLatin1());
    bytes.replace("beta.1", "beta.9");
    outer.insert("payload", QString::fromLatin1(bytes.toBase64()));
    rejects([&] { verifyRelease(QJsonDocument(outer).toJson(), signer.publicKeys); },
            "reject tampered payload");
    auto payload = fixture.payload;
    payload.insert("platform", "linux-x64");
    rejects([&] { verifyRelease(signer.sign(payload), signer.publicKeys); },
            "reject wrong platform");
    rejects([&] { verifyRelease(fixture.envelope); },
            "test signer must not be trusted in production");
    auto packages = fixture.payload.value("packages").toArray();
    auto package = packages[1].toObject();
    auto files = package.value("files").toArray();
    for (qsizetype index = 0; index < files.size(); ++index) {
        if (files[index].toObject().value("path").toString() == u"bin/snow-shot-updater.exe") {
            files.removeAt(index);
            break;
        }
    }
    package.insert("files", files);
    packages[1] = package;
    payload = fixture.payload;
    payload.insert("packages", packages);
    rejects([&] { verifyRelease(signer.sign(payload), signer.publicKeys); },
            "reject a signed release without its future recovery helper");
}

// Every diagnostic the Rust helper can emit must exist verbatim in the
// translation catalog; otherwise users would see untranslated failures.
void diagnosticsMatchCatalog() {
    const unsigned int count = updaterErrorDiagnosticCount();
    require(count > 0, "the Rust contract exposes its diagnostics");
    std::set<QString> catalog;
    for (const auto* source : updateErrorSources) {
        catalog.insert(QString::fromUtf8(source));
    }
    for (unsigned int code = 1; code <= count; ++code) {
        const char* message = updaterErrorDiagnostic(code);
        require(message != nullptr, "every diagnostic code resolves");
        require(catalog.contains(QString::fromUtf8(message)),
                "every Rust diagnostic exists in the translation catalog");
    }
}

void await(const std::function<bool()>& complete) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!complete() && elapsed.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(complete(), "asynchronous update operation completed");
}

void service(Signer& signer) {
    Fixture fixture(signer);
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "listen on isolated update server");
    bool tamper = false;
    bool interrupt = false;
    bool resumed = false;
    int requests = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        while (auto* socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) {
                    return;
                }
                socket->setProperty("sent", true);
                ++requests;
                const QByteArray body = request.startsWith("GET /latest-version.json")
                                            ? (tamper ? QByteArray("invalid") : fixture.envelope)
                                            : readLimited(fixture.archive);
                const bool package = !request.startsWith("GET /latest-version.json");
                const auto rangeAt = request.indexOf("Range: bytes=");
                if (package && rangeAt >= 0) {
                    const auto start = rangeAt + QByteArray("Range: bytes=").size();
                    const auto end = request.indexOf('-', start);
                    const qint64 offset = request.mid(start, end - start).toLongLong();
                    require(request.contains("If-Range: \"fixture\""),
                            "resume binds the stored server validator");
                    resumed = true;
                    const QByteArray remainder = body.mid(offset);
                    socket->write("HTTP/1.1 206 Partial Content\r\nConnection: close\r\nETag: "
                                  "\"fixture\"\r\nContent-Range: bytes " +
                                  QByteArray::number(offset) + '-' +
                                  QByteArray::number(body.size() - 1) + '/' +
                                  QByteArray::number(body.size()) + "\r\nContent-Length: " +
                                  QByteArray::number(remainder.size()) + "\r\n\r\n" + remainder);
                } else {
                    const QByteArray sent =
                        package && interrupt ? body.left(body.size() / 2) : body;
                    // The interrupted response claims the full length so the
                    // client sees a premature disconnect, not a clean body.
                    socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nETag: "
                                  "\"fixture\"\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\n\r\n" + sent);
                    if (package) {
                        interrupt = false;
                    }
                }
                socket->disconnectFromHost();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    UpdateService::Options options;
    options.root = fixture.root;
    options.cacheDirectory = fixture.temporary.filePath("cache");
    options.baseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
    options.allowLocalHttp = true;
    options.trustedKeys = signer.publicKeys;
    UpdateService updates(options);
    require(updates.status().state == UpdateState::Idle, "recognize fixture installation");
    updates.setMode("manual");
    updates.check(false);
    require(requests == 0, "manual mode does not check in background");
    updates.setMode("download");
    updates.check();
    await([&] {
        return updates.status().state == UpdateState::Ready ||
               updates.status().state == UpdateState::Failed;
    });
    if (updates.status().state == UpdateState::Failed) {
        std::cerr << qPrintable(updates.status().error) << '\n';
    }
    require(updates.status().state == UpdateState::Ready && requests == 2,
            "download and verify newer release automatically");
    const int before = requests;
    updates.check(false);
    require(requests == before, "successful checks are rate limited");
    tamper = true;
    updates.check();
    await([&] { return updates.status().state == UpdateState::Failed; });
    require(requests == before + 1, "invalid signature never authorizes another download");
    fixture.preserved();
    tamper = false;
    interrupt = true;
    options.cacheDirectory = fixture.temporary.filePath("resume-cache");
    UpdateService retrying(options);
    retrying.check();
    await([&] {
        return retrying.status().state == UpdateState::Ready ||
               retrying.status().state == UpdateState::Failed;
    });
    require(retrying.status().state == UpdateState::Ready && resumed,
            "resume an interrupted download and verify the complete hash");
    options.cacheDirectory = fixture.temporary.filePath("cancel-cache");
    UpdateService cancelled(options);
    cancelled.check();
    cancelled.cancel();
    require(cancelled.status().state == UpdateState::Idle,
            "cancel an outstanding metadata request");
    options.cacheDirectory = fixture.temporary.filePath("cache");
    UpdateService restored(options);
    require(restored.status().state == UpdateState::Ready,
            "restore a previously verified download across restart");
    put(fixture.root, ".snow-shot-update/failed-version.txt", "1.0.0-beta.1");
    UpdateService suppressed(options);
    require(suppressed.status().state == UpdateState::Available,
            "a cached failed version is not automatically ready after restart");
    suppressed.check();
    await([&] { return suppressed.status().state == UpdateState::Ready; });
    QFile::remove(QDir(fixture.root).filePath(".snow-shot-update/failed-version.txt"));
    auto older = fixture.payload;
    older.insert("version", "1.0.0-beta");
    fixture.envelope = signer.sign(older);
    restored.check();
    await([&] { return restored.status().state == UpdateState::Failed; });
    require(restored.status().version == u"1.0.0-beta.1",
            "signed feed downgrade must not overwrite observed release state");
    fixture.envelope = signer.sign(fixture.payload);
    options.cacheDirectory = fixture.temporary.filePath("check-cache");
    UpdateService checking(options);
    checking.setMode("check");
    const int beforeCheck = requests;
    checking.check(false);
    await([&] { return checking.status().state == UpdateState::Available; });
    require(requests == beforeCheck + 1, "check-only policy does not fetch a payload");
    options.cacheDirectory = fixture.temporary.filePath("policy-change-cache");
    UpdateService policyChange(options);
    policyChange.check(false);
    policyChange.setMode("check");
    const int beforePolicyChange = requests;
    await([&] { return policyChange.status().state == UpdateState::Available; });
    require(requests == beforePolicyChange + 1,
            "changing to check-only during metadata fetch must prevent automatic download");
    options.baseUrl = QUrl("http://example.invalid");
    UpdateService insecure(options);
    require(insecure.status().state == UpdateState::Unavailable,
            "even test HTTP allowance must reject a non-loopback origin");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        std::cout << "contract\n" << std::flush;
        Signer signer;
        contractThroughFfi(signer);
        std::cout << "diagnostics\n" << std::flush;
        diagnosticsMatchCatalog();
        std::cout << "service\n" << std::flush;
        service(signer);
        std::cout << "PASS: update contract, diagnostics and service\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
}
