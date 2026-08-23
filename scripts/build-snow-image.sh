#!/usr/bin/env sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
preset=${1:-windows-msvc-debug}
cmake --preset "$preset" -S "$repo_root"
cmake --build --preset "build-$preset" --target snow_image_static --parallel
