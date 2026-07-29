#!/usr/bin/env bash
# Usage:
#   ./build-local.sh          # incremental build (default)
#   ./build-local.sh --clean  # full clean build
#   ./build-local.sh --debug  # incremental debug build
#   ./build-local.sh --clean --debug

set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly BUILD_TEMP="${SCRIPT_DIR}/.build_temp"
readonly GCC_DIR="${SCRIPT_DIR}/PowerEditor/gcc"
readonly MINGW_BIN="/c/msys64/mingw64/bin"
readonly MSYS_BIN="/c/msys64/usr/bin"

CLEAN=0
DEBUG=0
for arg in "$@"; do
    case "${arg}" in
        --clean) CLEAN=1 ;;
        --debug) DEBUG=1 ;;
        *) printf 'Unknown option: %s\nUsage: %s [--clean] [--debug]\n' "${arg}" "$0" >&2; exit 1 ;;
    esac
done

if [[ ! -f "${GCC_DIR}/makefile" ]]; then
    printf 'Error: run this script from the repository checkout.\n' >&2
    exit 1
fi

export PATH="${MINGW_BIN}:${MSYS_BIN}:${PATH}"

command -v mingw32-make >/dev/null 2>&1 || {
    printf 'Error: mingw32-make was not found in %s.\n' "${MINGW_BIN}" >&2
    exit 1
}

mkdir -p -- "${BUILD_TEMP}"

if [[ "${CLEAN}" -eq 1 ]]; then
    printf 'Removing previous build output...\n'
    rm -rf -- "${BUILD_TEMP:?}"/*
    rm -rf -- "${GCC_DIR}"/bin.gcc.* "${GCC_DIR}"/bin.clang.*
fi

printf 'Generating the library version header...\n'
cmd.exe //C "PowerEditor\\src\\NppLibsVersionH-generator.bat"

MAKE_ARGS=(-j"$(nproc)" PREBUILD_EVENT_CMD=:)
if [[ "${DEBUG}" -eq 1 ]]; then
    MAKE_ARGS+=(DEBUG=1)
fi

if [[ "${CLEAN}" -eq 1 ]]; then
    printf 'Full clean build with MinGW-w64'
else
    printf 'Incremental build with MinGW-w64'
fi
[[ "${DEBUG}" -eq 1 ]] && printf ' (debug)' || printf ' (release)'
printf '...\n'
printf 'Build log: %s/build.log\n' "${BUILD_TEMP}"

(
    cd -- "${GCC_DIR}"
    mingw32-make "${MAKE_ARGS[@]}"
) 2>&1 | tee "${BUILD_TEMP}/build.log"

printf 'Build completed successfully.\n'
