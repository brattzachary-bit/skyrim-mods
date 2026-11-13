# Skyrim mods

Source for my released Skyrim Special Edition / Anniversary Edition SKSE plugins. The mods themselves are on
Nexus; this repository is where their source lives.

| Folder | What it is | Status | Licence |
|---|---|---|---|
| [SeamlessTransitions](SeamlessTransitions) | **Seamless Door and Travel Transitions - No Loading Screens.** Doors hold the last view and dissolve into the next place; fast travel, lifts, portals, jail, sleep and more get their own transitions. A modified version of [LoadingFlow](https://github.com/RandyLuaW/LoadingFlow) by RandyLua. | Released on Nexus | GPL-3.0 |
| [DoorPreloader](DoorPreloader) | **Door Preloader.** Loads the models of the place behind a door while you walk up to it. | Released on Nexus | MIT |

## Seamless Door and Travel Transitions: source and licence

`SeamlessTransitions/` is the complete corresponding source of the binaries published on Nexus, as GPL-3.0 requires.

- `src/main.cpp` is LoadingFlow 1.2.0's `main.cpp` with my changes; each change is tagged in the code and listed in
  [`licences/NOTICE.txt`](SeamlessTransitions/licences/NOTICE.txt).
- The unmodified original is kept in [`upstream-LoadingFlow-1.2.0/`](SeamlessTransitions/upstream-LoadingFlow-1.2.0)
  for comparison, with its own licence files.
- LoadingFlow is dual licensed; this work uses its GPL-3.0 option (see `licences/LICENSE.txt`, section 9).
- Build instructions: [`BUILDING.md`](SeamlessTransitions/BUILDING.md).

Not affiliated with or endorsed by the author of LoadingFlow.

## Building

Each plugin is a CMake project built against [CommonLibSSE-NG](https://github.com/alandtse/CommonLibSSE-NG)
(expected at `extern/CommonLibSSE-NG` inside the project) with vcpkg. Each folder has a `build.sh` that cross-compiles
on Linux with clang-cl and the Windows SDK from xwin; on Windows, configure with the vcpkg toolchain and build as usual.
