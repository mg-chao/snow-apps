#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/snow-build-environment.sh"
if [[ "${1:-}" == --help ]]; then
    echo 'Usage: run-snow-shot.sh [macOS-preset] [--clean] [--no-build] [--codesign-identity IDENTITY] [-- APP_ARGUMENTS...]'
    echo '--no-build launches the existing deployed app without reinstalling or signing it.'
    echo '--codesign-identity overrides the automatically provisioned local signing identity.'
    exit 0
fi
snow_require_macos
preset=''
if [[ $# -gt 0 && "$1" != --* ]]; then preset="$1"; shift; fi
snow_select_preset "$preset"
clean=false
build=true
codesign_identity=''
codesign_identity_set=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) clean=true; shift ;;
        --no-build) build=false; shift ;;
        --codesign-identity)
            [[ $# -ge 2 ]] || snow_die '--codesign-identity needs an identity'
            [[ -n "$2" ]] || snow_die '--codesign-identity needs an identity'
            codesign_identity="$2"
            codesign_identity_set=true
            shift 2
            ;;
        --) shift; break ;;
        *) snow_die "Unknown argument: $1" ;;
    esac
done
deployed_app="$snow_build_dir/run/snow_shot.app"
wait_for_process_exit() {
    local pid="$1"
    local attempts="$2"
    local attempt
    for ((attempt = 0; attempt < attempts; ++attempt)); do
        if ! kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}
stop_running_build_instances() {
    local pid
    local executable_path
    while read -r pid executable_path; do
        [[ -n "$pid" && "${executable_path##*/}" == snow_shot ]] || continue
        case "$executable_path" in
            "$snow_build_dir"/*) ;;
            *) continue ;;
        esac

        printf 'Stopping the running development instance (PID %s)...\n' "$pid"
        if ! kill -TERM "$pid" 2>/dev/null; then
            kill -0 "$pid" 2>/dev/null && snow_die "snow_shot process $pid could not be stopped."
            continue
        fi
        if wait_for_process_exit "$pid" 15; then
            continue
        fi

        kill -KILL "$pid" 2>/dev/null || true
        wait_for_process_exit "$pid" 50 || snow_die "snow_shot process $pid did not stop."
    done < <(ps -axww -o pid= -o comm=)
}
if [[ "$build" == false && "$clean" == true ]]; then
    snow_die '--clean cannot be combined with --no-build'
fi
if [[ "$build" == false && "$codesign_identity_set" == true ]]; then
    snow_die '--codesign-identity cannot be combined with --no-build'
fi
if [[ "$build" == false ]]; then
    [[ -x "$deployed_app/Contents/MacOS/snow_shot" ]] || snow_die "The deployed Snow Shot was not found for $snow_preset. Run without --no-build to create it."
    # Preserve the exact signed bundle to which the user granted permissions.
    exec open -n "$deployed_app" --args "$@"
fi
stop_running_build_instances
build_args=("$snow_preset" --target snow_shot)
if [[ "$clean" == true ]]; then build_args+=(--clean); fi
if [[ "$codesign_identity_set" == false ]]; then
    codesign_identity="$("$(dirname "$0")/ensure-macos-codesign-identity.sh")"
fi
build_args+=(-- "-DSNOW_MACOS_CODESIGN_IDENTITY=$codesign_identity")
"$(dirname "$0")/build.sh" "${build_args[@]}"
app="$snow_build_dir/snow_shot/snow_shot.app"
[[ -x "$app/Contents/MacOS/snow_shot" ]] || snow_die "Snow Shot was not found for $snow_preset. Run without --no-build to create it."
# Deploy a separate development copy so dlopen-only OCR dependencies and Qt
# plugins resolve exactly as they do in a package. Leave build products intact.
export PATH="$snow_repo_root/.tools/macos-dev/bin:$PATH"
cmake --install "$snow_build_dir" --component SnowShot --prefix "$snow_build_dir/run"
# The development bundle keeps the same path and version between builds. Force
# LaunchServices to discard stale metadata (including a previously missing icon)
# before Finder and the Dock resolve the bundle for the next launch.
touch "$deployed_app"
default_launch_services_register='/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister'
launch_services_register="${SNOW_LAUNCH_SERVICES_REGISTER:-$default_launch_services_register}"
"$launch_services_register" -f "$deployed_app"
# LaunchServices establishes the bundle identity used by macOS permissions.
exec open -n "$deployed_app" --args "$@"
