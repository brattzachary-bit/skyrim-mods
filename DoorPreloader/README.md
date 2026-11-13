# Door Preloader - loads what lies behind a door before you open it

SKSE plugin for Skyrim Special Edition / Anniversary Edition. When you come near a door, cave mouth or
any other load door, the models (and with them the textures) of the place behind it are loaded in the
background, so most of the work is already done when you step through.

## How it works

1. **Index.** The game only learns what a cell contains when it loads it. So after the game data has
   loaded, Door Preloader reads the placed objects straight from your plugin files and notes, for every
   interior cell and every exterior grid cell, which base objects stand in it. This takes a few seconds
   in the background and is saved to `DoorPreloader.cache` next to the SKSE logs. It is rebuilt only when
   the load order changes (plugin added, removed, reordered or updated).
2. **Watch.** A few times a second it looks for load doors within `fDistance` of the player and works
   out where each one leads: an interior cell, or a spot in a worldspace (then the cells around the
   arrival point are used).
3. **Load.** For the nearest destinations it asks the game's own model database for every model, on a
   background thread, and holds on to them. That includes the people who live there: skeleton, head,
   skin and the armour of their default outfit. When the real load runs, the game finds those models and
   their textures already in memory.
4. **Let go.** A destination nobody has been near for `uKeepSeconds` is released, and the game frees
   whatever is no longer used.

## What it does not speed up

Levelled (random) characters and creatures, terrain and distant LOD, navmesh, scripts and the save-game changes of
a cell are still loaded by the game at the door. Shader compilation on a first visit is not affected
either.

## Measuring

With `bLoadTimes = 1` the log (`DoorPreloader.log`) shows for every arrival how much of the destination had
been prepared, and how long any loading screen was up. Compare with `bEnabled = 0`.

## Requirements

SKSE64 and Address Library for SKSE Plugins. No plugin file, safe to add or remove at any time.

## Building

CMake + vcpkg + CommonLibSSE-NG (expected at `extern/CommonLibSSE-NG`). `./build.sh` cross-compiles on
Linux with clang-cl and stages `dist/DoorPreloader`.

## Licence and credits

MIT, see `LICENSE`. Uses CommonLibSSE-NG (MIT), SimpleIni (MIT), spdlog and fmt (MIT).
