#!/bin/bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
args=(--dry-run --Werror --style=file)
case "${1:-}" in
    --fix) args=(-i --style=file) ;;
    '') ;;
    *) echo 'Usage: check-cpp-format.sh [--fix]' >&2; exit 2 ;;
esac
cd "$repo_root"
while IFS= read -r -d '' file; do
    case "$file" in
        *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx|*.mm)
            clang-format "${args[@]}" "$file" ;;
    esac
done < <(git ls-files -z --cached --others --exclude-standard -- \
    ant_design_qt snow_draw_engine_qt snow_image snow_image_viewer snow_shot)
