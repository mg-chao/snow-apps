#!/bin/bash
# Homebrew preflight, including replay when Homebrew restores an upgrade backup.
set -Eeuo pipefail

homebrew_prepare_application() {
    [[ $# -ge 2 && $# -le 3 ]] || { printf '%s\n' 'Expected a DMG path and staged application path.' >&2; return 1; }
    local dmg="$1" bundle="$2" edition="${3:-full}"
    if [[ -e "$bundle" || -L "$bundle" ]]; then
        # A restored backup must not require the signing key that caused an upgrade
        # failure. Homebrew owns this directory; reject links and verify its seal.
        [[ -d "$bundle" && ! -L "$bundle" ]] || { printf '%s\n' 'Invalid staged Snow Shot application.' >&2; return 1; }
        codesign --verify --deep --strict "$bundle"
    else
        /bin/bash "$(dirname "${BASH_SOURCE[0]}")/install-snow-shot-macos.sh" \
            --edition "$edition" --dmg "$dmg" --prepare-app "$bundle"
    fi
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    export PATH=/usr/bin:/bin:/usr/sbin:/sbin
    homebrew_prepare_application "$@"
fi
