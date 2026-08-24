// Qt-free CLI dialog guard for test executables: registers the native
// Windows/MSVC interception layers (see snow_test_cli_guard.h) through a
// global constructor, without pulling in any Qt dependency.

#include "snow_test_cli_guard.h"

namespace {

struct NativeCliGuardInstaller {
    NativeCliGuardInstaller() {
        snow_test_cli::install_native_guards();
    }
};

const NativeCliGuardInstaller nativeCliGuardInstaller = {};

} // namespace
