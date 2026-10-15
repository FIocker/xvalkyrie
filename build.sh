#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
config=Release
run_tests=false

for arg in "$@"; do
    case "$arg" in
        Debug|Release) config="$arg" ;;
        --test) run_tests=true ;;
        -h|--help)
            printf 'Usage: %s [Debug|Release] [--test]\n' "${0##*/}"
            exit 0
            ;;
        *)
            printf 'Unknown option: %s\nUsage: %s [Debug|Release] [--test]\n' \
                "$arg" "${0##*/}" >&2
            exit 2
            ;;
    esac
done

build="$root/build/$config"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE="$config"
cmake --build "$build" --parallel "${JOBS:-4}"
if "$run_tests"; then
    ctest --test-dir "$build" --output-on-failure
fi
printf 'Built: %s/xscarlet\n' "$build"
