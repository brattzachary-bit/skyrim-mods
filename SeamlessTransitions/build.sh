#!/usr/bin/env bash
# Configure (first run) and build, then stage dist/SeamlessTransitions ready to install as a mod.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="$HOME/.cargo/bin:$PATH"
export LLVM_MINGW_BIN="$HOME/opt/llvm-mingw/bin"
export XWIN_SYSROOT="$HOME/xwin-out"
export VCPKG_ROOT="$HOME/.local/share/vcpkg"
[[ -f build/CMakeCache.txt ]] || cmake --preset linux-clangcl > /dev/null
cmake --build --preset linux-clangcl
mkdir -p dist/SeamlessTransitions/SKSE/Plugins
cp build/SeamlessTransitions.dll dist/SeamlessTransitions/SKSE/Plugins/
cp SeamlessTransitions.ini SeamlessTransitions_Rules.txt dist/SeamlessTransitions/SKSE/Plugins/
echo "Staged dist/SeamlessTransitions"
