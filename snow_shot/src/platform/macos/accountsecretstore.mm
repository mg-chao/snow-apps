#include "snow_shot/storage/accountsecretstore.h"

#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <Security/Security.h>

namespace snow_shot::storage {
namespace {
NSMutableDictionary* query(const QString& identity) {
    return [NSMutableDictionary dictionaryWithDictionary:@{
        (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService : @"com.snowshot.account",
        (__bridge id)kSecAttrAccount : identity.toNSString(),
        (__bridge id)kSecAttrSynchronizable : @NO
    }];
}
} // namespace

QByteArray AccountSecretStore::read() const {
    @autoreleasepool {
        auto* attributes = query(m_identity);
        attributes[(__bridge id)kSecReturnData] = @YES;
        attributes[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
        LAContext* context = [[LAContext alloc] init];
        context.interactionNotAllowed = YES;
        attributes[(__bridge id)kSecUseAuthenticationContext] = context;
        CFTypeRef result = nullptr;
        if (SecItemCopyMatching((__bridge CFDictionaryRef)attributes, &result) != errSecSuccess) {
            return {};
        }
        auto* data = (__bridge NSData*)result;
        const QByteArray bytes(static_cast<const char*>(data.bytes),
                               static_cast<qsizetype>(data.length));
        CFRelease(result);
        return bytes;
    }
}

bool AccountSecretStore::write(const QByteArray& value) const {
    @autoreleasepool {
        auto* attributes = query(m_identity);
        NSData* bytes = [NSData dataWithBytes:value.constData()
                                       length:static_cast<NSUInteger>(value.size())];
        const auto status =
            SecItemUpdate((__bridge CFDictionaryRef)attributes,
                          (__bridge CFDictionaryRef) @{(__bridge id)kSecValueData : bytes});
        if (status == errSecSuccess) {
            return true;
        }
        if (status != errSecItemNotFound) {
            return false;
        }
        attributes[(__bridge id)kSecValueData] = bytes;
        return SecItemAdd((__bridge CFDictionaryRef)attributes, nullptr) == errSecSuccess;
    }
}

bool AccountSecretStore::remove() const {
    @autoreleasepool {
        const auto status = SecItemDelete((__bridge CFDictionaryRef)query(m_identity));
        return status == errSecSuccess || status == errSecItemNotFound;
    }
}
} // namespace snow_shot::storage
