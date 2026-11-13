#!/usr/bin/env bash
# Configure (first run) and build, then stage dist/DoorPreloader ready to install as a mod.
set -euo pipefail
cd "$(dirname "$0")"
export PATH="$HOME/.cargo/bin:$PATH"
export LLVM_MINGW_BIN="$HOME/opt/llvm-mingw/bin"
export XWIN_SYSROOT="$HOME/xwin-out"
export VCPKG_ROOT="$HOME/.local/share/vcpkg"
[[ -f build/CMakeCache.txt ]] || cmake --preset linux-clangcl > build-configure.log 2>&1 || { tail -40 build-configure.log; exit 1; }
cmake --build --preset linux-clangcl
mkdir -p dist/DoorPreloader/SKSE/Plugins
cp build/DoorPreloader.dll DoorPreloader.ini dist/DoorPreloader/SKSE/Plugins/
echo "Staged dist/DoorPreloader"
