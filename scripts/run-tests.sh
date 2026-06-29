#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

preset="linux-debug"
run_ctest=1
build_samples=0
configure=1
build=1

usage() {
    cat <<'USAGE'
Usage: scripts/run-tests.sh [options] [-- <rmlui_bgfx_tests args>]

Options:
  --preset <name>       CMake configure/build preset to use (default: linux-debug).
  --release             Shortcut for --preset linux-release.
  --no-configure        Do not run cmake --preset before building.
  --no-build            Do not build rmlui_bgfx_tests before running it.
  --no-ctest            Do not run ctest after the test executable.
  --samples             Also build the samples-all preset after tests.
  -h, --help            Show this help.

Examples:
  scripts/run-tests.sh
  scripts/run-tests.sh -- --list-tests
  scripts/run-tests.sh --release
  scripts/run-tests.sh --samples
USAGE
}

test_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset)
            if [[ $# -lt 2 ]]; then
                echo "error: --preset requires a value" >&2
                exit 2
            fi
            preset="$2"
            shift 2
            ;;
        --release)
            preset="linux-release"
            shift
            ;;
        --no-configure)
            configure=0
            shift
            ;;
        --no-build)
            build=0
            shift
            ;;
        --no-ctest)
            run_ctest=0
            shift
            ;;
        --samples)
            build_samples=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            test_args=("$@")
            break
            ;;
        *)
            echo "error: unknown option: $1" >&2
            echo >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "$preset" in
    linux-debug)
        build_dir="build/linux-debug"
        ;;
    linux-release)
        build_dir="build/linux-release"
        ;;
    *)
        build_dir="build/$preset"
        ;;
esac

test_binary="$build_dir/rmlui_bgfx_tests"

if [[ "$configure" -eq 1 ]]; then
    echo "[run-tests] configuring preset: $preset"
    cmake --preset "$preset"
fi

if [[ "$build" -eq 1 ]]; then
    echo "[run-tests] building rmlui_bgfx_tests with preset: $preset"
    cmake --build --preset "$preset" --target rmlui_bgfx_tests
fi

if [[ ! -x "$test_binary" ]]; then
    echo "error: test binary is not executable or does not exist: $test_binary" >&2
    echo "hint: run with configure/build enabled, or check the selected preset." >&2
    exit 1
fi

echo "[run-tests] running: $test_binary ${test_args[*]}"
"$test_binary" "${test_args[@]}"

if [[ "$run_ctest" -eq 1 ]]; then
    echo "[run-tests] running ctest for: $build_dir"
    ctest --test-dir "$build_dir" --output-on-failure -R rmlui_bgfx || {
        status=$?
        echo "[run-tests] ctest exited with status $status" >&2
        echo "[run-tests] note: some local build trees may not have registered Catch2 tests; the test executable above is authoritative for this repo." >&2
        exit "$status"
    }
fi

if [[ "$build_samples" -eq 1 ]]; then
    echo "[run-tests] building samples-all"
    cmake --build --preset samples-all
fi
