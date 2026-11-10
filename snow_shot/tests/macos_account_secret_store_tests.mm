#include "snow_shot/storage/accountsecretstore.h"

#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <Security/Security.h>

#include <cstdlib>
#include <iostream>

namespace {
QByteArray storedSecret;
bool hasSecret = false;
OSStatus readStatus = errSecSuccess;
OSStatus updateStatus = errSecSuccess;
OSStatus deleteStatus = errSecSuccess;
int additions = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void checkQuery(CFDictionaryRef query) {
    require(CFEqual(CFDictionaryGetValue(query, kSecClass), kSecClassGenericPassword),
            "account secrets use generic-password items");
    require(CFEqual(CFDictionaryGetValue(query, kSecAttrService), CFSTR("com.snowshot.account")),
            "account secrets use their own keychain service");
    require(CFDictionaryGetValue(query, kSecAttrSynchronizable) == kCFBooleanFalse,
            "account secrets remain local to this device");
}
} // namespace

extern "C" OSStatus SecItemCopyMatching(CFDictionaryRef query, CFTypeRef* result) {
    checkQuery(query);
    auto* context = (__bridge LAContext*)CFDictionaryGetValue(query, kSecUseAuthenticationContext);
    require(context != nil && context.interactionNotAllowed,
            "reading account secrets must never display authentication UI");
    require(CFDictionaryGetValue(query, kSecReturnData) == kCFBooleanTrue,
            "reading account secrets requests the stored bytes");
    require(CFEqual(CFDictionaryGetValue(query, kSecMatchLimit), kSecMatchLimitOne),
            "reading account secrets requests one matching item");
    if (readStatus != errSecSuccess) {
        return readStatus;
    }
    if (!hasSecret) {
        return errSecItemNotFound;
    }
    NSData* data = [NSData dataWithBytes:storedSecret.constData()
                                  length:static_cast<NSUInteger>(storedSecret.size())];
    *result = CFRetain((__bridge CFTypeRef)data);
    return errSecSuccess;
}

extern "C" OSStatus SecItemUpdate(CFDictionaryRef query, CFDictionaryRef attributes) {
    checkQuery(query);
    if (updateStatus != errSecSuccess) {
        return updateStatus;
    }
    if (!hasSecret) {
        return errSecItemNotFound;
    }
    auto* data = (__bridge NSData*)CFDictionaryGetValue(attributes, kSecValueData);
    storedSecret =
        QByteArray(static_cast<const char*>(data.bytes), static_cast<qsizetype>(data.length));
    return errSecSuccess;
}

extern "C" OSStatus SecItemAdd(CFDictionaryRef attributes, CFTypeRef*) {
    checkQuery(attributes);
    ++additions;
    auto* data = (__bridge NSData*)CFDictionaryGetValue(attributes, kSecValueData);
    storedSecret =
        QByteArray(static_cast<const char*>(data.bytes), static_cast<qsizetype>(data.length));
    hasSecret = true;
    return errSecSuccess;
}

extern "C" OSStatus SecItemDelete(CFDictionaryRef query) {
    checkQuery(query);
    if (deleteStatus != errSecSuccess) {
        return deleteStatus;
    }
    if (!hasSecret) {
        return errSecItemNotFound;
    }
    storedSecret.clear();
    hasSecret = false;
    return errSecSuccess;
}

int main() {
    @autoreleasepool {
        snow_shot::storage::AccountSecretStore store(QStringLiteral("isolated-keychain-test"));
        require(store.read().isEmpty(), "missing secrets return no data without prompting");
        const QByteArray original("first\0secret", 12);
        require(store.write(original) && store.read() == original,
                "new secrets preserve binary data across a read");
        const QByteArray replacement("replacement");
        require(store.write(replacement) && store.read() == replacement && additions == 1,
                "writing an existing secret updates its item");
        updateStatus = errSecAuthFailed;
        require(!store.write(original) && additions == 1,
                "update failures do not create a replacement item");
        readStatus = errSecInteractionNotAllowed;
        require(store.read().isEmpty(), "authentication failures return no secret");
        readStatus = errSecSuccess;
        deleteStatus = errSecAuthFailed;
        require(!store.remove(), "deletion failures are reported");
        deleteStatus = errSecSuccess;
        require(store.remove() && store.read().isEmpty() && store.remove(),
                "removing a secret is idempotent");
        std::cout << "macOS account secret store tests passed\n";
    }
}
