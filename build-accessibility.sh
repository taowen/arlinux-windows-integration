#!/usr/bin/env bash
# Native ARM64 Windows observer; uses the existing Wine runtime.
set -euo pipefail
if [[ $# != 2 ]]; then
    echo 'Usage: windows-accessibility.sh LLVM_MINGW_BIN OUTPUT_DIR' >&2
    exit 2
fi
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
compiler="$(realpath "$1")/aarch64-w64-mingw32-clang++"
output="$(realpath -m "$2")"
mkdir -p "$output"
"$compiler" -std=c++17 -static -Wall -Wextra -Werror \
    "$repo/accessibility/windows-accessibility.cpp" \
    -loleacc -lole32 -loleaut32 -luuid -Wl,--no-insert-timestamp \
    -o "$output/windows-accessibility.exe"
sha256sum "$output/windows-accessibility.exe"
