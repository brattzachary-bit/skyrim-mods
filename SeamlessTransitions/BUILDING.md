# Building Seamless Door and Travel Transitions

This is the complete corresponding source of the released binary (GPL-3.0).

## What is here

- `src/main.cpp`, `src/PCH.h`, `include/` - the plugin. `main.cpp` is LoadingFlow 1.2.0's `main.cpp` with
  the modifications listed in `licences/NOTICE.txt`; the unmodified original is in
  `upstream-LoadingFlow-1.2.0/main.cpp.orig` for comparison.
- `extern/minhook` - MinHook 1.3.4 (BSD-2-Clause), built as a static library by `CMakeLists.txt`.
- `mcm/make_mcm.py` - writes the MCM Helper package: `SeamlessTransitions.esp`, the two compiled scripts
  (sources in `mcm/source`), `config.json` and `settings.ini`. Run `python3 mcm/make_mcm.py`.
- `SeamlessTransitions.ini`, `SeamlessTransitions_Rules.txt` - shipped next to the DLL.

## Dependencies not included

- CommonLibSSE-NG (MIT), https://github.com/alandtse/CommonLibSSE-NG at v9.2.0
  (commit 6ef06fb40fc475680f188469f83a162ac64b5039). Place or link it at `extern/CommonLibSSE-NG`.
- vcpkg for the packages in `vcpkg.json` (fmt, spdlog, directxtk, simpleini, ...).

## Build

The released DLL was cross-compiled on Linux with clang-cl, the Windows SDK from xwin and vcpkg, using
the preset in `CMakePresets.json`:

    ./build.sh

On Windows any recent MSVC or clang-cl works: configure with the vcpkg toolchain and build the
`SeamlessTransitions` target. The build targets the Anniversary Edition runtimes (`ENABLE_SKYRIM_SE` is
off in `CMakeLists.txt`).
