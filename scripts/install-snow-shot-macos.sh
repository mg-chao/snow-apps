#!/bin/bash
# Standalone installer. Keep compatible with Apple's Bash 3.2 and system tools.
set -Eeuo pipefail

language=auto
launch=1
local_dmg=''
work=''
mount_dir=''
package_mounted=0
slot=''
state=''
lock_owned=0
committed=0
replacement_started=0
had_previous=0
needs_sudo=0
destination='/Applications/Snow Shot.app'
previous_destination=''

message() {
    local en cn tw
    case "$1" in
        usage) en='Usage: install-snow-shot-macos.sh [--lang auto|en|zh-CN|zh-TW] [--no-launch] [--dmg PATH] [--help]'; cn='用法：install-snow-shot-macos.sh [--lang auto|en|zh-CN|zh-TW] [--no-launch] [--dmg 路径] [--help]'; tw='用法：install-snow-shot-macos.sh [--lang auto|en|zh-CN|zh-TW] [--no-launch] [--dmg 路徑] [--help]' ;;
        help) en='Downloads, verifies, locally signs, and installs Snow Shot. --dmg requires PATH.sha256. --no-launch skips launching. --lang overrides automatic language selection. First installation requires macOS privacy authorization.'; cn='下载、验证、本地签名并安装 Snow Shot。--dmg 需要对应的 PATH.sha256 文件。--no-launch 跳过启动。--lang 指定界面语言。首次安装需要授予 macOS 隐私权限。'; tw='下載、驗證、本機簽署並安裝 Snow Shot。--dmg 需要對應的 PATH.sha256 檔案。--no-launch 跳過啟動。--lang 指定介面語言。首次安裝需要授予 macOS 隱私權限。' ;;
        arguments) en='Invalid or incomplete option. Run with --help.'; cn='选项无效或不完整。请使用 --help 查看帮助。'; tw='選項無效或不完整。請使用 --help 查看說明。' ;;
        platform) en='Snow Shot requires macOS 15 or later on Apple Silicon or Intel.'; cn='Snow Shot 需要运行 macOS 15 或更新版本的 Apple Silicon 或 Intel Mac。'; tw='Snow Shot 需要執行 macOS 15 或更新版本的 Apple Silicon 或 Intel Mac。' ;;
        root) en='Run this script as your desktop user, without sudo. Administrator access is requested only when needed.'; cn='请以当前桌面用户运行脚本，不要直接使用 root。仅在需要时请求管理员权限。'; tw='請以目前桌面使用者執行指令碼，不要直接使用 root。僅在需要時請求管理員權限。' ;;
        primary) en='[1/5] Downloading the latest macOS package…'; cn='[1/5] 正在下载最新 macOS 安装包…'; tw='[1/5] 正在下載最新 macOS 安裝套件…' ;;
        fallback) en='Primary download unavailable or invalid; trying GitHub Releases…'; cn='主下载源不可用或验证失败，正在尝试 GitHub Releases…'; tw='主要下載來源無法使用或驗證失敗，正在嘗試 GitHub Releases…' ;;
        unavailable) en='No valid package is available. The release or checksum may not be uploaded, your architecture may be missing, or GitHub may be rate-limiting requests. Retry later; the installed app has not changed.'; cn='未找到有效安装包。可能尚未上传安装包或校验文件、缺少当前架构版本，或 GitHub 请求受到限流。请稍后重试；已安装的应用未被更改。'; tw='找不到有效安裝套件。可能尚未上傳安裝套件或校驗檔案、缺少目前架構版本，或 GitHub 請求受到限流。請稍後重試；已安裝的應用程式未被更改。' ;;
        verify) en='[2/5] Verifying the package and application…'; cn='[2/5] 正在验证安装包和应用…'; tw='[2/5] 正在驗證安裝套件與應用程式…' ;;
        invalid) en='Package validation failed. Download the matching macOS DMG and its .sha256 file again.'; cn='安装包验证失败。请重新下载匹配的 macOS DMG 及其 .sha256 文件。'; tw='安裝套件驗證失敗。請重新下載相符的 macOS DMG 及其 .sha256 檔案。' ;;
        signing) en='[3/5] Signing with your persistent local identity…'; cn='[3/5] 正在使用持久本地身份签名…'; tw='[3/5] 正在使用持久本機身分簽署…' ;;
        keychain) en='Creating a signing identity in your login Keychain. macOS may request Keychain access or code-signing trust confirmation. This identity is reused on future installations.'; cn='正在登录钥匙串中创建签名身份。macOS 可能请求钥匙串访问或代码签名信任确认。今后的安装将复用此身份。'; tw='正在登入鑰匙圈中建立簽署身分。macOS 可能請求鑰匙圈存取或程式碼簽署信任確認。之後的安裝將重複使用此身分。' ;;
        identity) en='The saved signing identity is missing, expired, inaccessible, or incomplete. Unlock your login Keychain or restore the original certificate and private key. Do not delete installer state unless you accept granting privacy permissions again; see docs-macos-build.md.'; cn='保存的签名身份丢失、过期、无法访问或不完整。请解锁登录钥匙串，或恢复原证书及私钥。删除安装器状态后需要重新授权；请参阅 docs-macos-build.md。'; tw='儲存的簽署身分遺失、過期、無法存取或不完整。請解鎖登入鑰匙圈，或還原原憑證及私密金鑰。刪除安裝程式狀態後需要重新授權；請參閱 docs-macos-build.md。' ;;
        continuity) en='The new app does not satisfy the saved signing requirement. Installation stopped to protect permission continuity. Keep the original signing identity and inspect the diagnostic below.'; cn='新应用不符合保存的签名要求。已停止安装以保护权限连续性。请保留原签名身份并检查下方诊断信息。'; tw='新應用程式不符合儲存的簽署要求。已停止安裝以保護權限連續性。請保留原簽署身分並檢查下方診斷資訊。' ;;
        install) en='[4/5] Installing into /Applications…'; cn='[4/5] 正在安装到 /Applications…'; tw='[4/5] 正在安裝到 /Applications…' ;;
        sudo) en='Administrator access is needed to replace the application. Enter your Mac login password if prompted.'; cn='替换应用需要管理员权限。如有提示，请输入 Mac 登录密码。'; tw='取代應用程式需要管理員權限。如有提示，請輸入 Mac 登入密碼。' ;;
        quit) en='Closing the installed Snow Shot…'; cn='正在关闭已安装的 Snow Shot…'; tw='正在關閉已安裝的 Snow Shot…' ;;
        running) en='Snow Shot did not quit. Finish any recording, quit the app manually, and run the installer again.'; cn='Snow Shot 未退出。请结束录制并手动退出应用，然后重新运行安装器。'; tw='Snow Shot 未結束。請結束錄製並手動結束應用程式，然後重新執行安裝程式。' ;;
        done) en='[5/5] Snow Shot is installed.'; cn='[5/5] Snow Shot 安装完成。'; tw='[5/5] Snow Shot 安裝完成。' ;;
        permissions) en='On first installation or migration from an older signature, grant Screen Recording and Accessibility in System Settings → Privacy & Security when requested. Use this installer for future updates to retain the local identity. Permission retention has not yet been qualified across supported macOS versions; macOS may still request consent.'; cn='首次安装或从旧签名迁移时，请按提示在“系统设置 → 隐私与安全性”中授予屏幕录制和辅助功能权限。今后请使用此安装器更新，以保留本地身份。跨 macOS 版本的权限保留尚未完成验证；系统仍可能要求授权。'; tw='首次安裝或從舊簽署遷移時，請依提示在「系統設定 → 隱私權與安全性」中授予螢幕錄製和輔助使用權限。之後請使用此安裝程式更新，以保留本機身分。跨 macOS 版本的權限保留尚未完成驗證；系統仍可能要求授權。' ;;
        failed) en='Installation failed. Check the diagnostic below, resolve the issue, and retry.'; cn='安装失败。请检查下方诊断信息，解决问题后重试。'; tw='安裝失敗。請檢查下方診斷資訊，解決問題後重試。' ;;
        rollback) en='Restoring the previous application…'; cn='正在恢复原应用…'; tw='正在還原原應用程式…' ;;
        recovery) en='Automatic cleanup or recovery failed. Keep the following directory; it may contain your previous app. Restore previous.app to the installation path printed below before retrying:'; cn='自动清理或恢复失败。请保留以下目录，其中可能包含原应用。重试前请将 previous.app 恢复到下方显示的安装路径：'; tw='自動清理或還原失敗。請保留以下目錄，其中可能包含原應用程式。重試前請將 previous.app 還原至下方顯示的安裝路徑：' ;;
        detach) en='Could not detach the installer volume. Eject it in Finder:'; cn='无法卸载安装卷。请在访达中推出：'; tw='無法卸載安裝卷。請在 Finder 中退出：' ;;
        state) en='Installer state is unsafe or another installation is running. Check ownership and symlinks in the directory below. Remove its lock directory only after confirming no installer is running:'; cn='安装器状态不安全或已有安装正在运行。请检查下方目录的所有权及符号链接。仅在确认没有安装器运行后移除 lock 目录：'; tw='安裝程式狀態不安全或已有安裝正在執行。請檢查下方目錄的擁有權及符號連結。僅在確認沒有安裝程式執行後移除 lock 目錄：' ;;
        launch) en='Installed successfully, but launch failed. Open Snow Shot from Applications in Finder.'; cn='安装成功，但启动失败。请从访达的“应用程序”打开 Snow Shot。'; tw='安裝成功，但啟動失敗。請從 Finder 的「應用程式」開啟 Snow Shot。' ;;
        *) return 1 ;;
    esac
    case "$language" in zh-CN) printf '%s\n' "$cn" ;; zh-TW) printf '%s\n' "$tw" ;; *) printf '%s\n' "$en" ;; esac
}

say() {
    if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then printf '\033[1;36m'; fi
    message "$1"
    if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then printf '\033[0m'; fi
}

die() { message "$1" >&2; exit 1; }
run() { "$@" >> "$work/diagnostic.log" 2>&1; }
as_install() {
    if [[ "$needs_sudo" == 1 ]]; then sudo -- "$@"; else "$@"; fi
}

select_language() {
    [[ "$language" == auto ]] || return 0
    local preference="${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}"
    if [[ -z "$preference" || "$preference" == C || "$preference" == POSIX || "$preference" == en_US.UTF-8 ]]; then
        preference=$(defaults read -g AppleLanguages 2>/dev/null | sed -n '2p') || preference=''
    fi
    case "$preference" in
        *zh-TW*|*zh_TW*|*zh-HK*|*zh_HK*|*zh-Hant*) language=zh-TW ;;
        *zh*) language=zh-CN ;;
        *) language=en ;;
    esac
}

version_supported() {
    awk -v actual="$1" -v required="$2" 'BEGIN {
        if (actual !~ /^[0-9]+(\.[0-9]+)*$/ || required !~ /^[0-9]+(\.[0-9]+)*$/) exit 1;
        split(actual,a,"."); split(required,b,".");
        for(i=1;i<=3;i++) { if(a[i]+0>b[i]+0) exit 0; if(a[i]+0<b[i]+0) exit 1; } exit 0;
    }'
}

detect_architecture() {
    local machine
    machine=$(uname -m)
    if [[ "$machine" == arm64 ]] || [[ "$(sysctl -n hw.optional.arm64 2>/dev/null || true)" == 1 ]]; then
        arch=arm64; asset_arch=arm64
    elif [[ "$machine" == x86_64 ]]; then
        arch=x64; asset_arch=x86_64
    else
        return 1
    fi
}

fetch() {
    run curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
        --connect-timeout 15 --max-time 600 --retry 2 --retry-max-time 120 \
        --output "$2" "$1"
}

verify_checksum() {
    local expected actual
    [[ -f "$1" && -f "$2" ]] || return 1
    expected=$(awk 'NF { count++; if (count==1) value=$1 } END { if(count!=1) exit 1; print value }' "$2") || return 1
    [[ "$expected" =~ ^[[:xdigit:]]{64}$ ]] || return 1
    actual=$(shasum -a 256 "$1") || return 1
    [[ "$(printf '%s' "$expected" | tr '[:upper:]' '[:lower:]')" == "${actual%% *}" ]]
}

# JXA uses system Foundation for JSON; no Python, jq, or developer tools needed.
github_urls() {
    osascript -l JavaScript - "$1" "$asset_arch" <<'JXA'
ObjC.import('Foundation');
function run(argv) {
    const data = $.NSData.dataWithContentsOfFile(argv[0]);
    if (!data) throw Error('Missing release metadata');
    const json = ObjC.unwrap($.NSString.alloc.initWithDataEncoding(data, $.NSUTF8StringEncoding));
    const release = JSON.parse(json);
    if (release.draft || release.prerelease || !Array.isArray(release.assets)) throw Error('No stable release');
    const pattern = new RegExp('^snow-shot-[0-9][A-Za-z0-9.+-]*-macos-' + argv[1] + '\\.dmg$');
    const images = release.assets.filter(a => pattern.test(a.name));
    if (images.length !== 1) throw Error('Expected one matching architecture asset');
    const sums = release.assets.filter(a => a.name === images[0].name + '.sha256');
    if (sums.length !== 1) throw Error('Missing or ambiguous checksum asset');
    const urls = [images[0].browser_download_url, sums[0].browser_download_url];
    urls.forEach(u => {
        if (typeof u !== 'string' || !/^https:\/\/github\.com\/mg-chao\/snow-apps\/releases\/download\/[^\s]+$/.test(u))
            throw Error('Unexpected release asset URL');
    });
    return urls.join('\n');
}
JXA
}

unmount_package() {
    # mount(8) can canonicalize /var aliases and repeated slashes; track ownership instead.
    [[ "$package_mounted" == 1 ]] || { mount_dir=''; return 0; }
    run hdiutil detach "$mount_dir" || return 1
    package_mounted=0
    mount_dir=''
}

validate_and_stage() {
    local dmg="$1" sum="$2" bundle minimum executable description
    verify_checksum "$dmg" "$sum" || return 1
    run hdiutil verify "$dmg" || return 1
    mount_dir="$work/volume"
    mkdir -p "$mount_dir" || return 1
    run hdiutil attach -readonly -nobrowse -noautoopen -mountpoint "$mount_dir" "$dmg" || return 1
    package_mounted=1
    bundle="$mount_dir/Snow Shot.app"
    if [[ ! -e "$bundle" ]]; then bundle="$mount_dir/snow_shot.app"; fi
    [[ -d "$bundle" && ! -L "$bundle" ]] || return 1
    [[ "$(plutil -extract CFBundleIdentifier raw -o - "$bundle/Contents/Info.plist" 2>> "$work/diagnostic.log")" == com.snowshot.snow_shot ]] || return 1
    executable=$(plutil -extract CFBundleExecutable raw -o - "$bundle/Contents/Info.plist" 2>> "$work/diagnostic.log") || return 1
    [[ "$executable" == snow_shot && -x "$bundle/Contents/MacOS/$executable" ]] || return 1
    minimum=$(plutil -extract LSMinimumSystemVersion raw -o - "$bundle/Contents/Info.plist" 2>> "$work/diagnostic.log") || return 1
    version_supported "$os_version" "$minimum" || return 1
    description=$(file -b "$bundle/Contents/MacOS/$executable") || return 1
    [[ "$description" == *Mach-O* && "$description" == *"$asset_arch"* ]] || return 1
    run codesign --verify --deep --strict "$bundle" || return 1
    run ditto "$bundle" "$work/snow_shot.app" || return 1
    run codesign --verify --deep --strict "$work/snow_shot.app" || return 1
    unmount_package || return 1
}

obtain_package() {
    local primary="https://snowshot.top/setup/snow-shot_macos-$arch.dmg" urls dmg_url sum_url
    if [[ -n "$local_dmg" ]]; then
        say verify
        validate_and_stage "$local_dmg" "$local_dmg.sha256" || die invalid
        return
    fi
    say primary
    if fetch "$primary" "$work/package.dmg" && fetch "$primary.sha256" "$work/package.dmg.sha256"; then
        say verify
        if validate_and_stage "$work/package.dmg" "$work/package.dmg.sha256"; then return; fi
    fi
    # Detach before attempting another image; never delete an active mount point.
    unmount_package || die invalid
    rm -rf -- "$work/snow_shot.app"
    say fallback
    fetch 'https://api.github.com/repos/mg-chao/snow-apps/releases/latest' "$work/release.json" || die unavailable
    urls=$(github_urls "$work/release.json" 2>> "$work/diagnostic.log") || die unavailable
    dmg_url=${urls%%$'\n'*}; sum_url=${urls#*$'\n'}
    [[ "$dmg_url" != "$sum_url" && "$sum_url" != *$'\n'* ]] || die unavailable
    fetch "$dmg_url" "$work/package.dmg" || die unavailable
    fetch "$sum_url" "$work/package.dmg.sha256" || die unavailable
    say verify
    validate_and_stage "$work/package.dmg" "$work/package.dmg.sha256" || die unavailable
}

prepare_state() {
    state="$HOME/Library/Application Support/Snow Shot/Installer"
    # Check every existing ancestor before creating anything beneath it.
    local part="$HOME" component
    for component in Library 'Application Support' 'Snow Shot' Installer; do
        part="$part/$component"
        [[ ! -L "$part" && ( ! -e "$part" || ( -d "$part" && "$(stat -f %u "$part")" == "$(id -u)" ) ) ]] || { message state >&2; printf '%s\n' "$part" >&2; exit 1; }
    done
    mkdir -p "$state"
    chmod 700 "$state"
    mkdir "$state/lock" 2>/dev/null || { message state >&2; printf '%s\n' "$state" >&2; exit 1; }
    lock_owned=1
    for component in identity requirement creating; do
        [[ ! -L "$state/$component" && ( ! -e "$state/$component" || ( -f "$state/$component" && "$(stat -f %u "$state/$component")" == "$(id -u)" ) ) ]] || die identity
    done
}

prepare_identity() {
    local keychain="$HOME/Library/Keychains/login.keychain-db" fingerprint identities pkcs12_password
    [[ -f "$keychain" ]] || die identity
    if [[ -f "$state/identity" ]]; then
        fingerprint=$(tr '[:lower:]' '[:upper:]' < "$state/identity")
        [[ "$fingerprint" =~ ^[[:xdigit:]]{40}$ ]] || die identity
    else
        [[ ! -e "$state/creating" && ! -e "$state/requirement" ]] || die identity
        : > "$state/creating"
        say keychain
        cat > "$work/certificate.cnf" <<CERT
[req]
prompt = no
distinguished_name = subject
x509_extensions = signing
[subject]
CN = Snow Shot Local Installer $(uuidgen)
[signing]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = codeSigning
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
CERT
        run openssl req -new -newkey rsa:2048 -nodes -x509 -days 3650 \
            -config "$work/certificate.cnf" -keyout "$work/private.pem" -out "$work/certificate.pem" || die identity
        fingerprint=$(openssl x509 -in "$work/certificate.pem" -noout -fingerprint -sha1 | sed 's/.*=//; s/://g')
        [[ "$fingerprint" =~ ^[[:xdigit:]]{40}$ ]] || die identity
        pkcs12_password=$(openssl rand -hex 32) || die identity
        SNOW_INSTALLER_P12_PASSWORD="$pkcs12_password" run openssl pkcs12 -export \
            -inkey "$work/private.pem" -in "$work/certificate.pem" \
            -out "$work/identity.p12" -passout env:SNOW_INSTALLER_P12_PASSWORD || die identity
        run security import "$work/identity.p12" -k "$keychain" -P "$pkcs12_password" \
            -T /usr/bin/codesign || die identity
        unset pkcs12_password
        # User trust domain only, scoped to code signing (no -d or -A).
        run security add-trusted-cert -r trustRoot -p codeSign -k "$keychain" "$work/certificate.pem" || die identity
        printf '%s\n' "$fingerprint" > "$state/identity.tmp"
        mv -f "$state/identity.tmp" "$state/identity"
        rm -f "$state/creating" "$work/private.pem" "$work/identity.p12"
    fi
    identities=$(security find-identity -v -p codesigning "$keychain" 2>> "$work/diagnostic.log") || die identity
    [[ "$identities" == *"$fingerprint"* ]] || die identity
    signing_identity="$fingerprint"
}

sign_application() {
    say signing
    prepare_identity
    # Do not use --deep when signing: OCR's manifest hashes the embedded helpers.
    run codesign --force --sign "$signing_identity" --identifier com.snowshot.snow_shot \
        --keychain "$HOME/Library/Keychains/login.keychain-db" \
        "$work/snow_shot.app" || die identity
    run codesign --verify --deep --strict "$work/snow_shot.app" || die invalid
    if [[ -f "$state/requirement" ]]; then
        requirement=$(cat "$state/requirement")
        [[ -n "$requirement" ]] || die continuity
        run codesign --verify --strict -R "=$requirement" "$work/snow_shot.app" || die continuity
    else
        requirement=$(codesign -d -r- "$work/snow_shot.app" 2>> "$work/diagnostic.log" | sed -n 's/^# //; s/^designated => //p')
        [[ -n "$requirement" && "$requirement" != *cdhash* ]] || die continuity
    fi
    # Only the validated, locally signed staged copy is eligible for removal.
    if xattr -p com.apple.quarantine "$work/snow_shot.app" >/dev/null 2>&1; then
        run xattr -dr com.apple.quarantine "$work/snow_shot.app"
    fi
}

installed_pids() {
    pgrep -f '^/Applications/(Snow Shot|snow_shot)[.]app/Contents/MacOS/snow_shot( |$)' || [[ $? == 1 ]]
}

quit_installed() {
    local pids attempt
    pids=$(installed_pids)
    [[ -n "$pids" ]] || return 0
    say quit
    # NSRunningApplication requests normal termination without Finder automation.
    osascript -l JavaScript - "$pids" >> "$work/diagnostic.log" 2>&1 <<'JXA' || die running
ObjC.import('AppKit');
function run(argv) {
    argv[0].split(/\s+/).filter(Boolean).forEach(pid => {
        const app = $.NSRunningApplication.runningApplicationWithProcessIdentifier(Number(pid));
        if (app) app.terminate;
    });
}
JXA
    for ((attempt=0; attempt<20; attempt++)); do
        [[ -n "$(installed_pids)" ]] || return 0
        sleep 1
    done
    die running
}

install_application() {
    say install
    local applications_dir="${destination%/*}"
    previous_destination="$destination"
    # Move the old bundle into the existing transaction backup so success adopts
    # the product name and failure restores the original installation path.
    if [[ ! -e "$destination" && -e "$applications_dir/snow_shot.app" ]]; then
        previous_destination="$applications_dir/snow_shot.app"
    fi
    [[ -d "$applications_dir" && ! -L "$applications_dir" && ! -L "$destination" && ( ! -e "$destination" || -d "$destination" ) ]] || die invalid
    [[ ! -L "$previous_destination" && ( ! -e "$previous_destination" || -d "$previous_destination" ) ]] || die invalid
    if [[ ! -w "$applications_dir" ]] || [[ -e "$previous_destination" && ! -w "$previous_destination" ]]; then
        needs_sudo=1
        say sudo
        sudo -v
    fi
    slot=$(as_install mktemp -d "$applications_dir/.snow-shot-install.XXXXXX")
    if [[ "$needs_sudo" == 1 ]]; then as_install chown "$(id -u):$(id -g)" "$slot"; fi
    run ditto "$work/snow_shot.app" "$slot/new.app"
    run codesign --verify --deep --strict "$slot/new.app"
    quit_installed
    [[ ! -e "$previous_destination" ]] || had_previous=1
    if [[ "$had_previous" == 1 ]]; then as_install mv "$previous_destination" "$slot/previous.app"; fi
    replacement_started=1
    as_install mv "$slot/new.app" "$destination"
    run codesign --verify --deep --strict "$destination"
    run codesign --verify --strict -R "=$requirement" "$destination"
    printf '%s\n' "$requirement" > "$state/requirement.tmp"
    mv -f "$state/requirement.tmp" "$state/requirement"
    committed=1
    say done
    say permissions
    if [[ "$launch" == 1 ]]; then run open "$destination" || { message launch >&2; return 1; }; fi
}

cleanup() {
    local status=$? safe_to_remove=1
    trap - EXIT ERR INT TERM HUP
    set +e
    if [[ "$status" != 0 && -n "$work" && -s "$work/diagnostic.log" ]]; then
        tail -n 15 "$work/diagnostic.log" >&2
    fi
    if [[ -n "$slot" && "$committed" == 0 ]]; then
        if [[ -d "$slot/previous.app" ]]; then
            message rollback >&2
            as_install rm -rf -- "$destination" && as_install mv "$slot/previous.app" "$previous_destination" || safe_to_remove=0
        elif [[ "$replacement_started" == 1 && "$had_previous" == 0 && ! -e "$slot/new.app" ]]; then
            as_install rm -rf -- "$destination" || safe_to_remove=0
        fi
    fi
    if [[ -n "$slot" ]]; then
        if [[ "$safe_to_remove" == 1 ]]; then as_install rm -rf -- "$slot" || safe_to_remove=0; fi
        if [[ "$safe_to_remove" == 0 ]]; then message recovery >&2; printf '%s\n' "$previous_destination" >&2; printf '%s\n' "$slot" >&2; status=1; fi
    fi
    if ! unmount_package; then
        message detach >&2; printf '%s\n' "$mount_dir" >&2; status=1
        # Never recursively remove a directory that might still contain a mount.
        [[ -z "$work" ]] || rm -f "$work/private.pem" "$work/identity.p12"
    elif [[ -n "$work" ]]; then
        rm -rf -- "$work"
    fi
    if [[ "$lock_owned" == 1 ]]; then rmdir "$state/lock"; fi
    exit "$status"
}

main() {
    # Ignore caller-controlled executable search paths, particularly under sudo.
    export PATH=/usr/bin:/bin:/usr/sbin:/sbin
    umask 077
    local help=0 original_args=("$@")
    select_language
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --lang) [[ $# -ge 2 ]] || die arguments; language="$2"; shift 2 ;;
            --no-launch) launch=0; shift ;;
            --dmg) [[ $# -ge 2 && -n "$2" ]] || die arguments; local_dmg="$2"; shift 2 ;;
            --help|-h) help=1; shift ;;
            *) select_language; die arguments ;;
        esac
    done
    case "$language" in auto|en|zh-CN|zh-TW) ;; *) die arguments ;; esac
    select_language
    if [[ "$help" == 1 ]]; then message usage; message help; return; fi
    [[ "$(uname -s)" == Darwin ]] || die platform
    if [[ "$(id -u)" == 0 ]]; then
        [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != root && "$(id -u "$SUDO_USER")" != 0 && "$SUDO_USER" == "$(stat -f %Su /dev/console)" ]] || die root
        # Bash 3.2 treats an empty array as unset under nounset.
        exec sudo -H -u "$SUDO_USER" -- /bin/bash "${BASH_SOURCE[0]}" ${original_args[@]+"${original_args[@]}"}
    fi
    os_version=$(sw_vers -productVersion)
    version_supported "$os_version" 15 || die platform
    detect_architecture || die platform
    work=$(mktemp -d "${TMPDIR:-/tmp}/snow-shot-installer.XXXXXX")
    trap cleanup EXIT
    trap 'message failed >&2; exit 1' ERR
    trap 'exit 130' INT
    trap 'exit 143' TERM
    trap 'exit 129' HUP
    prepare_state
    obtain_package
    sign_application
    install_application
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then main "$@"; fi
