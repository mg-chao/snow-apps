#include "../src/platform/macos/autostartregistration_p.h"

#include <QCoreApplication>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using namespace snow_shot::platform::macos::detail;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct LoginItem {
    LoginItemStatus status = LoginItemStatus::Unregistered;
    LoginItemStatus afterRegistration = LoginItemStatus::Enabled;
    LoginItemStatus afterUnregistration = LoginItemStatus::Unregistered;
    bool accepted = true;
    int statusReads = 0;
    int settingsOpened = 0;
    std::vector<bool> changes;

    LoginItemApi api() {
        return {[this] {
                    ++statusReads;
                    return status;
                },
                [this](bool enabled, QString* error) {
                    changes.push_back(enabled);
                    if (!accepted) {
                        if (error != nullptr) {
                            *error = QStringLiteral("The user rejected registration");
                        }
                        return false;
                    }
                    status = enabled ? afterRegistration : afterUnregistration;
                    return true;
                },
                [this] { ++settingsOpened; }};
    }
};

void registrationAndUnregistrationAreVerified() {
    LoginItem item;
    QString error;
    require(setLoginItemEnabled(item.api(), true, &error) &&
                item.status == LoginItemStatus::Enabled &&
                item.changes == std::vector<bool>{true} && item.statusReads == 2 && error.isEmpty(),
            "enabling a login item must register and verify the resulting native state");
    require(setLoginItemEnabled(item.api(), false, &error) &&
                item.status == LoginItemStatus::Unregistered &&
                item.changes == std::vector<bool>({true, false}) && item.settingsOpened == 0,
            "disabling an enabled login item must unregister without opening system settings");
}

void matchingStatesAreIdempotent() {
    LoginItem item;
    require(setLoginItemEnabled(item.api(), false, nullptr) && item.changes.empty(),
            "an already unregistered item must not invoke the native unregister API");
    item.status = LoginItemStatus::Enabled;
    require(setLoginItemEnabled(item.api(), true, nullptr) && item.changes.empty() &&
                item.settingsOpened == 0,
            "an enabled item must not be registered again or reopen approval settings");
}

void approvalIsDistinctFromCompletedEnablement() {
    LoginItem item;
    item.afterRegistration = LoginItemStatus::RequiresApproval;
    require(setLoginItemEnabled(item.api(), true, nullptr) &&
                item.status == LoginItemStatus::RequiresApproval && item.changes.size() == 1 &&
                item.settingsOpened == 1,
            "accepted registration needing approval must retain pending state and open settings");
    require(setLoginItemEnabled(item.api(), true, nullptr) && item.changes.size() == 1 &&
                item.settingsOpened == 2,
            "retrying pending approval must reopen settings without duplicate registration");
    item.status = LoginItemStatus::Enabled;
    require(setLoginItemEnabled(item.api(), true, nullptr) && item.changes.size() == 1 &&
                item.settingsOpened == 2,
            "external approval must be observed on retry without another registration");
    item.status = LoginItemStatus::RequiresApproval;
    require(setLoginItemEnabled(item.api(), false, nullptr) &&
                item.status == LoginItemStatus::Unregistered && item.changes.back() == false &&
                item.settingsOpened == 2,
            "a pending item must be cancellable without opening another approval page");
}

void rejectedAndIneffectiveChangesFail() {
    for (const bool enable : {true, false}) {
        LoginItem item;
        item.status = enable ? LoginItemStatus::Unregistered : LoginItemStatus::Enabled;
        const auto previous = item.status;
        item.accepted = false;
        QString error;
        require(!setLoginItemEnabled(item.api(), enable, &error) && item.status == previous &&
                    !error.isEmpty() && item.changes == std::vector<bool>{enable} &&
                    item.settingsOpened == 0,
                "native rejection must preserve state and report the failure");
        item.accepted = true;
        item.afterRegistration = LoginItemStatus::Unregistered;
        item.afterUnregistration = LoginItemStatus::Enabled;
        error.clear();
        require(!setLoginItemEnabled(item.api(), enable, &error) && !error.isEmpty() &&
                    item.status == previous && item.settingsOpened == 0,
                "a successful API call without the requested status must not count as applied");
    }
}

void unbundledExecutablesCannotChangeLoginItems() {
    for (const bool enable : {true, false}) {
        LoginItem item;
        item.status = LoginItemStatus::Unavailable;
        QString error;
        require(
            !setLoginItemEnabled(item.api(), enable, &error) && !error.isEmpty() &&
                item.changes.empty() && item.settingsOpened == 0,
            "unavailable non-bundle contexts must fail without registering or opening settings");
        require(!setLoginItemEnabled(item.api(), enable, nullptr) && item.changes.empty(),
                "callers may omit error output without allowing a non-bundle registration");
    }
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    registrationAndUnregistrationAreVerified();
    matchingStatesAreIdempotent();
    approvalIsDistinctFromCompletedEnablement();
    rejectedAndIneffectiveChangesFail();
    unbundledExecutablesCannotChangeLoginItems();
    return 0;
}
