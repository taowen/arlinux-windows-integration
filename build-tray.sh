#!/usr/bin/env bash
# Build the Windows Job Object task owner (independent of MSAA).
set -euo pipefail
[[ $# == 2 ]] || { echo 'Usage: windows-tray.sh LLVM_MINGW_BIN OUTPUT_DIR' >&2; exit 2; }
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
compiler="$(realpath "$1")/aarch64-w64-mingw32-clang++"
output="$(realpath -m "$2")"
mkdir -p "$output"
"$compiler" -std=c++17 -static -Wall -Wextra -Werror "$repo/tray/windows-tray.cpp" \
    -luser32 -lshell32 -Wl,--no-insert-timestamp -o "$output/windows-tray.exe"
sha256sum "$output/windows-tray.exe"
