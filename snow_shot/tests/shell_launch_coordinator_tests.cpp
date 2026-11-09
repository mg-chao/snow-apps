// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/app/shelllaunchcoordinator.h"

#include "snow_shot/app/shellimagelaunch.h"
#include "snow_shot/update/relaunchintent.h"

#include <QCoreApplication>
#include <QDir>

#include <cstdlib>
#include <functional>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

using Coordinator = snow_shot::app::ShellLaunchCoordinator;

Coordinator::Request files(const QStringList& paths, bool foreground = false) {
    auto arguments = snow_shot::app::shellImageLaunchArguments(paths);
    if (foreground)
        arguments.append(QStringLiteral("--show-main-window"));
    return Coordinator::parse(arguments);
}

void classificationPreservesLaunchPolicy() {
    const QString image = QDir::temp().filePath(QStringLiteral("image one.PNG"));
    const QString document = QDir::temp().filePath(QStringLiteral("notes.MD"));
    const QString unsupported = QDir::temp().filePath(QStringLiteral("unknown.bin"));
    const auto request = files({image, unsupported, document, image});
    require(request.filesRequested && request.valid && !request.foreground &&
                request.paths == QStringList{image, document, image} &&
                request.unsupportedPaths == QStringList{unsupported},
            "classification preserves ordered repeated supported files and individual errors");
    const auto invalid = Coordinator::parse(
        {QStringLiteral("--edit-with-snow-shot"), image, QStringLiteral("--edit-with-snow-shot")});
    require(invalid.filesRequested && !invalid.valid && invalid.paths.isEmpty() &&
                invalid.unsupportedPaths.isEmpty(),
            "malformed operands reject the whole file request before suffix feedback");
    require(Coordinator::parse({}).foreground &&
                !Coordinator::parse({QStringLiteral("--autostart")}).foreground &&
                Coordinator::parse(
                    {QStringLiteral("--autostart"), QStringLiteral("--show-main-window")})
                    .foreground,
            "ordinary activation, quiet autostart and explicit foreground remain distinct");
}

void prospectiveAdmissionAndReplayOwnDifferentStates() {
    Coordinator coordinator;
    const QString first = QDir::temp().filePath(QStringLiteral("first.png"));
    const QString second = QDir::temp().filePath(QStringLiteral("second.md"));
    const auto firstRequest = files({first});
    const auto secondRequest = files({second}, true);
    const auto fitsTwo = [](const Coordinator::Intent& intent) { return intent.paths.size() <= 2; };
    require(coordinator.acceptsUpdate(firstRequest, true, fitsTwo) &&
                coordinator.acceptsUpdate(firstRequest, true, fitsTwo),
            "repeated transport inspection does not reserve or duplicate launch intent");
    static_cast<void>(coordinator.retainUpdate(firstRequest));
    require(coordinator.acceptsUpdate(secondRequest, true, fitsTwo),
            "prospective admission includes previously accepted update paths");
    const auto transfer = coordinator.retainUpdate(secondRequest);
    require(transfer.paths == QStringList{first, second} && transfer.foreground &&
                !coordinator.acceptsUpdate(firstRequest, true, fitsTwo),
            "cumulative capacity rejects only new intent while retaining the accepted batch");
    const auto invalid = Coordinator::parse({QStringLiteral("--edit-with-snow-shot")});
    require(coordinator.acceptsUpdate(invalid, true, fitsTwo) &&
                !coordinator.acceptsUpdate(invalid, false, fitsTwo),
            "malformed requests reach validation feedback until terminal transfer closes input");

    coordinator.retainStorage(files({second}));
    const auto rollback = coordinator.takeUpdate();
    require(rollback.paths == transfer.paths && rollback.foreground,
            "failed updater handoff transfers every accepted file and foreground intent to replay");
    const auto empty = coordinator.takeUpdate();
    require(empty.paths.isEmpty() && !empty.foreground,
            "rollback can consume an update batch only once");
    static_cast<void>(coordinator.retainUpdate(firstRequest));
    const auto storageReplay = coordinator.takeStorage();
    require(storageReplay.paths == QStringList{second} && !storageReplay.foreground &&
                coordinator.takeStorage().paths.isEmpty() &&
                coordinator.takeUpdate().paths == QStringList{first},
            "reentrant requests form new batches and storage/update ownership stays independent");
    require(Coordinator::parse(rollback.replayArguments()).paths == rollback.paths &&
                Coordinator::parse(rollback.replayArguments()).foreground,
            "normal replay preserves supported paths and activation without private update flags");
}

void cumulativeAdmissionUsesSerializedTransferBudget() {
    Coordinator coordinator;
    const QString prefix = QDir::temp().filePath(QString());
    const auto longRequest =
        files({prefix + QString(9000, QChar(0x56fe)) + QStringLiteral(".png")});
    const auto serializedFits = [](const Coordinator::Intent& intent) {
        auto arguments = intent.replayArguments();
        arguments.append(QStringLiteral("--skip-startup-update"));
        return snow_shot::update::relaunchIntentFits(arguments, QStringLiteral("snow-shot.exe"));
    };
    require(!coordinator.acceptsUpdate(longRequest, true, serializedFits),
            "UTF-8 transfer bytes can exceed capacity before Windows command units do");
    const auto partial = files({prefix + QString(5000, u'a') + QStringLiteral(".png")});
    require(coordinator.acceptsUpdate(partial, true, serializedFits), "one large path fits");
    for (int index = 0; index < 4; ++index)
        static_cast<void>(coordinator.retainUpdate(partial));
    require(!coordinator.acceptsUpdate(partial, true, serializedFits) &&
                coordinator.takeUpdate().paths.size() == 4,
            "admission counts the complete escaped cumulative transfer without dropping repeats");
}

void firstFileCompletionFollowsPresentationOwnership() {
    int completions = 0;
    {
        Coordinator::FirstFileCompletion completion([&] { ++completions; });
    }
    require(completions == 1,
            "invalid or deferred file launch settles the startup continuation on return");
    std::function<void()> presented;
    {
        Coordinator::FirstFileCompletion completion([&] { ++completions; });
        presented = completion.take();
        require(!completion.take(), "presentation is the only owner after continuation transfer");
    }
    require(completions == 1, "background startup waits for actual first-file presentation");
    presented();
    require(completions == 2, "first-file presentation can resume background startup");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    classificationPreservesLaunchPolicy();
    prospectiveAdmissionAndReplayOwnDifferentStates();
    cumulativeAdmissionUsesSerializedTransferBudget();
    firstFileCompletionFollowsPresentationOwnership();
    return EXIT_SUCCESS;
}
