#!/bin/bash
set -euo pipefail

identity_name='Snow Shot Development (Local)'

find_identity_hash() {
    local line
    local hash
    while IFS= read -r line; do
        case "$line" in
            *\"$identity_name\"*)
                hash="$(sed -E 's/^[[:space:]]*[0-9]+\)[[:space:]]+([0-9A-Fa-f]{40}).*$/\1/' <<<"$line")"
                if [[ "$hash" =~ ^[0-9A-Fa-f]{40}$ ]]; then
                    printf '%s\n' "$hash"
                    return 0
                fi
                ;;
        esac
    done < <(security find-identity -p codesigning 2>/dev/null)
    return 1
}

if identity_hash="$(find_identity_hash)"; then
    printf '%s\n' "$identity_hash"
    exit 0
fi

temporary_directory="$(mktemp -d "${TMPDIR:-/tmp}/snow-shot-codesign.XXXXXX")"
private_key="$temporary_directory/private-key.pem"
certificate="$temporary_directory/certificate.pem"
identity_bundle="$temporary_directory/identity.p12"
cleanup() {
    unlink "$private_key" 2>/dev/null || true
    unlink "$certificate" 2>/dev/null || true
    unlink "$identity_bundle" 2>/dev/null || true
    rmdir "$temporary_directory" 2>/dev/null || true
}
trap cleanup EXIT
umask 077

printf 'Creating the persistent macOS signing identity "%s" in the default keychain.\n' \
    "$identity_name" >&2
openssl req -new -newkey rsa:2048 -x509 -nodes -days 3650 \
    -subj "/CN=$identity_name" \
    -addext 'basicConstraints=critical,CA:TRUE' \
    -addext 'keyUsage=critical,digitalSignature,keyCertSign' \
    -addext 'extendedKeyUsage=critical,codeSigning' \
    -keyout "$private_key" -out "$certificate" >/dev/null 2>&1
openssl pkcs12 -export -inkey "$private_key" -in "$certificate" \
    -name "$identity_name" -out "$identity_bundle" \
    -passout pass:snow-shot-local-import
security import "$identity_bundle" -P snow-shot-local-import \
    -T /usr/bin/codesign >/dev/null

if ! identity_hash="$(find_identity_hash)"; then
    echo "The macOS signing identity was imported but could not be found in the default keychain." >&2
    exit 1
fi
printf '%s\n' "$identity_hash"
