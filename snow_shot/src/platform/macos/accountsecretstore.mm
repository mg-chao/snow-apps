#include "snow_shot/storage/accountsecretstore.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace snow_shot::storage {
namespace {
NSMutableDictionary* query(const QString& identity) {
    return [NSMutableDictionary dictionaryWithDictionary:@{
        (id)kSecClass : (id)kSecClassGenericPassword,
        (id)kSecAttrService : @"com.snowshot.account",
        (id)kSecAttrAccount : identity.toNSString(),
        (id)kSecAttrSynchronizable : @NO
    }];
}
} // namespace

QByteArray AccountSecretStore::read() const {
    @autoreleasepool {
        auto* attributes = query(m_identity);
        attributes[(id)kSecReturnData] = @YES;
        attributes[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
        attributes[(id)kSecUseAuthenticationUI] = (id)kSecUseAuthenticationUIFail;
        CFTypeRef result = nullptr;
        if (SecItemCopyMatching((CFDictionaryRef)attributes, &result) != errSecSuccess)
            return {};
        auto* data = (NSData*)result;
        const QByteArray bytes(static_cast<const char*>(data.bytes),
                               static_cast<qsizetype>(data.length));
        CFRelease(result);
        return bytes;
    }
}

bool AccountSecretStore::write(const QByteArray& value) const {
    @autoreleasepool {
        auto* attributes = query(m_identity);
        NSData* bytes = [NSData dataWithBytes:value.constData() length:value.size()];
        const auto status = SecItemUpdate((CFDictionaryRef)attributes,
                                          (CFDictionaryRef) @{(id)kSecValueData : bytes});
        if (status == errSecSuccess)
            return true;
        if (status != errSecItemNotFound)
            return false;
        attributes[(id)kSecValueData] = bytes;
        return SecItemAdd((CFDictionaryRef)attributes, nullptr) == errSecSuccess;
    }
}

bool AccountSecretStore::remove() const {
    @autoreleasepool {
        const auto status = SecItemDelete((CFDictionaryRef)query(m_identity));
        return status == errSecSuccess || status == errSecItemNotFound;
    }
}
} // namespace snow_shot::storage
