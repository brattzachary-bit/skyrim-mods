// Seamless Door and Travel Transitions
//
// This file is a modified version of LoadingFlow 1.2.0 by RandyLua (https://github.com/RandyLuaW/LoadingFlow),
// used and distributed under the GNU General Public License version 3.0, which LoadingFlow offers as an
// alternative licence (see LICENSE, section 9, and LICENSE-GPL-3.0).
// Modified by zbratt, November 2026. The changes are marked in the code with these tags:
//   [Seamless]  hold the last frame and dissolve into the new cell     [EaseIn]  HUD-free picture, slow push-in
//   [FGPause]   pause FidelityFX frame generation during a transition  [TravelFx] covers for loads that are not doors
//   [Styles]    more transition styles, MCM settings, previews         [Rules]   per-door / per-place transitions, rule files
//   [Safe]      start-up self-check; frames are never withheld without a working overlay
//   [DLSSG]     experimental pause of DLSS frame generation (Streamline)
//   [Compat]    Loading Menu open/close events are announced for other mods (DynDOLOD)
// LoadingFlow copyright (c) 2026 RandyLua. Modifications copyright (c) 2026 zbratt. No warranty.

#include <MinHook.h>
#include <algorithm>
#include <sstream>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// IMPORTANT (P2-9): Keep <RE/Skyrim.h> BEFORE "SKSEMenuFramework.h" (which includes <windows.h>).
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>

#include "BlueprintBuilder.h"

#if defined(LOADINGFLOW_WITH_MENU)
#	include "SKSEMenuFramework.h"
#endif

// D3D11 types for the void overlay (v1.4.0). These MUST come after every CommonLib header:
// REX/W32/BASE.h refuses to compile once the Windows API has been pulled in, and <d3d11.h>
// drags in the whole world. Nothing here links d3d11.lib or d3dcompiler -- we only call COM
// methods on interfaces the renderer already owns, and the shaders are precompiled bytecode,
// so the DLL's import table stays system-DLL only (keep it that way: the audit checks it).
#include <d3d11.h>
#include <dxgi1_2.h>

#include "VoidOverlayShader.h"

namespace logger = SKSE::log;
using namespace std::literals;

// This CommonLib has no RE::DebugNotification; the HUD message route is the same thing.
// [Safe] A file name or path as text, for logs and comparisons. std::filesystem's own .string() throws when a name has
// characters the system code page cannot express (a file with a Chinese or Cyrillic name anywhere in the folder was
// enough to crash the game at start-up); these two never throw.
inline std::string PathText(const std::filesystem::path& a_path, unsigned int a_codePage = 65001 /* UTF-8 */)
{
    const std::wstring& wide = a_path.native();
    if (wide.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(a_codePage, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0) {
        ::WideCharToMultiByte(a_codePage, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
    }
    return out;
}

namespace lfnotify
{
    inline void Show(const char* a_msg)
    {
        if (a_msg) {
            RE::SendHUDMessage::ShowHUDMessage(a_msg, nullptr, true);
        }
    }
}

namespace
{
    // =========================================================================
    //  Configuration (persisted to Data/SKSE/Plugins/SeamlessTransitions.ini)
    // =========================================================================
    struct Config
    {
        bool          enabled = true;
        bool          allowInteriorToInterior = true;
        bool          allowPocketToExterior = true;
        // P1-1: OFF by default for the first playtest. Pulling an NPC to Z=+26000 is only safe once
        // the return path has been verified in-game; until then an empty-looking shop beats an NPC
        // stranded at pocket altitude in a save you keep playing.
        bool          moveActorsToPocket = false;
        bool          spawnBlackBoxShell = true;
        bool          playDoorSoundAndAnim = true;
        bool          triggerBlinkImod = true;
        bool          sneakForcesVanillaLoad = true;
        bool          showDebugNotifications = false;
        // L2 split into two independent switches (post-research correction):
        //  - location change is well-formed: TESActorLocationChangeEvent carries actor + old/new
        //    location and matches Papyrus OnLocationChange(Location akOldLoc, Location akNewLoc).
        //  - cell attach/loaded is NOT demonstrably ref-scoped. Engine semantics per the Nexus
        //    "OnCellAttach / OnCellDetach" thread is "the CELL enters the world", and the SKSE event
        //    carries only {reference, attached} with no cell field -- if the VM interprets it
        //    cell-wide it would fire OnCellAttach on every ref of the player's loaded cell (all of
        //    Tamriel). Separate switch, default off, so the risky half is never enabled by accident.
        // Both fake-event switches are off by default: with real transitions everywhere the engine
        // raises these events itself, and faking them would only confuse other mods.
        bool          fakeLocationEvents = false;
        bool          fakeCellLoadEvents = false;
        // MASTER ROUTING SWITCH. 0 (default) = every door uses the engine's own cell transition, so
        // navmesh, AI packages, NPCs, combat, interior lighting and quest events are 100% vanilla;
        // LoadingFlow then only hides/masks the loading menu. 1 = the legacy high-altitude clone
        // (zero I/O, but no navmesh and no NPCs -- kept only for A/B experiments).
        bool          usePocketCopies = false;
        bool          suppressLoadingMenu = true;
        std::uint32_t suppressTimeoutMs = 400;
        // How long after the door activation we still treat a LoadingMenu request as belonging to
        // THIS transition. Measured in-game: the engine requests the menu 0.6-1.0 s after Activate
        // (it fades and starts the load first), so a 400 ms window expired before the menu was even
        // asked for. suppressTimeoutMs keeps its own meaning: after we HAVE suppressed, how long we
        // wait for the swap before re-posting the menu.
        std::uint32_t transitionWindowMs = 10000;
        // Tier 3: proximity pre-warm. While walking around, demand the models of interiors that are
        // routed to a real transition, so the actual load finds them already in the model DB.
        bool          prewarmNearbyDoors = true;
        std::uint32_t prewarmModelCap = 200;
        float         prewarmRadius = 800.0f;
        // Visual envelope. We cannot shorten the real load, and the engine already plays the door
        // sound and its own fade -- so the only thing worth adding is TIMING: hold the screen black
        // for a beat after the cell swap so the fade-in does not reveal half-streamed geometry.
        // OFF by default. An image-space black only affects the WORLD render; the UI/Scaleform layer
        // is composited afterwards, so mods that draw widgets in the interface (BTPS and friends)
        // keep spinning on top of the black. Covering the swap artefact therefore does not work for
        // anyone who runs such a UI mod -- and the extra black is pure loss.
        bool          visualEnvelope = false;
        std::uint32_t envelopeBlackHoldMs = 300;
        float         envelopeFadeStrength = 1.0f;
        // "No black" mode: instead of letting the engine fade to black, stop its fade records for
        // the whole transition so the last rendered frame simply holds until the new cell appears.
        // Not black -- but a freeze. Long loads will look like a hitch, which is why the adaptive
        // rule (fall back to the vanilla menu on slow cells) exists.
        bool          noBlackTransition = false;
        // Policy: freeze-frame only makes sense for BUILDING doors. Cave/mine entrances are already
        // dark inside, so holding the last frame there buys nothing and reads as a still image --
        // those keep the normal fade. 1 (default) = apply no-black only to manual doors.
        bool          noBlackOnlyForManualDoors = true;
        // Cut mask: a brief effect fired exactly at the moment the cell swap lands, to hide the hard
        // cut from "door half open" to "inside". It is NOT the long black screen -- default 80 ms,
        // and the record is swappable (black family, FadeToWhite 0x0010100B, blur 0x000B97F7,
        // desaturate 0x000B7983 ...). 0 = no mask at all.
        std::uint32_t cutMaskImod = 0x000F756D;
        std::uint32_t cutMaskMs = 0;   // v1.5.2: default OFF -- see the note at the call site
        // Per-door diagnostic. Measured in-game: NPCs activate doors constantly (502 of 559 log
        // lines in one 3-minute session), so this stays off unless explicitly requested.
        bool          logDoorActivations = false;
        // Adaptive fallback. 0 = always suppress. When > 0, a cell whose last measured load took
        // longer than this shows the vanilla loading menu instead of 2 s of black: on this modlist
        // interior loads measure 1884-3269 ms, which is too long to hide.
        std::uint32_t suppressMaxMs = 0;
        // Adaptive freeze guard. Freezing only works while the load is short; once a cell has
        // been measured as slower than this, that cell uses the fade instead. 0 = never veto freeze.
        // This keeps the freeze route alive without gambling on every cell being fast.
        std::uint32_t freezeMaxMs = 600;
        // Pre-load wait override. MEASURED: from pressing E to the moment the engine even asks for
        // the LoadingMenu takes 1002-1022 ms, and it stayed at ~1 s even while we were cancelling
        // every fade record -- so that second is a TIMER inside the engine, not the fade itself.
        // "fFadeToBlackFadeSeconds" is the matching game setting. Negative = leave the engine alone;
        // 0.15-0.30 shortens the pre-load wait for every load door.
        // Engine transition timers, values taken from 1001Bits/Faster-Loadscreens (GPL-3.0) which
        // documents the vanilla defaults. These are INI-backed settings ("name:Section"), NOT
        // GameSettings -- looking them up in GameSettingCollection silently fails.
        // Measured on our own log: pressing E -> engine asks for the LoadingMenu takes ~1.0 s, which
        // is fFadeToBlackFadeSeconds at its vanilla 1.0.
        bool  tweakEngineTimers = true;
        float fadeToBlackFadeSeconds = 0.10f;    // vanilla 1.0  <- the pre-load wait
        float normalDoorFadeSecs = 0.10f;        // vanilla ?
        float normalDoorFadeWait = -1.0f;        // -1 = leave vanilla (measured 0.01 on this build)
        float autoDoorFadeSecs = 0.10f;
        float minSecondsForLoadFadeIn = 0.20f;   // vanilla 1.5; the upstream mod settled on 0.20
        // v1.6.0: 100 ms starves post-load scripts (reported crash with IED+SPID, and JK clutter
        // ending up scattered). 800 keeps most of the gain.
        // v1.6.4: default goes back to the v1.1.0 published value. One machine showing no difference
        // is NOT evidence that it makes no difference everywhere -- that inference was over-reach.
        // The menu now carries the trade-off (Stable restores the game's own values).
        float postLoadUpdateTimeMS = 100.0f;     // vanilla 500, but THIS MACHINE ships 2000 in
                                                 // Skyrim.ini [Papyrus] -- 2 s of pure waiting that
                                                 // happens AFTER the cell change, i.e. outside our
                                                 // own N measurement. -1 = leave alone.
        float actorTeleportFadeSeconds = 0.10f;  // fade used when NPCS are teleported with you
        // Cap on how much the engine may preload. -1 = leave the engine's value alone.
        // The ON/OFF switch (bPreloadLinkedInteriors) has been REMOVED from this plugin: enabling it
        // hangs save loading on this load order at both the 25 MiB default cap and ~95 MiB, so it is
        // never touched. Only the cap remains adjustable, and it has shown no measurable effect.
        int   preloadSizeLimit = -1;
        float fastTravelFadeSecs = -1.0f;        // -1 = leave vanilla (0.5)
        // v1.6.5: back to vanilla. We have NO measurement showing 500 buys anything, and a plausible
        // mechanism by which it costs frame time after a load -- so it should not be the default.
        // (Unlike postLoad below, where an aggressive value plausibly does buy speed.)
        int   loadingQueuedPriorityBudgetMs = 20;
        // Release curve for the cut mask. This is the one place where an IMAD-to-IMAD blend is both
        // needed and reachable: Papyrus documents RemoveCrossFade as "fading it out over the
        // specified number of seconds" and CommonLib wraps it as StopCrossFade. (The forward
        // direction, ApplyCrossFade, is NOT wrapped by CommonLibSSE-NG and has no published
        // relocation ID -- guessing one would call an arbitrary function, so it is not used.)
        float         cutMaskFadeOut = 0.15f;
        // Continuity: do not freeze the instant the door is activated -- let the door's own opening
        // animation play for this long first, THEN start cancelling the fades. The held frame is
        // then "door already swinging open" instead of "hand on the handle", which reads as a
        // continuous motion into the cut. 0 = freeze immediately (old behaviour).
        std::uint32_t freezeDelayMs = 0;
        // Walk-through passages (mod cave tunnels): no door, you just walk in and it loads. They are
        // auto/frameless doors with a CONTINUOUS scene on both sides, so freezing on the last frame
        // shows no loading screen -- that is what Interiors->Interior transit gets here. Ext->Int (cave
        // mouth from outside) still uses the normal fade.
        bool          noBlackForWalkThrough = true;
        // Global mode: suppress the LoadingMenu even when LoadingFlow's own door hook was not what
        // started the transition (trigger volumes, script MoveTo, anything else). Long loads fall
        // back automatically because the timeout re-posts the menu -- which is exactly what keeps
        // fast travel / load-game readable.
        bool          globalMenuSuppression = false;
        float         baseAltitudeZ = 26000.0f;
        float         slotStepZ = 3500.0f;
        float         blinkStrength = 1.0f;
        std::uint32_t doorOpenDelayMs = 260;
        std::uint32_t maxRefsPerCell = 800;
        std::uint32_t maxPocketSlots = 8;
        // ---- Assembly probe (READ-ONLY diagnostics) -------------------------------------------
        // After a transition completes, sample the player's 3D transform and the camera FOV for a
        // short window and report when they stop changing. This answers ONE question:
        //   how long does the engine keep assembling / animating the scene after the cell swap?
        // That number decides how long a retained frame would have to stay up to hide the assembly.
        // Read-only by design: it never writes a transform, never touches D3D, and only reads
        // objects the game already owns. Bounded by probeAssemblyMs per transition.
        bool          probeAssembly = false;
        std::uint32_t probeAssemblyMs = 2500;
        // Also read the engine's renderer struct (device / context / swap chain / main RT pointers)
        // so we know whether a capture path is reachable at all, and whether those pointers survive
        // a transition. Read-only; no COM call, no resource creation.
        bool          probeGfx = false;
        // ---- B: keep the presented frame instead of painting black (EXPERIMENTAL) --------------
        // Stops calling Present for the duration of the transition, so the display keeps showing
        // the exact frame the player last saw (ENB / CS included, because we never touch pixels).
        // Default OFF: this is the only COM vtable the plugin ever patches.
        bool          freezeFramePresent = false;
        std::uint32_t freezeFrameHoldMs = 300;
        // ---- Cover AT THE SWAP only (v1.2.2) ---------------------------------------------------
        // The engine's scene spin was measured at ~21 frames at 60 fps (about 350 ms) -- far shorter
        // than the load itself. Covering the whole load therefore buys nothing and reads as a hang.
        // When uCoverAtSwapMs > 0 the early cover (raised at menu suppression) is DISABLED and the
        // cover is applied only when the cell swap is detected, for at most this long.
        //   iCoverAtSwapMode: 1 = freeze the presented image (skip Present), 2 = black (IMAD)
        std::uint32_t coverAtSwapMs = 0;   // clamped to 350 by the loader
        int           coverAtSwapMode = 1;
        // ---- Void effect: give the 124 dead frames something to do (v1.2.3) --------------------
        // Measured timeline: press E -> hand touches the door, then ~124 frames (~2.07 s) where the
        // old cell is unloading and nothing new is drawn (the screen is one static frame), then ~21
        // frames (~350 ms) of scene-spin, then the new cell. Covering the 2 s is pointless; filling
        // it is not. This ramps an image-space darkening over the void so the wait reads as a
        // deliberate fade rather than a frozen picture, then takes it to full black across the spin
        // and fades back into the finished cell.
        std::uint32_t voidRampMs = 600;      // how long the darkening takes to reach fVoidDarken
        float         voidDarken = 0.0f;    // 0 = off; 0.75 keeps the old scene faintly visible
        std::uint32_t voidHoldMs = 220;      // full-black hold after the swap, before fading back
        // ---- World probe (v1.3.0, READ-ONLY) ---------------------------------------------------
        // The open question is whether the ~124 dead frames are dead because the world is PAUSED or
        // because the old cell is gone. If game time keeps advancing we may be able to keep the world
        // looking alive during the wait, which would beat any cover effect.
        bool          probeWorld = false;
        // ---- Step 2 probe (v1.3.1): present-hook ORDER + optional frame dump --------------------
        // capProbe   : log which module sits above/below us in the Present hook chain (no D3D call)
        // captureFrame: save a few backbuffer frames as BMP into LoadingFlow-capture/ for eyeballing
        bool          capProbe = false;
        bool          captureFrame = false;
        // v1.4.5: idle sampling used to eat the whole budget before the first door (16/16 captures were
        // tag=live). Off by default -- the transition-window burst owns the budget.
        bool          captureLiveFrames = false;
        std::uint32_t captureFrames = 4;
        // v1.4.6: spread the samples over the whole window (one every captureEveryMs) and cap the
        // session at captureTotalMax files -- 3840x2160 BMPs are 24.9 MB each at full resolution.
        std::uint32_t captureEveryMs = 150;
        std::uint32_t captureTotalMax = 48;
        // ---- Void overlay (v1.4.0, EXPERIMENTAL) ----------------------------------------------
        // Draws motion ON TOP of the frozen frame for the ~2 s dead window, then a soft pulse at
        // the cut. Additive only: it brightens pixels and physically cannot darken any of them,
        // which is what the "no darkening anywhere" decision requires. Rendered in the Present
        // hook (the one place that is guaranteed to run while the world is not drawing), never
        // touching engine pixels -- so ENB / Community Shaders grading is preserved underneath.
        bool          voidOverlay = false;
        std::uint32_t overlayMotes = 18;          // 0..32
        float         overlayBrightness = 0.55f;  // 0..2
        float         overlayBreath = 0.020f;     // 0..0.10 (additive lift, 0 disables)
        float         overlayPulse = 0.55f;       // 0..1.5, strength of the cut pulse
        std::uint32_t overlayPulseMs = 300;       // 50..800, must stay under the ~350 ms spin
        std::uint32_t overlayMaxMs = 12000;       // hard cap from arming -- failsafe only: measured windows are 4~5.9 s
        float         overlayMoteScale = 1.0f;    // 0.3..3 mote size multiplier
        // ---- v1.4.7 cross-fade at the cut ------------------------------------------------------
        // The ugliest part is the engine spinning the new cell into place AFTER the cell change
        // (~21 frames measured: radial smearing + a brightness spike). Instead of an additive pulse,
        // keep the last frame of the old cell and dissolve it into the finished one. A 1:1 blit means
        // the HUD is not distorted. The length is capped at 350 ms: the engine's scene assembly is about that long.
        bool          crossFade = true;
        // v1.4.13 diagnostic: lift the WHOLE screen additively during the dead window. If the picture
        // visibly brightens, our Present-layer drawing reaches the display; if it does not, nothing we
        // draw at Present time is being presented and the route has to change (UI/Scaleform instead).
        bool          overlayDebugFullscreen = false;
        // v1.5.5: how long to withhold Present at the cut so the engine's empty frames never show.
        std::uint32_t holdAtCutMs = 250;   // SAFETY CAP; the gate releases on its own (v1.5.6)
        std::uint32_t crossFadeMs = 350;
        // ---- Seamless door transition (private build) -----------------------------------------
        // Keep the last frame of the old cell on screen through the whole load (engine fade, dead
        // window and scene spin all happen underneath it), then dissolve it into the new cell once the
        // engine's picture is back and has stopped changing. Nothing is darkened and nothing is shown
        // that was not already on screen.
        bool          seamless = true;
        std::uint32_t seamlessFadeMs = 600;         // dissolve length, 100..2000
        std::uint32_t seamlessMinAfterCutMs = 0;    // 0 = fPostLoadUpdateTimeMS + 100
        std::uint32_t seamlessStableMs = 120;       // picture must be steady this long before the dissolve
        std::uint32_t seamlessMaxHoldMs = 3000;     // after the cut: dissolve anyway past this
        float         seamlessBlackLuma = 6.0f;     // below this (0..255) the engine's frame counts as black
        float         seamlessStableTol = 3.0f;     // luma change per sample that still counts as steady
        // ---- Travel effects (private build) ---------------------------------------------------
        // Non-door loads get a themed cover instead of the loading screen: eyes closing for boats,
        // carriages and other scripted travel, Apocrypha ink for Black Books / Soul Cairn portals,
        // a magic field for map fast travel. iTravelFxDefault: 0 = vanilla loading screen for
        // everything not recognised, 1 = eyes.
        bool          travelFx = true;
        int           travelFxDefault = 1;
        std::uint32_t travelFxInMs = 450;
        std::uint32_t travelFxOutMs = 900;
        std::uint32_t travelFxMaxHoldMs = 45000;
        std::uint32_t travelFxMagicInMs = 1000;   // fast travel: the cover builds over the live scene this long before the travel is allowed to start
        bool          travelFxSounds = true;
        int           travelFxFastTravel = 1;     // 1 = eyes close and open (default), 2 = the magic field
        bool          seamlessTrace = true;   // log per-frame luma before/after our draw for 1.2 s after the cut
        // Frame generation (FSR 3/4 via Community Shaders / Bottled Shaders) builds its generated frame
        // from the engine's HUD-less colour, which jumps to the new cell at the cut while our hold only
        // exists on the presented frame -> one generated frame shows the new area. While a hold or a
        // travel cover is up, frame generation is switched off through the FidelityFX API and reset
        // when it comes back.
        bool          pauseFrameGen = true;
        bool          pauseDlssFrameGen = false;   // [DLSSG] experimental
        bool          announceLoads = true;        // [Compat] tell other mods a load happened although no loading screen opened
        // While the held picture is static (door hold until the dissolve; travel cover from the cell swap until the
        // reveal) no frame is presented at all: the display keeps the last one, and the frame generator never sees the
        // cut. Fixes the one-frame flash of the new cell that survived the frame-generation pause.
        bool          holdPresents = true;
        // [Styles] which cover each kind of load gets (FxKind values; 0 = leave the vanilla screen / no cover)
        int           styleFastTravel = 1;
        int           styleCarriage = 4;    // travel that follows a conversation (carriages, boats)
        int           styleOther = 1;       // any other load during play
        int           styleBlackBook = 2;
        int           styleSoulCairn = 7;
        int           styleSleep = 6;       // waking up after sleeping (no load involved)
        int           styleLift = 8;        // Dwemer lifts (0 = like any other door)
        int           stylePortal = 5;      // wayshrines, paragon portals, Sovngarde, Skuldafn, Azura's Star
        int           styleCaptive = 6;     // jail, abductions, blackouts, being moved while asleep
        int           styleBoat = 4;        // the rowboat to Castle Volkihar
        int           styleDream = 9;       // the Mind of Madness, Vaermina's Dreamstride
        int           styleDoor = 0;        // ordinary load doors: 0 = hold and dissolve, otherwise an FxKind
        // [EaseIn]
        std::uint32_t seamlessEaseInMs = 4000;      // time for the push-in to reach full speed/amount
        float         seamlessPushIn = 0.008f;      // how far the held picture pushes forward over the ease-in
        float         seamlessDrift = 0.003f;       // further push per second
        bool          seamlessHideHud = true;       // hide the HUD from just before the door until the dissolve
        std::uint32_t seamlessDoorDelayFrames = 3;  // frames between the key press and the door, HUD hidden
    };
    Config g_config;

    std::unordered_set<RE::FormID> g_excludeCells;
    std::vector<std::string>       g_excludeSpecs;
    // Cells routed to the vanilla path on purpose (script-bearing interiors, scanned offline).
    // Same effect as [ExcludeCells]; kept in its own section so the intent stays readable.
    std::vector<std::string>       g_forceRealSpecs;
    std::vector<std::string>       g_rawMenuSectionLines;  // P0-3: preserved [Menu] lines (comments + overrides) in LoadingFlow.ini

    // ---------------- Localization (L() table) ----------------
    std::unordered_map<std::string, std::string> g_lang;

    std::size_t CountFormatSpecs(std::string_view a_fmt)
    {
        std::size_t n = 0;
        for (std::size_t i = 0; i < a_fmt.size(); ++i) {
            const char c = a_fmt[i];
            if (c == '%') {
                if (i + 1 < a_fmt.size() && a_fmt[i + 1] == '%') {
                    ++i;
                    continue;
                }
                ++n;
            } else if (c == '{') {
                if (i + 1 < a_fmt.size() && a_fmt[i + 1] == '{') {
                    ++i;
                    continue;
                }
                ++n;
            }
        }
        return n;
    }

    void LoadLangFile(const char* a_path, bool a_override)
    {
        std::ifstream f(a_path);
        if (!f.is_open()) {
            if (a_override) {
                logger::info("Language pack not found (using built-in English): {}", a_path);
            }
            return;
        }
        std::string line;
        std::string section;
        auto trim = [](std::string& x) {
            const auto b = x.find_first_not_of(" \t\r\n");
            const auto e = x.find_last_not_of(" \t\r\n");
            x = (b == std::string::npos) ? std::string{} : x.substr(b, e - b + 1);
        };
        std::size_t loaded = 0;
        while (std::getline(f, line)) {
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            trim(line);
            if (line.empty() || line.front() == ';' || line.front() == '#') {
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            if (section != "Menu") {
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            trim(k);
            trim(v);
            if (k.empty()) {
                continue;
            }
            if (a_override || g_lang.find(k) == g_lang.end()) {
                g_lang[k] = v;
                ++loaded;
            }
        }
        if (loaded) {
            logger::info("Language pack: {} -> {} menu strings", a_path, loaded);
        } else {
            logger::warn("Language pack: {} opened but yielded 0 strings", a_path);
        }
    }

    void LoadLang()
    {
        const auto backup = g_lang;
        g_lang.clear();
        LoadLangFile("Data/SKSE/Plugins/SeamlessTransitions.ini", false);
        LoadLangFile("Data/SKSE/Plugins/SeamlessTransitions_Lang.ini", true);
        if (g_lang.empty() && !backup.empty()) {
            g_lang = backup;
        }
    }

    const char* L(const char* a_key, const char* a_english)
    {
        const auto it = g_lang.find(a_key);
        if (it == g_lang.end() || it->second.empty()) {
            return a_english;
        }
        if (CountFormatSpecs(it->second) != CountFormatSpecs(a_english)) {
            logger::warn("Menu string '{}' has mismatched format specifiers; falling back to English", a_key);
            return a_english;
        }
        return it->second.c_str();
    }

    void SaveConfig()
    {
        static constexpr const char* kIniPath = "Data/SKSE/Plugins/SeamlessTransitions.ini";
        static constexpr const char* kTmpPath = "Data/SKSE/Plugins/SeamlessTransitions.ini.tmp";
        // P0-3: canonical must-write section list so new sections are never silently dropped
        static constexpr const char* kMustWriteSections[] = { "General", "ExcludeCells", "Menu" };
        (void)kMustWriteSections;

        {
            std::ofstream f(kTmpPath, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) {
                logger::error("Failed to open {} for atomic writing", kTmpPath);
                return;
            }
            f << "; Seamless Door and Travel Transitions configuration (all sections written atomically via .tmp + rename)\r\n";
            f << "[General]\r\n";
            f << "bEnabled = " << (g_config.enabled ? 1 : 0) << "\r\n";
            f << "bAllowInteriorToInterior = " << (g_config.allowInteriorToInterior ? 1 : 0) << "\r\n";
            f << "bAllowPocketToExterior = " << (g_config.allowPocketToExterior ? 1 : 0) << "\r\n";
            f << "bMoveActorsToPocket = " << (g_config.moveActorsToPocket ? 1 : 0) << "\r\n";
            f << "bSpawnBlackBoxShell = " << (g_config.spawnBlackBoxShell ? 1 : 0) << "\r\n";
            f << "bPlayDoorSoundAndAnim = " << (g_config.playDoorSoundAndAnim ? 1 : 0) << "\r\n";
            f << "bTriggerBlinkImod = " << (g_config.triggerBlinkImod ? 1 : 0) << "\r\n";
            f << "bSneakForcesVanillaLoad = " << (g_config.sneakForcesVanillaLoad ? 1 : 0) << "\r\n";
            f << "bShowDebugNotifications = " << (g_config.showDebugNotifications ? 1 : 0) << "\r\n";
            f << "bFakeLocationEvents = " << (g_config.fakeLocationEvents ? 1 : 0) << "\r\n";
            f << "bFakeCellLoadEvents = " << (g_config.fakeCellLoadEvents ? 1 : 0) << "\r\n";
            f << "bUsePocketCopies = " << (g_config.usePocketCopies ? 1 : 0) << "\r\n";
            f << "bSuppressLoadingMenu = " << (g_config.suppressLoadingMenu ? 1 : 0) << "\r\n";
            f << "uSuppressTimeoutMs = " << g_config.suppressTimeoutMs << "\r\n";
            f << "uTransitionWindowMs = " << g_config.transitionWindowMs << "\r\n";
            f << "bPrewarmNearbyDoors = " << (g_config.prewarmNearbyDoors ? 1 : 0) << "\r\n";
            f << "uPrewarmModelCap = " << g_config.prewarmModelCap << "\r\n";
            f << std::format("fPrewarmRadius = {:.0f}\r\n", g_config.prewarmRadius);
            f << "bVisualEnvelope = " << (g_config.visualEnvelope ? 1 : 0) << "\r\n";
            f << "uEnvelopeBlackHoldMs = " << g_config.envelopeBlackHoldMs << "\r\n";
            f << std::format("fEnvelopeFadeStrength = {:.2f}\r\n", g_config.envelopeFadeStrength);
            f << "bNoBlackTransition = " << (g_config.noBlackTransition ? 1 : 0) << "\r\n";
            f << "bTweakEngineTimers = " << (g_config.tweakEngineTimers ? 1 : 0) << "\r\n";
            f << std::format("fFadeToBlackFadeSeconds = {:.3f}\r\n", g_config.fadeToBlackFadeSeconds);
            f << std::format("fNormalDoorFadeSecs = {:.3f}\r\n", g_config.normalDoorFadeSecs);
            f << std::format("fNormalDoorFadeWait = {:.3f}\r\n", g_config.normalDoorFadeWait);
            f << std::format("fAutoDoorFadeSecs = {:.3f}\r\n", g_config.autoDoorFadeSecs);
            f << std::format("fMinSecondsForLoadFadeIn = {:.4f}\r\n", g_config.minSecondsForLoadFadeIn);
            f << std::format("fPostLoadUpdateTimeMS = {:.1f}\r\n", g_config.postLoadUpdateTimeMS);
            f << std::format("fActorTeleportFadeSeconds = {:.2f}\r\n", g_config.actorTeleportFadeSeconds);
            f << "iPreloadSizeLimit = " << g_config.preloadSizeLimit << "\r\n";
            f << std::format("fFastTravelFadeSecs = {:.2f}\r\n", g_config.fastTravelFadeSecs);
            f << "iLoadingQueuedPriorityBudgetMs = " << g_config.loadingQueuedPriorityBudgetMs << "\r\n";
            f << "bNoBlackOnlyForManualDoors = " << (g_config.noBlackOnlyForManualDoors ? 1 : 0) << "\r\n";
            f << std::format("sCutMaskImod = 0x{:08X}\r\n", g_config.cutMaskImod);
            f << "uCutMaskMs = " << g_config.cutMaskMs << "\r\n";
            f << "bLogDoorActivations = " << (g_config.logDoorActivations ? 1 : 0) << "\r\n";
            f << "uSuppressMaxMs = " << g_config.suppressMaxMs << "\r\n";
            f << "uFreezeMaxMs = " << g_config.freezeMaxMs << "\r\n";
            f << std::format("fCutMaskFadeOut = {:.2f}\r\n", g_config.cutMaskFadeOut);
            f << "uFreezeDelayMs = " << g_config.freezeDelayMs << "\r\n";
            f << "bProbeAssembly = " << (g_config.probeAssembly ? 1 : 0) << "\r\n";
            f << "uProbeAssemblyMs = " << g_config.probeAssemblyMs << "\r\n";
            f << "bProbeGfx = " << (g_config.probeGfx ? 1 : 0) << "\r\n";
            f << "bFreezeFramePresent = " << (g_config.freezeFramePresent ? 1 : 0) << "\r\n";
            f << "uFreezeFrameHoldMs = " << g_config.freezeFrameHoldMs << "\r\n";
            f << "uCoverAtSwapMs = " << g_config.coverAtSwapMs << "\r\n";
            f << "iCoverAtSwapMode = " << g_config.coverAtSwapMode << "\r\n";
            f << "uVoidRampMs = " << g_config.voidRampMs << "\r\n";
            f << std::format("fVoidDarken = {:.2f}\r\n", g_config.voidDarken);
            f << "uVoidHoldMs = " << g_config.voidHoldMs << "\r\n";
            f << "bProbeWorld = " << (g_config.probeWorld ? 1 : 0) << "\r\n";
            f << "bCapProbe = " << (g_config.capProbe ? 1 : 0) << "\r\n";
            f << "bCaptureFrame = " << (g_config.captureFrame ? 1 : 0) << "\r\n";
            f << "bCaptureLiveFrames = " << (g_config.captureLiveFrames ? 1 : 0) << "\r\n";
            f << "uCaptureEveryMs = " << g_config.captureEveryMs << "\r\n";
            f << "uCaptureTotalMax = " << g_config.captureTotalMax << "\r\n";
            f << "uCaptureFrames = " << g_config.captureFrames << "\r\n";
            f << "bVoidOverlay = " << (g_config.voidOverlay ? 1 : 0) << "\r\n";
            f << "uOverlayMotes = " << g_config.overlayMotes << "\r\n";
            f << std::format("fOverlayBrightness = {:.2f}\r\n", g_config.overlayBrightness);
            f << std::format("fOverlayBreath = {:.3f}\r\n", g_config.overlayBreath);
            f << std::format("fOverlayPulse = {:.2f}\r\n", g_config.overlayPulse);
            f << "uOverlayPulseMs = " << g_config.overlayPulseMs << "\r\n";
            f << "uOverlayMaxMs = " << g_config.overlayMaxMs << "\r\n";
            f << std::format("fOverlayMoteScale = {:.2f}\r\n", g_config.overlayMoteScale);
            f << "bCrossFade = " << (g_config.crossFade ? 1 : 0) << "\r\n";
            f << "uCrossFadeMs = " << g_config.crossFadeMs << "\r\n";
            f << "bSeamless = " << (g_config.seamless ? 1 : 0) << "\r\n";
            f << "uSeamlessFadeMs = " << g_config.seamlessFadeMs << "\r\n";
            f << "uSeamlessMinAfterCutMs = " << g_config.seamlessMinAfterCutMs << "\r\n";
            f << "uSeamlessStableMs = " << g_config.seamlessStableMs << "\r\n";
            f << "uSeamlessMaxHoldMs = " << g_config.seamlessMaxHoldMs << "\r\n";
            f << std::format("fSeamlessBlackLuma = {:.1f}\r\n", g_config.seamlessBlackLuma);
            f << std::format("fSeamlessStableTol = {:.1f}\r\n", g_config.seamlessStableTol);
            f << "bTravelFx = " << (g_config.travelFx ? 1 : 0) << "\r\n";
            f << "iTravelFxDefault = " << g_config.travelFxDefault << "\r\n";
            f << "uTravelFxInMs = " << g_config.travelFxInMs << "\r\n";
            f << "uTravelFxOutMs = " << g_config.travelFxOutMs << "\r\n";
            f << "uTravelFxMaxHoldMs = " << g_config.travelFxMaxHoldMs << "\r\n";
            f << "uTravelFxMagicInMs = " << g_config.travelFxMagicInMs << "\r\n";
            f << "bTravelFxSounds = " << (g_config.travelFxSounds ? 1 : 0) << "\r\n";
            f << "iTravelFxFastTravel = " << g_config.travelFxFastTravel << "\r\n";
            f << "bSeamlessTrace = " << (g_config.seamlessTrace ? 1 : 0) << "\r\n";
            f << "bPauseFrameGen = " << (g_config.pauseFrameGen ? 1 : 0) << "\r\n";
            f << "bPauseDlssFrameGen = " << (g_config.pauseDlssFrameGen ? 1 : 0) << "\r\n";
            f << "bAnnounceLoads = " << (g_config.announceLoads ? 1 : 0) << "\r\n";
            f << "bHoldPresents = " << (g_config.holdPresents ? 1 : 0) << "\r\n";
            f << "iStyleFastTravel = " << g_config.styleFastTravel << "\r\n";
            f << "iStyleCarriage = " << g_config.styleCarriage << "\r\n";
            f << "iStyleOther = " << g_config.styleOther << "\r\n";
            f << "iStyleBlackBook = " << g_config.styleBlackBook << "\r\n";
            f << "iStyleSoulCairn = " << g_config.styleSoulCairn << "\r\n";
            f << "iStyleSleep = " << g_config.styleSleep << "\r\n";
            f << "iStyleLift = " << g_config.styleLift << "\r\n";
            f << "iStylePortal = " << g_config.stylePortal << "\r\n";
            f << "iStyleCaptive = " << g_config.styleCaptive << "\r\n";
            f << "iStyleBoat = " << g_config.styleBoat << "\r\n";
            f << "iStyleDream = " << g_config.styleDream << "\r\n";
            f << "iStyleDoor = " << g_config.styleDoor << "\r\n";
            f << "uSeamlessEaseInMs = " << g_config.seamlessEaseInMs << "\r\n";
            f << std::format("fSeamlessPushIn = {:.4f}\r\n", g_config.seamlessPushIn);
            f << std::format("fSeamlessDrift = {:.4f}\r\n", g_config.seamlessDrift);
            f << "bSeamlessHideHud = " << (g_config.seamlessHideHud ? 1 : 0) << "\r\n";
            f << "uSeamlessDoorDelayFrames = " << g_config.seamlessDoorDelayFrames << "\r\n";
            f << "bOverlayDebugFullscreen = " << (g_config.overlayDebugFullscreen ? 1 : 0) << "\r\n";
            f << "bNoBlackForWalkThrough = " << (g_config.noBlackForWalkThrough ? 1 : 0) << "\r\n";
            f << "bGlobalMenuSuppression = " << (g_config.globalMenuSuppression ? 1 : 0) << "\r\n";
            f << std::format("fBaseAltitudeZ = {:.1f}\r\n", g_config.baseAltitudeZ);
            f << std::format("fSlotStepZ = {:.1f}\r\n", g_config.slotStepZ);
            f << std::format("fBlinkStrength = {:.2f}\r\n", g_config.blinkStrength);
            f << "uDoorOpenDelayMs = " << g_config.doorOpenDelayMs << "\r\n";
            f << "uMaxRefsPerCell = " << g_config.maxRefsPerCell << "\r\n";
            f << "uMaxPocketSlots = " << g_config.maxPocketSlots << "\r\n";
            f << "\r\n[ExcludeCells]\r\n";
            f << "; Add interior Cell FormIDs (0x...) or EditorIDs here to force vanilla loading screen\r\n";
            for (const auto& s : g_excludeSpecs) {
                f << s << " = 1\r\n";
            }
            f << "\r\n[ForceRealTransition]\r\n";
            f << "; Cells that must use the engine's own (real) cell transition instead of a pocket\n";
            f << "; copy: quest/location scripts inside them listen to cell events we cannot fake.\n";
            for (const auto& s : g_forceRealSpecs) {
                f << s << " = 1\r\n";
            }
            if (!g_rawMenuSectionLines.empty()) {
                f << "\r\n[Menu]\r\n";
                for (const auto& rawLine : g_rawMenuSectionLines) {
                    f << rawLine << "\r\n";
                }
            }
            f.flush();
            if (!f.good()) {
                logger::error("I/O error while flushing {}; aborting atomic replace", kTmpPath);
                return;
            }
        }

        std::error_code ec;
        std::filesystem::rename(kTmpPath, kIniPath, ec);
        if (ec) {
            std::filesystem::copy_file(kTmpPath, kIniPath, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(kTmpPath, ec);
        }
        logger::info("Config saved atomically to {}", kIniPath);
    }

    void LoadConfig()
    {
        // P2-6: Always reset to defaults before reading so removed lines revert cleanly on Reload INI
        g_config = Config{};
        g_excludeSpecs.clear();
        g_forceRealSpecs.clear();
        g_rawMenuSectionLines.clear();

        std::ifstream f("Data/SKSE/Plugins/SeamlessTransitions.ini");
        if (!f.is_open()) {
            logger::info("SeamlessTransitions.ini not found; writing default configuration file");
            SaveConfig();  // P2-7: create default INI on first run
            LoadLang();
            return;
        }
        std::string line;
        std::string section;
        // Duplicate-key guard: a key appearing twice is a packaging/editing accident. Whichever copy
        // sits outside [General] is silently ignored, which once made a user's edit do nothing.
        std::unordered_map<std::string, int> keySeen;
        int lineNo = 0;
        auto trim = [](std::string& s) {
            const auto b = s.find_first_not_of(" \t\r\n");
            const auto e = s.find_last_not_of(" \t\r\n");
            s = (b == std::string::npos) ? std::string{} : s.substr(b, e - b + 1);
        };
        while (std::getline(f, line)) {
            ++lineNo;
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.pop_back();
            }
            std::string trimmedHdr = line;
            trim(trimmedHdr);
            if (!trimmedHdr.empty() && trimmedHdr.front() == '[' && trimmedHdr.back() == ']') {
                section = trimmedHdr.substr(1, trimmedHdr.size() - 2);
                trim(section);
                continue;
            }
            if (section == "Menu") {
                // P0-3: preserve every raw line (comments + overrides) under [Menu] in LoadingFlow.ini
                if (!trimmedHdr.empty()) {
                    g_rawMenuSectionLines.push_back(line);
                }
                continue;
            }
            const auto sc = line.find(';');
            if (sc != std::string::npos) {
                line.erase(sc);
            }
            trim(line);
            if (line.empty() || line.front() == '#') {
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            trim(key);
            trim(val);
            if (key.empty()) {
                continue;
            }
            // Any key outside [General] is ignored by this parser, so a duplicated key silently does
            // nothing. Warn loudly: that exact accident made a user's preload settings inert.
            if (++keySeen[key] > 1) {
                logger::warn("[Config] duplicate key '{}' at line {} (section [{}]) -- only the copy inside [General] takes effect",
                    key, lineNo, section);
            } else if (section != "General" && section != "ExcludeCells" && section != "ForceRealTransition") {
                logger::warn("[Config] key '{}' at line {} is in section [{}], which is not read; move it to [General]",
                    key, lineNo, section);
            }
            try {
                if (section == "General") {
                    if (key == "bEnabled") {
                        g_config.enabled = (std::stoi(val) != 0);
                    } else if (key == "bAllowInteriorToInterior") {
                        g_config.allowInteriorToInterior = (std::stoi(val) != 0);
                    } else if (key == "bAllowPocketToExterior") {
                        g_config.allowPocketToExterior = (std::stoi(val) != 0);
                    } else if (key == "bMoveActorsToPocket") {
                        g_config.moveActorsToPocket = (std::stoi(val) != 0);
                    } else if (key == "bSpawnBlackBoxShell") {
                        g_config.spawnBlackBoxShell = (std::stoi(val) != 0);
                    } else if (key == "bPlayDoorSoundAndAnim") {
                        g_config.playDoorSoundAndAnim = (std::stoi(val) != 0);
                    } else if (key == "bTriggerBlinkImod") {
                        g_config.triggerBlinkImod = (std::stoi(val) != 0);
                    } else if (key == "bSneakForcesVanillaLoad") {
                        g_config.sneakForcesVanillaLoad = (std::stoi(val) != 0);
                    } else if (key == "bShowDebugNotifications") {
                        g_config.showDebugNotifications = (std::stoi(val) != 0);
                    } else if (key == "bFakeLocationEvents") {
                        g_config.fakeLocationEvents = (std::stoi(val) != 0);
                    } else if (key == "bFakeCellLoadEvents") {
                        g_config.fakeCellLoadEvents = (std::stoi(val) != 0);
                    } else if (key == "bUsePocketCopies") {
                        g_config.usePocketCopies = (std::stoi(val) != 0);
                    } else if (key == "bSuppressLoadingMenu") {
                        g_config.suppressLoadingMenu = (std::stoi(val) != 0);
                    } else if (key == "uSuppressTimeoutMs") {
                        g_config.suppressTimeoutMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 5000));
                    } else if (key == "uTransitionWindowMs") {
                        g_config.transitionWindowMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 500, 30000));
                    } else if (key == "bPrewarmNearbyDoors") {
                        g_config.prewarmNearbyDoors = (std::stoi(val) != 0);
                    } else if (key == "uPrewarmModelCap") {
                        g_config.prewarmModelCap = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 10, 2000));
                    } else if (key == "fPrewarmRadius") {
                        g_config.prewarmRadius = std::clamp(std::stof(val), 200.0f, 4000.0f);
                    } else if (key == "bVisualEnvelope") {
                        g_config.visualEnvelope = (std::stoi(val) != 0);
                    } else if (key == "uEnvelopeBlackHoldMs") {
                        g_config.envelopeBlackHoldMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 1000));
                    } else if (key == "fEnvelopeFadeStrength") {
                        g_config.envelopeFadeStrength = std::clamp(std::stof(val), 0.1f, 1.0f);
                    } else if (key == "bNoBlackTransition") {
                        g_config.noBlackTransition = (std::stoi(val) != 0);
                    } else if (key == "bNoBlackOnlyForManualDoors") {
                        g_config.noBlackOnlyForManualDoors = (std::stoi(val) != 0);
                    } else if (key == "sCutMaskImod") {
                        g_config.cutMaskImod = (val.starts_with("0x") || val.starts_with("0X"))
                            ? static_cast<std::uint32_t>(std::stoul(val, nullptr, 16))
                            : static_cast<std::uint32_t>(std::stoul(val));
                    } else if (key == "uCutMaskMs") {
                        g_config.cutMaskMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 1000));
                    } else if (key == "bLogDoorActivations") {
                        g_config.logDoorActivations = (std::stoi(val) != 0);
                    } else if (key == "uSuppressMaxMs") {
                        g_config.suppressMaxMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 30000));
                    } else if (key == "uFreezeMaxMs") {
                        g_config.freezeMaxMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 30000));
                    } else if (key == "bTweakEngineTimers") {
                        g_config.tweakEngineTimers = (std::stoi(val) != 0);
                    } else if (key == "fFadeToBlackFadeSeconds") {
                        g_config.fadeToBlackFadeSeconds = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "fNormalDoorFadeSecs") {
                        g_config.normalDoorFadeSecs = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "fNormalDoorFadeWait") {
                        g_config.normalDoorFadeWait = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "fAutoDoorFadeSecs") {
                        g_config.autoDoorFadeSecs = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "fMinSecondsForLoadFadeIn") {
                        g_config.minSecondsForLoadFadeIn = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "fPostLoadUpdateTimeMS") {
                        g_config.postLoadUpdateTimeMS = std::clamp(std::stof(val), -1.0f, 5000.0f);
                    } else if (key == "fActorTeleportFadeSeconds") {
                        g_config.actorTeleportFadeSeconds = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "iPreloadSizeLimit") {
                        // Upper bound is the int32 limit because the engine stores this in a Setting's
                        // int32 slot. Clamping used to happen silently and quietly turned a 1 GiB
                        // request into 95 MiB, so now any truncation is logged.
                        const long long raw = std::stoll(val);
                        constexpr long long kMax = 2147483647LL;
                        const long long clamped = std::clamp(raw, -1LL, kMax);
                        if (clamped != raw) {
                            logger::warn("[Config] iPreloadSizeLimit {} clamped to {} (int32 limit)", raw, clamped);
                        }
                        g_config.preloadSizeLimit = static_cast<int>(clamped);
                    } else if (key == "fFastTravelFadeSecs") {
                        g_config.fastTravelFadeSecs = std::clamp(std::stof(val), -1.0f, 5.0f);
                    } else if (key == "iLoadingQueuedPriorityBudgetMs") {
                        g_config.loadingQueuedPriorityBudgetMs = std::clamp(std::stoi(val), -1, 5000);
                    } else if (key == "fCutMaskFadeOut") {
                        g_config.cutMaskFadeOut = std::clamp(std::stof(val), 0.0f, 2.0f);
                    } else if (key == "uFreezeDelayMs") {
                        g_config.freezeDelayMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 600));
                    } else if (key == "bProbeAssembly") {
                        g_config.probeAssembly = (std::stoi(val) != 0);
                    } else if (key == "uProbeAssemblyMs") {
                        g_config.probeAssemblyMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 10000));
                    } else if (key == "bProbeGfx") {
                        g_config.probeGfx = (std::stoi(val) != 0);
                    } else if (key == "bFreezeFramePresent") {
                        g_config.freezeFramePresent = (std::stoi(val) != 0);
                    } else if (key == "uFreezeFrameHoldMs") {
                        g_config.freezeFrameHoldMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 5000));
                    } else if (key == "uCoverAtSwapMs") {
                        g_config.coverAtSwapMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 350));
                    } else if (key == "iCoverAtSwapMode") {
                        g_config.coverAtSwapMode = std::clamp(std::stoi(val), 1, 2);
                    } else if (key == "uVoidRampMs") {
                        g_config.voidRampMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 3000));
                    } else if (key == "fVoidDarken") {
                        g_config.voidDarken = std::clamp(std::stof(val), 0.0f, 1.0f);
                    } else if (key == "uVoidHoldMs") {
                        g_config.voidHoldMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 1500));
                    } else if (key == "bProbeWorld") {
                        g_config.probeWorld = (std::stoi(val) != 0);
                    } else if (key == "bCapProbe") {
                        g_config.capProbe = (std::stoi(val) != 0);
                    } else if (key == "bCaptureFrame") {
                        g_config.captureFrame = (std::stoi(val) != 0);
                    } else if (key == "bCaptureLiveFrames") {
                        g_config.captureLiveFrames = (std::stoi(val) != 0);
                    } else if (key == "uCaptureEveryMs") {
                        g_config.captureEveryMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 16, 2000));
                    } else if (key == "uCaptureTotalMax") {
                        g_config.captureTotalMax = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 1, 200));
                    } else if (key == "uCaptureFrames") {
                        g_config.captureFrames = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 16));
                    } else if (key == "bVoidOverlay") {
                        g_config.voidOverlay = (std::stoi(val) != 0);
                    } else if (key == "uOverlayMotes") {
                        g_config.overlayMotes = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 32));
                    } else if (key == "fOverlayBrightness") {
                        g_config.overlayBrightness = std::clamp(std::stof(val), 0.0f, 2.0f);
                    } else if (key == "fOverlayBreath") {
                        g_config.overlayBreath = std::clamp(std::stof(val), 0.0f, 0.10f);
                    } else if (key == "fOverlayPulse") {
                        g_config.overlayPulse = std::clamp(std::stof(val), 0.0f, 1.5f);
                    } else if (key == "uOverlayPulseMs") {
                        g_config.overlayPulseMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 50, 800));
                    } else if (key == "uOverlayMaxMs") {
                        g_config.overlayMaxMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 500, 15000));
                    } else if (key == "fOverlayMoteScale") {
                        g_config.overlayMoteScale = std::clamp(std::stof(val), 0.3f, 3.0f);
                    } else if (key == "bCrossFade") {
                        g_config.crossFade = (std::stoi(val) != 0);
                    } else if (key == "uCrossFadeMs") {
                        g_config.crossFadeMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 60, 350));
                    } else if (key == "bSeamless") {
                        g_config.seamless = (std::stoi(val) != 0);
                    } else if (key == "uSeamlessFadeMs") {
                        g_config.seamlessFadeMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 100, 2000));
                    } else if (key == "uSeamlessMinAfterCutMs") {
                        g_config.seamlessMinAfterCutMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 3000));
                    } else if (key == "uSeamlessStableMs") {
                        g_config.seamlessStableMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 1000));
                    } else if (key == "uSeamlessMaxHoldMs") {
                        g_config.seamlessMaxHoldMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 300, 8000));
                    } else if (key == "fSeamlessBlackLuma") {
                        g_config.seamlessBlackLuma = std::clamp(std::stof(val), 0.0f, 64.0f);
                    } else if (key == "fSeamlessStableTol") {
                        g_config.seamlessStableTol = std::clamp(std::stof(val), 0.0f, 32.0f);
                    } else if (key == "bTravelFx") {
                        g_config.travelFx = (std::stoi(val) != 0);
                    } else if (key == "iTravelFxDefault") {
                        g_config.travelFxDefault = std::clamp(std::stoi(val), 0, 1);
                    } else if (key == "uTravelFxInMs") {
                        g_config.travelFxInMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 100, 3000));
                    } else if (key == "uTravelFxOutMs") {
                        g_config.travelFxOutMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 100, 3000));
                    } else if (key == "iStyleFastTravel") {
                        g_config.styleFastTravel = std::clamp(std::stoi(val), 0, 7);
                    } else if (key == "iStyleCarriage") {
                        g_config.styleCarriage = std::clamp(std::stoi(val), 0, 7);
                    } else if (key == "iStyleOther") {
                        g_config.styleOther = std::clamp(std::stoi(val), 0, 7);
                    } else if (key == "iStyleBlackBook") {
                        g_config.styleBlackBook = std::clamp(std::stoi(val), 0, 7);
                    } else if (key == "iStyleSoulCairn") {
                        g_config.styleSoulCairn = std::clamp(std::stoi(val), 0, 7);
                    } else if (key == "iStyleLift") {
                        g_config.styleLift = std::clamp(std::stoi(val), 0, 8);
                    } else if (key == "iStylePortal") {
                        g_config.stylePortal = std::clamp(std::stoi(val), 0, 8);
                    } else if (key == "iStyleCaptive") {
                        g_config.styleCaptive = std::clamp(std::stoi(val), 0, 8);
                    } else if (key == "iStyleBoat") {
                        g_config.styleBoat = std::clamp(std::stoi(val), 0, 9);
                    } else if (key == "iStyleDream") {
                        g_config.styleDream = std::clamp(std::stoi(val), 0, 10);
                    } else if (key == "iStyleDoor") {
                        g_config.styleDoor = std::clamp(std::stoi(val), 0, 10);
                    } else if (key == "iStyleSleep") {
                        g_config.styleSleep = std::clamp(std::stoi(val), 0, 7);
                    } else if (key == "uSeamlessEaseInMs") {
                        g_config.seamlessEaseInMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 20000));
                    } else if (key == "fSeamlessPushIn") {
                        g_config.seamlessPushIn = std::clamp(std::stof(val), 0.0f, 0.2f);
                    } else if (key == "fSeamlessDrift") {
                        g_config.seamlessDrift = std::clamp(std::stof(val), 0.0f, 0.05f);
                    } else if (key == "bSeamlessHideHud") {
                        g_config.seamlessHideHud = (std::stoi(val) != 0);
                    } else if (key == "uSeamlessDoorDelayFrames") {
                        g_config.seamlessDoorDelayFrames = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 2, 30));
                    } else if (key == "bHoldPresents") {
                        g_config.holdPresents = (std::stoi(val) != 0);
                    } else if (key == "bAnnounceLoads") {
                        g_config.announceLoads = (std::stoi(val) != 0);
                    } else if (key == "bPauseDlssFrameGen") {
                        g_config.pauseDlssFrameGen = (std::stoi(val) != 0);
                    } else if (key == "bPauseFrameGen") {
                        g_config.pauseFrameGen = (std::stoi(val) != 0);
                    } else if (key == "bSeamlessTrace") {
                        g_config.seamlessTrace = (std::stoi(val) != 0);
                    } else if (key == "uTravelFxMagicInMs") {
                        g_config.travelFxMagicInMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 200, 4000));
                    } else if (key == "iTravelFxFastTravel") {
                        g_config.travelFxFastTravel = std::clamp(std::stoi(val), 1, 2);
                    } else if (key == "bTravelFxSounds") {
                        g_config.travelFxSounds = (std::stoi(val) != 0);
                    } else if (key == "uTravelFxMaxHoldMs") {
                        g_config.travelFxMaxHoldMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 5000, 120000));
                    } else if (key == "bOverlayDebugFullscreen") {
                        g_config.overlayDebugFullscreen = (std::stoi(val) != 0);
                    } else if (key == "bNoBlackForWalkThrough") {
                        g_config.noBlackForWalkThrough = (std::stoi(val) != 0);
                    } else if (key == "bGlobalMenuSuppression") {
                        g_config.globalMenuSuppression = (std::stoi(val) != 0);
                    } else if (key == "fBaseAltitudeZ") {
                        g_config.baseAltitudeZ = std::clamp(std::stof(val), 4000.0f, 60000.0f);
                    } else if (key == "fSlotStepZ") {
                        g_config.slotStepZ = std::clamp(std::stof(val), 1500.0f, 10000.0f);
                    } else if (key == "fBlinkStrength") {
                        g_config.blinkStrength = std::clamp(std::stof(val), 0.1f, 1.0f);
                    } else if (key == "uDoorOpenDelayMs") {
                        g_config.doorOpenDelayMs = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 0, 1000));
                    } else if (key == "uMaxRefsPerCell") {
                        g_config.maxRefsPerCell = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 50, 5000));
                    } else if (key == "uMaxPocketSlots") {
                        g_config.maxPocketSlots = static_cast<std::uint32_t>(std::clamp(std::stoi(val), 1, 32));
                    }
                } else if (section == "ExcludeCells") {
                    g_excludeSpecs.push_back(key);
                } else if (section == "ForceRealTransition") {
                    g_forceRealSpecs.push_back(key);
                }
            } catch (...) {
                logger::warn("Failed to parse INI entry: {}={}", key, val);
            }
        }
        LoadLang();
        logger::info("Config loaded: enabled={} baseZ={:.0f} stepZ={:.0f} maxRefs={} maxSlots={} moveActors={}",
            g_config.enabled, g_config.baseAltitudeZ, g_config.slotStepZ,
            g_config.maxRefsPerCell, g_config.maxPocketSlots, g_config.moveActorsToPocket);
    }

    void ResolveExcludedCells()
    {
        g_excludeCells.clear();
        for (const auto* list : { &g_excludeSpecs, &g_forceRealSpecs }) {
            for (const auto& spec : *list) {
            try {
                if (spec.starts_with("0x") || spec.starts_with("0X")) {
                    const auto fid = static_cast<RE::FormID>(std::stoul(spec, nullptr, 16));
                    if (fid != 0) {
                        g_excludeCells.insert(fid);
                    }
                } else if (auto* form = RE::TESForm::LookupByEditorID(spec)) {
                    g_excludeCells.insert(form->GetFormID());
                }
                } catch (...) {
                }
            }
        }
        logger::info("[LF] routing: {} cell spec(s) resolved to REAL transitions "
                     "({} excluded + {} scripted)",
            g_excludeCells.size(), g_excludeSpecs.size(), g_forceRealSpecs.size());
    }

    // =========================================================================
    //  Auto-Indexed Blueprint Database (Data/SKSE/Plugins/SeamlessTransitions_Blueprints.bin)
    //  P1-4: Uses an immutable snapshot published under g_bpMutex so main-thread
    //  and render-thread readers never race with Force Rebuild Index, and Force
    //  Rebuild runs on the SKSE task thread instead of stalling the UI render thread.
    // =========================================================================
    using BlueprintDoorDisk = PocketBlueprint::DoorDisk;
    using BlueprintRefDisk = PocketBlueprint::RefDisk;
    using BlueprintActorDisk = PocketBlueprint::ActorDisk;
    using BlueprintCell = PocketBlueprint::CellBlueprint;

    struct BlueprintSnapshot
    {
        std::unordered_map<RE::FormID, BlueprintDoorDisk> doors;
        std::unordered_map<RE::FormID, BlueprintCell>     cells;
    };

    std::mutex                             g_bpMutex;
    std::shared_ptr<const BlueprintSnapshot> g_bpSnapshot = std::make_shared<BlueprintSnapshot>();
    std::atomic<std::size_t>               g_bpCellCount{ 0 };
    std::atomic<std::size_t>               g_bpDoorCount{ 0 };
    std::atomic<std::uint64_t>             g_currentFingerprint{ 0 };
    std::atomic<bool>                      g_cacheHitOnStartup{ false };
    std::atomic<bool>                      g_rebuildInProgress{ false };

    std::shared_ptr<const BlueprintSnapshot> GetBlueprintSnapshot()
    {
        std::lock_guard<std::mutex> lk(g_bpMutex);
        return g_bpSnapshot;
    }

    void EnsureBlueprintsLoaded(bool a_forceRebuild = false)
    {
        static constexpr const char* kCachePath = "Data/SKSE/Plugins/SeamlessTransitions_Blueprints.bin";
        std::vector<const RE::TESFile*> loadedFiles;
        const std::uint64_t fp = PocketBlueprint::ComputeLoadOrderFingerprint(loadedFiles, g_config.maxRefsPerCell);
        g_currentFingerprint.store(fp);

        auto nextSnap = std::make_shared<BlueprintSnapshot>();
        if (!a_forceRebuild && PocketBlueprint::TryLoadCachedBlueprints(kCachePath, fp, nextSnap->doors, nextSnap->cells)) {
            g_cacheHitOnStartup.store(true);
            g_bpCellCount.store(nextSnap->cells.size());
            g_bpDoorCount.store(nextSnap->doors.size());
            {
                std::lock_guard<std::mutex> lk(g_bpMutex);
                g_bpSnapshot = nextSnap;
            }
            logger::info("[LF] Blueprint cache HIT (fingerprint=0x{:016X}, {} mods): "
                         "loaded {} interior cells, {} doors without re-indexing",
                fp, loadedFiles.size(), nextSnap->cells.size(), nextSnap->doors.size());
            return;
        }

        g_cacheHitOnStartup.store(false);
        logger::info("[LF] Blueprint cache {} (fingerprint=0x{:016X}, {} mods) -> auto-indexing CELL records...",
            a_forceRebuild ? "FORCE REBUILD requested" : "MISS / load order changed",
            fp, loadedFiles.size());
        PocketBlueprint::RebuildAndSaveBlueprints(kCachePath, fp, loadedFiles, g_config.maxRefsPerCell, nextSnap->doors, nextSnap->cells);
        g_bpCellCount.store(nextSnap->cells.size());
        g_bpDoorCount.store(nextSnap->doors.size());
        {
            std::lock_guard<std::mutex> lk(g_bpMutex);
            g_bpSnapshot = nextSnap;
        }
    }

    // =========================================================================
    //  Pocket Slot & Runtime State
    // =========================================================================
    struct MovedActorRecord
    {
        RE::ActorHandle actorHandle;
        RE::FormID      actorFormID = 0;
        RE::FormID      origCellID = 0;
        RE::NiPoint3    origPos{};
        RE::NiPoint3    origRot{};
    };

    struct PocketInstance
    {
        std::uint32_t                    slotIndex = 0;
        RE::FormID                       interiorCellID = 0;
        std::string                      interiorEditorID;
        std::string                      interiorFullName;
        RE::FormID                       hostCellID = 0;
        RE::FormID                       hostWorldID = 0;
        RE::FormID                       locationID = 0;
        RE::NiPoint3                     pocketOrigin{};
        RE::NiPoint3                     boundMin{};
        RE::NiPoint3                     boundMax{};
        RE::NiPoint3                     returnWorldPos{};
        RE::NiPoint3                     returnWorldRot{};
        bool                             hasReturnWorldPos = false;
        std::vector<RE::ObjectRefHandle> spawnedRefs;
        std::vector<MovedActorRecord>    movedActors;
        std::vector<RE::FormID>          clonedDoorIDs;
        std::vector<RE::FormID>          clonedContIDs;
        std::size_t                      clonedStaticCount = 0;
        std::size_t                      clonedDoorCount = 0;
        std::size_t                      clonedContainerCount = 0;
        bool                             builtFromBlueprint = false;
        std::uint64_t                    lastUsed = 0;  // P0-2: LRU stamp for slot recycling
    };

    // defined further down; needed here so LRU eviction can send that pocket's NPCs home first
    void TransferPocketActors(PocketInstance& a_inst, bool a_intoPocket);

    // P0-1: recursive by design. The hooked Activate / condition-function hooks can re-enter this
    // critical section on the SAME thread (CreateReferenceAtLocation -> cell attach -> activate,
    // SetPosition -> linked-ref activate, BGSOpenCloseForm -> open/close events). A plain std::mutex
    // turned every such re-entry into a hard game-thread deadlock.
    std::recursive_mutex                           g_mutex;
    std::uint64_t                                  g_slotTick = 0;  // P0-2: monotonic LRU clock
    std::unordered_map<RE::FormID, PocketInstance> g_pockets;
    std::unordered_map<RE::FormID, RE::FormID>     g_clonedDoorToOriginalDoor;
    std::unordered_map<RE::FormID, RE::FormID>     g_clonedContToOriginalCont;
    std::atomic<RE::FormID>                        g_activeLoadingFlowID{ 0 };
    std::atomic<std::uint32_t>                     g_transitionCount{ 0 };
    std::atomic<bool>                              g_bypassContainerHook{ false };
    std::atomic<bool>                              g_transitionInProgress{ false };
    RE::NiPoint3                                   g_lastExteriorReturnPos{};
    RE::NiPoint3                                   g_lastExteriorReturnRot{};
    std::atomic<bool>                              g_hasExteriorReturnPos{ false };

    RE::TESObjectSTAT*         g_blackPlaneStat = nullptr;
    RE::TESImageSpaceModifier* g_fadeImod = nullptr;
    RE::TESImageSpaceModifier* g_fadeBackImod = nullptr;
    // 0x000F756E = FadeToBlackHoldImod -- the engine's own "stay black" stage. Verified by dumping
    // all 263 IMAD records from Skyrim.esm + DLCs; using it is cleaner than re-triggering
    // FadeToBlackImod to fake a hold.
    RE::TESImageSpaceModifier* g_fadeHoldImod = nullptr;

    void CacheForms()
    {
        if (!g_blackPlaneStat) {
            g_blackPlaneStat = RE::TESForm::LookupByID<RE::TESObjectSTAT>(0x000F13C4);
        }
        if (!g_fadeImod) {
            g_fadeImod = RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(0x000F756D);
        }
        // P2-10: Cache 0x000F756F (FadeToBlackBackImod) so we don't LookupByID on every transition
        if (!g_fadeBackImod) {
            g_fadeBackImod = RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(0x000F756F);
        }
        if (!g_fadeHoldImod) {
            g_fadeHoldImod = RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(0x000F756E);
        }
    }

    // Check if a door is a frameless cave/mine auto-load trigger or hidden portal
    bool IsAutoOrFramelessDoor(RE::TESObjectREFR* a_doorRef)
    {
        if (!a_doorRef) {
            return true;
        }
        auto* baseObj = a_doorRef->GetBaseObject();
        auto* doorForm = baseObj ? baseObj->As<RE::TESObjectDOOR>() : nullptr;
        if (!doorForm) {
            return true;
        }
        if (doorForm->flags.any(RE::TESObjectDOOR::Flag::kAutomatic, RE::TESObjectDOOR::Flag::kHidden)) {
            return true;
        }
        return !doorForm->HasWorldModel();
    }

    // =========================================================================
    //  L2: fake cell / location events (opt-in)
    //  ScriptEventSourceHolder is the source that feeds BOTH SKSE plugin sinks and SkyrimVM (which
    //  turns these into Papyrus OnCellAttach / OnCellLoad / OnLocationChange), so sending here
    //  reaches quest fragments and alias scripts. It also reaches every other mod's sink, hence the
    //  INI switch and a single shot per transition.
    // =========================================================================
    RE::TESObjectCELL* LookupCellOrNull(RE::FormID a_id)
    {
        return a_id != 0 ? RE::TESForm::LookupByID<RE::TESObjectCELL>(a_id) : nullptr;
    }

    void FireFakeCellEvents(RE::TESObjectCELL* a_fromCell, RE::TESObjectCELL* a_toCell, bool a_entering)
    {
        if (!g_config.fakeLocationEvents && !g_config.fakeCellLoadEvents) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!player || !holder) {
            return;
        }
        RE::BGSLocation* fromLoc = a_fromCell ? a_fromCell->GetLocation() : nullptr;
        RE::BGSLocation* toLoc = a_toCell ? a_toCell->GetLocation() : nullptr;

        if (g_config.fakeCellLoadEvents) {
            // NOTE: engine semantics (Nexus "OnCellAttach / OnCellDetach" thread) is cell-scoped --
            // "the cell enters the world". This event carries no cell field, so how far it reaches is
            // determined by how the VM resolves the reference. Kept behind its own switch.
            RE::TESCellAttachDetachEvent attachEvt{};
            attachEvt.reference = RE::NiPointer<RE::TESObjectREFR>(player);
            attachEvt.attached = a_entering;
            holder->SendEvent(&attachEvt);

            if (a_entering && a_toCell) {
                RE::TESCellFullyLoadedEvent loadedEvt{};
                loadedEvt.cell = a_toCell;
                holder->SendEvent(&loadedEvt);
            }
        }

        if (g_config.fakeLocationEvents) {
            RE::TESActorLocationChangeEvent locEvt{};
            locEvt.actor = RE::NiPointer<RE::TESObjectREFR>(player);
            locEvt.oldLoc = a_entering ? fromLoc : toLoc;
            locEvt.newLoc = a_entering ? toLoc : fromLoc;
            holder->SendEvent(&locEvt);
        }

        logger::info("[LF] FAKE events ({}) cell 0x{:08X} -> 0x{:08X} | loc 0x{:08X} -> 0x{:08X} | sent:{}",
            a_entering ? "enter" : "exit",
            a_fromCell ? a_fromCell->GetFormID() : 0,
            a_toCell ? a_toCell->GetFormID() : 0,
            fromLoc ? fromLoc->GetFormID() : 0,
            toLoc ? toLoc->GetFormID() : 0,
            g_config.fakeCellLoadEvents
                ? (g_config.fakeLocationEvents ? " attach+loaded+location" : " attach+loaded")
                : " location");
    }

    // Stage 1 (T=0ms): Play door sound, crack door/trapdoor open slightly, play player hand reach, and start fade-in
    void BeginDoorTransitionVisuals(RE::TESObjectREFR* a_doorRef, bool a_isAutoCaveDoor)
    {
        if (a_doorRef && g_config.playDoorSoundAndAnim) {
            auto* baseObj = a_doorRef->GetBaseObject();
            auto* doorForm = baseObj ? baseObj->As<RE::TESObjectDOOR>() : nullptr;
            if (doorForm && doorForm->openSound) {
                if (auto* audio = RE::BSAudioManager::GetSingleton()) {
                    audio->Play(doorForm->openSound->GetFormID());
                }
            }
            if (!a_isAutoCaveDoor) {
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    player->NotifyAnimationGraph("IdleActivateDoor");
                }
                RE::BGSOpenCloseForm::SetOpenState(a_doorRef, true, false);
            }
        }
        if (g_config.triggerBlinkImod) {
            CacheForms();
            if (g_fadeImod) {
                RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeImod, g_config.blinkStrength, nullptr);
            }
        }
    }

    // Stage 2 (T=delayMs, at peak dark frame): Snap source door closed so it isn't left ajar, and fade back from black
    void FinishDoorTransitionVisuals(RE::ObjectRefHandle a_sourceDoorHandle, bool a_isAutoCaveDoor)
    {
        if (!a_isAutoCaveDoor && g_config.playDoorSoundAndAnim) {
            if (auto doorRef = a_sourceDoorHandle.get()) {
                RE::BGSOpenCloseForm::SetOpenState(doorRef.get(), false, true);
            }
        }
        if (g_config.triggerBlinkImod) {
            CacheForms();
            if (g_fadeImod) {
                RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
            }
            if (g_fadeBackImod) {
                RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeBackImod, g_config.blinkStrength, nullptr);
            }
        }
    }

    bool ShouldCloneBaseObject(RE::TESBoundObject* a_base, RE::TESObjectREFR* a_origRef)
    {
        if (!a_base || !a_origRef) {
            return false;
        }
        if (a_origRef->IsDisabled() || a_origRef->IsDeleted()) {
            return false;
        }
        if (auto* xEsp = a_origRef->extraList.GetByType<RE::ExtraEnableStateParent>()) {
            if (auto parentPtr = xEsp->parent.get()) {
                if (parentPtr->IsDisabled()) {
                    return false;
                }
            }
        }
        const auto ft = a_base->GetFormType();
        const auto fid = a_base->GetFormID();
        if (fid == 0x0000003B || fid == 0x00000034 || fid == 0x00000003 || fid == 0x00000032) {
            return false;
        }
        switch (ft) {
        case RE::FormType::Static:
        case RE::FormType::MovableStatic:
        case RE::FormType::Furniture:
        case RE::FormType::Door:
        case RE::FormType::Container:
        case RE::FormType::Light:
        case RE::FormType::Misc:
        case RE::FormType::Book:
        case RE::FormType::Flora:
        case RE::FormType::Tree:
        case RE::FormType::AlchemyItem:
        case RE::FormType::Ingredient:
        case RE::FormType::Weapon:
        case RE::FormType::Armor:
        case RE::FormType::Ammo:
        case RE::FormType::KeyMaster:
        case RE::FormType::SoulGem:
        case RE::FormType::Scroll:
        case RE::FormType::Apparatus:
            return true;
        case RE::FormType::Activator:
            if (a_origRef->extraList.HasType(RE::ExtraDataType::kPrimitive) && !a_base->HasWorldModel()) {
                return false;
            }
            return a_base->HasWorldModel();
        default:
            return false;
        }
    }

    void SpawnBlackShell(PocketInstance& a_inst, RE::TESObjectCELL* a_hostCell, RE::TESWorldSpace* a_hostWorld)
    {
        if (!g_config.spawnBlackBoxShell) {
            return;
        }
        CacheForms();
        if (!g_blackPlaneStat) {
            return;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return;
        }
        const float cx = 0.5f * (a_inst.boundMin.x + a_inst.boundMax.x);
        const float cy = 0.5f * (a_inst.boundMin.y + a_inst.boundMax.y);
        const float cz = 0.5f * (a_inst.boundMin.z + a_inst.boundMax.z);
        const float pad = 400.0f;
        const float minX = a_inst.boundMin.x - pad;
        const float maxX = a_inst.boundMax.x + pad;
        const float minY = a_inst.boundMin.y - pad;
        const float maxY = a_inst.boundMax.y + pad;
        const float minZ = a_inst.boundMin.z - pad;
        const float maxZ = a_inst.boundMax.z + pad;

        // P1-8: BlackPlane01 (0x000F13C4) has local OBND x:[-256..0], y:[-256..0], z:[0..1] (corner origin).
        // Compute a dynamic scale that covers the full span of this room, and offset each plane's placement
        // by its rotated OBND half-span so the plane center aligns with the face center (cx, cy, cz).
        const float spanX = maxX - minX;
        const float spanY = maxY - minY;
        const float spanZ = maxZ - minZ;
        const float maxSpan = (std::max)({ spanX, spanY, spanZ, 2560.0f });
        const float neededScale = std::clamp((maxSpan * 1.25f) / 256.0f, 10.0f, 40.0f);
        const auto  refScalePct = static_cast<std::uint16_t>( neededScale * 100.0f );
        const float halfSide = 128.0f * neededScale;

        struct PlaneSpec
        {
            RE::NiPoint3 pos;
            RE::NiPoint3 rot;
        };
        constexpr float kHalfPi = 1.5707963f;
        constexpr float kPi = 3.1415926f;
        const PlaneSpec planes[] = {
            { { cx + halfSide, cy - halfSide, maxZ }, { kPi, 0.0f, 0.0f } },
            { { cx + halfSide, cy + halfSide, minZ }, { 0.0f, 0.0f, 0.0f } },
            { { cx + halfSide, maxY, cz - halfSide }, { kHalfPi, 0.0f, 0.0f } },
            { { cx + halfSide, minY, cz + halfSide }, { -kHalfPi, 0.0f, 0.0f } },
            { { maxX, cy + halfSide, cz - halfSide }, { 0.0f, -kHalfPi, 0.0f } },
            { { minX, cy + halfSide, cz + halfSide }, { 0.0f, kHalfPi, 0.0f } }
        };

        for (const auto& pl : planes) {
            auto handle = dh->CreateReferenceAtLocation(
                g_blackPlaneStat, pl.pos, pl.rot, a_hostCell, a_hostWorld,
                nullptr, nullptr, RE::ObjectRefHandle(), false, true);
            if (auto ref = handle.get()) {
                ref->GetReferenceRuntimeData().refScale = refScalePct;
                a_inst.spawnedRefs.push_back(handle);
            }
        }
    }

    PocketInstance* GetOrCreatePocket(RE::FormID a_interiorCellID, RE::TESObjectCELL* a_interiorCell,
        RE::TESObjectCELL* a_hostCell, RE::TESWorldSpace* a_hostWorld, const RE::NiPoint3& a_anchorXY,
        const std::shared_ptr<const BlueprintSnapshot>& a_bpSnap)
    {
        if (a_interiorCellID == 0 || !a_hostCell) {
            return nullptr;
        }
        auto it = g_pockets.find(a_interiorCellID);
        if (it != g_pockets.end()) {
            bool valid = !it->second.spawnedRefs.empty();
            if (valid) {
                auto firstRef = it->second.spawnedRefs.front().get();
                if (!firstRef || firstRef->IsDeleted() || firstRef->IsDisabled()) {
                    valid = false;
                }
            }
            if (valid) {
                it->second.lastUsed = ++g_slotTick;
                return &it->second;
            }
            g_pockets.erase(it);
        }

        // P0-2: reclaim the least-recently-used slot instead of silently degrading to a black
        // loading screen once the player has visited more interiors than uMaxPocketSlots.
        if (g_pockets.size() >= g_config.maxPocketSlots) {
            const RE::FormID activeID = g_activeLoadingFlowID.load();
            auto victimIt = g_pockets.end();
            std::uint64_t victimTick = (std::numeric_limits<std::uint64_t>::max)();
            for (auto vIt = g_pockets.begin(); vIt != g_pockets.end(); ++vIt) {
                if (vIt->first == activeID) {
                    continue;  // never recycle the pocket the player is currently standing in
                }
                if (vIt->second.lastUsed < victimTick) {
                    victimTick = vIt->second.lastUsed;
                    victimIt = vIt;
                }
            }
            if (victimIt != g_pockets.end()) {
                // P1-1: send that pocket's NPCs home before the mapping tables are torn down.
                TransferPocketActors(victimIt->second, false);
                std::size_t freed = 0;
                for (auto& h : victimIt->second.spawnedRefs) {
                    if (auto ref = h.get()) {
                        ref->Disable();
                        ref->SetDelete(true);
                        ++freed;
                    }
                }
                for (const auto fid : victimIt->second.clonedDoorIDs) {
                    g_clonedDoorToOriginalDoor.erase(fid);
                }
                for (const auto fid : victimIt->second.clonedContIDs) {
                    g_clonedContToOriginalCont.erase(fid);
                }
                logger::info("Recycling Pocket Slot #{} cell=0x{:08X} ({}) [LRU] to free a slot for 0x{:08X}: {} refs marked for deletion",
                    victimIt->second.slotIndex, victimIt->first, victimIt->second.interiorEditorID,
                    a_interiorCellID, freed);
                g_pockets.erase(victimIt);
            }
        }

        if (g_pockets.size() >= g_config.maxPocketSlots) {
            logger::warn("Pocket slot limit ({}) reached and no slot could be recycled (player inside pocket); "
                         "falling back to vanilla transition for cell 0x{:08X}",
                g_config.maxPocketSlots, a_interiorCellID);
            return nullptr;
        }

        // First attempt: collect live references from a_interiorCell if its temporary STATs are loaded
        std::vector<RE::TESObjectREFR*> origRefs;
        std::vector<RE::Actor*>         origActors;
        std::size_t                     liveStatCount = 0;

        if (a_interiorCell) {
            a_interiorCell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
                if (!a_ref || a_ref->IsDeleted() || a_ref->IsDisabled()) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                if (auto* actor = a_ref->As<RE::Actor>()) {
                    if (!actor->IsPlayerRef() && !actor->IsDead(false)) {
                        origActors.push_back(actor);
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                auto* base = a_ref->GetBaseObject();
                if (ShouldCloneBaseObject(base, a_ref)) {
                    origRefs.push_back(a_ref);
                    if (base->GetFormType() == RE::FormType::Static) {
                        ++liveStatCount;
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }

        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return nullptr;
        }

        const auto slotIdx = static_cast<std::uint32_t>(g_pockets.size());
        const float slotZ = g_config.baseAltitudeZ + static_cast<float>(slotIdx) * g_config.slotStepZ;

        // Branch 1: Live memory has full room geometry (liveStatCount >= 5)
        if (liveStatCount >= 5 && origRefs.size() <= g_config.maxRefsPerCell) {
            RE::NiPoint3 localMin{ 1e9f, 1e9f, 1e9f };
            RE::NiPoint3 localMax{ -1e9f, -1e9f, -1e9f };
            for (auto* r : origRefs) {
                const auto p = r->GetPosition();
                localMin.x = (std::min)(localMin.x, p.x);
                localMin.y = (std::min)(localMin.y, p.y);
                localMin.z = (std::min)(localMin.z, p.z);
                localMax.x = (std::max)(localMax.x, p.x);
                localMax.y = (std::max)(localMax.y, p.y);
                localMax.z = (std::max)(localMax.z, p.z);
            }
            const float localCenterX = 0.5f * (localMin.x + localMax.x);
            const float localCenterY = 0.5f * (localMin.y + localMax.y);

            PocketInstance inst{};
            inst.slotIndex = slotIdx;
            inst.interiorCellID = a_interiorCellID;
            inst.interiorEditorID = (a_interiorCell && a_interiorCell->GetFormEditorID()) ? a_interiorCell->GetFormEditorID() : "";
            inst.interiorFullName = (a_interiorCell && a_interiorCell->GetFullName()) ? a_interiorCell->GetFullName() : "";
            inst.hostCellID = a_hostCell->GetFormID();
            inst.hostWorldID = a_hostWorld ? a_hostWorld->GetFormID() : 0;
            inst.locationID = (a_interiorCell && a_interiorCell->GetLocation()) ? a_interiorCell->GetLocation()->GetFormID() : 0;
            inst.pocketOrigin = { a_anchorXY.x - localCenterX, a_anchorXY.y - localCenterY, slotZ };
            inst.boundMin = { localMin.x + inst.pocketOrigin.x, localMin.y + inst.pocketOrigin.y, localMin.z + inst.pocketOrigin.z };
            inst.boundMax = { localMax.x + inst.pocketOrigin.x, localMax.y + inst.pocketOrigin.y, localMax.z + inst.pocketOrigin.z };
            inst.builtFromBlueprint = false;

            for (auto* origRef : origRefs) {
                auto* baseObj = origRef->GetBaseObject();
                if (!baseObj) {
                    continue;
                }
                const RE::NiPoint3 newPos = origRef->GetPosition() + inst.pocketOrigin;
                const RE::NiPoint3 newRot = origRef->GetAngle();

                auto handle = dh->CreateReferenceAtLocation(
                    baseObj, newPos, newRot, a_hostCell, a_hostWorld,
                    nullptr, nullptr, RE::ObjectRefHandle(), false, true);
                auto newRef = handle.get();
                if (!newRef) {
                    continue;
                }
                newRef->GetReferenceRuntimeData().refScale = origRef->GetReferenceRuntimeData().refScale;
                if (auto* owner = origRef->GetOwner()) {
                    newRef->extraList.SetOwner(owner);
                } else if (a_interiorCell && a_interiorCell->GetOwner()) {
                    newRef->extraList.SetOwner(a_interiorCell->GetOwner());
                }

                const auto ft = baseObj->GetFormType();
                if (ft == RE::FormType::Door) {
                    if (origRef->extraList.HasType(RE::ExtraDataType::kTeleport) || (a_bpSnap && a_bpSnap->doors.count(origRef->GetFormID()) != 0)) {
                        g_clonedDoorToOriginalDoor[newRef->GetFormID()] = origRef->GetFormID();
                        inst.clonedDoorIDs.push_back(newRef->GetFormID());
                        ++inst.clonedDoorCount;
                    }
                } else if (ft == RE::FormType::Container) {
                    g_clonedContToOriginalCont[newRef->GetFormID()] = origRef->GetFormID();
                    inst.clonedContIDs.push_back(newRef->GetFormID());
                    ++inst.clonedContainerCount;
                } else {
                    ++inst.clonedStaticCount;
                }
                inst.spawnedRefs.push_back(handle);
            }

            SpawnBlackShell(inst, a_hostCell, a_hostWorld);

            if (g_config.moveActorsToPocket) {
                for (auto* actor : origActors) {
                    if (!actor) {
                        continue;
                    }
                    MovedActorRecord rec{};
                    rec.actorHandle = actor->GetHandle();
                    rec.actorFormID = actor->GetFormID();
                    rec.origCellID = a_interiorCellID;
                    rec.origPos = actor->GetPosition();
                    rec.origRot = actor->GetAngle();
                    inst.movedActors.push_back(rec);
                }
            }

            logger::info("Built Pocket Slot #{} [LIVE] for cell 0x{:08X} ({}) at origin=({:.1f},{:.1f},{:.1f}): "
                         "statics={} doors={} containers={} actors={}",
                inst.slotIndex, inst.interiorCellID, inst.interiorEditorID,
                inst.pocketOrigin.x, inst.pocketOrigin.y, inst.pocketOrigin.z,
                inst.clonedStaticCount, inst.clonedDoorCount, inst.clonedContainerCount, inst.movedActors.size());

            auto [insIt, _] = g_pockets.emplace(a_interiorCellID, std::move(inst));
            return &insIt->second;
        }

        // Branch 2: Live cell temporary geometry is not in memory yet -> build from offline Blueprint!
        if (!a_bpSnap) {
            logger::info("Cell 0x{:08X} not ready in live memory (stats={}) and no Blueprint snapshot loaded; using vanilla load",
                a_interiorCellID, liveStatCount);
            return nullptr;
        }
        const auto bpIt = a_bpSnap->cells.find(a_interiorCellID);
        if (bpIt == a_bpSnap->cells.end() || bpIt->second.refs.empty() || bpIt->second.refs.size() > g_config.maxRefsPerCell) {
            logger::info("Cell 0x{:08X} not ready in live memory (stats={}) and not in Blueprint table; using vanilla load",
                a_interiorCellID, liveStatCount);
            return nullptr;
        }

        const BlueprintCell& bp = bpIt->second;
        RE::NiPoint3 localMin{ 1e9f, 1e9f, 1e9f };
        RE::NiPoint3 localMax{ -1e9f, -1e9f, -1e9f };
        for (const auto& r : bp.refs) {
            localMin.x = (std::min)(localMin.x, r.pos[0]);
            localMin.y = (std::min)(localMin.y, r.pos[1]);
            localMin.z = (std::min)(localMin.z, r.pos[2]);
            localMax.x = (std::max)(localMax.x, r.pos[0]);
            localMax.y = (std::max)(localMax.y, r.pos[1]);
            localMax.z = (std::max)(localMax.z, r.pos[2]);
        }
        const float localCenterX = 0.5f * (localMin.x + localMax.x);
        const float localCenterY = 0.5f * (localMin.y + localMax.y);

        PocketInstance inst{};
        inst.slotIndex = slotIdx;
        inst.interiorCellID = a_interiorCellID;
        inst.interiorEditorID = bp.edid;
        inst.interiorFullName = (a_interiorCell && a_interiorCell->GetFullName()) ? a_interiorCell->GetFullName() : bp.edid;
        inst.hostCellID = a_hostCell->GetFormID();
        inst.hostWorldID = a_hostWorld ? a_hostWorld->GetFormID() : 0;
        inst.locationID = bp.locationID;
        inst.pocketOrigin = { a_anchorXY.x - localCenterX, a_anchorXY.y - localCenterY, slotZ };
        inst.boundMin = { localMin.x + inst.pocketOrigin.x, localMin.y + inst.pocketOrigin.y, localMin.z + inst.pocketOrigin.z };
        inst.boundMax = { localMax.x + inst.pocketOrigin.x, localMax.y + inst.pocketOrigin.y, localMax.z + inst.pocketOrigin.z };
        inst.builtFromBlueprint = true;

        RE::TESForm* cellOwnerForm = bp.ownerID ? RE::TESForm::LookupByID(bp.ownerID) : nullptr;

        for (const auto& r : bp.refs) {
            auto* form = RE::TESForm::LookupByID(r.baseID);
            auto* baseObj = form ? form->As<RE::TESBoundObject>() : nullptr;
            if (!baseObj) {
                continue;
            }
            const RE::NiPoint3 newPos{
                r.pos[0] + inst.pocketOrigin.x,
                r.pos[1] + inst.pocketOrigin.y,
                r.pos[2] + inst.pocketOrigin.z
            };
            const RE::NiPoint3 newRot{ r.rot[0], r.rot[1], r.rot[2] };

            auto handle = dh->CreateReferenceAtLocation(
                baseObj, newPos, newRot, a_hostCell, a_hostWorld,
                nullptr, nullptr, RE::ObjectRefHandle(), false, true);
            auto newRef = handle.get();
            if (!newRef) {
                continue;
            }
            newRef->GetReferenceRuntimeData().refScale = r.scale;
            if (r.ownerID != 0) {
                if (auto* own = RE::TESForm::LookupByID(r.ownerID)) {
                    newRef->extraList.SetOwner(own);
                }
            } else if (cellOwnerForm) {
                newRef->extraList.SetOwner(cellOwnerForm);
            }

            if ((r.flags & 1) != 0) {
                g_clonedDoorToOriginalDoor[newRef->GetFormID()] = r.origRefID;
                inst.clonedDoorIDs.push_back(newRef->GetFormID());
                ++inst.clonedDoorCount;
            } else if ((r.flags & 2) != 0) {
                g_clonedContToOriginalCont[newRef->GetFormID()] = r.origRefID;
                inst.clonedContIDs.push_back(newRef->GetFormID());
                ++inst.clonedContainerCount;
            } else {
                ++inst.clonedStaticCount;
            }
            inst.spawnedRefs.push_back(handle);
        }

        SpawnBlackShell(inst, a_hostCell, a_hostWorld);

        if (g_config.moveActorsToPocket) {
            for (const auto& a : bp.actors) {
                if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(a.achrRefID)) {
                    if (!actor->IsDead(false) && !actor->IsDisabled()) {
                        MovedActorRecord rec{};
                        rec.actorHandle = actor->GetHandle();
                        rec.actorFormID = actor->GetFormID();
                        rec.origCellID = a_interiorCellID;
                        rec.origPos = { a.pos[0], a.pos[1], a.pos[2] };
                        rec.origRot = { a.rot[0], a.rot[1], a.rot[2] };
                        inst.movedActors.push_back(rec);
                    }
                }
            }
        }

        logger::info("Built Pocket Slot #{} [BLUEPRINT] for cell 0x{:08X} ({}) at origin=({:.1f},{:.1f},{:.1f}): "
                     "statics={} doors={} containers={} actors={}",
            inst.slotIndex, inst.interiorCellID, inst.interiorEditorID,
            inst.pocketOrigin.x, inst.pocketOrigin.y, inst.pocketOrigin.z,
            inst.clonedStaticCount, inst.clonedDoorCount, inst.clonedContainerCount, inst.movedActors.size());

        auto [insIt, _] = g_pockets.emplace(a_interiorCellID, std::move(inst));
        return &insIt->second;
    }

    void TransferPocketActors(PocketInstance& a_inst, bool a_intoPocket)
    {
        if (!g_config.moveActorsToPocket) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }

        // P1-1: to send an NPC home we first need a reference that lives in the ORIGINAL interior
        // cell (MoveTo switches the parent cell, SetPosition alone would leave them at Z=+26000).
        // The pocket's cloned doors map back to the real doors, which are exactly such references.
        RE::TESObjectREFR* origCellAnchor = nullptr;
        if (!a_intoPocket) {
            for (const auto doorID : a_inst.clonedDoorIDs) {
                const auto mapIt = g_clonedDoorToOriginalDoor.find(doorID);
                if (mapIt == g_clonedDoorToOriginalDoor.end()) {
                    continue;
                }
                if (auto* origDoor = RE::TESForm::LookupByID<RE::TESObjectREFR>(mapIt->second)) {
                    origCellAnchor = origDoor;
                    break;
                }
            }
        }

        std::size_t restored = 0;
        std::size_t stranded = 0;
        for (const auto& rec : a_inst.movedActors) {
            auto actorPtr = rec.actorHandle.get();
            if (!actorPtr) {
                actorPtr = RE::NiPointer<RE::Actor>(RE::TESForm::LookupByID<RE::Actor>(rec.actorFormID));
            }
            if (!actorPtr || actorPtr->IsDead(false)) {
                continue;
            }
            if (a_intoPocket) {
                const RE::NiPoint3 targetPos = rec.origPos + a_inst.pocketOrigin;
                actorPtr->MoveTo(player);
                actorPtr->SetPosition(targetPos, true);
                actorPtr->SetHeading(rec.origRot.z);
            } else if (origCellAnchor) {
                actorPtr->MoveTo(origCellAnchor);          // back into the real interior cell
                actorPtr->SetPosition(rec.origPos, true);  // then onto its recorded spot
                actorPtr->SetHeading(rec.origRot.z);
                ++restored;
            } else {
                ++stranded;
            }
        }

        if (!a_intoPocket) {
            if (restored > 0) {
                logger::info("[LF] Restored {} NPC(s) to cell 0x{:08X}", restored, a_inst.interiorCellID);
            }
            if (stranded > 0) {
                logger::warn("[LF] {} NPC(s) of cell 0x{:08X} could not be restored (no original door anchor); "
                             "they are still at Pocket altitude - toggle 'Move interior NPCs' off and reload a save "
                             "that predates this transition to clear them",
                    stranded, a_inst.interiorCellID);
            }
        }
    }

    void PurgeAllPockets(bool a_teleportPlayerOutIfInside)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_transitionInProgress.store(false);
        const RE::FormID activeID = g_activeLoadingFlowID.exchange(0);

        // P1-2: actually get the player out BEFORE the geometry is deleted, otherwise they are left
        // standing at Z=+26000 with the floor marked for deletion and fall to their death.
        if (a_teleportPlayerOutIfInside && activeID != 0) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                bool moved = false;
                auto it = g_pockets.find(activeID);
                if (it != g_pockets.end() && it->second.hasReturnWorldPos) {
                    // P1-1: send the NPCs of that pocket home while the mapping tables still exist.
                    TransferPocketActors(it->second, false);
                    player->SetPosition(it->second.returnWorldPos, true);
                    player->SetHeading(it->second.returnWorldRot.z);
                    logger::info("PurgeAllPockets: teleported player out of pocket 0x{:08X} to recorded return pos=({:.1f},{:.1f},{:.1f})",
                        activeID, it->second.returnWorldPos.x, it->second.returnWorldPos.y, it->second.returnWorldPos.z);
                    moved = true;
                }
                if (!moved && g_hasExteriorReturnPos.load()) {
                    player->SetPosition(g_lastExteriorReturnPos, true);
                    logger::info("PurgeAllPockets: no per-pocket return pos; used last exterior return pos=({:.1f},{:.1f},{:.1f})",
                        g_lastExteriorReturnPos.x, g_lastExteriorReturnPos.y, g_lastExteriorReturnPos.z);
                    moved = true;
                }
                if (!moved) {
                    const RE::NiPoint3 localPos = player->GetPosition();
                    logger::warn("PurgeAllPockets: player was inside pocket 0x{:08X} at ({:.1f},{:.1f},{:.1f}) and NO return "
                                 "position was recorded; you may fall - press ~ and use 'coc WhiterunOrigin' or reload",
                        activeID, localPos.x, localPos.y, localPos.z);
                }
            }
        }

        std::size_t deletedRefs = 0;
        for (auto& [cellID, inst] : g_pockets) {
            for (auto& h : inst.spawnedRefs) {
                if (auto ref = h.get()) {
                    ref->Disable();
                    ref->SetDelete(true);
                    ++deletedRefs;
                }
            }
        }
        g_pockets.clear();
        g_clonedDoorToOriginalDoor.clear();
        g_clonedContToOriginalCont.clear();
        if (deletedRefs > 0) {
            logger::info("Purged all pockets ({} temporary references marked for deletion)", deletedRefs);
        }
    }

    const PocketInstance* FindPocketContainingPoint(const RE::NiPoint3& a_pt)
    {
        constexpr float kMargin = 350.0f;
        for (const auto& [cellID, inst] : g_pockets) {
            if (a_pt.x >= inst.boundMin.x - kMargin && a_pt.x <= inst.boundMax.x + kMargin &&
                a_pt.y >= inst.boundMin.y - kMargin && a_pt.y <= inst.boundMax.y + kMargin &&
                a_pt.z >= inst.boundMin.z - kMargin && a_pt.z <= inst.boundMax.z + kMargin) {
                return &inst;
            }
        }
        return nullptr;
    }

    // =========================================================================
    //  Condition Function Hooks -- semantics verified against the CK wiki (ck.uesp.net):
    //    GetInCell           exact cell match only (Skyrim does NOT prefix-match)
    //    GetInSameCell       compare the cell each reference is considered to occupy
    //    GetInCurrentLoc     supplied location or any of its ancestors
    //    GetInWorldspace     always 0 while in an interior cell -> 0 inside a pocket
    //    GetInZone           parameter is an EncounterZone (0x32), not a keyword
    // =========================================================================
    RE::SCRIPT_FUNCTION::Condition_t* g_origGetInCell = nullptr;
    RE::SCRIPT_FUNCTION::Condition_t* g_origGetInSameCell = nullptr;
    RE::SCRIPT_FUNCTION::Condition_t* g_origGetInCurrentLoc = nullptr;
    RE::SCRIPT_FUNCTION::Condition_t* g_origGetInWorldspace = nullptr;
    RE::SCRIPT_FUNCTION::Condition_t* g_origGetInZone = nullptr;

    // CK wiki: GetInCurrentLoc matches "the supplied location or a location that has it as a parent".
    // Walking parentLoc by hand avoids depending on the IsParent/IsChild argument-direction convention.
    bool IsLocationOrDescendantOf(RE::BGSLocation* a_loc, RE::BGSLocation* a_ancestor)
    {
        if (!a_loc || !a_ancestor) {
            return false;
        }
        for (auto* loc = a_loc; loc; loc = loc->parentLoc) {
            if (loc == a_ancestor) {
                return true;
            }
        }
        return false;
    }

    // The cell a reference should be *considered* to be in: the pocket's interior cell while it
    // stands inside a pocket volume, otherwise its real parent cell. Caller holds g_mutex.
    RE::FormID EffectiveCellOf(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref) {
            return 0;
        }
        if (const auto* inst = FindPocketContainingPoint(a_ref->GetPosition())) {
            return inst->interiorCellID;
        }
        auto* cell = a_ref->GetParentCell();
        return cell ? cell->GetFormID() : 0;
    }

    bool HookedGetInCell(RE::TESObjectREFR* a_thisObj, void* a_param1, void* a_param2, double& a_result)
    {
        if (g_origGetInCell && g_origGetInCell(a_thisObj, a_param1, a_param2, a_result) && a_result != 0.0) {
            return true;
        }
        if (a_thisObj && a_param1 && g_activeLoadingFlowID.load() != 0) {
            auto* targetCell = static_cast<RE::TESObjectCELL*>(a_param1);
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            if (const auto* inst = FindPocketContainingPoint(a_thisObj->GetPosition())) {
                // CK wiki: Skyrim's GetInCell requires an EXACT cell match -- no name prefix matching.
                if (inst->interiorCellID == targetCell->GetFormID()) {
                    a_result = 1.0;
                    return true;
                }
            }
        }
        return true;
    }

    bool HookedGetInSameCell(RE::TESObjectREFR* a_thisObj, void* a_param1, void* a_param2, double& a_result)
    {
        if (g_origGetInSameCell && g_origGetInSameCell(a_thisObj, a_param1, a_param2, a_result)) {
            if (a_result != 0.0) {
                return true;
            }
        }
        if (a_thisObj && a_param1 && g_activeLoadingFlowID.load() != 0) {
            auto* otherRef = static_cast<RE::TESObjectREFR*>(a_param1);
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            // CK wiki: "in the same cell as TargetRefID" -- so compare the cells each reference is
            // considered to occupy, not just whether both stand in the same pocket volume.
            const RE::FormID cellA = EffectiveCellOf(a_thisObj);
            const RE::FormID cellB = EffectiveCellOf(otherRef);
            if (cellA != 0 && cellA == cellB) {
                a_result = 1.0;
                return true;
            }
        }
        return true;
    }

    bool HookedGetInCurrentLoc(RE::TESObjectREFR* a_thisObj, void* a_param1, void* a_param2, double& a_result)
    {
        if (g_origGetInCurrentLoc && g_origGetInCurrentLoc(a_thisObj, a_param1, a_param2, a_result) && a_result != 0.0) {
            return true;
        }
        if (a_thisObj && a_param1 && g_activeLoadingFlowID.load() != 0) {
            auto* targetLoc = static_cast<RE::BGSLocation*>(a_param1);
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            if (const auto* inst = FindPocketContainingPoint(a_thisObj->GetPosition())) {
                RE::BGSLocation* pocketLoc = nullptr;
                if (inst->locationID != 0) {
                    pocketLoc = RE::TESForm::LookupByID<RE::BGSLocation>(inst->locationID);
                } else if (auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(inst->interiorCellID)) {
                    pocketLoc = cell->GetLocation();
                }
                if (pocketLoc && IsLocationOrDescendantOf(pocketLoc, targetLoc)) {
                    a_result = 1.0;
                    return true;
                }
            }
        }
        return true;
    }

    // L1: the pocket copy sits inside an exterior cell, so "is this ref in worldspace X" answers
    // "yes, Tamriel" even when the player is semantically standing in an interior. Inside a pocket
    // the honest answer is "no worldspace at all", because interior cells belong to none.
    bool HookedGetInWorldspace(RE::TESObjectREFR* a_thisObj, void* a_param1, void* a_param2, double& a_result)
    {
        if (g_origGetInWorldspace && g_origGetInWorldspace(a_thisObj, a_param1, a_param2, a_result) && a_result != 0.0) {
            return true;
        }
        if (!a_thisObj || g_activeLoadingFlowID.load() == 0) {
            return true;
        }
        const RE::NiPoint3 pt = a_thisObj->GetPosition();
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        if (FindPocketContainingPoint(pt)) {
            a_result = 0.0;
        }
        return true;
    }

    // CK wiki: "GetInZone returns a 1 if the designated target is in the Encounter Zone" -- the
    // parameter is a BGSEncounterZone (SCRIPT_PARAM_TYPE kEncounterZone = 0x32), NOT a location
    // keyword. Matching on a keyword was wrong and is exactly what the param-type gate rejected.
    bool HookedGetInZone(RE::TESObjectREFR* a_thisObj, void* a_param1, void* a_param2, double& a_result)
    {
        if (g_origGetInZone && g_origGetInZone(a_thisObj, a_param1, a_param2, a_result) && a_result != 0.0) {
            return true;
        }
        if (!a_thisObj || !a_param1 || g_activeLoadingFlowID.load() == 0) {
            return true;
        }
        auto* zoneForm = static_cast<RE::TESForm*>(a_param1);
        if (zoneForm->GetFormType() != RE::FormType::EncounterZone) {
            return true;
        }
        const RE::NiPoint3 pt = a_thisObj->GetPosition();
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        const auto* inst = FindPocketContainingPoint(pt);
        if (!inst) {
            return true;
        }
        auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(inst->interiorCellID);
        // The EncounterZone only exists in LOADED_CELL_DATA, i.e. while that cell is actually loaded.
        // A pocket built from the offline blueprint has no loaded data, so leave the vanilla answer.
        if (cell && cell->GetRuntimeData().loadedData &&
            cell->GetRuntimeData().loadedData->encounterZone == static_cast<RE::BGSEncounterZone*>(zoneForm)) {
            a_result = 1.0;
        }
        return true;
    }

    void InstallConditionHooks()
    {
        auto* cmds = RE::SCRIPT_FUNCTION::GetFirstScriptCommand();
        if (!cmds) {
            logger::warn("GetFirstScriptCommand returned null; condition hooks skipped");
            return;
        }

        struct HookSpec
        {
            const char*                        name;
            RE::SCRIPT_FUNCTION::Condition_t** origSlot;
            RE::SCRIPT_FUNCTION::Condition_t*  replacement;
            std::uint32_t                      expectedParam0;  // SCRIPT_PARAM_TYPE value
        };

        // Parameter types are what the engine will actually hand us, so every entry is gated on the
        // real signature instead of trusting the name alone.
        const HookSpec specs[] = {
            { "GetInCell",       &g_origGetInCell,       &HookedGetInCell,       0x09u },  // kCell
            { "GetInSameCell",   &g_origGetInSameCell,   &HookedGetInSameCell,   0x04u },  // kObjectRef
            { "GetInCurrentLoc", &g_origGetInCurrentLoc, &HookedGetInCurrentLoc, 0x3Du },  // kLocation
            { "GetInWorldspace", &g_origGetInWorldspace, &HookedGetInWorldspace, 0x1Bu },  // kWorldOrList
            { "GetInZone",       &g_origGetInZone,       &HookedGetInZone,       0x32u },  // kEncounterZone
        };

        std::size_t hooked = 0;
        std::size_t dumped = 0;
        for (std::uint16_t i = 0; i < RE::SCRIPT_FUNCTION::Commands::kScriptCommandsEnd; ++i) {
            auto& cmd = cmds[i];
            if (!cmd.functionName || !cmd.conditionFunction) {
                continue;
            }

            // L1 diagnostic: print the real name + parameter signature of every cell/location-flavoured
            // condition, so extending this list never depends on guesswork from the CK wiki.
            const char* n = cmd.functionName;
            if (strstr(n, "Cell") || strstr(n, "Loc") || strstr(n, "World") ||
                strstr(n, "Zone") || strstr(n, "Space") || strstr(n, "Marker")) {
                std::string sig;
                for (std::uint16_t p = 0; p < cmd.numParams && cmd.params && p < 8; ++p) {
                    sig += std::format("{}:0x{:X} ", cmd.params[p].paramName ? cmd.params[p].paramName : "?",
                        static_cast<std::uint32_t>(cmd.params[p].paramType.get()));
                }
                logger::info("[cond-dump] idx={} name={} params={} | {}", i, n, cmd.numParams, sig);
                ++dumped;
            }

            for (const auto& spec : specs) {
                if (*spec.origSlot || _stricmp(n, spec.name) != 0) {
                    continue;
                }
                const std::uint32_t param0 = (cmd.numParams > 0 && cmd.params)
                    ? static_cast<std::uint32_t>(cmd.params[0].paramType.get())
                    : 0xFFFFu;
                if (param0 != spec.expectedParam0) {
                    logger::warn("[LF] Skip hooking {}: param0 type 0x{:X}, expected 0x{:X}",
                        spec.name, param0, spec.expectedParam0);
                    continue;
                }
                *spec.origSlot = cmd.conditionFunction;
                cmd.conditionFunction = spec.replacement;
                ++hooked;
                logger::info("Installed condition hook: {} (idx={}, param0=0x{:X})", spec.name, i, param0);
            }
        }
        logger::info("[LF] Condition hooks installed: {}/{} | candidate conditions dumped: {} "
                     "(see [cond-dump] lines above)",
            hooked, sizeof(specs) / sizeof(specs[0]), dumped);
    }

    // =========================================================================
    //  Vtable Hook Helpers
    // =========================================================================
    bool IsAddressInTextSegment(const void* a_addr)
    {
        const auto& mod = REL::Module::get();
        const auto seg = mod.segment(REL::Segment::textx);
        const auto segBase = seg.address();
        const auto segSize = static_cast<std::uintptr_t>(seg.size());
        const auto addr = reinterpret_cast<std::uintptr_t>(a_addr);
        return addr >= segBase && addr < (segBase + segSize);
    }

    bool IsExecutableAddress(const void* a_addr)
    {
        if (!a_addr) {
            return false;
        }
        constexpr std::uint32_t kMemCommit = 0x1000;
        constexpr std::uint32_t kExecMask = 0x10u | 0x20u | 0x40u | 0x80u;
        REX::W32::MEMORY_BASIC_INFORMATION mbi{};
        if (REX::W32::VirtualQuery(a_addr, &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (static_cast<std::uint32_t>(mbi.state) != kMemCommit) {
            return false;
        }
        return (static_cast<std::uint32_t>(mbi.protect) & kExecMask) != 0;
    }

    using Activate_t = bool (*)(RE::TESBoundObject*, RE::TESObjectREFR*, RE::TESObjectREFR*, std::uint8_t, RE::TESBoundObject*, std::int32_t);
    Activate_t g_origContActivate = nullptr;
    Activate_t g_origDoorActivate = nullptr;
    Activate_t g_origActiActivate = nullptr;   // travel effects (private build)
    Activate_t g_origBookActivate = nullptr;

    // =========================================================================
    //  Tier 2: suppress the Loading Menu during a REAL cell transition
    //  A real transition keeps every quest / location / navmesh / interior-lighting semantic
    //  intact -- only the menu is worth hiding. We drop its kShow at the menu's own
    //  ProcessMessage (vtable idx 4) inside a short window, and re-post it if the transition
    //  overruns, so the worst case degrades to vanilla behaviour instead of a silent hang.
    // =========================================================================
    using ProcessMessage_t = RE::UI_MESSAGE_RESULTS (*)(RE::IMenu*, RE::UIMessage&);
    ProcessMessage_t g_origLoadingProcessMessage = nullptr;

    std::atomic<bool>         g_suppressArmed{ false };
    std::atomic<bool>         g_suppressWanted{ false };  // captured at arm time, not read live
    std::atomic<bool>         g_noBlackWanted{ false };   // ditto
    std::atomic<std::int64_t> g_noBlackStartAt{ 0 };      // fades are left alone until this instant
    // Exemption window for global mode. Duration alone cannot tell a fast travel from a room
    // transition -- on a fast machine a short fast-travel load finishes inside the timeout. The
    // reliable signal is the menu that *precedes* it: the map closes before a fast travel, the main
    // menu closes before load-game. While this window is open, the LoadingMenu is left alone.
    std::atomic<std::int64_t> g_exemptUntil{ 0 };
    // Generation guard. Each arm() starts a watchdog thread; without this, a second transition
    // arming while the first watchdog is still looping would have two threads clearing/re-posting
    // state that belongs to the other one. Every watchdog remembers its generation and exits as
    // soon as a newer one is armed.
    std::atomic<std::uint32_t> g_suppressGen{ 0 };
    // Last measured load time per destination cell. Written and read on the main thread only, so no
    // lock is needed. Drives the adaptive fallback (uSuppressMaxMs).
    std::unordered_map<RE::FormID, std::uint32_t> g_cellLoadMs;
    // ---- Assembly probe state (READ-ONLY) ----------------------------------------------------
    // Armed right after a transition reports "finished"; the per-step main-thread task then samples
    // the player's transform and the camera FOV until they stop changing. Only the watchdog thread
    // writes the counters, and sampling always happens inside the main-thread task.
    std::atomic<bool>         g_probeArmed{ false };
    std::atomic<std::int64_t> g_probeStartAt{ 0 };
    std::atomic<std::uint32_t> g_probeSamples{ 0 };
    std::atomic<std::uint32_t> g_probeChanges{ 0 };
    std::atomic<std::int64_t> g_probeLastChange{ -1 };
    float g_probePrev[16] = {};
    std::size_t g_probePrevN = 0;
    float g_probePrevPlayer[3] = {};
    float g_probeFadeIn = -1.0f;
    std::atomic<bool> g_probeWalking{ false };
    // v1.1.3: separated accounting (translate / rotation / cell3D transform / child count).
    std::atomic<bool>         g_probeLooking{ false };
    std::atomic<std::int64_t> g_probeLastTranslate{ -1 };
    std::atomic<std::int64_t> g_probeLastRotate{ -1 };
    std::atomic<std::int64_t> g_probeLastCellXform{ -1 };
    std::atomic<std::int64_t> g_probeLastChild{ -1 };
    std::atomic<std::uint32_t> g_probeTranslateChanges{ 0 };
    std::atomic<std::uint32_t> g_probeRotateChanges{ 0 };
    std::atomic<std::uint32_t> g_probeChildChanges{ 0 };
    std::int64_t g_probePrevChildCount = -1;
    // Renderer pointer identities (read-only), so we notice when a texture must be re-created.
    std::atomic<std::uintptr_t> g_probeGfxMainTex{ 0 };
    std::atomic<std::uintptr_t> g_probeGfxFbTex{ 0 };
    std::atomic<std::uintptr_t> g_probeGfxSwapChain{ 0 };
    std::atomic<int> g_voidStep{ -1 };   // last applied darkening step (0..8) for the void ramp
    // One-shot instrumentation: LoadingMenu is a kAlwaysOpen menu, so after the first display the
    // engine may reuse the instance and request it with kReshow instead of kShow. Log the first few
    // message types we actually receive so this can be settled from a log instead of guessed.
    std::atomic<int> g_msgTypeLogBudget{ 12 };
    std::atomic<bool>         g_menuWasSuppressed{ false };
    // v1.6.7: after the cut the engine can ask for the LoadingMenu AGAIN (a late/second request).
    // We used to disarm at the cut, so that request went through -> the vanilla loading screen came
    // up over a playable game, nothing ever hid it, and it ate all input (ESC and console included).
    // Reported by two users. Keep eating those requests for a grace window after the cut.
    std::atomic<std::int64_t> g_graceSuppressUntil{ 0 };
    std::atomic<std::int64_t> g_suppressDeadline{ 0 };
    std::atomic<std::int64_t> g_fallbackAt{ 0 };   // set only AFTER we suppressed a menu
    std::atomic<std::int64_t> g_suppressStarted{ 0 };
    std::atomic<RE::FormID>   g_suppressFromCell{ 0 };

    std::int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // defined below; the global-mode branch of the menu hook needs it before the definition
    void ArmLoadingMenuSuppression(const char* a_reason, bool a_isAutoCaveDoor, RE::FormID a_destCell = 0);
    // ---- Assembly probe (READ-ONLY) ----------------------------------------------------------
    RE::Setting* FindIniSetting(const char* a_name);   // defined in the settings section below
    void         ProbeCheckGfxStability();             // defined below, used by ProbeArm

    // Answers ONE question with data: after the cell swap completes, how long does the engine keep
    // changing the scene? That number is what a retained-frame hold would have to cover, so it has
    // to be measured before any capture work is designed. Nothing here writes engine state.
    float EffectiveFadeInSeconds()
    {
        if (!g_config.tweakEngineTimers) {
            return -1.0f;
        }
        auto* s = FindIniSetting("fMinSecondsForLoadFadeIn:Interface");
        return (s && s->GetType() == RE::Setting::Type::kFloat) ? s->data.f : -1.0f;
    }

    void ProbeArm()
    {
        g_probeSamples.store(0);
        g_probeChanges.store(0);
        g_probeLastChange.store(-1);
        g_probePrevN = 0;
        g_probeWalking.store(false);
        g_probeLooking.store(false);
        g_probeLastTranslate.store(-1);
        g_probeLastRotate.store(-1);
        g_probeLastCellXform.store(-1);
        g_probeLastChild.store(-1);
        g_probeTranslateChanges.store(0);
        g_probeRotateChanges.store(0);
        g_probeChildChanges.store(0);
        g_probePrevChildCount = -1;
        g_probeFadeIn = EffectiveFadeInSeconds();
        g_probeStartAt.store(NowMs());
        g_probeArmed.store(true);
        if (g_config.probeGfx) {
            ProbeCheckGfxStability();
        }
        logger::info("[Probe] assembly watch armed (effective fMinSecondsForLoadFadeIn={:.2f}s, budget {} ms)",
            g_probeFadeIn, g_config.probeAssemblyMs);
    }

    // Runs on the MAIN thread, inside the existing per-step task.
    //
    // v1.1.2: samples NON-PLAYER signals only. The first probe version sampled the player's world
    // transform, and every "change" it reported was simply the player walking -- the camera FOV was
    // constant 80.00 across 551 samples, so that whole run measured footsteps, not the engine. The
    // scene-assembly spin can only live on the camera root or on the cell's 3D root, so sample those.
    void ProbeSample()
    {
        float       sig[16] = {};
        std::size_t n = 0;

        // 1) camera root (the node the engine rotates/scales while the new scene is assembled)
        if (auto* cam = RE::PlayerCamera::GetSingleton()) {
            if (auto* root = cam->cameraRoot.get()) {
                sig[n++] = root->world.translate.x;
                sig[n++] = root->world.translate.y;
                sig[n++] = root->world.translate.z;
                sig[n++] = root->world.scale;
                sig[n++] = root->local.scale;
                sig[n++] = root->world.rotate.entry[0][0];
                sig[n++] = root->world.rotate.entry[1][1];
                sig[n++] = root->world.rotate.entry[2][2];
            }
            sig[n++] = cam->worldFOV;
        }

        // 2) the cell's own 3D root. Its child count is the cleanest assembly signal we have:
        // the engine attaches objects one by one, so "children stop growing" == assembly finished.
        // Nothing the player does can change this number.
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* cell = pl ? pl->GetParentCell() : nullptr;
        std::uint32_t childCount = 0;
        bool          haveCell3D = false;
        if (cell && cell->GetRuntimeData().loadedData) {
            if (auto* cell3d = cell->GetRuntimeData().loadedData->cell3D.get()) {
                sig[n++] = cell3d->local.scale;
                sig[n++] = cell3d->world.scale;
                sig[n++] = cell3d->local.rotate.entry[0][0];
                sig[n++] = cell3d->local.rotate.entry[1][1];
                sig[n++] = cell3d->local.rotate.entry[2][2];
                sig[n++] = cell3d->world.translate.z;
                childCount = static_cast<std::uint32_t>(cell3d->GetChildren().size());
                haveCell3D = true;
            }
        }

        // 3) walking guard: the player's own movement must never be mistaken for assembly activity.
        float       px = 0.0f, py = 0.0f, pz = 0.0f;
        bool        havePos = false;
        if (auto* node = pl ? pl->Get3D() : nullptr) {
            px = node->world.translate.x;
            py = node->world.translate.y;
            pz = node->world.translate.z;
            havePos = true;
        }

        const auto elapsed = NowMs() - g_probeStartAt.load();
        g_probeSamples.fetch_add(1);

        bool walking = false;
        if (havePos && g_probePrevN > 0) {
            const float dx = px - g_probePrevPlayer[0];
            const float dy = py - g_probePrevPlayer[1];
            const float dz = pz - g_probePrevPlayer[2];
            walking = (dx * dx + dy * dy + dz * dz) > (0.02f * 0.02f);
        }
        if (havePos) {
            g_probePrevPlayer[0] = px;
            g_probePrevPlayer[1] = py;
            g_probePrevPlayer[2] = pz;
        }
        if (walking) {
            if (!g_probeWalking.exchange(true)) {
                logger::info("[Probe] player is moving from +{} ms on -- movement is excluded from the assembly accounting", elapsed);
            }
        }

        // ---- separated accounting -----------------------------------------------------------
        // v1.1.3: translate / rotation / cell-child-count are tracked independently. The previous
        // version merged them into one signature, so a player turning the camera looked exactly like
        // "the engine is still assembling" -- that is why several runs reported 2.5 s of "movement".
        const bool havePrev = (g_probePrevN == n && n > 0);
        if (havePrev) {
            // camera root translation (engine camera interpolation; player input cannot move it)
            if (std::fabs(sig[0] - g_probePrev[0]) > 1.0e-4f ||
                std::fabs(sig[1] - g_probePrev[1]) > 1.0e-4f ||
                std::fabs(sig[2] - g_probePrev[2]) > 1.0e-4f) {
                g_probeTranslateChanges.fetch_add(1);
                g_probeLastTranslate.store(elapsed);
            }
            // camera root rotation (may be the PLAYER looking around -- flagged, not trusted)
            if (std::fabs(sig[5] - g_probePrev[5]) > 1.0e-3f ||
                std::fabs(sig[6] - g_probePrev[6]) > 1.0e-3f ||
                std::fabs(sig[7] - g_probePrev[7]) > 1.0e-3f) {
                g_probeRotateChanges.fetch_add(1);
                g_probeLastRotate.store(elapsed);
                g_probeLooking.store(true);
            }
            // cell 3D own transform (engine-side)
            bool cellMoved = false;
            for (std::size_t i = 8; i < n - 1; ++i) {
                if (std::fabs(sig[i] - g_probePrev[i]) > 1.0e-4f) {
                    cellMoved = true;
                    break;
                }
            }
            if (cellMoved) {
                g_probeLastCellXform.store(elapsed);
            }
        }
        // cell child count: the clean assembly signal -- log every change, nothing else per step
        if (haveCell3D) {
            if (g_probePrevChildCount >= 0 && static_cast<std::uint32_t>(g_probePrevChildCount) != childCount) {
                g_probeChildChanges.fetch_add(1);
                g_probeLastChild.store(elapsed);
                logger::info("[Probe] +{} ms cell3D children {} -> {}{}",
                    elapsed, g_probePrevChildCount, childCount, walking ? " (player walking)" : "");
            }
            g_probePrevChildCount = static_cast<std::int64_t>(childCount);
        }
        for (std::size_t i = 0; i < n; ++i) {
            g_probePrev[i] = sig[i];
        }
        g_probePrevN = n;

        if (elapsed >= static_cast<std::int64_t>(g_config.probeAssemblyMs)) {
            g_probeArmed.store(false);
            const auto samples = g_probeSamples.load();
            logger::info("[Probe] assembly summary after {} ms ({} samples, fadeIn={:.2f}s, walking={}, looking={}):",
                elapsed, samples, g_probeFadeIn,
                g_probeWalking.load() ? "yes(excluded)" : "no",
                g_probeLooking.load() ? "yes(rotation untrusted)" : "no");
            logger::info("[Probe]   camRoot translate : last change +{} ms ({} samples)",
                g_probeLastTranslate.load(), g_probeTranslateChanges.load());
            logger::info("[Probe]   camRoot rotation  : last change +{} ms ({} samples){}",
                g_probeLastRotate.load(), g_probeRotateChanges.load(),
                g_probeLooking.load() ? "  <- player may have been looking around" : "");
            logger::info("[Probe]   cell3D transform  : last change +{} ms",
                g_probeLastCellXform.load());
            logger::info("[Probe]   cell3D children   : {} changes, last at +{} ms, final count {}",
                g_probeChildChanges.load(), g_probeLastChild.load(), g_probePrevChildCount);
            if (g_probeChildChanges.load() == 0) {
                logger::info("[Probe]   -> child count never changed inside the window: assembly was already complete when the transition was reported finished");
            }
        }
    }

    // ---- Renderer reachability (READ-ONLY) ---------------------------------------------------
    // These two answer "can we even touch the pipeline?" without hooking anything: they read the
    // engine's own renderer struct (BSGraphics::Renderer::GetRendererData) and remember pointer
    // identities so a later capture implementation knows when a texture must be re-created.
    // No COM call, no resource creation, no state change -- plain struct reads.
    void ProbeDumpGfxOnce()
    {
        using namespace RE::BSGraphics;
        auto* rd = Renderer::GetRendererDataSingleton();
        if (!rd) {
            logger::warn("[Probe] gfx: GetRendererData() returned null -- renderer not reachable this way");
            return;
        }
        auto& win = rd->renderWindows[0];
        auto& fb = rd->renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
        auto& main = rd->renderTargets[RE::RENDER_TARGET::kMAIN];
        logger::info("[Probe] gfx: device={} context={} window={}x{} borderless={} fullscreen={} interval={}",
            static_cast<const void*>(rd->forwarder), static_cast<const void*>(rd->context),
            rd->newWidth, rd->newHeight, rd->borderlessDisplay ? 1 : 0, rd->fullScreen ? 1 : 0,
            rd->presentInterval);
        logger::info("[Probe] gfx: swapchain={} renderView={} | kFRAMEBUFFER tex={} srv={} | kMAIN tex={} rtv={} srv={}",
            static_cast<const void*>(win.swapChain), static_cast<const void*>(win.renderView),
            static_cast<const void*>(fb.texture), static_cast<const void*>(fb.SRV),
            static_cast<const void*>(main.texture), static_cast<const void*>(main.RTV),
            static_cast<const void*>(main.SRV));
        g_probeGfxMainTex.store(reinterpret_cast<std::uintptr_t>(main.texture));
        g_probeGfxFbTex.store(reinterpret_cast<std::uintptr_t>(fb.texture));
        g_probeGfxSwapChain.store(reinterpret_cast<std::uintptr_t>(win.swapChain));
        const bool reachable = rd->forwarder && rd->context && main.texture != nullptr;
        logger::info("[Probe] gfx verdict: capture path {} (device+context+main RT all present? {})",
            reachable ? "REACHABLE without a Present hook" : "NOT reachable -- needs investigation",
            reachable ? "yes" : "no");
    }

    // Called at each transition so we learn whether the pointers survive cell changes / resizes.
    void ProbeCheckGfxStability()
    {
        using namespace RE::BSGraphics;
        auto* rd = Renderer::GetRendererDataSingleton();
        if (!rd) {
            return;
        }
        auto& win = rd->renderWindows[0];
        const auto mainTex = reinterpret_cast<std::uintptr_t>(rd->renderTargets[RE::RENDER_TARGET::kMAIN].texture);
        const auto fbTex = reinterpret_cast<std::uintptr_t>(rd->renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER].texture);
        const auto sc = reinterpret_cast<std::uintptr_t>(win.swapChain);
        const auto prevMain = g_probeGfxMainTex.exchange(mainTex);
        const auto prevFb = g_probeGfxFbTex.exchange(fbTex);
        const auto prevSc = g_probeGfxSwapChain.exchange(sc);
        if (prevMain != 0 && prevMain != mainTex) {
            logger::warn("[Probe] gfx: kMAIN texture pointer CHANGED (0x{:X} -> 0x{:X}) -- a retained texture must be re-created", prevMain, mainTex);
        }
        if (prevFb != 0 && prevFb != fbTex) {
            logger::warn("[Probe] gfx: kFRAMEBUFFER texture pointer CHANGED (0x{:X} -> 0x{:X})", prevFb, fbTex);
        }
        if (prevSc != 0 && prevSc != sc) {
            logger::warn("[Probe] gfx: swapChain pointer CHANGED (0x{:X} -> 0x{:X})", prevSc, sc);
        }
        logger::info("[Probe] gfx at transition: kMAIN=0x{:X} kFRAMEBUFFER=0x{:X} swapchain=0x{:X} window={}x{}",
            mainTex, fbTex, sc, rd->newWidth, rd->newHeight);
    }

    void LogGraphicsEnvironment()
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const bool enbDir = fs::exists("enbseries", ec);
        const bool d3d11 = fs::exists("d3d11.dll", ec);
        std::vector<std::string> gfx;
        for (const auto& e : fs::directory_iterator("Data/SKSE/Plugins", ec)) {
            if (ec || !e.is_regular_file(ec)) {
                continue;
            }
            auto        name = PathText(e.path().filename());
            std::string low = name;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (low.find("community") != std::string::npos || low.find("dlss") != std::string::npos ||
                low.find("fsr") != std::string::npos || low.find("upscal") != std::string::npos ||
                low.find("reshade") != std::string::npos || low.find("enb") != std::string::npos) {
                gfx.push_back(name);
            }
        }
        std::string joined;
        for (const auto& s : gfx) {
            if (!joined.empty()) {
                joined += ", ";
            }
            joined += s;
        }
        logger::info("[Probe] env: enbseries_dir={} d3d11_in_gamedir={} graphics_plugins=[{}]",
            enbDir ? "yes" : "no", d3d11 ? "yes" : "no", joined.empty() ? "none" : joined.c_str());
    }

    // ---- B: freeze the presented image instead of painting black (EXPERIMENTAL) --------------
    // Idea: the player's last frame is already on screen. If we simply do not call Present for the
    // duration of the transition, the display keeps showing that exact image -- no texture capture,
    // no format/sRGB handling, no shader, and it automatically includes whatever ENB / Community
    // Shaders produced, because we never touch their pixels. When the window ends we resume
    // presenting, and the first frame after that is the finished new cell.
    //
    // This patches the swap chain's COM vtable slot for IDXGISwapChain::Present (index 8). It is the
    // only place in this plugin that touches a COM vtable; it is therefore OFF by default and logs
    // loudly. Failure modes seen in the field are "no effect" or a frozen picture -- not a crash --
    // and EndFreezeFrame() is called on every finish path so presentation can never stay suppressed.
    namespace capprobe { void OnPresent(void* a_swapChain); }
    // [Rules] places, doors and situations with a transition of their own; defined after the travel code.
    namespace rules
    {
        int SourceStyle(const char*& a_why);
        int ArrivalStyle(const char*& a_why);
    }
    // [EaseIn] door deferral with the HUD hidden; defined next to the door hook.
    namespace easein
    {
        void OnIdlePresent();
        void RestoreHud(const char* a_why);
        void HideHudNow();   // [Styles] for the door preview
        void IdleWatch();    // [Safe] the HUD never stays hidden without a transition
        void ShowHudNow();   // [Safe] main thread, at once: a loading screen is about to be shown
    }
    // v1.4.0: the void overlay draws in the Present hook, so it has to be declared before the
    // hook itself. Full definition further down (after the capture probe).
    namespace voidoverlay
    {
        void OnPresent(void* a_swapChain);
        void NotePresent();
        void Arm();
        void Cut();
        void Disarm(const char* a_reason);
        bool Active();
        bool Failed();              // [Safe] the overlay cannot draw on this setup
        // travel effects (private build)
        enum FxKind : int { kFxNone = 0, kFxEyes = 1, kFxInk = 2, kFxMagic = 3, kFxFade = 4, kFxLight = 5, kFxDrowsy = 6, kFxSoul = 7, kFxWipe = 8, kFxDream = 9, kFxDissolve = 10 };
        void SetFxDirection(float a_dir);   // [Rules] +1 = the player moves up, -1 = down (wipe)
        std::uint32_t FxBuildMs();
        bool CutSeen();
        const char* FxName(int a_kind);
        void ArmWakeUp(int a_kind);   // [Styles] starts closed and opens (after sleeping)
        void ArmTravelFx(int a_kind, const char* a_reason);
        bool TravelFxActive();
    }
    // Set while a non-door (travel) load owns the LoadingMenu suppression: lengthens every timeout.
    std::atomic<bool> g_travelFxLoad{ false };
    // [Safe] A load that a transition covers may take as long as it needs: the cover stays up for it. The short door
    // timeouts are only right when nothing covers the load (plain cut); with them, a load longer than a few seconds
    // had its loading screen re-posted behind a hidden HUD and ended on a black screen.
    // [Safe] When the player last used a load door that the door logic then passed on untouched (a door opened
    // with its key, an unresolved destination, sneaking). If a load follows, it is still a door.
    std::atomic<std::int64_t> g_playerDoorPassedAt{ 0 };

    // A locked door the player holds the key to opens and loads in one go.
    inline bool PlayerHasKeyFor(RE::TESObjectREFR* a_door)
    {
        auto* lock = a_door ? a_door->GetLock() : nullptr;
        auto* player = RE::PlayerCharacter::GetSingleton();
        return lock && lock->key && player && player->GetItemCount(lock->key) > 0;
    }

    // [Compat] Other mods wait for the Loading Menu to open and close to know that the player has travelled (DynDOLOD
    // refreshes its dynamic LOD that way). When this mod keeps the menu from opening they never hear about the load,
    // so the two menu events are sent on its behalf: "opened" when the request is swallowed, "closed" at the swap.
    std::atomic<bool> g_announcedOpen{ false };
    std::atomic<bool> g_announcing{ false };   // our own event sinks ignore these

    inline void AnnounceLoad(bool a_opening)
    {
        if (!g_config.announceLoads || g_announcedOpen.exchange(a_opening) == a_opening) {
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        auto* source = ui ? ui->GetEventSource<RE::MenuOpenCloseEvent>() : nullptr;
        if (!source) {
            return;
        }
        RE::MenuOpenCloseEvent event{ RE::LoadingMenu::MENU_NAME, a_opening };
        g_announcing.store(true);
        source->SendEvent(&event);
        g_announcing.store(false);
    }

    inline bool CoveredLoad()
    {
        return g_travelFxLoad.load() || (g_config.seamless && g_config.suppressLoadingMenu && !voidoverlay::Failed());
    }

    namespace presentfreeze
    {
        using PresentFn = long (*)(void*, unsigned int, unsigned int);
        // IDXGISwapChain1::Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS*)
        // -- the parameter order is NOT the same as a leading pointer, get it wrong and the real
        // Present1 receives a pointer where it expects a sync interval.
        using Present1Fn = long (*)(void*, unsigned int, unsigned int, const void*);

        PresentFn         g_origPresent = nullptr;
        Present1Fn        g_origPresent1 = nullptr;
        void*             g_hookedSwapChain = nullptr;
        std::atomic<bool> g_suppressPresent{ false };
        std::atomic<bool> g_holdPresent{ false };     // private build: the overlay asks for presents to be withheld while its picture is static
        std::atomic<std::uint32_t> g_heldPresents{ 0 };
        std::atomic<bool> g_installed{ false };
        std::atomic<bool> g_firstSkipLogged{ false };
        std::atomic<std::uint32_t> g_skippedFrames{ 0 };
        // Every Present / Present1 that reaches us, suppressed or not (v1.4.2). The world probe
        // turns this into a frame rate for the transition window: a healthy 55-60 Hz means the main
        // loop is still spinning freely, a low number means it is blocked inside the load path --
        // which decides whether throttling simulation work could shorten the window at all.
        std::atomic<std::uint64_t> g_frameCount{ 0 };
        std::atomic<std::int64_t>  g_suppressStartAt{ 0 };
        // v1.5.5: at the cut the engine can present 1~3 frames with nothing drawn in them (the new
        // cell is not up yet) -- that is the "black flash". We cannot paint over them (that needs
        // presentation rights) but we CAN decline to present them, which leaves the last good frame
        // on screen. Bounded to a few tens of ms.
        std::atomic<std::int64_t>  g_holdUntil{ 0 };
        std::atomic<int>           g_heldFrames{ 0 };
        ID3D11Texture2D*           g_probeTex = nullptr;
        std::atomic<bool>          g_holdActive{ false };
        std::atomic<std::int64_t>  g_holdStart{ 0 };
        std::atomic<std::uint32_t> g_holdCapMs{ 250 };
        // v1.5.7: trace what the back buffer actually contains across the whole armed window, so the
        // position of the "black" stops being a guess. 50 ms spacing, 8x8 centre patch.
        std::atomic<std::int64_t>  g_traceNextAt{ 0 };
        std::atomic<std::int64_t>  g_traceArmAt{ 0 };
        // v1.6.5: stutter is measurable -- count frames whose inter-present gap exceeds 40 ms
        // during the 3 s after a transition. That is the only honest way to judge
        // iLoadingQueuedPriorityBudgetMs, which never shows up in the transition-total metric.
        std::atomic<std::int64_t>  g_prevPresentAt{ 0 };
        std::atomic<std::int64_t>  g_hitchUntil{ 0 };
        std::atomic<int>           g_hitchCount{ 0 };
        std::atomic<int>           g_hitchFrames{ 0 };
        std::atomic<bool>          g_hitchReported{ true };

        void ArmHitchWindow()
        {
            g_hitchCount.store(0);
            g_hitchFrames.store(0);
            g_hitchReported.store(false);
            g_hitchUntil.store(NowMs() + 3000);
        }

        void HitchTick()
        {
            const auto now = NowMs();
            const auto prev = g_prevPresentAt.exchange(now);
            if (g_hitchUntil.load() == 0) {
                return;
            }
            if (now > g_hitchUntil.load()) {
                if (!g_hitchReported.exchange(true)) {
                    const int frames = g_hitchFrames.load();
                    logger::info("[Perf] 3 s after the transition: {} frames, {} hitches (>40 ms gap)",
                        frames, g_hitchCount.load());
                }
                return;
            }
            g_hitchFrames.fetch_add(1);
            if (prev != 0 && (now - prev) > 40) {
                g_hitchCount.fetch_add(1);
            }
        }

        float SampleBackbufferLuma(void* a_swapChain);   // defined below

        void TraceArm()
        {
            g_traceArmAt.store(NowMs());
            g_traceNextAt.store(NowMs());
        }

        void TraceStop()
        {
            g_traceNextAt.store(0);
            g_traceArmAt.store(0);
        }

        // v1.5.8: the black is NOT at the cut. Measured trace: one frame at suppression
        // (luma 3.8) and then ~500 ms of pure black (luma 1.0) about 900 ms AFTER the cut.
        // So the gate must be brightness-triggered, not event-triggered.
        std::atomic<bool>         g_autoGate{ false };   // v1.5.9: off by default, see note above
        std::atomic<std::int64_t> g_autoUntil{ 0 };   // keep watching this long after activation

        void ArmAutoGate()
        {
            g_autoUntil.store(NowMs() + 4000);
            g_holdActive.store(false);
            g_holdUntil.store(0);
        }

        void TraceTick(void* a_swapChain)
        {
            if (g_traceNextAt.load() == 0) {
                return;
            }
            if (NowMs() - g_traceArmAt.load() > 6000) {
                TraceStop();
                logger::info("[Trace] stopped (6 s window)");
                return;
            }
            // Sample EVERY frame inside the watch window (a 1-frame black would slip through a 50 ms
            // sampler); only write the trace line every 50 ms so the log stays readable.
            const float luma = SampleBackbufferLuma(a_swapChain);
            if (g_autoGate.load() && NowMs() < g_autoUntil.load() && !g_holdActive.load() &&
                luma >= 0.0f && luma < 8.0f) {
                // A black frame is about to be presented -- close the gate right now.
                g_holdStart.store(NowMs());
                g_holdCapMs.store(600);
                g_holdActive.store(true);
                logger::info("[Hold] black frame detected (luma {:.1f}) -- withholding until the picture is back", luma);
            }
            if (NowMs() < g_traceNextAt.load()) {
                return;
            }
            g_traceNextAt.store(NowMs() + 50);
            logger::info("[Trace] t=+{} ms luma={:.1f}", NowMs() - g_traceArmAt.load(), luma);
        }

        void HoldAtCut(std::uint32_t a_ms)
        {
            // a_ms is now a SAFETY CAP; the gate opens by itself when the picture returns.
            g_heldFrames.store(0);
            g_holdCapMs.store(a_ms == 0 ? 250u : a_ms);
            g_holdStart.store(NowMs());
            g_holdActive.store(true);
            g_holdUntil.store(0);
        }

        bool HoldingNow()
        {
            const auto until = g_holdUntil.load();
            return until != 0 && NowMs() < until;
        }

        // ---- v1.5.6: measure the empty-frame run instead of assuming a duration ---------------
        // A fixed 80 ms is wrong on any machine but the author's. Instead we sample a few pixels of
        // the back buffer each frame while the gate is closed and open it as soon as the picture is
        // no longer black. uHoldAtCutMs becomes a SAFETY CAP, not a duration.
        // Average luma of a small centre patch, 0..255. Returns -1 when it cannot be read.
        float SampleBackbufferLuma(void* a_swapChain)
        {
            using namespace RE::BSGraphics;
            auto* rd = Renderer::GetRendererDataSingleton();
            auto* device = rd ? reinterpret_cast<ID3D11Device*>(rd->forwarder) : nullptr;
            auto* ctx = rd ? reinterpret_cast<ID3D11DeviceContext*>(rd->context) : nullptr;
            if (!device || !ctx || !a_swapChain) {
                return -1.0f;
            }
            auto* sc = static_cast<IDXGISwapChain*>(a_swapChain);
            ID3D11Texture2D* back = nullptr;
            if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
                return -1.0f;
            }
            D3D11_TEXTURE2D_DESC desc{};
            back->GetDesc(&desc);
            if (!g_probeTex) {
                D3D11_TEXTURE2D_DESC sd = desc;
                sd.Width = 8;
                sd.Height = 8;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.MiscFlags = 0;
                sd.MipLevels = 1;
                sd.ArraySize = 1;
                if (FAILED(device->CreateTexture2D(&sd, nullptr, &g_probeTex)) || !g_probeTex) {
                    back->Release();
                    return -1.0f;
                }
            }
            const D3D11_BOX box{ desc.Width / 2 - 4, desc.Height / 2 - 4, 0, desc.Width / 2 + 4, desc.Height / 2 + 4, 1 };
            ctx->CopySubresourceRegion(g_probeTex, 0, 0, 0, 0, back, 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            float luma = -1.0f;
            if (SUCCEEDED(ctx->Map(g_probeTex, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
                const auto* px = static_cast<const std::uint8_t*>(mapped.pData);
                std::uint64_t sum = 0;
                const bool tenBit = (desc.Format == 24);
                for (unsigned int y = 0; y < 8; ++y) {
                    const auto* row = px + static_cast<std::size_t>(y) * mapped.RowPitch;
                    for (unsigned int x = 0; x < 8; ++x) {
                        if (tenBit) {
                            std::uint32_t v = 0;
                            std::memcpy(&v, row + x * 4, 4);
                            sum += (((v >> 20) & 0x3FF) + ((v >> 10) & 0x3FF) + (v & 0x3FF)) / 12u;
                        } else {
                            sum += (row[x * 4 + 0] + row[x * 4 + 1] + row[x * 4 + 2]) / 3u;
                        }
                    }
                }
                luma = static_cast<float>(sum) / 64.0f;
                ctx->Unmap(g_probeTex, 0);
            }
            back->Release();
            return luma;
        }

        // true => withhold this frame. Opens the gate by itself as soon as the picture is back.
        bool ShouldWithhold(void* a_swapChain)
        {
            if (g_holdUntil.load() != 0 && NowMs() < g_holdUntil.load()) {
                return true;   // legacy fixed window (unused when the dynamic path is on)
            }
            if (!g_holdActive.load()) {
                return false;
            }
            const auto elapsed = NowMs() - g_holdStart.load();
            const auto cap = static_cast<std::int64_t>(g_holdCapMs.load());
            const float luma = SampleBackbufferLuma(a_swapChain);
            const bool pictureBack = (luma >= 0.0f) && (luma > 6.0f);
            if (pictureBack || elapsed >= cap || luma < 0.0f) {
                g_holdActive.store(false);
                logger::info("[Hold] released after {} ms (luma {:.1f}, cap {} ms)", elapsed, luma, cap);
                return false;   // present this one -- it has a picture
            }
            return true;
        }

        // [Safe] Frames may only be withheld while a working overlay owns the screen, and never for long. Without
        // this, an overlay that fails on some setup would leave the last frame (often black) on screen for good
        // while the game runs on underneath.
        bool HoldWanted()
        {
            static std::atomic<std::int64_t> s_since{ 0 };
            if (!g_holdPresent.load(std::memory_order_relaxed)) {
                s_since.store(0, std::memory_order_relaxed);
                return false;
            }
            const std::int64_t now = NowMs();
            std::int64_t       since = s_since.load(std::memory_order_relaxed);
            if (since == 0) {
                s_since.store(now, std::memory_order_relaxed);
                since = now;
            }
            const char* why = nullptr;
            if (voidoverlay::Failed()) {
                why = "the overlay is not available";
            } else if (!voidoverlay::Active()) {
                why = "no transition is running";
            } else if (now - since > 8000) {
                why = "held for more than 8 seconds";
            }
            if (why) {
                g_holdPresent.store(false);
                s_since.store(0, std::memory_order_relaxed);
                logger::warn("[Safe] frames released: {}", why);
                return false;
            }
            return true;
        }

        long HookedPresent(void* a_swapChain, unsigned int a_sync, unsigned int a_flags)
        {
            g_frameCount.fetch_add(1, std::memory_order_relaxed);
            if (ShouldWithhold(a_swapChain)) {
                if (g_heldFrames.fetch_add(1) == 0) {
                    logger::info("[Hold] cut: withholding Present until the picture is back");
                }
                return 0;   // keep the last good frame displayed
            }
            HitchTick();
            TraceTick(a_swapChain);
            capprobe::OnPresent(a_swapChain);
            voidoverlay::NotePresent();
            // Additive overlay for the dead window. No-op unless a transition armed it, and it
            // runs before the suppress check so the pulse can still land on suppressed frames.
            voidoverlay::OnPresent(a_swapChain);
            if (HoldWanted()) {
                g_heldPresents.fetch_add(1, std::memory_order_relaxed);
                return 0;   // the display keeps the last presented frame; the frame generator sees nothing
            }
            if (g_suppressPresent.load()) {
                g_skippedFrames.fetch_add(1);
                if (!g_firstSkipLogged.exchange(true)) {
                    logger::info("[B] Present suppressed from here (swapchain {})", a_swapChain);
                }
                return 0;   // S_OK: keep the currently displayed image on screen
            }
            return g_origPresent ? g_origPresent(a_swapChain, a_sync, a_flags) : 0;
        }

        // Same body for IDXGISwapChain1::Present1 (slot 22). A flip-model chain can present through
        // Present1 only, in which case the slot-8 hook would never run -- and the overlay with it.
        long HookedPresent1(void* a_swapChain, unsigned int a_sync, unsigned int a_flags, const void* a_params)
        {
            g_frameCount.fetch_add(1, std::memory_order_relaxed);
            if (ShouldWithhold(a_swapChain)) {
                g_heldFrames.fetch_add(1);
                return 0;
            }
            capprobe::OnPresent(a_swapChain);
            voidoverlay::NotePresent();
            voidoverlay::OnPresent(a_swapChain);
            if (HoldWanted()) {
                g_heldPresents.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            if (g_suppressPresent.load()) {
                g_skippedFrames.fetch_add(1);
                if (!g_firstSkipLogged.exchange(true)) {
                    logger::info("[B] Present1 suppressed from here (swapchain {})", a_swapChain);
                }
                return 0;
            }
            return g_origPresent1 ? g_origPresent1(a_swapChain, a_sync, a_flags, a_params) : 0;
        }

        bool InstallOn(void* a_swapChain)
        {
            if (!a_swapChain) {
                return false;
            }
            if (a_swapChain == g_hookedSwapChain) {
                return true;
            }
            auto** vtbl = *reinterpret_cast<void***>(a_swapChain);
            if (!vtbl) {
                return false;
            }
            constexpr std::size_t kPresentIdx = 8;   // IDXGISwapChain::Present
            DWORD oldProtect = 0;
            if (!VirtualProtect(&vtbl[kPresentIdx], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                logger::warn("[B] VirtualProtect failed on the swap chain vtable; freeze-frame disabled");
                return false;
            }
            if (!g_origPresent) {
                g_origPresent = reinterpret_cast<PresentFn>(vtbl[kPresentIdx]);
            }
            vtbl[kPresentIdx] = reinterpret_cast<void*>(&HookedPresent);
            DWORD ignored = 0;
            VirtualProtect(&vtbl[kPresentIdx], sizeof(void*), oldProtect, &ignored);

            // Slot 22 (IDXGISwapChain1::Present1) only exists on a 1+ chain. Writing past the end
            // of a shorter vtable would corrupt the neighbouring .rdata, so ask first.
            IDXGISwapChain1* sc1 = nullptr;
            if (SUCCEEDED((reinterpret_cast<IUnknown*>(a_swapChain))->QueryInterface(
                    __uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1))) &&
                sc1) {
                auto** vtbl1 = *reinterpret_cast<void***>(sc1);
                constexpr std::size_t kPresent1Idx = 22;   // IDXGISwapChain1::Present1
                DWORD prot1 = 0;
                if (vtbl1 && VirtualProtect(&vtbl1[kPresent1Idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &prot1)) {
                    if (!g_origPresent1) {
                        g_origPresent1 = reinterpret_cast<Present1Fn>(vtbl1[kPresent1Idx]);
                    }
                    vtbl1[kPresent1Idx] = reinterpret_cast<void*>(&HookedPresent1);
                    DWORD ign1 = 0;
                    VirtualProtect(&vtbl1[kPresent1Idx], sizeof(void*), prot1, &ign1);
                    logger::info("[B] Present1 hook installed (slot 22, original {})",
                        reinterpret_cast<const void*>(g_origPresent1));
                }
                sc1->Release();
            }

            g_hookedSwapChain = a_swapChain;
            g_installed.store(true);
            logger::info("[B] Present hook installed on swapchain {} (original {})",
                a_swapChain, reinterpret_cast<const void*>(g_origPresent));
            return true;
        }

        void* CurrentSwapChain()
        {
            using namespace RE::BSGraphics;
            auto* rd = Renderer::GetRendererDataSingleton();
            return rd ? static_cast<void*>(rd->renderWindows[0].swapChain) : nullptr;
        }
    }

    // ---- Step 2 probe: present-hook ORDER + optional frame dump (v1.3.1, research only) --------
    // Why: on this setup Community Shaders also hooks Present. Whoever writes the vtable slot LAST
    // sits in the slot and is called FIRST. That decides which image we are looking at:
    //   our hook installed after CS  -> we are called first -> we see the frame BEFORE CS grading
    //   our hook installed before CS -> CS wraps us         -> we are called after CS graded it
    // The order check below needs no D3D call at all: it only compares pointers and module ranges.
    // The optional BMP dump then proves visually which image we actually get.
    namespace capprobe
    {
        using PresentFn  = long (*)(void*, unsigned int, unsigned int);
        using GetBufferFn = long (*)(void*, unsigned int, const GUID*, void**);
        using CreateTexFn = long (*)(void*, const void*, const void*, void**);
        // Map(ID3D11Resource*, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE*)
        // -- FIVE arguments after `this`. The old typedef dropped MapFlags, so pMappedResource landed in
        // the MapFlags slot and the real out-pointer was read from uninitialised stack: Map() then
        // failed with E_INVALIDARG on every single call (217 x "Map failed" in the field log).
        using MapFn       = long (*)(void*, void*, unsigned int, unsigned int, unsigned int, void*);
        using UnmapFn     = void (*)(void*, void*, unsigned int);
        using CopyResFn   = void (*)(void*, void*, void*);
        using GetDescFn   = void (*)(void*, void*);
        using ReleaseFn   = unsigned long (*)(void*);

        inline constexpr GUID kIIDTexture2D{ 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

        struct TexDesc
        {
            unsigned int Width, Height, MipLevels, ArraySize, Format;
            unsigned int SampleCount, SampleQuality;
            unsigned int Usage, BindFlags, CPUAccessFlags, MiscFlags;
        };
        struct Mapped
        {
            void*         pData;
            unsigned int  RowPitch, DepthPitch;
        };

        template <class F>
        F Slot(void* a_obj, std::size_t a_idx)
        {
            return reinterpret_cast<F>((*reinterpret_cast<void***>(a_obj))[a_idx]);
        }

        std::atomic<std::uint32_t> g_presents{ 0 };
        // v1.4.3: the field backbuffer is DXGI_FORMAT_R10G10B10A2_UNORM (24), so the old 8-bit-only
        // guard made every capture bail out -- 271 warnings, zero frames. Support it, and stop
        // trying (loudly, once) on a format we still cannot decode.
        std::atomic<bool>          g_captureUnsupported{ false };
        std::atomic<bool>          g_mapFailLogged{ false };
        std::atomic<std::uint32_t> g_captured{ 0 };
        // v1.4.6: a global serial (file names never repeat across transitions), a shared "last capture"
        // stamp so samples are spread over the whole window instead of twelve frames in 0.28 s, and a
        // session-wide ceiling (each BMP used to be 24.9 MB at 4K).
        std::atomic<std::uint32_t> g_serial{ 0 };
        std::atomic<std::int64_t>  g_lastCapAt{ 0 };
        std::atomic<bool>          g_orderChecked{ false };
        void*                      g_slotAddr = nullptr;
        std::atomic<bool>          g_downstream{ false };

        std::string ModuleOf(const void* a_addr)
        {
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(a_addr), &mod) &&
                mod) {
                char buf[MAX_PATH]{};
                if (GetModuleFileNameA(mod, buf, MAX_PATH)) {
                    return std::filesystem::path(buf).filename().string();
                }
            }
            return "?";
        }

        // Called once, on the very first Present that reaches us.
        void CheckOrder(void* a_swapChain)
        {
            if (g_orderChecked.exchange(true)) {
                return;
            }
            g_slotAddr = reinterpret_cast<void*>(a_swapChain);
            auto** vtbl = *reinterpret_cast<void***>(a_swapChain);
            const auto slotNow = vtbl ? vtbl[8] : nullptr;
            logger::info("[Cap] present hook installed: our slot fn=0x{:X} in {}",
                reinterpret_cast<std::uintptr_t>(slotNow),
                slotNow ? ModuleOf(slotNow) : "?");
            if (presentfreeze::g_origPresent) {
                const auto orig = reinterpret_cast<const void*>(presentfreeze::g_origPresent);
                logger::info("[Cap] chain below us starts at 0x{:X} in {}",
                    reinterpret_cast<std::uintptr_t>(orig), ModuleOf(orig));
            }
        }

        // Re-check on every Present: if the slot no longer holds our function, someone hooked AFTER
        // us and now sits on top, which means we are called after their work -> downstream.
        void RecheckOrder(void* a_swapChain)
        {
            if (!g_slotAddr || !a_swapChain) {
                return;
            }
            auto** vtbl = *reinterpret_cast<void***>(a_swapChain);
            if (!vtbl) {
                return;
            }
            const auto slotNow = vtbl[8];
            const auto ourFn = reinterpret_cast<void*>(&presentfreeze::HookedPresent);
            if (slotNow != ourFn && !g_downstream.exchange(true)) {
                logger::info("[Cap] another hooker installed on top of us: slot now 0x{:X} in {}",
                    reinterpret_cast<std::uintptr_t>(slotNow),
                    slotNow ? ModuleOf(slotNow) : "?");
                logger::info("[Cap] => we are called AFTER that module processed the frame (downstream: we see the graded image)");
            }
        }

        // a_step > 1 writes a nearest-neighbour downscaled BMP. A 3840x2160 BMP is 24.9 MB, which
        // makes a handful of captures fill a gigabyte; the fingerprint is always taken at full
        // resolution, the BMP only exists to be looked at.
        bool WriteBmp(const std::filesystem::path& a_path, const std::uint8_t* a_pixels,
            unsigned int a_width, unsigned int a_height, unsigned int a_rowPitch, bool a_bgra,
            unsigned int a_step = 1)
        {
            const unsigned int step = (a_step > 0) ? a_step : 1u;
            const unsigned int ow = (a_width / step) ? (a_width / step) : 1u;
            const unsigned int oh = (a_height / step) ? (a_height / step) : 1u;
            const unsigned int rowBytes = ow * 3u;
            const unsigned int pad = (4u - (rowBytes % 4u)) % 4u;
            const unsigned int dataSize = (rowBytes + pad) * oh;
            std::ofstream out(a_path, std::ios::binary);
            if (!out) {
                return false;
            }
            std::uint8_t header[54]{};
            header[0] = 'B'; header[1] = 'M';
            const std::uint32_t fileSize = 54u + dataSize;
            std::memcpy(header + 2, &fileSize, 4);
            const std::uint32_t offset = 54u;
            std::memcpy(header + 10, &offset, 4);
            const std::uint32_t infoSize = 40u;
            std::memcpy(header + 14, &infoSize, 4);
            std::int32_t w = static_cast<std::int32_t>(ow);
            std::int32_t h = static_cast<std::int32_t>(oh);
            std::memcpy(header + 18, &w, 4);
            std::memcpy(header + 22, &h, 4);
            const std::uint16_t planes = 1, bpp = 24;
            std::memcpy(header + 26, &planes, 2);
            std::memcpy(header + 28, &bpp, 2);
            std::memcpy(header + 34, &dataSize, 4);
            out.write(reinterpret_cast<const char*>(header), 54);
            std::vector<std::uint8_t> row(rowBytes + pad, 0);
            for (std::int32_t y = h - 1; y >= 0; --y) {   // BMP stores bottom-up
                const std::uint8_t* src = a_pixels + static_cast<std::size_t>(y) * step * a_rowPitch;
                for (unsigned int x = 0; x < ow; ++x) {
                    const unsigned int sx = x * step;
                    const std::uint8_t b0 = src[sx * 4 + 0];
                    const std::uint8_t b1 = src[sx * 4 + 1];
                    const std::uint8_t b2 = src[sx * 4 + 2];
                    // BGRA source -> BMP BGR wants b,g,r ; RGBA source needs a swap
                    row[x * 3 + 0] = a_bgra ? b0 : b2;
                    row[x * 3 + 1] = b1;
                    row[x * 3 + 2] = a_bgra ? b2 : b0;
                }
                out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(rowBytes + pad));
            }
            logger::info("[Cap] wrote {} ({}x{}, rowPitch {})", PathText(a_path), a_width, a_height, a_rowPitch);
            return true;
        }

        // ---- v1.4.1: frame fingerprint ---------------------------------------------------------
        // The question this answers: "is the frame we see at our hook the same one that ends up on
        // screen, and does it change at all while the world is not drawing?"
        //   - identical hash on consecutive presents  => the presented image really is frozen
        //   - different hash                          => something is still drawing
        //   - the `chain=` field names whoever currently sits in vtable slot 8: if that is another
        //     module, it hooked AFTER us, so we are called AFTER it (downstream: we see its output,
        //     i.e. the graded frame). If it is us, we are called first (upstream: pre-grading).
        struct Fingerprint
        {
            double        mean = 0.0;
            std::uint32_t hist[16]{};
            std::uint32_t hash = 0;
        };

        // Samples every 4th pixel in both axes (129 k samples at 1080p): cheap enough to run inside
        // the Present hook, dense enough that a real content change cannot hash the same.
        Fingerprint ComputeFingerprint(const std::uint8_t* a_pixels, unsigned int a_width,
            unsigned int a_height, unsigned int a_rowPitch, bool a_bgra)
        {
            Fingerprint fp{};
            if (!a_pixels || !a_width || !a_height) {
                return fp;
            }
            constexpr unsigned int kGridX = 32;
            constexpr unsigned int kGridY = 18;
            std::uint64_t blockSum[kGridY][kGridX]{};
            std::uint32_t blockCount[kGridY][kGridX]{};
            std::uint64_t total = 0;
            std::uint64_t samples = 0;
            for (unsigned int y = 0; y < a_height; y += 4) {
                const auto* row = a_pixels + static_cast<std::size_t>(y) * a_rowPitch;
                const unsigned int gy = (y * kGridY) / a_height;
                for (unsigned int x = 0; x < a_width; x += 4) {
                    const std::uint8_t c0 = row[x * 4 + 0];
                    const std::uint8_t c1 = row[x * 4 + 1];
                    const std::uint8_t c2 = row[x * 4 + 2];
                    // BGRA source: (B,G,R); RGBA source: (R,G,B). Integer luma, 0..255.
                    const std::uint32_t l = a_bgra
                        ? ((29u * c0 + 150u * c1 + 77u * c2) >> 8)
                        : ((77u * c0 + 150u * c1 + 29u * c2) >> 8);
                    total += l;
                    ++samples;
                    ++fp.hist[(l >> 4) & 0xFu];
                    if (gy < kGridY) {
                        const unsigned int gx = (x * kGridX) / a_width;
                        if (gx < kGridX) {
                            blockSum[gy][gx] += l;
                            ++blockCount[gy][gx];
                        }
                    }
                }
            }
            if (!samples) {
                return fp;
            }
            fp.mean = static_cast<double>(total) / static_cast<double>(samples);
            std::uint32_t h = 2166136261u;
            for (unsigned int gy = 0; gy < kGridY; ++gy) {
                for (unsigned int gx = 0; gx < kGridX; ++gx) {
                    const auto q = blockCount[gy][gx]
                        ? static_cast<std::uint32_t>(blockSum[gy][gx] / blockCount[gy][gx])
                        : 0u;
                    h = (h ^ q) * 16777619u;
                }
            }
            fp.hash = h;
            return fp;
        }

        std::string HistToString(const std::uint32_t (&a_hist)[16])
        {
            std::uint32_t total = 0;
            for (auto v : a_hist) {
                total += v;
            }
            std::string s;
            for (int i = 0; i < 16; ++i) {
                if (!s.empty()) {
                    s += ',';
                }
                s += std::to_string(total ? static_cast<std::uint32_t>((100.0 * a_hist[i]) / total) : 0u);
            }
            return s;
        }

        std::string ChainOwner(void* a_swapChain)
        {
            auto** vtbl = a_swapChain ? *reinterpret_cast<void***>(a_swapChain) : nullptr;
            if (!vtbl) {
                return "?";
            }
            const auto fn = vtbl[8];
            if (fn == reinterpret_cast<void*>(&presentfreeze::HookedPresent)) {
                return "us=first-in-chain";
            }
            return ModuleOf(fn) + "=called-after-us";
        }

        // ---- v1.4.1: capture a burst inside the transition window -------------------------------
        // The old gate was "every 120th present", i.e. roughly one frame per 2 s of ordinary play --
        // which almost never lands inside the ~2 s dead window. Arming a burst makes the next N
        // presents capture back to back, so the frames actually come from the window.
        std::atomic<std::uint32_t> g_windowBurst{ 0 };
        std::atomic<std::int64_t>  g_windowArmedAt{ 0 };

        void ArmWindowCapture(std::uint32_t a_frames)
        {
            if (!g_config.captureFrame) {
                return;
            }
            if (g_serial.load() >= g_config.captureTotalMax) {
                return;
            }
            g_captured.store(0);          // fresh budget for THIS transition, not for the session
            g_windowArmedAt.store(NowMs());
            g_windowBurst.store(a_frames);
            logger::info("[Cap] window burst armed: up to {} frames, one every {} ms (session {} / {})",
                a_frames, g_config.captureEveryMs, g_serial.load(), g_config.captureTotalMax);
        }

        bool CaptureOne(void* a_swapChain, void* a_device, void* a_context, const char* a_tag)
        {
            if (!a_swapChain || !a_device || !a_context) {
                return false;
            }
            auto getBuffer = Slot<GetBufferFn>(a_swapChain, 9);
            auto getDesc = Slot<GetDescFn>(a_device, 5);       // filled below via texture, see note
            (void)getDesc;
            void* tex = nullptr;
            if (getBuffer(a_swapChain, 0, &kIIDTexture2D, &tex) != 0 || !tex) {
                logger::warn("[Cap] GetBuffer failed");
                return false;
            }
            TexDesc desc{};
            Slot<GetDescFn>(tex, 10)(tex, &desc);
            // 87 = B8G8R8A8_UNORM, 28 = R8G8B8A8_UNORM, 24 = R10G10B10A2_UNORM (what this setup runs).
            const bool bgra  = (desc.Format == 87);
            const bool rgba  = (desc.Format == 28);
            const bool rgb10 = (desc.Format == 24);
            if (!bgra && !rgba && !rgb10) {
                if (!g_captureUnsupported.exchange(true)) {
                    logger::warn("[Cap] backbuffer format {} is not decodable (28/87/24 are); capture disabled for this session",
                        desc.Format);
                }
                Slot<ReleaseFn>(tex, 2)(tex);
                return false;
            }
            TexDesc staged = desc;
            staged.MipLevels = 1;
            staged.ArraySize = 1;
            staged.Usage = 3;            // D3D11_USAGE_STAGING
            staged.BindFlags = 0;
            staged.CPUAccessFlags = 0x20000;   // D3D11_CPU_ACCESS_READ
            staged.MiscFlags = 0;
            void* staging = nullptr;
            if (Slot<CreateTexFn>(a_device, 5)(a_device, &staged, nullptr, &staging) != 0 || !staging) {
                logger::warn("[Cap] staging texture creation failed");
                Slot<ReleaseFn>(tex, 2)(tex);
                return false;
            }
            Slot<CopyResFn>(a_context, 47)(a_context, staging, tex);
            Mapped mapped{};
            constexpr unsigned int kMapRead = 1u;   // D3D11_MAP_READ -- the staging texture is CPU_ACCESS_READ
            if (Slot<MapFn>(a_context, 14)(a_context, staging, 0, kMapRead, 0, &mapped) != 0 || !mapped.pData) {
                if (!g_mapFailLogged.exchange(true)) {
                    logger::warn("[Cap] Map failed (logged once; further failures are silent)");
                }
                Slot<ReleaseFn>(staging, 2)(staging);
                Slot<ReleaseFn>(tex, 2)(tex);
                return false;
            }
            // 10-bit path: expand to 8-bit BGRA once, then feed both the fingerprint and the BMP from
            // that buffer, so there is exactly one place that knows about the bit layout.
            std::vector<std::uint8_t> expanded;
            const std::uint8_t* pixels = static_cast<const std::uint8_t*>(mapped.pData);
            unsigned int       rowPitch = mapped.RowPitch;
            if (rgb10) {
                expanded.resize(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
                for (unsigned int y = 0; y < desc.Height; ++y) {
                    const auto* srcRow = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
                    auto*       dstRow = expanded.data() + static_cast<std::size_t>(y) * desc.Width * 4;
                    for (unsigned int x = 0; x < desc.Width; ++x) {
                        std::uint32_t v = 0;
                        std::memcpy(&v, srcRow + static_cast<std::size_t>(x) * 4, 4);
                        dstRow[x * 4 + 0] = static_cast<std::uint8_t>(((v >> 20) & 0x3FF) * 255 / 1023);   // B
                        dstRow[x * 4 + 1] = static_cast<std::uint8_t>(((v >> 10) & 0x3FF) * 255 / 1023);   // G
                        dstRow[x * 4 + 2] = static_cast<std::uint8_t>(((v >> 0) & 0x3FF) * 255 / 1023);    // R
                        dstRow[x * 4 + 3] = 255;
                    }
                }
                pixels = expanded.data();
                rowPitch = desc.Width * 4;
            }
            const bool pixBgra = !rgba;   // 87 and the expanded 10-bit buffer are BGRA, 28 is RGBA
            const auto fp = ComputeFingerprint(pixels, desc.Width, desc.Height, rowPitch, pixBgra);
            const auto dir = std::filesystem::path("LoadingFlow-capture");
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const auto n = g_serial.fetch_add(1) + 1;
            const auto slot = g_captured.fetch_add(1) + 1;
            const auto path = dir / std::format("cap-{:02}-{}.bmp", n, a_tag);
            // Keep the BMP browsable: cap its long edge at ~1280 px, the fingerprint stays full-res.
            const unsigned int bmpStep = (desc.Width > 1280u) ? (desc.Width / 1280u) : 1u;
            WriteBmp(path, pixels, desc.Width, desc.Height, rowPitch, pixBgra, bmpStep);
            const auto armedAt = g_windowArmedAt.load();
            const auto relMs = armedAt ? (NowMs() - armedAt) : -1;
            std::string line = std::format(
                "cap-{:02} (win#{}) tag={} {}x{} fmt={} mean={:.2f} hash=0x{:08X} chain={} hist={} t={}ms -> {}",
                n, slot, a_tag, desc.Width, desc.Height, desc.Format, fp.mean, fp.hash,
                ChainOwner(a_swapChain), HistToString(fp.hist),
                relMs >= 0 ? std::to_string(relMs) : std::string("n/a"),
                PathText(std::filesystem::absolute(path, ec)));
            logger::info("[Cap] {}", line);
            Slot<UnmapFn>(a_context, 15)(a_context, staging, 0);
            Slot<ReleaseFn>(staging, 2)(staging);
            Slot<ReleaseFn>(tex, 2)(tex);
            return true;
        }

        // Called from the Present hook, before we decide to skip.
        void OnPresent(void* a_swapChain)
        {
            if (!g_config.capProbe && !g_config.captureFrame) {
                return;
            }
            CheckOrder(a_swapChain);
            RecheckOrder(a_swapChain);
            if (!g_config.captureFrame) {
                return;
            }
            const auto n = g_presents.fetch_add(1) + 1;
            if (g_captured.load() >= g_config.captureFrames || g_captureUnsupported.load()) {
                return;
            }
            // v1.4.1: a transition-window burst captures consecutive presents, so the frames really
            // come from the dead window instead of from ordinary play.
            const bool inWindow = (g_windowBurst.load() > 0);
            if (inWindow) {
                const auto lastCap = g_lastCapAt.load();
                const auto nowCap = NowMs();
                if (lastCap != 0 && (nowCap - lastCap) < static_cast<std::int64_t>(g_config.captureEveryMs)) {
                    return;   // one sample per captureEveryMs, so the frames span the whole window
                }
                g_windowBurst.fetch_sub(1);
                g_lastCapAt.store(nowCap);
            } else if (!g_config.captureLiveFrames || (n % 120) != 0) {
                // v1.4.5: idle frames are opt-in; otherwise they eat the budget the window burst needs.
                return;
            }
            if (g_serial.load() >= g_config.captureTotalMax) {
                return;
            }
            using namespace RE::BSGraphics;
            auto* rd = Renderer::GetRendererDataSingleton();
            if (!rd) {
                return;
            }
            const char* tag = inWindow
                ? "win"
                : (presentfreeze::g_suppressPresent.load() ? "frozen" : "live");
            CaptureOne(a_swapChain, static_cast<void*>(rd->forwarder), static_cast<void*>(rd->context), tag);
        }
    }

    // ---- Void overlay (v1.4.0, EXPERIMENTAL): fill the dead window with motion ----------------
    // The measured timeline of one real transition is: press E, hand touches the door, then ~124
    // frames (~2.07 s) in which the old cell is detaching and nothing new is drawn -- the screen
    // is a single frozen picture -- then ~21 frames (~350 ms) of scene spin, then the new cell.
    //
    // The 2 s are not black, they are DEAD, and that is what reads as "stiff". Every route that
    // darkened the picture has been rejected (it drags the mod back towards the vanilla loading
    // screen, which is mostly plain black), so this one only ADDS light: a slow breathing lift, a
    // field of soft drifting motes, and one pulse at the cut that hands over to the engine's spin.
    //
    // Why Present: while the world is not drawing there is nowhere else to draw. The swap chain is
    // the only surface guaranteed to be touched, it still runs at the game's frame rate, and the
    // frame we add to already contains whatever ENB / Community Shaders produced -- we never touch
    // their pixels. Blend state is (ONE, ONE): additive, so nothing can be darkened, ever.
    //
    // Failure policy: any D3D error disables the overlay for the rest of the session and says so
    // in the log. It can never take presentation down with it -- the game's own Present is called
    // unchanged either way, and the state block we touch is saved and restored around the draw.
    // =========================================================================
    //  Frame-generation pause (private build)
    //  FidelityFX SDK 2.x ffx_api: Community/Bottled Shaders loads amd_fidelityfx_framegeneration_dx12.dll
    //  and calls its exported ffxConfigure every frame (the frameID must advance by one per frame) with
    //  an ffxConfigureDescFrameGeneration, and ffxDispatch with an ffxDispatchDescFrameGeneration from
    //  the generation callback. Both exports are detoured (MinHook); while paused the configure call
    //  goes through with frameGenerationEnabled = false, and the first dispatches after unpausing carry
    //  reset = true so no stale history is interpolated. Struct layouts verified identical in SDK
    //  2.0.0 .. 2.4.0 headers (the shipped DLL reports 4.0.1).
    // =========================================================================
    namespace fgpause
    {
        struct ApiHeader { std::uint64_t type; ApiHeader* pNext; };
        constexpr std::uint64_t kConfigureFG = 0x00020002u;   // FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION
        constexpr std::uint64_t kDispatchFG = 0x00020003u;    // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION
        struct ApiResourceDescription { std::uint32_t type, format, width, height, depth, mipCount, flags, usage; };
        struct ApiResource { void* resource; ApiResourceDescription description; std::uint32_t state; };
        struct Rect2D { std::int32_t left, top, width, height; };
        struct ConfigureDescFG
        {
            ApiHeader     header;
            void*         swapChain;
            void*         presentCallback;
            void*         presentCallbackUserContext;
            void*         frameGenerationCallback;
            void*         frameGenerationCallbackUserContext;
            bool          frameGenerationEnabled;
            bool          allowAsyncWorkloads;
            ApiResource   HUDLessColor;
            std::uint32_t flags;
            bool          onlyPresentGenerated;
            Rect2D        generationRect;
            std::uint64_t frameID;
        };
        struct DispatchDescFG
        {
            ApiHeader     header;
            void*         commandList;
            ApiResource   presentColor;
            ApiResource   outputs[4];
            std::uint32_t numGeneratedFrames;
            bool          reset;
            std::uint32_t backbufferTransferFunction;
            float         minMaxLuminance[2];
            Rect2D        generationRect;
            std::uint64_t frameID;
        };
        using Fn = std::uint32_t (*)(void*, ApiHeader*);

        Fn                         g_origConfigure = nullptr;
        Fn                         g_origDispatch = nullptr;
        std::atomic<bool>          g_installed{ false };
        std::atomic<bool>          g_gaveUp{ false };
        std::atomic<std::int64_t>  g_nextTryAt{ 0 };
        std::atomic<bool>          g_paused{ false };
        std::atomic<int>           g_resetLeft{ 0 };
        std::atomic<std::uint64_t> g_configureCalls{ 0 };
        std::atomic<std::uint64_t> g_forcedOff{ 0 };
        std::atomic<std::uint64_t> g_dispatchCalls{ 0 };
        std::atomic<std::uint64_t> g_dispatchZeroed{ 0 };
        std::atomic<std::uint64_t> g_configEnabledSeen{ 0 };
        std::atomic<std::uint64_t> g_resetsSent{ 0 };
        std::atomic<bool>          g_lastEnabled{ false };
        std::atomic<bool>          g_seenLogged{ false };

        std::uint32_t HookConfigure(void* a_ctx, ApiHeader* a_desc)
        {
            ConfigureDescFG* d = nullptr;
            for (auto* h = a_desc; h; h = h->pNext) {
                if (h->type == kConfigureFG) {
                    d = reinterpret_cast<ConfigureDescFG*>(h);
                    break;
                }
            }
            bool restore = false;
            bool prev = false;
            if (d) {
                g_configureCalls.fetch_add(1, std::memory_order_relaxed);
                prev = d->frameGenerationEnabled;
                g_lastEnabled.store(prev, std::memory_order_relaxed);
                if (prev) {
                    g_configEnabledSeen.fetch_add(1, std::memory_order_relaxed);
                }
                if (!g_seenLogged.exchange(true)) {
                    logger::info("[FGPause] first frame-generation configure seen (enabled={}, async={}, hudless={})",
                        prev, d->allowAsyncWorkloads, d->HUDLessColor.resource != nullptr);
                }
                if (prev && g_paused.load(std::memory_order_relaxed)) {
                    d->frameGenerationEnabled = false;
                    restore = true;
                    g_forcedOff.fetch_add(1, std::memory_order_relaxed);
                }
            }
            const auto rc = g_origConfigure ? g_origConfigure(a_ctx, a_desc) : 0u;
            if (restore) {
                d->frameGenerationEnabled = prev;
            }
            return rc;
        }

        std::uint32_t HookDispatch(void* a_ctx, ApiHeader* a_desc)
        {
            for (auto* h = a_desc; h; h = h->pNext) {
                if (h->type == kDispatchFG) {
                    auto* d = reinterpret_cast<DispatchDescFG*>(h);
                    g_dispatchCalls.fetch_add(1, std::memory_order_relaxed);
                    if (g_paused.load(std::memory_order_relaxed)) {
                        d->numGeneratedFrames = 0;   // belt and braces: no interpolated frame this dispatch whatever the configure state
                        d->reset = true;
                        g_dispatchZeroed.fetch_add(1, std::memory_order_relaxed);
                    }
                    int left = g_resetLeft.load(std::memory_order_relaxed);
                    while (left > 0 && !g_resetLeft.compare_exchange_weak(left, left - 1)) {}
                    if (left > 0) {
                        d->reset = true;
                        g_resetsSent.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }
            }
            return g_origDispatch ? g_origDispatch(a_ctx, a_desc) : 0u;
        }

        // True once the hooks are in. Safe to call repeatedly; the DLL may only appear once frame
        // generation is actually enabled in the shader menu.
        bool Install()
        {
            if (g_installed.load() || g_gaveUp.load()) {
                return g_installed.load();
            }
            const auto now = NowMs();
            if (now < g_nextTryAt.load()) {
                return false;
            }
            g_nextTryAt.store(now + 2000);
            HMODULE mod = ::GetModuleHandleW(L"amd_fidelityfx_framegeneration_dx12.dll");
            if (!mod) {
                return false;
            }
            auto* cfg = reinterpret_cast<void*>(::GetProcAddress(mod, "ffxConfigure"));
            auto* dsp = reinterpret_cast<void*>(::GetProcAddress(mod, "ffxDispatch"));
            if (!cfg) {
                logger::warn("[FGPause] frame generation DLL loaded but ffxConfigure not exported; pause unavailable");
                g_gaveUp.store(true);
                return false;
            }
            const auto init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
                logger::warn("[FGPause] MH_Initialize failed: {}", MH_StatusToString(init));
                g_gaveUp.store(true);
                return false;
            }
            auto st = MH_CreateHook(cfg, reinterpret_cast<void*>(&HookConfigure), reinterpret_cast<void**>(&g_origConfigure));
            if (st != MH_OK) {
                logger::warn("[FGPause] hooking ffxConfigure failed: {}", MH_StatusToString(st));
                g_gaveUp.store(true);
                return false;
            }
            if (dsp) {
                st = MH_CreateHook(dsp, reinterpret_cast<void*>(&HookDispatch), reinterpret_cast<void**>(&g_origDispatch));
                if (st != MH_OK) {
                    logger::warn("[FGPause] hooking ffxDispatch failed ({}); resets after unpausing will not be sent", MH_StatusToString(st));
                }
            }
            st = MH_EnableHook(MH_ALL_HOOKS);
            if (st != MH_OK) {
                logger::warn("[FGPause] MH_EnableHook failed: {}", MH_StatusToString(st));
                g_gaveUp.store(true);
                return false;
            }
            g_installed.store(true);
            logger::info("[FGPause] FidelityFX frame generation hooks installed (ffxConfigure 0x{:X}, ffxDispatch 0x{:X})",
                reinterpret_cast<std::uintptr_t>(cfg), reinterpret_cast<std::uintptr_t>(dsp));
            return true;
        }

        // ---- [DLSSG] NVIDIA DLSS frame generation (Streamline, as used by Community Shaders). EXPERIMENTAL.
        // Streamline has one call that switches frame generation, slDLSSGSetOptions(viewport, options). It is found
        // through the interposer and detoured: while a transition is on screen every call is passed on with
        // mode = off, and the last options the game sent are replayed with mode = off when the pause starts and with
        // their own mode when it ends. Layouts follow the public Streamline headers (MIT): BaseStructure is 32 bytes,
        // DLSSGOptions::mode sits at 32, flags at 40, struct version 5 is 120 bytes.
        namespace dlssg
        {
            using SetOptionsFn = std::int32_t (*)(const void* a_viewport, const void* a_options);
            using GetFeatureFn = std::int32_t (*)(std::uint32_t a_feature, const char* a_name, void*& a_function);
            constexpr std::size_t   kVersionAt = 24, kModeAt = 32, kFlagsAt = 40, kOptionsSize = 120, kViewportSize = 40;
            constexpr std::uint32_t kRetainResourcesWhenOff = 1u << 3;

            SetOptionsFn              g_orig = nullptr;
            std::atomic<bool>         g_installed{ false };
            std::atomic<bool>         g_gaveUp{ false };
            std::atomic<std::int64_t> g_nextTryAt{ 0 };
            std::atomic<bool>         g_paused{ false };
            std::mutex                g_lock;
            std::uint8_t              g_lastViewport[kViewportSize]{};
            std::uint8_t              g_lastOptions[kOptionsSize]{};
            bool                      g_haveLast = false;
            std::atomic<std::uint32_t> g_forcedOff{ 0 };

            bool KnownLayout(const void* a_options)
            {
                std::uint64_t version = 0;
                std::memcpy(&version, static_cast<const std::uint8_t*>(a_options) + kVersionAt, sizeof(version));
                return version >= 1 && version <= 5;
            }

            std::int32_t Hook(const void* a_viewport, const void* a_options)
            {
                if (!a_viewport || !a_options || !KnownLayout(a_options)) {
                    return g_orig(a_viewport, a_options);   // a layout this code does not know: leave it alone
                }
                std::uint8_t copy[kOptionsSize];
                std::memcpy(copy, a_options, kOptionsSize);
                {
                    std::scoped_lock guard(g_lock);
                    std::memcpy(g_lastViewport, a_viewport, kViewportSize);
                    std::memcpy(g_lastOptions, copy, kOptionsSize);
                    g_haveLast = true;
                }
                if (g_paused.load(std::memory_order_relaxed)) {
                    std::uint32_t off = 0, flags = 0;
                    std::memcpy(&flags, copy + kFlagsAt, 4);
                    flags |= kRetainResourcesWhenOff;
                    std::memcpy(copy + kModeAt, &off, 4);
                    std::memcpy(copy + kFlagsAt, &flags, 4);
                    g_forcedOff.fetch_add(1, std::memory_order_relaxed);
                    return g_orig(a_viewport, copy);
                }
                return g_orig(a_viewport, a_options);
            }

            void Install()
            {
                if (g_installed.load() || g_gaveUp.load()) {
                    return;
                }
                const auto now = NowMs();
                if (now < g_nextTryAt.load()) {
                    return;
                }
                g_nextTryAt.store(now + 5000);
                HMODULE interposer = ::GetModuleHandleW(L"sl.interposer.dll");
                if (!interposer || !::GetModuleHandleW(L"sl.dlss_g.dll")) {
                    return;   // DLSS frame generation is not loaded (yet)
                }
                auto get = reinterpret_cast<GetFeatureFn>(::GetProcAddress(interposer, "slGetFeatureFunction"));
                void* fn = nullptr;
                constexpr std::uint32_t kFeatureDLSS_G = 1000;
                if (!get || get(kFeatureDLSS_G, "slDLSSGSetOptions", fn) != 0 || !fn) {
                    return;   // not available yet; try again later
                }
                const auto init = MH_Initialize();
                if ((init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) ||
                    MH_CreateHook(fn, reinterpret_cast<void*>(&Hook), reinterpret_cast<void**>(&g_orig)) != MH_OK || MH_EnableHook(fn) != MH_OK) {
                    logger::warn("[DLSSG] DLSS frame generation found but could not be hooked; it will not be paused");
                    g_gaveUp.store(true);
                    return;
                }
                g_installed.store(true);
                logger::info("[DLSSG] DLSS frame generation found; it will pause during transitions (experimental)");
            }

            void SetPaused(bool a_on)
            {
                if (!g_config.pauseDlssFrameGen) {
                    return;
                }
                if (a_on) {
                    Install();
                }
                if (!g_installed.load() || g_paused.exchange(a_on) == a_on) {
                    return;
                }
                std::uint8_t viewport[kViewportSize], options[kOptionsSize];
                {
                    std::scoped_lock guard(g_lock);
                    if (!g_haveLast) {
                        return;   // the game has not set any options yet; the hook handles its next call
                    }
                    std::memcpy(viewport, g_lastViewport, kViewportSize);
                    std::memcpy(options, g_lastOptions, kOptionsSize);
                }
                std::uint32_t flags = 0;
                std::memcpy(&flags, options + kFlagsAt, 4);
                flags |= kRetainResourcesWhenOff;
                std::memcpy(options + kFlagsAt, &flags, 4);
                if (a_on) {
                    const std::uint32_t off = 0;
                    std::memcpy(options + kModeAt, &off, 4);
                }
                const auto rc = g_orig(viewport, options);
                logger::info("[DLSSG] frame generation {} (result {}, {} calls forced off so far)", a_on ? "paused" : "resumed", rc, g_forcedOff.load());
            }
        }

        void SetPaused(bool a_on, const char* a_why)
        {
            dlssg::SetPaused(a_on);   // [DLSSG] independent of the FidelityFX switch
            if (!g_config.pauseFrameGen) {
                return;
            }
            if (a_on) {
                Install();
            }
            const bool was = g_paused.exchange(a_on);
            if (was == a_on) {
                return;
            }
            if (!a_on) {
                g_resetLeft.store(3);
            }
            if (g_installed.load()) {
                logger::info("[FGPause] frame generation {} ({}; configure calls {} (enabled seen {}, forced off {}), dispatch calls {} (zeroed {}), resets {})",
                    a_on ? "PAUSED" : "resumed", a_why ? a_why : "?", g_configureCalls.load(), g_configEnabledSeen.load(), g_forcedOff.load(),
                    g_dispatchCalls.load(), g_dispatchZeroed.load(), g_resetsSent.load());
            }
        }
    }

    namespace voidoverlay
    {
        struct alignas(16) Params
        {
            float resolution[2];
            float age;
            float phase;
            float phaseAge;
            float bright;
            float breath;
            float pulseStrength;
            float motes;
            float pulseMs;
            float moteScale;
            float blitAlpha;   // 1 = fully the old frame, 0 = fully the new one (cross-fade)
        };
        static_assert(sizeof(Params) == 48, "Params must match the HLSL cbuffer layout exactly");

        std::atomic<bool>          g_armed{ false };
        std::atomic<bool>          g_cut{ false };
        std::atomic<bool>          g_failed{ false };
        std::atomic<std::int64_t>  g_armAt{ 0 };
        std::atomic<std::int64_t>  g_cutAt{ 0 };
        std::atomic<std::int64_t>  g_lastDrawAt{ 0 };
        std::atomic<std::uint32_t> g_draws{ 0 };
        // Diagnostic: how many Present/Present1 calls actually reached us while a transition was
        // armed. If the game stops presenting during the dead window this counter stays at 0 and
        // the log says so -- that is the difference between "the effect is invisible" and "there
        // was never a frame to draw on".
        std::atomic<std::uint32_t> g_presentSeen{ 0 };
        // Config snapshot taken at Arm() so the render thread never reads g_config directly.
        std::atomic<float>         g_cfgBright{ 0.55f };
        std::atomic<float>         g_cfgBreath{ 0.02f };
        std::atomic<float>         g_cfgPulse{ 0.55f };
        std::atomic<float>         g_cfgMoteScale{ 1.0f };
        std::atomic<std::uint32_t> g_cfgMotes{ 18 };
        std::atomic<std::uint32_t> g_cfgPulseMs{ 300 };
        std::atomic<std::uint32_t> g_cfgCrossFadeMs{ 350 };
        std::atomic<bool>          g_cfgCrossFade{ true };
        // seamless (private build)
        std::atomic<bool>          g_seamless{ false };
        std::atomic<std::int64_t>  g_readyAt{ 0 };
        std::atomic<std::int64_t>  g_stableSince{ 0 };
        std::atomic<float>         g_lastLuma{ -1.0f };
        std::atomic<std::uint32_t> g_cfgFadeMs{ 600 };
        std::atomic<std::uint32_t> g_cfgMinAfterCut{ 900 };
        std::atomic<std::uint32_t> g_cfgStableMs{ 120 };
        std::atomic<std::uint32_t> g_cfgMaxHold{ 3000 };
        std::atomic<float>         g_cfgBlackLuma{ 6.0f };
        std::atomic<float>         g_cfgStableTol{ 3.0f };
        std::atomic<std::int64_t>  g_idleSnapAt{ 0 };
        std::atomic<bool>          g_fadeLogged{ false };
        std::atomic<bool>          g_fadeResWarned{ false };
        std::atomic<bool>          g_cfgDebugFull{ false };
        std::atomic<int>           g_selfCheckLeft{ 0 };
        std::atomic<std::uint32_t> g_cfgMaxMs{ 4000 };
        std::atomic<bool>          g_cfgTrace{ false };

        // Created on and used by the render thread only (inside the Present hook).
        ID3D11VertexShader*    g_vs = nullptr;
        ID3D11PixelShader*     g_ps = nullptr;
        ID3D11Buffer*          g_cb = nullptr;
        ID3D11BlendState*      g_blend = nullptr;
        ID3D11RasterizerState* g_raster = nullptr;
        // v1.4.7 cross-fade: the last frame of the old cell, blitted 1:1 over the new one at the cut.
        ID3D11PixelShader*            g_blitPs = nullptr;
        ID3D11PixelShader*            g_eyesPs = nullptr;
        ID3D11PixelShader*            g_inkPs = nullptr;
        ID3D11PixelShader*            g_magicPs = nullptr;
        ID3D11PixelShader*            g_fadePs = nullptr;
        ID3D11PixelShader*            g_lightPs = nullptr;
        ID3D11PixelShader*            g_drowsyPs = nullptr;
        ID3D11PixelShader*            g_soulPs = nullptr;
        ID3D11PixelShader*            g_wipePs = nullptr;
        ID3D11PixelShader*            g_dreamPs = nullptr;
        ID3D11PixelShader*            g_dissolvePs = nullptr;
        std::atomic<float>            g_fxDir{ 1.0f };
        void SetFxDirection(float a_dir) { g_fxDir.store(a_dir >= 0.0f ? 1.0f : -1.0f); }
        std::atomic<bool>             g_fxStartClosed{ false };   // [Styles] next travel effect starts in its hold (wake-up)

        ID3D11PixelShader* FxShader(int a_kind)
        {
            switch (a_kind) {
            case kFxEyes: return g_eyesPs;
            case kFxInk: return g_inkPs;
            case kFxFade: return g_fadePs;
            case kFxLight: return g_lightPs;
            case kFxDrowsy: return g_drowsyPs;
            case kFxSoul: return g_soulPs;
            case kFxWipe: return g_wipePs;
            case kFxDream: return g_dreamPs;
            case kFxDissolve: return g_dissolvePs;
            default: return g_eyesPs;   // the retired magic field falls back to the eyes
            }
        }

        const char* FxName(int a_kind)
        {
            switch (a_kind) {
            case kFxEyes: return "eyes";
            case kFxInk: return "ink";
            case kFxFade: return "fade";
            case kFxLight: return "light";
            case kFxDrowsy: return "drowsy";
            case kFxSoul: return "soul mist";
            case kFxWipe: return "wipe";
            case kFxDream: return "dream";
            case kFxDissolve: return "dissolve";
            case kFxMagic: return "eyes (was magic)";
            default: return "none";
            }
        }
        ID3D11PixelShader*            g_overridePs = nullptr;   // render thread only: used instead of the blit PS when set
        ID3D11BlendState*             g_alphaBlend = nullptr;
        ID3D11SamplerState*           g_sampler = nullptr;
        ID3D11Texture2D*              g_snapTex = nullptr;
        ID3D11ShaderResourceView*     g_snapSrv = nullptr;
        UINT                          g_snapW = 0;
        UINT                          g_snapH = 0;
        DXGI_FORMAT                   g_snapFmt = DXGI_FORMAT_UNKNOWN;
        std::int64_t                  g_snapAt = 0;

        template <class T>
        void SafeRelease(T*& a_ptr)
        {
            if (a_ptr) {
                a_ptr->Release();
                a_ptr = nullptr;
            }
        }

        bool EnsureResources(ID3D11Device* a_device)
        {
            if (g_vs && g_ps && g_cb && g_blend && g_raster) {
                return true;
            }
            if (!a_device) {
                return false;
            }
            {
                std::string err;
                ID3DBlob* vs = overlayshaders::Compile(overlayshaders::kVS, "main", "vs_5_0", err);
                if (!vs || FAILED(a_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs))) {
                    logger::warn("[Overlay] vertex shader compile/create failed; overlay disabled: {}", err);
                    if (vs) vs->Release();
                    g_failed.store(true);
                    return false;
                }
                vs->Release();
                ID3DBlob* ps = overlayshaders::Compile(overlayshaders::kVoidPS, "main", "ps_5_0", err);
                if (!ps || FAILED(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps))) {
                    logger::warn("[Overlay] void pixel shader compile/create failed; overlay disabled: {}", err);
                    if (ps) ps->Release();
                    g_failed.store(true);
                    return false;
                }
                ps->Release();
            }
            D3D11_BUFFER_DESC cbDesc{};
            cbDesc.ByteWidth = sizeof(Params);
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(a_device->CreateBuffer(&cbDesc, nullptr, &g_cb))) {
                logger::warn("[Overlay] constant buffer creation failed; overlay disabled");
                g_failed.store(true);
                return false;
            }
            D3D11_BLEND_DESC blendDesc{};
            blendDesc.RenderTarget[0].BlendEnable = TRUE;
            blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
            blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(a_device->CreateBlendState(&blendDesc, &g_blend))) {
                logger::warn("[Overlay] blend state creation failed; overlay disabled");
                g_failed.store(true);
                return false;
            }
            {
                std::string err;
                ID3DBlob* ps = overlayshaders::Compile(overlayshaders::kBlitPS, "main", "ps_5_0", err);
                if (!ps || FAILED(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_blitPs))) {
                    logger::warn("[Overlay] blit pixel shader compile/create failed; cross-fade disabled for this session: {}", err);
                }
                if (ps) ps->Release();
            }
            {
                struct Fx { const char* body; ID3D11PixelShader** out; const char* name; };
                const Fx fx[] = {
                    { overlayshaders::kEyesPS, &g_eyesPs, "eyes" },
                    { overlayshaders::kInkPS, &g_inkPs, "ink" },
                    { overlayshaders::kFadePS, &g_fadePs, "fade" },
                    { overlayshaders::kLightPS, &g_lightPs, "light" },
                    { overlayshaders::kDrowsyPS, &g_drowsyPs, "drowsy" },
                    { overlayshaders::kSoulPS, &g_soulPs, "soul mist" },
                    { overlayshaders::kWipePS, &g_wipePs, "wipe" },
                    { overlayshaders::kDreamPS, &g_dreamPs, "dream" },
                    { overlayshaders::kDissolvePS, &g_dissolvePs, "dissolve" },
                };
                for (const auto& f : fx) {
                    std::string err;
                    std::string src = std::string(overlayshaders::kFxCommon) + f.body;
                    ID3DBlob* ps = overlayshaders::Compile(src.c_str(), "main", "ps_5_0", err);
                    if (!ps || FAILED(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, f.out))) {
                        logger::warn("[TravelFx] '{}' pixel shader failed; that effect falls back to the plain hold: {}", f.name, err);
                    } else {
                        logger::info("[TravelFx] '{}' pixel shader compiled", f.name);
                    }
                    if (ps) ps->Release();
                }
            }
            D3D11_BLEND_DESC alphaDesc{};
            alphaDesc.RenderTarget[0].BlendEnable = TRUE;
            alphaDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            alphaDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            alphaDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            alphaDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            alphaDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            alphaDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            alphaDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(a_device->CreateBlendState(&alphaDesc, &g_alphaBlend))) {
                logger::warn("[Overlay] alpha blend state failed; cross-fade disabled for this session");
            }
            D3D11_SAMPLER_DESC sampDesc{};
            sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(a_device->CreateSamplerState(&sampDesc, &g_sampler))) {
                logger::warn("[Overlay] sampler failed; cross-fade disabled for this session");
            }
            D3D11_RASTERIZER_DESC rsDesc{};
            rsDesc.FillMode = D3D11_FILL_SOLID;
            rsDesc.CullMode = D3D11_CULL_NONE;
            rsDesc.DepthClipEnable = FALSE;
            rsDesc.ScissorEnable = FALSE;
            if (FAILED(a_device->CreateRasterizerState(&rsDesc, &g_raster))) {
                logger::warn("[Overlay] rasterizer state creation failed; overlay disabled");
                g_failed.store(true);
                return false;
            }
            logger::info("[Overlay] resources ready (runtime-compiled shaders, blit {}, no depth)",
                g_blitPs ? "ok" : "MISSING");
            return true;
        }

        // Everything the draw touches, saved and put back. We deliberately do not touch shader
        // resources / samplers / index buffers, so those need no save. Even if a restore were
        // skipped the game rebinds its own state at the top of every frame -- this is belt and
        // braces for any other Present hook (ENB / Community Shaders) that runs after us.
        struct StateBlock
        {
            ID3D11RenderTargetView*  rtv[8]{};
            ID3D11DepthStencilView*  dsv = nullptr;
            ID3D11BlendState*        blend = nullptr;
            float                    blendFactor[4]{};
            UINT                     sampleMask = 0;
            ID3D11DepthStencilState* depth = nullptr;
            UINT                     stencilRef = 0;
            ID3D11RasterizerState*   raster = nullptr;
            D3D11_VIEWPORT           vp[8]{};
            UINT                     nVp = 0;
            ID3D11InputLayout*       layout = nullptr;
            D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
            ID3D11VertexShader*      vs = nullptr;
            ID3D11PixelShader*       ps = nullptr;
            ID3D11Buffer*            vsCb[2]{};
            ID3D11Buffer*            psCb[2]{};
            ID3D11ShaderResourceView* psSrv0 = nullptr;
            ID3D11SamplerState*       psSamp0 = nullptr;

            void Grab(ID3D11DeviceContext* a_ctx)
            {
                a_ctx->OMGetRenderTargets(8, rtv, &dsv);
                a_ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
                a_ctx->OMGetDepthStencilState(&depth, &stencilRef);
                a_ctx->RSGetState(&raster);
                nVp = 8;
                a_ctx->RSGetViewports(&nVp, vp);
                a_ctx->IAGetInputLayout(&layout);
                a_ctx->IAGetPrimitiveTopology(&topology);
                a_ctx->VSGetShader(&vs, nullptr, nullptr);
                a_ctx->PSGetShader(&ps, nullptr, nullptr);
                a_ctx->VSGetConstantBuffers(0, 2, vsCb);
                a_ctx->PSGetConstantBuffers(0, 2, psCb);
                a_ctx->PSGetShaderResources(0, 1, &psSrv0);
                a_ctx->PSGetSamplers(0, 1, &psSamp0);
            }

            void Put(ID3D11DeviceContext* a_ctx)
            {
                a_ctx->OMSetRenderTargets(8, rtv, dsv);
                a_ctx->OMSetBlendState(blend, blendFactor, sampleMask);
                a_ctx->OMSetDepthStencilState(depth, stencilRef);
                a_ctx->RSSetState(raster);
                if (nVp) {
                    a_ctx->RSSetViewports(nVp, vp);
                }
                a_ctx->IASetInputLayout(layout);
                if (topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
                    a_ctx->IASetPrimitiveTopology(topology);
                }
                a_ctx->VSSetShader(vs, nullptr, 0);
                a_ctx->PSSetShader(ps, nullptr, 0);
                a_ctx->VSSetConstantBuffers(0, 2, vsCb);
                a_ctx->PSSetConstantBuffers(0, 2, psCb);
                a_ctx->PSSetShaderResources(0, 1, &psSrv0);
                a_ctx->PSSetSamplers(0, 1, &psSamp0);
                for (auto*& p : rtv) {
                    SafeRelease(p);
                }
                SafeRelease(dsv);
                SafeRelease(blend);
                SafeRelease(depth);
                SafeRelease(raster);
                SafeRelease(layout);
                SafeRelease(vs);
                SafeRelease(ps);
                for (auto*& p : vsCb) {
                    SafeRelease(p);
                }
                for (auto*& p : psCb) {
                    SafeRelease(p);
                }
                SafeRelease(psSrv0);
                SafeRelease(psSamp0);
            }
        };

        // Reads the backbuffer back and returns its average luma (0..255). Staging readback is
        // expensive, so this only runs a handful of frames -- it exists to answer one question:
        // did the overlay actually change the pixels we are about to present?
        float MeanOfBackbuffer(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_tex)
        {
            if (!a_device || !a_ctx || !a_tex) {
                return -1.0f;
            }
            D3D11_TEXTURE2D_DESC desc{};
            a_tex->GetDesc(&desc);
            D3D11_TEXTURE2D_DESC sd = desc;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            ID3D11Texture2D* staging = nullptr;
            if (FAILED(a_device->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
                return -1.0f;
            }
            a_ctx->CopyResource(staging, a_tex);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(a_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
                staging->Release();
                return -1.0f;
            }
            const auto* px = static_cast<const std::uint8_t*>(mapped.pData);
            std::uint64_t sum = 0;
            std::uint64_t n = 0;
            const bool tenBit = (desc.Format == 24);
            for (unsigned int y = 0; y < desc.Height; y += 4) {
                const auto* row = px + static_cast<std::size_t>(y) * mapped.RowPitch;
                for (unsigned int x = 0; x < desc.Width; x += 4) {
                    if (tenBit) {
                        std::uint32_t v = 0;
                        std::memcpy(&v, row + static_cast<std::size_t>(x) * 4, 4);
                        sum += (((v >> 20) & 0x3FF) + ((v >> 10) & 0x3FF) + (v & 0x3FF)) / 12u;
                    } else {
                        sum += (row[x * 4 + 0] + row[x * 4 + 1] + row[x * 4 + 2]) / 3u;
                    }
                    ++n;
                }
            }
            a_ctx->Unmap(staging, 0);
            staging->Release();
            return n ? static_cast<float>(sum) / static_cast<float>(n) : -1.0f;
        }

        bool Render(void* a_swapChain, Params a_params)
        {
            using namespace RE::BSGraphics;
            auto* rd = Renderer::GetRendererDataSingleton();
            auto* device = rd ? reinterpret_cast<ID3D11Device*>(rd->forwarder) : nullptr;
            auto* ctx = rd ? reinterpret_cast<ID3D11DeviceContext*>(rd->context) : nullptr;
            if (!device || !ctx || !a_swapChain) {
                logger::warn("[Overlay] renderer device/context/swap chain unavailable; overlay disabled for this session");
                g_failed.store(true);
                return false;
            }
            if (!EnsureResources(device)) {
                return false;
            }
            auto* sc = static_cast<IDXGISwapChain*>(a_swapChain);
            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || !backBuffer) {
                logger::warn("[Overlay] swap chain GetBuffer(0) failed; overlay disabled for this session");
                g_failed.store(true);
                return false;
            }
            ID3D11RenderTargetView* rtv = nullptr;
            if (FAILED(device->CreateRenderTargetView(backBuffer, nullptr, &rtv)) || !rtv) {
                SafeRelease(backBuffer);
                logger::warn("[Overlay] CreateRenderTargetView failed; overlay disabled for this session");
                g_failed.store(true);
                return false;
            }
            D3D11_TEXTURE2D_DESC desc{};
            backBuffer->GetDesc(&desc);
            a_params.resolution[0] = static_cast<float>(desc.Width);
            a_params.resolution[1] = static_cast<float>(desc.Height);

            // ---- cross-fade snapshot (v1.4.7) --------------------------------------------
            // Keep a copy of the frame as it looks BEFORE the cut. During the dead window it is
            // static, so one copy every 400 ms is plenty and costs nothing visible.
            if (g_config.crossFade && g_blitPs && g_alphaBlend && g_sampler) {
                if (!g_snapTex || g_snapW != desc.Width || g_snapH != desc.Height || g_snapFmt != desc.Format) {
                    SafeRelease(g_snapSrv);
                    SafeRelease(g_snapTex);
                    D3D11_TEXTURE2D_DESC sd = desc;
                    sd.Usage = D3D11_USAGE_DEFAULT;
                    sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    sd.CPUAccessFlags = 0;
                    sd.MiscFlags = 0;
                    sd.MipLevels = 1;
                    sd.ArraySize = 1;
                    if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &g_snapTex)) && g_snapTex &&
                        SUCCEEDED(device->CreateShaderResourceView(g_snapTex, nullptr, &g_snapSrv)) && g_snapSrv) {
                        g_snapW = desc.Width;
                        g_snapH = desc.Height;
                        g_snapFmt = desc.Format;
                        g_snapAt = 0;
                    } else {
                        SafeRelease(g_snapSrv);
                        SafeRelease(g_snapTex);
                        logger::warn("[Overlay] snapshot texture creation failed; cross-fade disabled");
                    }
                }
                if (g_snapTex && a_params.blitAlpha <= 0.0f && !g_cut.load(std::memory_order_relaxed)) {
                    // Copy EVERY frame of the dead window. A 400 ms timer could land on the one
                    // near-black frame right after suppression, and then the "cross-fade" would be
                    // fading BLACK over the new cell -- exactly the "it goes black for a moment"
                    // report. The last pre-cut frame is by definition the static old picture.
                    ctx->CopyResource(g_snapTex, backBuffer);
                    g_snapAt = NowMs();
                }
            }

            const bool selfCheck = (g_selfCheckLeft.load() > 0);
            float      meanBefore = -1.0f;
            if (selfCheck) {
                g_selfCheckLeft.fetch_sub(1);
                meanBefore = MeanOfBackbuffer(device, ctx, backBuffer);
            }

            StateBlock saved;
            saved.Grab(ctx);

            D3D11_VIEWPORT vp{};
            vp.Width = static_cast<float>(desc.Width);
            vp.Height = static_cast<float>(desc.Height);
            vp.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &vp);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->OMSetBlendState(g_blend, nullptr, 0xFFFFFFFFu);
            ctx->OMSetDepthStencilState(nullptr, 0);
            ctx->RSSetState(g_raster);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(g_vs, nullptr, 0);
            ctx->VSSetConstantBuffers(0, 1, &g_cb);
            ctx->PSSetConstantBuffers(0, 1, &g_cb);

            const bool wantFade = (a_params.blitAlpha > 0.0f);
            const bool haveFadeRes = g_blitPs && g_alphaBlend && g_snapSrv && g_sampler;
            if (wantFade && !haveFadeRes && !g_fadeResWarned.exchange(true)) {
                logger::warn("[Overlay] cross-fade requested but resources missing (blitPs={} blend={} srv={} sampler={}) -- drawing the motes instead",
                    g_blitPs != nullptr, g_alphaBlend != nullptr, g_snapSrv != nullptr, g_sampler != nullptr);
            }
            const bool fading = wantFade && haveFadeRes;
            if (fading) {
                // The old frame, 1:1, dissolving into the finished cell. 350 ms.
                ctx->OMSetBlendState(g_alphaBlend, nullptr, 0xFFFFFFFFu);
                ctx->PSSetShader(g_overridePs ? g_overridePs : g_blitPs, nullptr, 0);
                ctx->PSSetShaderResources(0, 1, &g_snapSrv);
                ctx->PSSetSamplers(0, 1, &g_sampler);
            } else {
                ctx->PSSetShader(g_ps, nullptr, 0);
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, &a_params, sizeof(a_params));
                ctx->Unmap(g_cb, 0);
                ctx->Draw(3, 0);
            }

            if (selfCheck) {
                const float meanAfter = MeanOfBackbuffer(device, ctx, backBuffer);
                logger::info("[Overlay] self-check: backbuffer mean {:.2f} -> {:.2f} (delta {:+.2f}); {}",
                    meanBefore, meanAfter, meanAfter - meanBefore,
                    fading ? "cross-fade pass" : "motes pass");
            }

            saved.Put(ctx);
            SafeRelease(rtv);
            SafeRelease(backBuffer);
            return true;
        }

        // ---- seamless (private build) --------------------------------------------------------
        // Keeps g_snapTex current with the backbuffer while no transition is running, so the frame
        // held over the load is the one the player saw right before the door. One GPU copy per
        // frame at most every 10 ms; no CPU readback.
        bool EnsureSnapshotTex(ID3D11Device* a_device, const D3D11_TEXTURE2D_DESC& a_desc)
        {
            if (g_snapTex && g_snapW == a_desc.Width && g_snapH == a_desc.Height && g_snapFmt == a_desc.Format) {
                return true;
            }
            SafeRelease(g_snapSrv);
            SafeRelease(g_snapTex);
            D3D11_TEXTURE2D_DESC sd = a_desc;
            sd.Usage = D3D11_USAGE_DEFAULT;
            sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            sd.CPUAccessFlags = 0;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.SampleDesc.Count = 1;
            sd.SampleDesc.Quality = 0;
            if (SUCCEEDED(a_device->CreateTexture2D(&sd, nullptr, &g_snapTex)) && g_snapTex &&
                SUCCEEDED(a_device->CreateShaderResourceView(g_snapTex, nullptr, &g_snapSrv)) && g_snapSrv) {
                g_snapW = a_desc.Width;
                g_snapH = a_desc.Height;
                g_snapFmt = a_desc.Format;
                g_snapAt = 0;
                logger::info("[Seamless] snapshot texture {}x{} fmt {}", g_snapW, g_snapH, static_cast<int>(g_snapFmt));
                return true;
            }
            SafeRelease(g_snapSrv);
            SafeRelease(g_snapTex);
            logger::warn("[Seamless] snapshot texture creation failed; seamless transition disabled");
            g_failed.store(true);
            return false;
        }

        void IdleSnapshot(void* a_swapChain, bool a_force)
        {
            if (!a_swapChain || g_failed.load(std::memory_order_relaxed)) {
                return;
            }
            const std::int64_t now = NowMs();
            if (!a_force && now - g_idleSnapAt.load(std::memory_order_relaxed) < 10) {
                return;
            }
            using namespace RE::BSGraphics;
            auto* rd = Renderer::GetRendererDataSingleton();
            auto* device = rd ? reinterpret_cast<ID3D11Device*>(rd->forwarder) : nullptr;
            auto* ctx = rd ? reinterpret_cast<ID3D11DeviceContext*>(rd->context) : nullptr;
            if (!device || !ctx) {
                return;
            }
            auto* sc = static_cast<IDXGISwapChain*>(a_swapChain);
            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || !backBuffer) {
                return;
            }
            D3D11_TEXTURE2D_DESC desc{};
            backBuffer->GetDesc(&desc);
            if (desc.SampleDesc.Count <= 1 && EnsureSnapshotTex(device, desc)) {
                ctx->CopyResource(g_snapTex, backBuffer);
                g_snapAt = now;
                g_idleSnapAt.store(now, std::memory_order_relaxed);
            }
            SafeRelease(backBuffer);
        }

        void ArmSeamless()
        {
            g_cfgFadeMs.store(std::max<std::uint32_t>(1, g_config.seamlessFadeMs));
            const std::uint32_t autoMin = static_cast<std::uint32_t>(std::max(0.0f, g_config.postLoadUpdateTimeMS)) + 100u;
            g_cfgMinAfterCut.store(g_config.seamlessMinAfterCutMs ? g_config.seamlessMinAfterCutMs : autoMin);
            g_cfgStableMs.store(g_config.seamlessStableMs);
            g_cfgMaxHold.store(g_config.seamlessMaxHoldMs);
            g_cfgBlackLuma.store(g_config.seamlessBlackLuma);
            g_cfgStableTol.store(g_config.seamlessStableTol);
            g_cfgMaxMs.store(std::max<std::uint32_t>(g_config.overlayMaxMs, g_config.travelFxMaxHoldMs + 10000u));   // [Safe] long loads stay covered
            g_cfgTrace.store(g_config.seamlessTrace);
            g_readyAt.store(0);
            g_stableSince.store(0);
            g_lastLuma.store(-1.0f);
            g_selfCheckLeft.store(0);
            g_fadeLogged.store(false);
            g_armAt.store(NowMs());
            g_cutAt.store(0);
            g_cut.store(false);
            g_draws.store(0);
            g_lastDrawAt.store(0);
            g_presentSeen.store(0);
            g_seamless.store(true);
            if (auto* sc = presentfreeze::CurrentSwapChain()) {
                presentfreeze::InstallOn(sc);
                // [EaseIn] frames keep going out until the cell swap so the push-in is visible; the hold starts in Cut().
                presentfreeze::g_heldPresents.store(0);
            } else {
                logger::warn("[Seamless] no swap chain yet; this transition goes without the hold");
            }
            fgpause::SetPaused(true, "seamless hold armed");
            if (!g_armed.exchange(true)) {
                logger::info("[Seamless] armed: snapshot age {} ms, dissolve {} ms, min wait after cut {} ms, steady {} ms, max hold {} ms",
                    g_snapAt ? (NowMs() - g_snapAt) : -1, g_cfgFadeMs.load(), g_cfgMinAfterCut.load(),
                    g_cfgStableMs.load(), g_cfgMaxHold.load());
            }
        }

        // Render-thread side of the seamless transition. Returns after drawing (or disarming).
        void SeamlessPresent(void* a_swapChain)
        {
            const std::int64_t now = NowMs();
            if (now - g_lastDrawAt.load() < 2) {
                return;
            }
            const std::int64_t armAt = g_armAt.load();
            if (now - armAt > static_cast<std::int64_t>(g_cfgMaxMs.load())) {
                g_lastDrawAt.store(now);
                Disarm("hard cap reached -- the transition never reported back");
                return;
            }
            Params p{};
            p.age = static_cast<float>(now - armAt) / 1000.0f;
            p.blitAlpha = 1.0f;
            // [EaseIn] The push-in runs on a clock that only counts frames that reach the screen. While frames are
            // withheld across the swap the clock stands still, so the dissolve carries on from exactly the zoom
            // that was last shown instead of jumping.
            {
                static std::int64_t s_arm = 0, s_last = 0;
                static double       s_shownMs = 0.0;
                if (s_arm != armAt) {
                    s_arm = armAt;
                    s_last = now;
                    s_shownMs = 0.0;
                }
                const double dt = static_cast<double>(std::clamp<std::int64_t>(now - s_last, 0, 100));
                s_last = now;
                if (!presentfreeze::g_holdPresent.load(std::memory_order_relaxed)) {
                    s_shownMs += dt;
                }
                float e = g_config.seamlessEaseInMs ? static_cast<float>(std::min(1.0, s_shownMs / static_cast<double>(g_config.seamlessEaseInMs))) : 1.0f;
                e = e * e * e * (e * (e * 6.0f - 15.0f) + 10.0f);
                p.moteScale = 1.0f + g_config.seamlessPushIn * e + std::min(g_config.seamlessDrift * static_cast<float>(s_shownMs / 1000.0), 0.04f);
            }
            if (g_cut.load(std::memory_order_relaxed)) {
                p.phase = 1.0f;
                const std::int64_t cutAge = now - g_cutAt.load();
                p.phaseAge = static_cast<float>(cutAge) / 1000.0f;
                std::int64_t readyAt = g_readyAt.load();
                if (readyAt == 0) {
                    // Probe the ENGINE's frame (we have not drawn on it yet this Present).
                    const float luma = presentfreeze::SampleBackbufferLuma(a_swapChain);
                    const float last = g_lastLuma.load();
                    if (luma >= 0.0f) {
                        if (last >= 0.0f && std::fabs(luma - last) <= g_cfgStableTol.load()) {
                            if (g_stableSince.load() == 0) {
                                g_stableSince.store(now);
                            }
                        } else {
                            g_stableSince.store(0);
                        }
                        g_lastLuma.store(luma);
                    }
                    const bool pastMin = cutAge >= static_cast<std::int64_t>(g_cfgMinAfterCut.load());
                    const bool bright = luma > g_cfgBlackLuma.load();
                    const auto ss = g_stableSince.load();
                    const bool steady = ss != 0 && (now - ss) >= static_cast<std::int64_t>(g_cfgStableMs.load());
                    bool        ready = pastMin && bright && steady;
                    const char* why = "picture back and steady";
                    if (!ready && cutAge >= static_cast<std::int64_t>(g_cfgMaxHold.load())) {
                        ready = true;
                        why = "max hold reached";
                    }
                    if (!ready && luma < 0.0f && pastMin) {
                        ready = true;
                        why = "luma probe unavailable";
                    }
                    if (ready) {
                        g_readyAt.store(now);
                        readyAt = now;
                        const bool wasHeld = presentfreeze::g_holdPresent.exchange(false);
                        easein::RestoreHud("dissolve starts");   // [EaseIn] the new place's HUD fades in with it
                        logger::info("[Seamless] dissolve starts {} ms after the cut (luma {:.1f}, {}; {} frames held{})",
                            cutAge, luma, why, g_draws.load(), wasHeld ? std::format(", {} presents withheld", presentfreeze::g_heldPresents.load()) : "");
                    }
                }
                if (readyAt != 0) {
                    const float t = static_cast<float>(now - readyAt) / static_cast<float>(g_cfgFadeMs.load());
                    if (t >= 1.0f) {
                        g_lastDrawAt.store(now);
                        Disarm("seamless dissolve finished");
                        return;
                    }
                    const float s = t * t * (3.0f - 2.0f * t);
                    p.blitAlpha = 1.0f - s;
                }
            }
            g_lastDrawAt.store(now);
            if (!g_snapSrv || g_snapAt == 0) {
                // First transition after the hook went in: no idle snapshot yet. This frame is still
                // the old cell (the engine has not swapped), so it is the best we have.
                IdleSnapshot(a_swapChain, true);
            }
            const float lumaBefore = (g_cfgTrace.load() && g_cut.load(std::memory_order_relaxed)) ? g_lastLuma.load() : -2.0f;
            if (!Render(a_swapChain, p)) {
                return;
            }
            if (g_draws.fetch_add(1) == 0) {
                logger::info("[Seamless] holding the last frame from {} ms after arming (snapshot age {} ms)",
                    now - armAt, g_snapAt ? (now - g_snapAt) : -1);
            }
            if (lumaBefore > -2.0f) {
                const std::int64_t cutAge = now - g_cutAt.load();
                if (cutAge <= 1200) {
                    const float after = presentfreeze::SampleBackbufferLuma(a_swapChain);
                    logger::info("[Seamless] trace +{} ms: engine luma {:.1f} -> presented {:.1f} (alpha {:.2f})",
                        cutAge, lumaBefore, after, p.blitAlpha);
                }
            }
        }

        // ---- travel effects (private build) ---------------------------------------------------
        // Ring of older frames. The seamless path copies every frame, which is right for a door (the
        // frame before the activation is the picture we want). Travel is different: by the time the
        // LoadingMenu is requested the script or the map has already faded the screen to black, so
        // the held picture has to come from BEFORE that. Three copies 600 ms apart, each with its
        // mean luma, and no copies while the game is paused (map / dialogue / inventory open), so
        // the newest bright slot is the last thing the player actually saw of the world.
        constexpr int kRingN = 3;
        ID3D11Texture2D* g_ringTex[kRingN] = {};
        float            g_ringLuma[kRingN] = { -1.0f, -1.0f, -1.0f };
        std::int64_t     g_ringAt[kRingN] = {};
        int              g_ringNext = 0;
        UINT             g_ringW = 0, g_ringH = 0;
        DXGI_FORMAT      g_ringFmt = DXGI_FORMAT_UNKNOWN;
        std::atomic<std::int64_t> g_ringLastAt{ 0 };

        std::atomic<bool>         g_fx{ false };
        std::atomic<int>          g_fxKind{ kFxNone };
        std::atomic<int>          g_fxPhase{ 0 };       // 0 in, 1 hold, 2 out
        std::atomic<std::int64_t> g_fxPhaseAt{ 0 };
        std::atomic<bool>         g_fxFirst{ true };
        std::atomic<std::uint32_t> g_fxInMs{ 700 };
        std::atomic<std::uint32_t> g_fxOutMs{ 900 };
        std::atomic<std::uint32_t> g_fxMaxHoldMs{ 45000 };
        std::atomic<bool>          travelfx_fastTravelArming{ false };   // set by the map-close handler around ArmTravelFx: the fast-travel build-up length applies
        // Fast travel (private build): the map's travel order is held back while the magic builds over the live scene, then released.
        std::atomic<bool>          g_fxReleasePending{ false };
        std::function<void()>      g_fxOnBuildUpDone;    // main-thread work queued when phase 0 ends (set from the game thread before arming)
        std::function<void()>      g_fxOnClearStart;     // main-thread work queued when phase 2 starts
        std::mutex                 g_fxCbMutex;

        void SetTravelCallbacks(std::function<void()> a_onBuildUpDone, std::function<void()> a_onClearStart)
        {
            std::lock_guard lk(g_fxCbMutex);
            g_fxOnBuildUpDone = std::move(a_onBuildUpDone);
            g_fxOnClearStart = std::move(a_onClearStart);
        }

        void FireCallback(bool a_buildUpDone)
        {
            std::function<void()> fn;
            {
                std::lock_guard lk(g_fxCbMutex);
                fn = a_buildUpDone ? std::move(g_fxOnBuildUpDone) : std::move(g_fxOnClearStart);
            }
            if (fn) {
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask(std::move(fn));
                }
            }
        }

        void RingTick(void* a_swapChain)
        {
            const std::int64_t now = NowMs();
            if (now - g_ringLastAt.load(std::memory_order_relaxed) < 600) {
                return;
            }
            if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
                return;   // a menu is up: the backbuffer is the menu, not the world
            }
            using namespace RE::BSGraphics;
            auto* rd = Renderer::GetRendererDataSingleton();
            auto* device = rd ? reinterpret_cast<ID3D11Device*>(rd->forwarder) : nullptr;
            auto* ctx = rd ? reinterpret_cast<ID3D11DeviceContext*>(rd->context) : nullptr;
            if (!device || !ctx) {
                return;
            }
            auto* sc = static_cast<IDXGISwapChain*>(a_swapChain);
            ID3D11Texture2D* backBuffer = nullptr;
            if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || !backBuffer) {
                return;
            }
            D3D11_TEXTURE2D_DESC desc{};
            backBuffer->GetDesc(&desc);
            if (desc.SampleDesc.Count > 1) {
                SafeRelease(backBuffer);
                return;
            }
            if (g_ringW != desc.Width || g_ringH != desc.Height || g_ringFmt != desc.Format) {
                for (auto& t : g_ringTex) SafeRelease(t);
                for (auto& l : g_ringLuma) l = -1.0f;
                D3D11_TEXTURE2D_DESC sd = desc;
                sd.Usage = D3D11_USAGE_DEFAULT;
                sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                sd.CPUAccessFlags = 0;
                sd.MiscFlags = 0;
                sd.MipLevels = 1;
                sd.ArraySize = 1;
                bool ok = true;
                for (auto& t : g_ringTex) {
                    if (FAILED(device->CreateTexture2D(&sd, nullptr, &t)) || !t) { ok = false; break; }
                }
                if (!ok) {
                    for (auto& t : g_ringTex) SafeRelease(t);
                    g_ringW = g_ringH = 0;
                    SafeRelease(backBuffer);
                    return;
                }
                g_ringW = desc.Width; g_ringH = desc.Height; g_ringFmt = desc.Format;
            }
            const float luma = presentfreeze::SampleBackbufferLuma(a_swapChain);
            const int slot = g_ringNext;
            ctx->CopyResource(g_ringTex[slot], backBuffer);
            g_ringLuma[slot] = luma;
            g_ringAt[slot] = now;
            g_ringNext = (slot + 1) % kRingN;
            g_ringLastAt.store(now, std::memory_order_relaxed);
            SafeRelease(backBuffer);
        }

        // Picks the picture to hold for a travel effect. Returns true when the LIVE frame is usable
        // (bright), in which case the build-up phase plays over it; false means the screen is already
        // dark and the build-up is skipped.
        bool PickTravelSnapshot(void* a_swapChain, float a_blackLuma)
        {
            const float live = presentfreeze::SampleBackbufferLuma(a_swapChain);
            if (live > a_blackLuma) {
                IdleSnapshot(a_swapChain, true);
                logger::info("[TravelFx] live frame is bright (luma {:.1f}); building up over it", live);
                return true;
            }
            int best = -1;
            for (int k = 0; k < kRingN; ++k) {
                if (g_ringTex[k] && g_ringLuma[k] > a_blackLuma && (best < 0 || g_ringAt[k] > g_ringAt[best])) {
                    best = k;
                }
            }
            using namespace RE::BSGraphics;
            auto* rd = Renderer::GetRendererDataSingleton();
            auto* device = rd ? reinterpret_cast<ID3D11Device*>(rd->forwarder) : nullptr;
            auto* ctx = rd ? reinterpret_cast<ID3D11DeviceContext*>(rd->context) : nullptr;
            if (best >= 0 && device && ctx) {
                D3D11_TEXTURE2D_DESC desc{};
                g_ringTex[best]->GetDesc(&desc);
                if (EnsureSnapshotTex(device, desc)) {
                    ctx->CopyResource(g_snapTex, g_ringTex[best]);
                    g_snapAt = g_ringAt[best];
                    logger::info("[TravelFx] live frame dark (luma {:.1f}); holding a frame from {} ms ago (luma {:.1f})",
                        live, NowMs() - g_ringAt[best], g_ringLuma[best]);
                    return false;
                }
            }
            logger::info("[TravelFx] live frame dark (luma {:.1f}) and no bright frame in the ring; holding what there is", live);
            if (!g_snapSrv || g_snapAt == 0) {
                IdleSnapshot(a_swapChain, true);
            }
            return false;
        }

        void ArmTravelFx(int a_kind, const char* a_reason)
        {
            if (g_failed.load() || a_kind == kFxNone) {
                return;
            }
            if (g_fx.load() && g_armed.load()) {
                logger::info("[TravelFx] already running ({}); keeping it", a_reason);
                return;
            }
            g_fxInMs.store(std::max<std::uint32_t>(100, (a_kind == kFxMagic || g_travelFxLoad.load()) && travelfx_fastTravelArming ? g_config.travelFxMagicInMs : (a_kind == kFxMagic ? g_config.travelFxMagicInMs : g_config.travelFxInMs)));
            g_fxOutMs.store(std::max<std::uint32_t>(100, g_config.travelFxOutMs));
            if (a_kind == kFxMagic) {
                a_kind = kFxEyes;   // [Styles] the magic field is retired
                g_fxInMs.store(std::max<std::uint32_t>(100, g_config.travelFxInMs));
            }
            // [Styles] each style has its own pace on top of the speed setting
            if (a_kind == kFxDrowsy) {
                g_fxInMs.store(std::max<std::uint32_t>(2200, g_fxInMs.load() * 5));   // heavy lids take their time
            } else if (a_kind == kFxDream) {
                g_fxInMs.store(g_fxInMs.load() * 3);
            } else if (a_kind == kFxInk || a_kind == kFxSoul) {
                g_fxInMs.store(g_fxInMs.load() * 5 / 2);
                g_fxOutMs.store(g_fxInMs.load() * 3 / 2);
            } else if (a_kind == kFxDissolve) {
                g_fxInMs.store(100);    // nothing to build: the picture is simply held
                g_fxOutMs.store(std::max<std::uint32_t>(100, g_config.seamlessFadeMs));
            } else if (a_kind == kFxWipe) {
                g_fxInMs.store(100);    // nothing to build: the picture is simply held
                g_fxOutMs.store(std::max<std::uint32_t>(900, g_config.travelFxInMs * 2));
            }
            if (a_kind == kFxEyes || a_kind == kFxFade || a_kind == kFxLight || a_kind == kFxDrowsy || a_kind == kFxDream) {
                g_fxOutMs.store(g_fxInMs.load());   // these clear exactly as they built up: same curve, same length
            }
            g_fxMaxHoldMs.store(g_config.travelFxMaxHoldMs);
            g_cfgStableMs.store(g_config.seamlessStableMs);
            g_cfgBlackLuma.store(g_config.seamlessBlackLuma);
            g_cfgStableTol.store(g_config.seamlessStableTol);
            const std::uint32_t autoMin = static_cast<std::uint32_t>(std::max(0.0f, g_config.postLoadUpdateTimeMS)) + 100u;
            g_cfgMinAfterCut.store(g_config.seamlessMinAfterCutMs ? g_config.seamlessMinAfterCutMs : autoMin);
            g_cfgMaxMs.store(g_config.travelFxMaxHoldMs + 10000u);
            g_readyAt.store(0);
            g_stableSince.store(0);
            g_lastLuma.store(-1.0f);
            g_selfCheckLeft.store(0);
            g_fxKind.store(a_kind);
            g_fxPhase.store(0);
            g_fxFirst.store(true);
            const std::int64_t now = NowMs();
            g_fxPhaseAt.store(now);
            g_armAt.store(now);
            g_cutAt.store(0);
            g_cut.store(false);
            g_draws.store(0);
            g_lastDrawAt.store(0);
            g_presentSeen.store(0);
            g_seamless.store(false);
            g_fx.store(true);
            if (auto* sc = presentfreeze::CurrentSwapChain()) {
                presentfreeze::InstallOn(sc);
            }
            g_armed.store(true);
            fgpause::SetPaused(true, "travel cover armed");
            logger::info("[TravelFx] armed: {} ({}); build-up {} ms, clear {} ms, max hold {} ms",
                FxName(a_kind), a_reason ? a_reason : "?",
                g_fxInMs.load(), g_fxOutMs.load(), g_fxMaxHoldMs.load());
        }

        void ArmWakeUp(int a_kind)
        {
            if (g_armed.load() || a_kind == kFxNone) {
                return;
            }
            g_fxStartClosed.store(true);
            ArmTravelFx(a_kind, "woke up");
            g_cfgMinAfterCut.store(250);   // nothing is loading; open soon
        }

        bool TravelFxActive()
        {
            return g_armed.load() && g_fx.load();
        }

        bool Failed()
        {
            return g_failed.load(std::memory_order_relaxed);
        }

        // [Safe] Once, on the first frame after start-up: can the overlay draw here at all? If not, say so in the log
        // and leave every transition to the plain behaviour instead of finding out halfway through a door.
        void SelfTest(void* a_swapChain)
        {
            using namespace RE::BSGraphics;
            auto* rd = Renderer::GetRendererDataSingleton();
            auto* device = rd ? reinterpret_cast<ID3D11Device*>(rd->forwarder) : nullptr;
            auto* ctx = rd ? reinterpret_cast<ID3D11DeviceContext*>(rd->context) : nullptr;
            const char* problem = nullptr;
            if (!device || !ctx || !a_swapChain) {
                problem = "the game's renderer could not be reached";
            } else if (!EnsureResources(device)) {
                problem = "the shaders or render states could not be created (see the lines above)";
            } else {
                auto*            sc = static_cast<IDXGISwapChain*>(a_swapChain);
                ID3D11Texture2D* back = nullptr;
                if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
                    problem = "the swap chain did not hand out its back buffer";
                } else {
                    ID3D11RenderTargetView* rtv = nullptr;
                    if (FAILED(device->CreateRenderTargetView(back, nullptr, &rtv)) || !rtv) {
                        problem = "a render target could not be created on the back buffer (another mod may wrap the device)";
                    }
                    D3D11_TEXTURE2D_DESC desc{};
                    back->GetDesc(&desc);
                    if (!problem) {
                        logger::info("[Safe] overlay self-check passed: back buffer {}x{}, format {}", desc.Width, desc.Height, static_cast<int>(desc.Format));
                    }
                    SafeRelease(rtv);
                    SafeRelease(back);
                }
            }
            if (problem) {
                g_failed.store(true);
                logger::error("[Safe] overlay self-check FAILED: {}. Transitions are switched off for this session; doors behave as in plain LoadingFlow and other loads show the normal loading screen.", problem);
            }
        }

        bool CutSeen()
        {
            return g_cut.load();
        }

        std::uint32_t FxBuildMs()
        {
            return g_fxInMs.load();
        }

        void TravelFxPresent(void* a_swapChain)
        {
            const std::int64_t now = NowMs();
            if (now - g_lastDrawAt.load() < 2) {
                return;
            }
            const std::int64_t armAt = g_armAt.load();
            if (now - armAt > static_cast<std::int64_t>(g_cfgMaxMs.load())) {
                g_lastDrawAt.store(now);
                Disarm("travel fx hard cap reached");
                return;
            }
            const int kind = g_fxKind.load();
            ID3D11PixelShader* ps = FxShader(kind);
            if (g_fxFirst.exchange(false)) {
                // Make sure the shaders exist before deciding anything (first use compiles them).
                using namespace RE::BSGraphics;
                auto* rd = Renderer::GetRendererDataSingleton();
                auto* device = rd ? reinterpret_cast<ID3D11Device*>(rd->forwarder) : nullptr;
                if (device) {
                    EnsureResources(device);
                }
                ps = FxShader(kind);
                const bool liveBright = PickTravelSnapshot(a_swapChain, g_cfgBlackLuma.load());
                if (g_fxStartClosed.exchange(false)) {
                    g_fxPhase.store(1);   // [Styles] wake-up: begin closed
                    g_fxPhaseAt.store(now);
                } else if (!liveBright) {
                    g_fxPhase.store(1);   // already dark: no build-up, hold straight away
                    g_fxPhaseAt.store(now);
                    FireCallback(true);
                }
                if (!ps) {
                    logger::warn("[TravelFx] effect shader unavailable; plain hold + dissolve instead");
                }
            }
            int phase = g_fxPhase.load();
            std::int64_t phaseAt = g_fxPhaseAt.load();
            if (phase == 0) {
                // the build-up plays over the live scene: keep the snapshot current while it is bright, so the hold that follows shows this scene
                const float live = presentfreeze::SampleBackbufferLuma(a_swapChain);
                if (live > g_cfgBlackLuma.load()) {
                    IdleSnapshot(a_swapChain, true);
                }
                if (now - phaseAt >= static_cast<std::int64_t>(g_fxInMs.load())) {
                    phase = 1; phaseAt = now;
                    g_fxPhase.store(1); g_fxPhaseAt.store(now);
                    FireCallback(true);
                }
            }
            if (phase == 1) {
                bool        ready = false;
                const char* why = "";
                if (g_cut.load(std::memory_order_relaxed)) {
                    const std::int64_t cutAge = now - g_cutAt.load();
                    const float luma = presentfreeze::SampleBackbufferLuma(a_swapChain);
                    const float last = g_lastLuma.load();
                    if (luma >= 0.0f) {
                        if (last >= 0.0f && std::fabs(luma - last) <= g_cfgStableTol.load()) {
                            if (g_stableSince.load() == 0) g_stableSince.store(now);
                        } else {
                            g_stableSince.store(0);
                        }
                        g_lastLuma.store(luma);
                    }
                    const bool pastMin = cutAge >= static_cast<std::int64_t>(g_cfgMinAfterCut.load());
                    const bool bright = luma > g_cfgBlackLuma.load();
                    const auto ss = g_stableSince.load();
                    const bool steady = ss != 0 && (now - ss) >= static_cast<std::int64_t>(g_cfgStableMs.load());
                    if (pastMin && bright && steady) { ready = true; why = "picture back and steady"; }
                    else if (pastMin && bright && cutAge >= static_cast<std::int64_t>(g_cfgMinAfterCut.load()) + 600) { ready = true; why = "picture back (not steady, outdoors)"; }
                    else if (cutAge >= static_cast<std::int64_t>(g_cfgMaxHold.load())) { ready = true; why = "max hold after the swap"; }
                    else if (luma < 0.0f && pastMin) { ready = true; why = "luma probe unavailable"; }
                }
                if (!ready && now - armAt >= static_cast<std::int64_t>(g_fxMaxHoldMs.load())) {
                    ready = true; why = "overall max hold";
                }
                if (ready) {
                    phase = 2; phaseAt = now;
                    g_fxPhase.store(2); g_fxPhaseAt.store(now);
                    presentfreeze::g_holdPresent.store(false);
                    easein::RestoreHud("clearing starts");   // [Rules] door-triggered effects hid the HUD
                    FireCallback(false);
                    logger::info("[TravelFx] clearing starts {} ms after arming ({}; cut {}; {} frames held)",
                        now - armAt, why, g_cut.load() ? "seen" : "not seen", g_draws.load());
                }
            }
            float progress = 1.0f;
            if (phase == 0) {
                progress = std::clamp(static_cast<float>(now - phaseAt) / static_cast<float>(g_fxInMs.load()), 0.0f, 1.0f);
            } else if (phase == 2) {
                progress = static_cast<float>(now - phaseAt) / static_cast<float>(g_fxOutMs.load());
                if (progress >= 1.0f) {
                    g_lastDrawAt.store(now);
                    Disarm("travel fx finished");
                    return;
                }
            }
            Params p{};
            p.age = static_cast<float>(now - armAt) / 1000.0f;
            p.phase = static_cast<float>(phase);
            p.phaseAge = static_cast<float>(now - phaseAt) / 1000.0f;
            p.bright = progress;
            p.pulseStrength = g_fxDir.load();   // [Rules] wipe direction
            if (ps) {
                p.blitAlpha = 1.0f;   // takes the alpha-blend + snapshot path; the PS decides the alpha
            } else {
                // No effect shader: behave like the seamless hold (opaque snapshot, then dissolve).
                p.blitAlpha = (phase == 2) ? (1.0f - progress * progress * (3.0f - 2.0f * progress)) : 1.0f;
            }
            g_lastDrawAt.store(now);
            g_overridePs = ps;
            const bool ok = Render(a_swapChain, p);
            g_overridePs = nullptr;
            if (!ok) {
                return;
            }
            if (g_draws.fetch_add(1) == 0) {
                logger::info("[TravelFx] first frame drawn {} ms after arming (phase {})", now - armAt, phase);
            }
        }

        void Arm()
        {
            if (g_failed.load()) {
                return;
            }
            if (g_fx.load() && g_armed.load()) {
                return;   // a travel effect already owns this load
            }
            if (g_config.seamless) {
                ArmSeamless();
                return;
            }
            if (!g_config.voidOverlay) {
                return;
            }
            g_cfgBright.store(g_config.overlayBrightness);
            g_cfgBreath.store(g_config.overlayBreath);
            g_cfgPulse.store(g_config.overlayPulse);
            g_cfgMoteScale.store(g_config.overlayMoteScale);
            g_cfgMotes.store(g_config.overlayMotes);
            g_cfgPulseMs.store(g_config.overlayPulseMs);
            g_cfgCrossFade.store(g_config.crossFade);
            g_cfgCrossFadeMs.store(g_config.crossFadeMs);
            g_cfgDebugFull.store(g_config.overlayDebugFullscreen);
            g_snapAt = 0;   // force a fresh snapshot for this transition
            g_fadeLogged.store(false);
            g_selfCheckLeft.store(5);   // 前 5 帧做读回自检
            g_cfgMaxMs.store(g_config.overlayMaxMs);
            g_armAt.store(NowMs());
            g_cutAt.store(0);
            g_cut.store(false);
            g_draws.store(0);
            g_lastDrawAt.store(0);
            g_presentSeen.store(0);
            // The overlay lives in the Present hook, so install it here -- otherwise it would only
            // exist when freeze-frame or the swap cover happened to install it first.
            if (auto* sc = presentfreeze::CurrentSwapChain()) {
                presentfreeze::InstallOn(sc);
            } else {
                logger::warn("[Overlay] no swap chain yet; the first transition will go without the overlay");
            }
            fgpause::SetPaused(true, "overlay armed");
            if (!g_armed.exchange(true)) {
                logger::info("[Overlay] armed: {} motes, brightness {:.2f}, breathing {:.3f}, pulse {:.2f}/{} ms, cap {} ms",
                    g_cfgMotes.load(), g_cfgBright.load(), g_cfgBreath.load(), g_cfgPulse.load(),
                    g_cfgPulseMs.load(), g_cfgMaxMs.load());
            }
        }

        // Fired the moment the new cell becomes the player's parent cell -- that is exactly when the
        // engine starts the ~350 ms scene spin. From here the motes fade out over the pulse.
        void Cut()
        {
            if (!g_armed.load()) {
                return;
            }
            const std::int64_t now = NowMs();
            g_cutAt.store(now);
            g_cut.store(true);
            // [Rules] Now the destination is known. A closed-eyes style may change to another one for the opening half
            // (they all hold on black), e.g. eyes shut by a guard's arrest open drowsily in the cell.
            if (g_fx.load() && g_fxPhase.load() <= 1) {
                const auto dark = [](int k) { return k == kFxEyes || k == kFxFade || k == kFxDrowsy; };
                const int   current = g_fxKind.load();
                const char* why = "";
                const int   arrive = rules::ArrivalStyle(why);
                if (dark(current) && dark(arrive) && arrive != current) {
                    g_fxKind.store(arrive);
                    g_fxOutMs.store(arrive == kFxDrowsy ? std::max<std::uint32_t>(2200, g_config.travelFxInMs * 5) : g_fxInMs.load());
                    logger::info("[Rules] opening as {} instead of {} ({})", FxName(arrive), FxName(current), why);
                }
            }
            if ((g_fx.load() || g_seamless.load()) && g_config.holdPresents) {   // [EaseIn] door hold now starts at the swap
                presentfreeze::g_heldPresents.store(0);
                presentfreeze::g_holdPresent.store(true);   // travel cover: nothing is presented across the swap until the reveal
            }
            logger::info("[Overlay] cut after {} ms: {} starting ({} frames drawn, {} Present calls in the dead window)",
                now - g_armAt.load(),
                g_cfgCrossFade.load() ? "cross-fade" : "pulse",
                g_draws.load(), g_presentSeen.load());
        }

        void Disarm(const char* a_reason)
        {
            easein::RestoreHud("overlay disarmed");   // [EaseIn] never leave the HUD hidden
            g_seamless.store(false);
            g_fx.store(false);
            g_fxKind.store(kFxNone);
            g_readyAt.store(0);
            presentfreeze::g_holdPresent.store(false);
            if (!g_armed.exchange(false)) {
                return;
            }
            fgpause::SetPaused(false, a_reason);
            const std::int64_t now = NowMs();
            logger::info("[Overlay] off after {} ms, {} frames drawn, {} Present calls seen ({})",
                now - g_armAt.load(), g_draws.load(), g_presentSeen.load(), a_reason ? a_reason : "?");
        }

        bool Active()
        {
            return g_armed.load();
        }

        // Called on every Present / Present1 so the log can state whether the game was presenting
        // at all during the dead window.
        void NotePresent()
        {
            if (g_armed.load(std::memory_order_relaxed)) {
                g_presentSeen.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Called from the Present hook on the render thread.
        void OnPresent(void* a_swapChain)
        {
            if (!a_swapChain || g_failed.load(std::memory_order_relaxed)) {
                return;
            }
            if (!g_armed.load(std::memory_order_relaxed)) {
                static bool s_tested = false;
                if (!s_tested) {
                    s_tested = true;
                    SelfTest(a_swapChain);   // [Safe]
                    if (g_failed.load()) {
                        return;
                    }
                }
                if (g_config.seamless) {
                    IdleSnapshot(a_swapChain, false);
                }
                easein::OnIdlePresent();   // [EaseIn]
                easein::IdleWatch();       // [Safe]
                if (g_config.travelFx) {
                    RingTick(a_swapChain);
                }
                return;
            }
            if (g_fx.load(std::memory_order_relaxed)) {
                TravelFxPresent(a_swapChain);
                return;
            }
            if (g_seamless.load(std::memory_order_relaxed)) {
                SeamlessPresent(a_swapChain);
                return;
            }
            const std::int64_t now = NowMs();
            // Present and Present1 can both be routed here; draw at most once per frame.
            if (now - g_lastDrawAt.load() < 2) {
                return;
            }
            const std::int64_t armAt = g_armAt.load();
            if (now - armAt > static_cast<std::int64_t>(g_cfgMaxMs.load())) {
                g_lastDrawAt.store(now);
                Disarm("hard cap reached -- the transition never reported back");
                return;
            }

            Params p{};
            p.age = static_cast<float>(now - armAt) / 1000.0f;
            const bool         fading = g_cfgCrossFade.load();
            const std::uint32_t tailMs = fading ? g_cfgCrossFadeMs.load() : g_cfgPulseMs.load();
            if (g_cut.load(std::memory_order_relaxed)) {
                const std::int64_t cutAt = g_cutAt.load();
                const std::int64_t cutAge = now - cutAt;
                if (cutAge > static_cast<std::int64_t>(tailMs)) {
                    g_lastDrawAt.store(now);
                    Disarm(fading ? "cross-fade finished" : "pulse finished");
                    return;
                }
                p.phase = 1.0f;
                p.phaseAge = static_cast<float>(cutAge) / 1000.0f;
                if (fading && tailMs > 0) {
                    // 1 -> 0 over crossFadeMs: the old frame dissolving into the finished cell.
                    const float t = (p.phaseAge * 1000.0f) / static_cast<float>(tailMs);
                    p.blitAlpha = (t >= 1.0f) ? 0.0f : (1.0f - t);
                    p.pulseStrength = 0.0f;
                }
            }
            p.bright = g_cfgBright.load();
            p.breath = g_cfgBreath.load();
            if (!(fading && p.phase >= 0.5f)) {
                p.pulseStrength = g_cfgPulse.load();   // the cross-fade replaces the pulse in its window
            }
            p.motes = static_cast<float>(g_cfgMotes.load());
            p.pulseMs = static_cast<float>(tailMs) / 1000.0f;
            p.moteScale = g_cfgMoteScale.load();
            if (g_cfgDebugFull.load() && p.phase < 0.5f) {
                p.breath = 0.15f;   // full-screen additive lift -- impossible to miss if it reaches the screen
                p.motes = 0.0f;
                p.pulseStrength = 0.0f;
            }

            g_lastDrawAt.store(now);
            const bool firstFadeFrame = (p.blitAlpha > 0.0f) && !g_fadeLogged.exchange(true);
            if (!Render(a_swapChain, p)) {
                return;
            }
            if (firstFadeFrame) {
                logger::info("[Overlay] cross-fade frame 1: snapshot age {} ms, alpha {:.2f}",
                    g_snapAt ? (now - g_snapAt) : -1, p.blitAlpha);
            }
            if (g_draws.fetch_add(1) == 0) {
                logger::info("[Overlay] first frame drawn {:.0f} ms after arming", p.age * 1000.0f);
            }
        }
    }

    void BeginFreezeFrame(const char* a_reason)
    {
        if (!g_config.freezeFramePresent) {
            return;
        }
        auto* sc = presentfreeze::CurrentSwapChain();
        if (!sc) {
            logger::warn("[B] no swap chain yet; freeze-frame skipped ({})", a_reason);
            return;
        }
        if (!presentfreeze::InstallOn(sc)) {
            return;
        }
        presentfreeze::g_skippedFrames.store(0);
        presentfreeze::g_firstSkipLogged.store(false);
        presentfreeze::g_suppressStartAt.store(NowMs());
        presentfreeze::g_suppressPresent.store(true);
        logger::info("[B] freeze-frame ON ({}): presentation will stay on the last frame", a_reason);
        // Failsafe. Everything else may go wrong (the transition may never report back, the player
        // object may be null, a save may be loading), but the screen must never stay frozen forever.
        const auto capMs = static_cast<std::int64_t>(g_config.transitionWindowMs) + 5000;
        std::thread([capMs]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(capMs));
            if (!presentfreeze::g_suppressPresent.load()) {
                return;
            }
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([]() {
                    if (presentfreeze::g_suppressPresent.exchange(false)) {
                        logger::warn("[B] freeze-frame force-disabled by the failsafe -- the transition never reported back");
                    }
                });
            }
        }).detach();
    }

    // ---- Void effect (v1.2.3) ----------------------------------------------------------------
    // The ~124 frames between "hand touches the door" and the spin are dead: the old cell is
    // unloading, the screen is one static frame. This ramps an image-space darkening across that
    // window in a handful of discrete steps (not every tick -- re-triggering a fade record at 40 Hz
    // is what used to strobe), so the wait reads as a deliberate fade instead of a frozen picture.
    void VoidRampStep()
    {
        if (g_config.voidDarken <= 0.01f) {
            return;
        }
        const auto elapsed = NowMs() - g_suppressStarted.load();
        const auto ramp = static_cast<std::int64_t>(g_config.voidRampMs);
        const float t = (ramp <= 0) ? 1.0f
                                    : std::clamp(static_cast<float>(elapsed) / static_cast<float>(ramp), 0.0f, 1.0f);
        const int step = static_cast<int>(t * 8.0f);   // 9 discrete levels
        if (step == g_voidStep.load()) {
            return;
        }
        g_voidStep.store(step);
        CacheForms();
        if (!g_fadeHoldImod) {
            return;
        }
        const float strength = g_config.voidDarken * (static_cast<float>(step) / 8.0f);
        RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeHoldImod, strength, nullptr);
    }

    // At the swap: take the (already dimmed) picture to full black so the spin happens behind it,
    // then hand back to the engine's fade-in. Reuses the holding record, which is the only one that
    // sets a level directly instead of animating.
    void VoidFadeOutAtSwap(std::uint32_t a_holdMs)
    {
        CacheForms();
        if (g_fadeHoldImod) {
            RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeHoldImod, 1.0f, nullptr);
        }
        logger::info("[Void] full black at the swap, holding {} ms before fading into the new cell", a_holdMs);
        std::thread([a_holdMs]() {
            if (a_holdMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(a_holdMs));
            }
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([]() {
                    CacheForms();
                    if (g_fadeHoldImod) {
                        RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
                    }
                    if (g_fadeImod) {
                        RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
                    }
                    if (g_fadeBackImod) {
                        RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeBackImod, 1.0f, nullptr);
                    }
                    logger::info("[Void] released; fading into the new cell");
                });
            }
        }).detach();
    }

    // ---- World probe (v1.3.0, READ-ONLY) -----------------------------------------------------
    // Answers ONE question: during the ~124 dead frames, is the world paused or just not visible?
    //   game time advancing  -> the world is still simulating; "keep it alive" becomes thinkable
    //   game time frozen     -> the engine pauses the transition; covering is the only option
    // It also records when the old cell's 3D root disappears and whether the music track changes.
    struct WorldProbeState
    {
        bool          started = false;
        std::int64_t  startAt = 0;
        float         startHours = 0.0f;
        std::uintptr_t startCell3D = 0;
        std::uint32_t startChildren = 0;
        std::uintptr_t startMusic = 0;
        std::int64_t  cell3DLostAt = -1;
        std::int64_t  musicChangedAt = -1;
        std::uint32_t samples = 0;
        // ---- v1.4.2: "who is competing for the CPU during the window?" -------------------------
        std::uint64_t startFrames = 0;          // present calls seen at the moment we arm
        bool          startFreezeTime = false;  // Main::freezeTime -- the engine's own time freeze
        std::uint32_t highActors = 0;           // what could be throttled IF the world is running
        std::uint32_t lowActors = 0;
        bool          hookInstalled = false;    // without the Present hook the frame rate is unknown
        static constexpr int kTimeline = 16;    // one point every 8th 25 ms tick, i.e. ~200 ms
        std::int64_t  tlAt[kTimeline]{};
        std::uint64_t tlFrames[kTimeline]{};
        float         tlHours[kTimeline]{};
        std::uint8_t  tlHas3D[kTimeline]{};
        int           tlCount = 0;
    };
    WorldProbeState g_worldProbe;
    std::uintptr_t g_probeWorldLastCell3D = 0;
    std::uintptr_t g_probeWorldLastMusic = 0;

    void WorldProbeBegin()
    {
        g_worldProbe = WorldProbeState{};
        if (!g_config.probeWorld) {
            return;
        }
        auto* cal = RE::Calendar::GetSingleton();
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* cell = pl ? pl->GetParentCell() : nullptr;
        auto* cell3d = (cell && cell->GetRuntimeData().loadedData)
            ? cell->GetRuntimeData().loadedData->cell3D.get()
            : nullptr;
        g_worldProbe.started = true;
        g_worldProbe.startAt = NowMs();
        g_worldProbe.startHours = cal ? cal->GetHoursPassed() : 0.0f;
        g_worldProbe.startCell3D = reinterpret_cast<std::uintptr_t>(cell3d);
        g_worldProbe.startChildren = (cell3d && cell3d->GetChildren().size() > 0)
            ? static_cast<std::uint32_t>(cell3d->GetChildren().size())
            : 0;
        if (auto* mus = RE::BSMusicManager::GetSingleton()) {
            g_worldProbe.startMusic = reinterpret_cast<std::uintptr_t>(mus->current);
        }
        logger::info("[World] void start: hoursPassed={:.6f} cell3D=0x{:X} children={} music=0x{:X}",
            g_worldProbe.startHours, g_worldProbe.startCell3D, g_worldProbe.startChildren, g_worldProbe.startMusic);
        // v1.4.2 CPU-contention baseline: three cheap reads that decide whether throttling anything
        // during the window could help at all.
        g_worldProbe.startFrames = presentfreeze::g_frameCount.load();
        g_worldProbe.hookInstalled = presentfreeze::g_installed.load();
        if (auto* main = RE::Main::GetSingleton()) {
            g_worldProbe.startFreezeTime = main->freezeTime;
        }
        if (auto* lists = RE::ProcessLists::GetSingleton()) {
            g_worldProbe.highActors = static_cast<std::uint32_t>(lists->highActorHandles.size());
            g_worldProbe.lowActors = static_cast<std::uint32_t>(lists->lowActorHandles.size());
        }
        logger::info("[World] cpu baseline: freezeTime={} presentHook={} frames={} highActors={} lowActors={}",
            g_worldProbe.startFreezeTime ? "ON" : "off",
            g_worldProbe.hookInstalled ? "installed" : "NOT installed (frame rate unknown)",
            g_worldProbe.startFrames, g_worldProbe.highActors, g_worldProbe.lowActors);
    }

    void WorldProbeStep()
    {
        if (!g_worldProbe.started) {
            return;
        }
        ++g_worldProbe.samples;
        const auto elapsed = NowMs() - g_worldProbe.startAt;
        auto* pl = RE::PlayerCharacter::GetSingleton();
        auto* cell = pl ? pl->GetParentCell() : nullptr;
        auto* cell3d = (cell && cell->GetRuntimeData().loadedData)
            ? cell->GetRuntimeData().loadedData->cell3D.get()
            : nullptr;
        const auto cell3dAddr = reinterpret_cast<std::uintptr_t>(cell3d);
        if (g_probeWorldLastCell3D != cell3dAddr) {
            logger::info("[World] +{} ms cell3D changed: 0x{:X} -> 0x{:X} (children {})",
                elapsed, g_probeWorldLastCell3D, cell3dAddr,
                cell3d ? cell3d->GetChildren().size() : 0);
            g_probeWorldLastCell3D = cell3dAddr;
            if (g_worldProbe.cell3DLostAt < 0 && cell3dAddr == 0) {
                g_worldProbe.cell3DLostAt = elapsed;
            }
        }
        if (auto* mus = RE::BSMusicManager::GetSingleton()) {
            const auto musicAddr = reinterpret_cast<std::uintptr_t>(mus->current);
            if (g_probeWorldLastMusic != musicAddr) {
                logger::info("[World] +{} ms music track changed: 0x{:X} -> 0x{:X}", elapsed, g_probeWorldLastMusic, musicAddr);
                g_probeWorldLastMusic = musicAddr;
                if (g_worldProbe.musicChangedAt < 0) {
                    g_worldProbe.musicChangedAt = elapsed;
                }
            }
        }
        // v1.4.2 timeline: one point every 8th tick (~200 ms). This is what shows WHEN the window
        // actually elapses, whether the frame loop keeps running through it, and whether game time
        // advances -- i.e. whether the wait is CPU-busy or CPU-blocked.
        if ((g_worldProbe.samples % 8) == 0 && g_worldProbe.tlCount < WorldProbeState::kTimeline) {
            const int i = g_worldProbe.tlCount++;
            g_worldProbe.tlAt[i] = elapsed;
            g_worldProbe.tlFrames[i] = presentfreeze::g_frameCount.load();
            auto* calStep = RE::Calendar::GetSingleton();
            g_worldProbe.tlHours[i] = calStep ? calStep->GetHoursPassed() : 0.0f;
            g_worldProbe.tlHas3D[i] = (cell3dAddr != 0) ? 1 : 0;
        }
    }

    void WorldProbeFinish()
    {
        if (!g_worldProbe.started) {
            return;
        }
        g_worldProbe.started = false;
        auto* cal = RE::Calendar::GetSingleton();
        const auto endHours = cal ? cal->GetHoursPassed() : 0.0f;
        const auto delta = endHours - g_worldProbe.startHours;
        const auto elapsed = NowMs() - g_worldProbe.startAt;
        logger::info("[World] void end after {} ms ({} samples): hoursPassed {:.6f} -> {:.6f} (delta {:+.6f}) => world is {}",
            elapsed, g_worldProbe.samples, g_worldProbe.startHours, endHours, delta,
            delta > 0.0005f ? "RUNNING (not paused)" : "PAUSED (time frozen)");
        logger::info("[World] cell3D lost at {} ; music changed at {}",
            g_worldProbe.cell3DLostAt, g_worldProbe.musicChangedAt);

        // ---- v1.4.2: "who is competing for the CPU?" ------------------------------------------
        // fps: the frame loop's actual rate over the window. ~50-60 means the loop is spinning
        // freely (so the time is going into per-frame work, and throttling simulation could help);
        // a low number means the main thread is blocked inside the load path (nothing to throttle).
        const auto endFrames = presentfreeze::g_frameCount.load();
        const std::uint64_t frames = (presentfreeze::g_installed.load() && endFrames >= g_worldProbe.startFrames)
            ? (endFrames - g_worldProbe.startFrames)
            : 0u;
        const double fps = (elapsed > 0 && frames > 0)
            ? (1000.0 * static_cast<double>(frames) / static_cast<double>(elapsed))
            : 0.0;
        bool endFreezeTime = false;
        if (auto* main = RE::Main::GetSingleton()) {
            endFreezeTime = main->freezeTime;
        }
        const double hoursPerSec = (elapsed > 0)
            ? (static_cast<double>(delta) / (static_cast<double>(elapsed) / 1000.0))
            : 0.0;
        logger::info("[World] cpu: {} presents in {} ms => {:.1f} fps | freezeTime {} -> {} | game speed {:.3f} h/s (normal ~0.00029)",
            frames, elapsed, fps,
            g_worldProbe.startFreezeTime ? "ON" : "off", endFreezeTime ? "ON" : "off", hoursPerSec);

        std::string timeline;
        for (int i = 0; i < g_worldProbe.tlCount; ++i) {
            const auto step = (i > 0) ? (g_worldProbe.tlFrames[i] - g_worldProbe.tlFrames[i - 1]) : 0u;
            timeline += std::format("{}@{}ms f{} h{:.6f} 3d{}",
                i > 0 ? " | " : "",
                g_worldProbe.tlAt[i], step, g_worldProbe.tlHours[i],
                g_worldProbe.tlHas3D[i] ? "+" : "-");
        }
        logger::info("[World] timeline ({} points): {}", g_worldProbe.tlCount, timeline);
    }

    void EndFreezeFrameWith(std::uint32_t a_holdMs)    {
        if (!presentfreeze::g_suppressPresent.load()) {
            return;
        }
        const auto startedAt = presentfreeze::g_suppressStartAt.load();
        std::thread([a_holdMs, startedAt]() {
            if (a_holdMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(a_holdMs));
            }
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([startedAt]() {
                    if (!presentfreeze::g_suppressPresent.exchange(false)) {
                        return;
                    }
                    logger::info("[B] freeze-frame OFF after {} ms frozen ({} frames not presented)",
                        NowMs() - startedAt, presentfreeze::g_skippedFrames.load());
                });
            }
        }).detach();
    }

    void EndFreezeFrame()
    {
        if (!g_config.freezeFramePresent) {
            return;
        }
        EndFreezeFrameWith(g_config.freezeFrameHoldMs);
    }

    void RaiseSwapBlack(const char* a_reason);    void ReleaseSwapBlack();

    // =========================================================================
    //  Travel effects (private build): which non-door loads get which cover
    // =========================================================================
    namespace travelfx
    {
        std::atomic<int>          g_pendingKind{ voidoverlay::kFxNone };
        std::atomic<std::int64_t> g_pendingUntil{ 0 };
        std::atomic<std::int64_t> g_dialogueClosedAt{ 0 };
        std::atomic<std::int64_t> g_mapClosedAt{ 0 };   // [Safe] a load right after the map closes is fast travel
        std::atomic<bool>         g_loadGamePending{ false };
        std::vector<RE::FormID>   g_inkForms;      // Black Books, Apocrypha book activators, Soul Cairn portals
        RE::FormID                g_apocrypha = 0;  // DLC2ApocryphaWorld
        RE::FormID                g_soulCairn = 0;  // DLC01SoulCairn

        void CacheForms()
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return;
            }
            auto add = [&](RE::FormID a_local, std::string_view a_mod) {
                if (auto* f = dh->LookupForm(a_local, a_mod)) {
                    g_inkForms.push_back(f->GetFormID());
                }
            };
            // Dragonborn: the seven Black Books + generic, the in-Apocrypha book activators and portal book
            for (RE::FormID id : { 0x016E22u, 0x016E2Cu, 0x016E2Du, 0x01E99Cu, 0x01E99Du, 0x01E99Eu, 0x01E99Fu, 0x032198u,
                     0x032199u, 0x026675u, 0x0270EDu, 0x0270EEu, 0x0270EFu, 0x0270F0u, 0x0270F1u, 0x0270F2u, 0x0270F3u }) {
                add(id, "Dragonborn.esm");
            }
            // Dawnguard: Soul Cairn portal activators / triggers
            for (RE::FormID id : { 0x003C54u, 0x007F5Du, 0x00D411u, 0x01A98Au }) {
                add(id, "Dawnguard.esm");
            }
            if (auto* w = dh->LookupForm(0x01C0B2, "Dragonborn.esm")) g_apocrypha = w->GetFormID();
            if (auto* w = dh->LookupForm(0x001408, "Dawnguard.esm")) g_soulCairn = w->GetFormID();
            logger::info("[TravelFx] cached {} ink trigger forms, Apocrypha 0x{:08X}, Soul Cairn 0x{:08X}",
                g_inkForms.size(), g_apocrypha, g_soulCairn);
        }

        // A 2D sound by descriptor (main thread).
        void PlaySound2D(RE::FormID a_descriptor)
        {
            if (!g_config.travelFxSounds) {
                return;
            }
            auto* sd = RE::TESForm::LookupByID<RE::BGSSoundDescriptorForm>(a_descriptor);
            auto* am = RE::BSAudioManager::GetSingleton();
            if (!sd || !am) {
                return;
            }
            am->Play(sd);   // 2D, at the listener
        }

        // The travel order the map left behind, held back while the magic builds (then put back so the engine travels).
        RE::PLAYER_TARGET_LOC g_heldTravel{};
        bool                  g_travelHeld{ false };

        void ReleaseHeldTravel(const char* a_why)
        {
            if (!g_travelHeld) {
                return;
            }
            g_travelHeld = false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return;
            }
            player->GetPlayerRuntimeData().queuedTargetLoc = g_heldTravel;
            logger::info("[TravelFx] fast travel released to the engine ({})", a_why);
        }

        bool IsInkForm(RE::FormID a_id)
        {
            return std::find(g_inkForms.begin(), g_inkForms.end(), a_id) != g_inkForms.end();
        }

        void NotePending(int a_kind, std::int64_t a_ms, const char* a_why)
        {
            g_pendingKind.store(a_kind);
            g_pendingUntil.store(NowMs() + a_ms);
            logger::info("[TravelFx] pending {} for {} ms ({})",
                voidoverlay::FxName(a_kind), a_ms, a_why);
        }

        // Main thread, at the LoadingMenu request. 0 = leave this load alone.
        int KindForThisLoad(const char*& a_why)
        {
            if (!g_config.travelFx) {
                return voidoverlay::kFxNone;
            }
            if (voidoverlay::Failed()) {   // [Safe]
                a_why = "overlay not available on this setup";
                return voidoverlay::kFxNone;
            }
            if (g_loadGamePending.load()) {
                a_why = "load game pending";
                return voidoverlay::kFxNone;
            }
            auto* ui = RE::UI::GetSingleton();
            if (ui && (ui->IsMenuOpen("Main Menu") || ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME))) {
                a_why = "main menu / chargen";
                return voidoverlay::kFxNone;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || player->IsDead(false) || !player->Is3DLoaded()) {
                a_why = "player dead or not in the world";
                return voidoverlay::kFxNone;
            }
            const std::int64_t now = NowMs();
            const int pending = g_pendingKind.exchange(voidoverlay::kFxNone);
            if (pending != voidoverlay::kFxNone && now < g_pendingUntil.load()) {
                a_why = "pending trigger";
                return pending;
            }
            // [Safe] Fast travel whose order could not be held back at the map (so nothing is pending): it is still fast
            // travel, and its own setting decides - including "vanilla loading screen". Before this it fell through to
            // "other loads" and always blinked.
            if (now - g_mapClosedAt.load() < 10000) {
                g_mapClosedAt.store(0);
                a_why = "fast travel (the map closed just before)";
                return g_config.styleFastTravel;
            }
            // [Rules] moved while asleep (abduction, visions): the Sleep/Wait menu is still up
            if (ui && ui->IsMenuOpen("Sleep/Wait Menu") && !ui->IsMenuOpen("Journal Menu") && !ui->IsMenuOpen("Console") &&
                g_config.styleCaptive != voidoverlay::kFxNone) {
                a_why = "moved while asleep";
                return g_config.styleCaptive;
            }
            // No explicit trigger: a load requested while a pausing menu is up (system menu -> load /
            // quit to main menu, console, inventory) is not travel. Leave it to the vanilla screen.
            if (ui && (ui->IsMenuOpen("Journal Menu") || ui->IsMenuOpen("Console") || ui->GameIsPaused())) {
                a_why = "requested from a menu (load / quit), not travel";
                return voidoverlay::kFxNone;
            }
            if (!player->GetParentCell()) {
                a_why = "player has no cell";
                return voidoverlay::kFxNone;
            }
            if (const int fromHere = rules::SourceStyle(a_why); fromHere != voidoverlay::kFxNone) {   // [Rules]
                return fromHere;
            }
            if (auto* ws = player->GetWorldspace(); ws && (ws->GetFormID() == g_apocrypha || ws->GetFormID() == g_soulCairn)) {
                a_why = "leaving Apocrypha / Soul Cairn";
                return ws->GetFormID() == g_soulCairn ? g_config.styleSoulCairn : g_config.styleBlackBook;   // [Styles]
            }
            if (now - g_dialogueClosedAt.load() < 8000) {
                a_why = "dialogue ended just before (boat / carriage / scripted travel)";
                return g_config.styleCarriage;   // [Styles]
            }
            if (g_config.travelFxDefault == 1 && g_config.styleOther != voidoverlay::kFxNone) {
                a_why = "default for unrecognised travel";
                return g_config.styleOther;   // [Styles]
            }
            a_why = "unrecognised travel, default is the vanilla screen";
            return voidoverlay::kFxNone;
        }
    }

    // Travel effects (private build): a load we covered turned out to be a save load / quit to menu.
    // Stop covering it and give the engine its loading screen back.
    void ReleaseTravelCover(const char* a_why)
    {
        AnnounceLoad(false);   // [Compat] whatever follows, the swallowed load is no longer ours
        const bool hadFx = voidoverlay::TravelFxActive();
        const bool suppressed = g_menuWasSuppressed.load();
        g_travelFxLoad.store(false);
        travelfx::ReleaseHeldTravel("cover released");
        if (!hadFx && !g_suppressArmed.load()) {
            return;
        }
        g_suppressWanted.store(false);
        g_suppressArmed.store(false);
        g_graceSuppressUntil.store(0);
        voidoverlay::Disarm(a_why);
        if (suppressed) {
            if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                q->AddMessage(RE::BSFixedString(RE::LoadingMenu::MENU_NAME.data()), RE::UI_MESSAGE_TYPE::kShow, nullptr);
            }
            logger::info("[TravelFx] cover released and LoadingMenu re-posted ({})", a_why);
        } else {
            logger::info("[TravelFx] cover released ({})", a_why);
        }
    }

    // Watches menu closes that imply "not a door transition": MapMenu (fast travel) and Main Menu
    // (load game). Sets an exemption window so global suppression passes those loads through.
    // ------------------------------------------------------------------------------------------
    // [Rules] Doors, places and situations that get a transition of their own.
    // ------------------------------------------------------------------------------------------
    namespace rules
    {
        std::vector<RE::FormID> g_liftUp, g_liftDown, g_portalDoors, g_boatDoors, g_portalWorlds, g_portalCells, g_captiveCells;
        std::vector<RE::FormID> g_dreamPlaces, g_captiveSources;

        // Rules from SeamlessTransitions_Rules*.txt. They are looked at before the built-in ones.
        struct FileRule
        {
            int   kind = voidoverlay::kFxNone;
            float dir = 1.0f;
        };
        std::unordered_map<RE::FormID, FileRule> g_fileDoor, g_fileFrom, g_fileTo;

        int StyleByName(std::string a_name)
        {
            std::transform(a_name.begin(), a_name.end(), a_name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            static const std::pair<const char*, int> names[] = { { "default", voidoverlay::kFxNone }, { "none", voidoverlay::kFxNone },
                { "eyes", voidoverlay::kFxEyes }, { "fade", voidoverlay::kFxFade }, { "light", voidoverlay::kFxLight },
                { "drowsy", voidoverlay::kFxDrowsy }, { "ink", voidoverlay::kFxInk }, { "mist", voidoverlay::kFxSoul },
                { "wipe", voidoverlay::kFxWipe }, { "dream", voidoverlay::kFxDream }, { "dissolve", voidoverlay::kFxDissolve } };
            for (const auto& [name, kind] : names) {
                if (a_name == name) {
                    return kind;
                }
            }
            return -1;
        }

        // One line:  door|from|to  Plugin.esp|HEXID = style [up|down]
        void ReadRuleFile(const std::filesystem::path& a_path)
        {
            auto*         data = RE::TESDataHandler::GetSingleton();
            std::ifstream in(a_path);
            if (!data || !in) {
                return;
            }
            auto trim = [](std::string s) {
                const auto a = s.find_first_not_of(" \t\r\n");
                const auto b = s.find_last_not_of(" \t\r\n");
                return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
            };
            std::string line;
            int         number = 0, taken = 0;
            while (std::getline(in, line)) {
                ++number;
                if (const auto comment = line.find(';'); comment != std::string::npos) {
                    line.erase(comment);
                }
                line = trim(line);
                if (line.empty()) {
                    continue;
                }
                const auto space = line.find_first_of(" \t");
                const auto eq = line.find('=');
                const auto bar = line.find('|');
                bool       ok = space != std::string::npos && eq != std::string::npos && bar != std::string::npos && space < bar && bar < eq;
                if (ok) {
                    std::string when = line.substr(0, space);
                    std::transform(when.begin(), when.end(), when.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    const std::string plugin = trim(line.substr(space + 1, bar - space - 1));
                    const std::string idText = trim(line.substr(bar + 1, eq - bar - 1));
                    std::istringstream rest(line.substr(eq + 1));
                    std::string        styleName, dirName;
                    rest >> styleName >> dirName;
                    FileRule rule;
                    rule.kind = StyleByName(styleName);
                    rule.dir = (dirName == "down" || dirName == "Down" || dirName == "DOWN") ? -1.0f : 1.0f;
                    RE::FormID local = 0;
                    try {
                        local = static_cast<RE::FormID>(std::stoul(idText, nullptr, 16)) & 0x00FFFFFF;
                    } catch (...) {
                        ok = false;
                    }
                    auto* form = ok ? data->LookupForm(local, plugin) : nullptr;
                    auto* map = when == "door" ? &g_fileDoor : when == "from" ? &g_fileFrom : when == "to" ? &g_fileTo : nullptr;
                    if (ok && rule.kind >= 0 && map && form) {
                        (*map)[form->GetFormID()] = rule;
                        ++taken;
                        continue;
                    }
                    if (ok && rule.kind >= 0 && map && !form) {
                        continue;   // that plugin or form is not in this load order: not an error
                    }
                }
                logger::warn("[Rules] {} line {} not understood: {}", PathText(a_path.filename()), number, line);
            }
            logger::info("[Rules] {}: {} rules", PathText(a_path.filename()), taken);
        }

        void ReadRuleFiles()
        {
            g_fileDoor.clear();
            g_fileFrom.clear();
            g_fileTo.clear();
            std::error_code ec;
            std::vector<std::filesystem::path> files;
            for (const auto& entry : std::filesystem::directory_iterator("Data\\SKSE\\Plugins", ec)) {
                const auto name = PathText(entry.path().filename());
                if (name.size() > 30 && _strnicmp(name.c_str(), "SeamlessTransitions_Rules_", 26) == 0 && _stricmp(name.c_str() + name.size() - 4, ".txt") == 0) {
                    files.push_back(entry.path());
                } else if (_stricmp(name.c_str(), "SeamlessTransitions_Rules.txt") == 0) {
                    files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const auto& file : files) {
                ReadRuleFile(file);
            }
        }

        // Looks the player's surroundings up in a rule table: the cell, its worldspace, then the location and its parents.
        const FileRule* FindPlace(const std::unordered_map<RE::FormID, FileRule>& a_rules)
        {
            if (a_rules.empty()) {
                return nullptr;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell = player ? player->GetParentCell() : nullptr;
            if (!cell) {
                return nullptr;
            }
            if (auto it = a_rules.find(cell->GetFormID()); it != a_rules.end()) {
                return &it->second;
            }
            if (const auto* world = player->GetWorldspace()) {
                if (auto it = a_rules.find(world->GetFormID()); it != a_rules.end()) {
                    return &it->second;
                }
            }
            int depth = 0;
            for (auto* location = player->GetCurrentLocation(); location && depth < 8; location = location->parentLoc, ++depth) {
                if (auto it = a_rules.find(location->GetFormID()); it != a_rules.end()) {
                    return &it->second;
                }
            }
            return nullptr;
        }
        RE::BGSKeyword*         g_jailKeyword = nullptr;

        void Resolve()
        {
            auto* data = RE::TESDataHandler::GetSingleton();
            if (!data) {
                return;
            }
            const auto add = [&](std::vector<RE::FormID>& a_list, std::string_view a_file, std::initializer_list<RE::FormID> a_ids) {
                for (const RE::FormID id : a_ids) {
                    if (auto* form = data->LookupForm(id, a_file)) {
                        a_list.push_back(form->GetFormID());
                    }
                }
            };
            // Dwemer lift doors come in an "up" and a "down" kind
            add(g_liftUp, "Skyrim.esm", { 0x061517, 0x0D3972 });       // DweFacadeLiftLeverLoadUp01 (+MinUse)
            add(g_liftDown, "Skyrim.esm", { 0x036ADB, 0x0CD6BF });     // DweFacadeLiftLeverLoadDown01 (+MinUse)
            add(g_liftUp, "Dragonborn.esm", { 0x020754 });             // DLC2DweFacadeLiftLeverLoadUp01
            add(g_liftDown, "Dragonborn.esm", { 0x020753 });           // DLC2DweFacadeLiftLeverLoadDown01
            // portals that are doors: the Forgotten Vale wayshrines, the paragon portals, the gate to Misty Grove
            add(g_portalDoors, "Dawnguard.esm", { 0x00C83A, 0x01503F, 0x015040, 0x01503E, 0x01A346, 0x015041, 0x015043, 0x01A3B8, 0x015042,
                                                    0x019AC2, 0x00F725 });
            add(g_portalDoors, "Skyrim.esm", { 0x03DC41 });            // dunda14PortalDoor01
            add(g_boatDoors, "Dawnguard.esm", { 0x00BDDE });           // DLC1RowboatDoor
            // leaving these places is always by portal or other magic
            add(g_portalWorlds, "Skyrim.esm", { 0x02EE41, 0x0278DD }); // Sovngarde, SkuldafnWorld
            add(g_portalCells, "Skyrim.esm", { 0x0152BD });            // AzurasStarInterior01
            // arriving here by anything but a door means you were carried in
            add(g_captiveCells, "Skyrim.esm", { 0x050F1E, 0x016DF3 }); // AbandonedShackInterior, MarkarthTempleofDibella (A Night to Remember)
            // leaving these without a door: the Ebonmere in the Twilight Sepulcher (portal), the Underforge after the blood ritual (blackout)
            add(g_portalCells, "Skyrim.esm", { 0x02E521 });            // TwilightSepulcher02
            add(g_captiveSources, "Skyrim.esm", { 0x0165A6 });         // WhiterunUnderforge
            // other minds and dreams: Pelagius's wing and mind, the Dreamstride in Nightcaller Temple
            add(g_dreamPlaces, "Skyrim.esm", { 0x09B238, 0x034240, 0x09AA4A });   // BluePalaceWing01, BluePalaceWingWorld, NightcallerTemple01
            ReadRuleFiles();
            g_jailKeyword = data->LookupForm<RE::BGSKeyword>(0x01CD59, "Skyrim.esm");   // LocTypeJail
            logger::info("[Rules] {} lift doors, {} portal doors, {} boat doors, {} portal places, {} captive cells, jail keyword {}",
                g_liftUp.size() + g_liftDown.size(), g_portalDoors.size(), g_boatDoors.size(), g_portalWorlds.size() + g_portalCells.size(),
                g_captiveCells.size(), g_jailKeyword ? "found" : "missing");
        }

        bool In(const std::vector<RE::FormID>& a_list, RE::FormID a_id)
        {
            return std::find(a_list.begin(), a_list.end(), a_id) != a_list.end();
        }

        // A load door the player is about to use. a_dir: +1 up, -1 down (lifts).
        int DoorStyle(RE::TESBoundObject* a_base, float& a_dir, const char*& a_why)
        {
            if (!a_base) {
                return voidoverlay::kFxNone;
            }
            const RE::FormID id = a_base->GetFormID();
            if (auto it = g_fileDoor.find(id); it != g_fileDoor.end()) {
                a_dir = it->second.dir;
                a_why = "rule file (door)";
                return it->second.kind;
            }
            if (In(g_liftUp, id)) {
                a_dir = 1.0f;
                a_why = "Dwemer lift, going up";
                return g_config.styleLift;
            }
            if (In(g_liftDown, id)) {
                a_dir = -1.0f;
                a_why = "Dwemer lift, going down";
                return g_config.styleLift;
            }
            if (In(g_portalDoors, id)) {
                a_why = "portal door";
                return g_config.stylePortal;
            }
            if (In(g_boatDoors, id)) {
                a_why = "rowboat";
                return g_config.styleBoat;
            }
            if (g_config.styleDoor != voidoverlay::kFxNone && g_config.styleDoor != voidoverlay::kFxDissolve) {
                a_why = "door (style chosen in the settings)";
                return g_config.styleDoor;   // every ordinary door uses this instead of the hold and dissolve
            }
            return voidoverlay::kFxNone;
        }

        // A load that starts without a door: does the place the player is leaving decide the style?
        int SourceStyle(const char*& a_why)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell = player ? player->GetParentCell() : nullptr;
            if (!cell) {
                return voidoverlay::kFxNone;
            }
            if (const auto* rule = FindPlace(g_fileFrom)) {
                a_why = "rule file (from)";
                return rule->kind;
            }
            const auto* world = player->GetWorldspace();
            const RE::FormID worldId = world ? world->GetFormID() : 0;
            if ((world && In(g_portalWorlds, worldId)) || In(g_portalCells, cell->GetFormID())) {
                a_why = "leaving by portal (Sovngarde, Skuldafn, Azura's Star, the Ebonmere)";
                return g_config.stylePortal;
            }
            if (In(g_dreamPlaces, cell->GetFormID()) || (world && In(g_dreamPlaces, worldId))) {
                a_why = "a dream or another mind";
                return g_config.styleDream;
            }
            if (In(g_captiveSources, cell->GetFormID())) {
                a_why = "blackout (the Underforge)";
                return g_config.styleCaptive;
            }
            return voidoverlay::kFxNone;
        }

        // After the swap: does the place the player arrived in decide how the cover opens?
        int ArrivalStyle(const char*& a_why)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell = player ? player->GetParentCell() : nullptr;
            if (!cell) {
                return voidoverlay::kFxNone;
            }
            if (const auto* rule = FindPlace(g_fileTo)) {
                a_why = "rule file (to)";
                return rule->kind;
            }
            if (In(g_captiveCells, cell->GetFormID())) {
                a_why = "carried here";
                return g_config.styleCaptive;
            }
            if (auto* location = player->GetCurrentLocation(); location && g_jailKeyword && location->HasKeyword(g_jailKeyword)) {
                a_why = "jail";
                return g_config.styleCaptive;
            }
            return voidoverlay::kFxNone;
        }
    }

    // ------------------------------------------------------------------------------------------
    // [Styles] MCM Helper settings, previews from the config page, and the wake-up transition.
    // ------------------------------------------------------------------------------------------
    namespace mcm
    {
        // Order of the style lists on the config page -> FxKind.
        constexpr int kStyleByIndex[] = { voidoverlay::kFxNone, voidoverlay::kFxEyes, voidoverlay::kFxFade, voidoverlay::kFxLight,
            voidoverlay::kFxDrowsy, voidoverlay::kFxInk, voidoverlay::kFxSoul, voidoverlay::kFxWipe, voidoverlay::kFxDream, voidoverlay::kFxDissolve };
        constexpr int kStyleCount = static_cast<int>(sizeof(kStyleByIndex) / sizeof(kStyleByIndex[0]));

        int StyleFromIndex(int a_index)
        {
            return (a_index >= 0 && a_index < kStyleCount) ? kStyleByIndex[a_index] : voidoverlay::kFxNone;
        }

        void Apply(const std::string& a_section, const std::string& a_key, const std::string& a_val)
        {
            try {
                const auto id = a_key + ":" + a_section;
                const auto b = [&] { return std::stoi(a_val) != 0; };
                const auto i = [&] { return std::stoi(a_val); };
                const auto f = [&] { return std::stof(a_val); };
                if (id == "bSeamless:Doors") g_config.seamless = b();
                else if (id == "bHideHud:Doors") g_config.seamlessHideHud = b();
                else if (id == "iDissolveMs:Doors") g_config.seamlessFadeMs = static_cast<std::uint32_t>(std::clamp(i(), 100, 2000));
                else if (id == "iEaseInMs:Doors") g_config.seamlessEaseInMs = static_cast<std::uint32_t>(std::clamp(i(), 0, 20000));
                else if (id == "fPushInPct:Doors") g_config.seamlessPushIn = std::clamp(f() / 100.0f, 0.0f, 0.2f);
                else if (id == "fDriftPct:Doors") g_config.seamlessDrift = std::clamp(f() / 100.0f, 0.0f, 0.05f);
                else if (id == "bEnabled:Travel") g_config.travelFx = b();
                else if (id == "iFastTravel:Travel") g_config.styleFastTravel = StyleFromIndex(i());
                else if (id == "iCarriage:Travel") g_config.styleCarriage = StyleFromIndex(i());
                else if (id == "iOther:Travel") g_config.styleOther = StyleFromIndex(i());
                else if (id == "iBlackBook:Travel") g_config.styleBlackBook = StyleFromIndex(i());
                else if (id == "iSoulCairn:Travel") g_config.styleSoulCairn = StyleFromIndex(i());
                else if (id == "iSleep:Travel") g_config.styleSleep = StyleFromIndex(i());
                else if (id == "iLift:Places") g_config.styleLift = StyleFromIndex(i());
                else if (id == "iPortal:Places") g_config.stylePortal = StyleFromIndex(i());
                else if (id == "iCaptive:Places") g_config.styleCaptive = StyleFromIndex(i());
                else if (id == "iBoat:Places") g_config.styleBoat = StyleFromIndex(i());
                else if (id == "iDream:Places") g_config.styleDream = StyleFromIndex(i());
                else if (id == "iDoor:Doors") g_config.styleDoor = StyleFromIndex(i());
                else if (id == "iEffectMs:Travel") {
                    const auto ms = static_cast<std::uint32_t>(std::clamp(i(), 100, 3000));
                    g_config.travelFxInMs = ms;
                    g_config.travelFxOutMs = std::min<std::uint32_t>(3000, ms * 2);   // ink and mist drain more slowly
                } else if (id == "bPauseDlss:FrameGeneration") g_config.pauseDlssFrameGen = b();
                else if (id == "bAnnounceLoads:Doors") g_config.announceLoads = b();
                else if (id == "iFastTravelMs:Travel") g_config.travelFxMagicInMs = static_cast<std::uint32_t>(std::clamp(i(), 200, 4000));
                else if (id == "bPause:FrameGeneration") g_config.pauseFrameGen = b();
                else if (id == "bHoldPresents:FrameGeneration") g_config.holdPresents = b();
            } catch (...) {
            }
        }

        bool ReadFile(const char* a_path)
        {
            std::ifstream in(a_path);
            if (!in) {
                return false;
            }
            auto trim = [](std::string s) {
                const auto a = s.find_first_not_of(" \t\r\n");
                const auto b = s.find_last_not_of(" \t\r\n");
                return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
            };
            std::string line, section;
            while (std::getline(in, line)) {
                line = trim(line);
                if (line.empty() || line[0] == ';' || line[0] == '#') {
                    continue;
                }
                if (line.front() == '[' && line.back() == ']') {
                    section = trim(line.substr(1, line.size() - 2));
                    continue;
                }
                const auto eq = line.find('=');
                if (eq != std::string::npos) {
                    Apply(section, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
                }
            }
            return true;
        }

        // Defaults first, then what the user set on the config page. Without MCM Helper neither file
        // exists and LoadingFlow.ini alone decides.
        void LoadSettings(const char* a_why)
        {
            const bool defaults = ReadFile("Data\\MCM\\Config\\SeamlessTransitions\\settings.ini");
            const bool user = ReadFile("Data\\MCM\\Settings\\SeamlessTransitions.ini");
            if (defaults || user) {
                logger::info("[MCM] settings read ({}): doors {}, travel {} (fast travel {}, carriage {}, other {}, black book {}, soul cairn {}, sleep {})",
                    a_why, g_config.seamless ? "seamless" : "off", g_config.travelFx ? "on" : "off", voidoverlay::FxName(g_config.styleFastTravel),
                    voidoverlay::FxName(g_config.styleCarriage), voidoverlay::FxName(g_config.styleOther), voidoverlay::FxName(g_config.styleBlackBook),
                    voidoverlay::FxName(g_config.styleSoulCairn), voidoverlay::FxName(g_config.styleSleep));
            }
        }

        void After(int a_ms, std::function<void()> a_fn)
        {
            std::thread([a_ms, fn = std::move(a_fn)]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(a_ms));
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask(std::move(fn));
                }
            }).detach();
        }

        // ---- previews: the transition plays where you stand, with no travel behind it
        std::atomic<int> g_previewPending{ -1 };

        void RunPreview()
        {
            const int index = g_previewPending.exchange(-1);
            auto*     ui = RE::UI::GetSingleton();
            if (index < 0 || !ui || ui->GameIsPaused() || voidoverlay::Active()) {
                return;
            }
            if (index == 0) {
                // door: HUD away, hold the picture with its slow push-in, then dissolve back into the live view
                easein::HideHudNow();
                After(80, [] {
                    voidoverlay::Arm();
                    logger::info("[MCM] preview: door dissolve");
                    After(1200, [] { voidoverlay::Cut(); });
                });
                return;
            }
            const int kind = StyleFromIndex(index);
            if (kind == voidoverlay::kFxNone) {
                return;
            }
            voidoverlay::SetFxDirection(1.0f);
            voidoverlay::ArmTravelFx(kind, "preview from the config page");
            After(static_cast<int>(voidoverlay::FxBuildMs()) + (kind == voidoverlay::kFxWipe ? 1100 : 500), [] { voidoverlay::Cut(); });
        }

        void OnJournalClosed()
        {
            LoadSettings("config page closed");
            if (g_previewPending.load() >= 0) {
                After(600, [] { RunPreview(); });
            }
        }

        // Papyrus: LoadingFlow_Native.Preview(int)
        void PreviewNative(RE::StaticFunctionTag*, std::int32_t a_style)
        {
            g_previewPending.store(a_style);
            lfnotify::Show("Close the menu to see the transition.");
        }

        bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
        {
            a_vm->RegisterFunction("Preview", "SeamlessTransitions_Native", PreviewNative);
            return true;
        }

        // ---- waking up after sleeping
        class SleepSink : public RE::BSTEventSink<RE::TESSleepStopEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStopEvent*, RE::BSTEventSource<RE::TESSleepStopEvent>*) override
            {
                if (g_config.enabled && g_config.travelFx && g_config.styleSleep != voidoverlay::kFxNone) {
                    After(250, [] {
                        if (voidoverlay::Active()) {
                            return;
                        }
                        voidoverlay::ArmWakeUp(g_config.styleSleep);
                        After(350, [] { voidoverlay::Cut(); });
                    });
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        void Install()
        {
            static SleepSink sleepSink;
            if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
                events->AddEventSink<RE::TESSleepStopEvent>(&sleepSink);
            }
            LoadSettings("game data loaded");
            rules::Resolve();
        }
    }

    class TransitionExemptSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (g_announcing.load()) {
                return RE::BSEventNotifyControl::kContinue;   // [Compat] our own announcement, not a real menu
            }
            if (a_event && a_event->opening) {
                const char* name = a_event->menuName.c_str();
                // [Safe] a menu the player opens must be visible: undo this mod's HUD hiding at once
                if (name) {
                    static const char* const kPlayerMenus[] = { "TweenMenu", "StatsMenu", "Journal Menu", "InventoryMenu", "MagicMenu",
                        "MapMenu", "ContainerMenu", "BarterMenu", "GiftMenu", "Lockpicking Menu", "Book Menu", "Sleep/Wait Menu", "Console",
                        "MessageBoxMenu", "Dialogue Menu", "FavoritesMenu", "Crafting Menu", "Training Menu", "LevelUp Menu", "RaceSex Menu",
                        "Main Menu", "Tutorial Menu", "Loading Menu" };
                    for (const char* menu : kPlayerMenus) {
                        if (std::strcmp(name, menu) == 0) {
                            easein::ShowHudNow();
                            break;
                        }
                    }
                }
                if (name && (std::strcmp(name, "Main Menu") == 0 || std::strcmp(name, "Loading Menu") == 0) &&
                    (voidoverlay::TravelFxActive() || g_travelFxLoad.load())) {
                    ReleaseTravelCover(std::strcmp(name, "Main Menu") == 0 ? "main menu opened" : "loading menu opened anyway");
                }
            }
            if (a_event && !a_event->opening) {
                const char* name = a_event->menuName.c_str();
                if (name && std::strcmp(name, "Journal Menu") == 0) {
                    mcm::OnJournalClosed();   // [Styles] the config page lives in the journal
                }
                if (name && (std::strcmp(name, "MapMenu") == 0 || std::strcmp(name, "Main Menu") == 0)) {
                    g_exemptUntil.store(NowMs() + 5000);
                    logger::info("[Tier2] suppression exempted for 5 s (menu '{}' closed -> fast travel / load game)", name);
                }
                // Travel effects (private build)
                if (name && g_config.travelFx) {
                    if (std::strcmp(name, "MapMenu") == 0) {
                        travelfx::g_mapClosedAt.store(NowMs());
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        bool travelling = false;
                        if (player) {
                            const auto& q = player->GetPlayerRuntimeData().queuedTargetLoc;
                            travelling = (q.world != nullptr) || (q.interior != nullptr) || (q.fastTravelMarker != 0);
                        }
                        if (travelling && g_config.styleFastTravel != voidoverlay::kFxNone && !voidoverlay::Failed()) {
                            const int ftKind = g_config.styleFastTravel;   // [Styles]
                            travelfx::NotePending(ftKind, 12000, "map closed with a travel target queued");
                            // Hold the engine's travel order: the map closes onto the live scene, the magic builds over it with a sound,
                            // and only then is the order given back (the engine fades and loads under our cover).
                            auto& q = player->GetPlayerRuntimeData().queuedTargetLoc;
                            travelfx::g_heldTravel = q;
                            q = RE::PLAYER_TARGET_LOC{};
                            travelfx::g_travelHeld = true;
                            if (ftKind == voidoverlay::kFxMagic) {
                                voidoverlay::SetTravelCallbacks(
                                    [] { travelfx::ReleaseHeldTravel("build-up finished"); },
                                    [] { travelfx::PlaySound2D(0x00086EFE); });   // MAGConjurePortalClose2DSD on the reveal
                                travelfx::PlaySound2D(0x00086EFD);                 // MAGConjurePortalOpen2DSD as the magic rises
                            } else {
                                voidoverlay::SetTravelCallbacks([] { travelfx::ReleaseHeldTravel("build-up finished"); }, nullptr);
                            }
                            g_travelFxLoad.store(true);
                            voidoverlay::travelfx_fastTravelArming.store(true);
                            voidoverlay::ArmTravelFx(ftKind, "fast travel confirmed on the map");
                            voidoverlay::travelfx_fastTravelArming.store(false);
                            // Safety: if the cover never reaches its hold (shader failure, disarm), the travel still happens.
                            std::thread([] {
                                std::this_thread::sleep_for(std::chrono::milliseconds(g_config.travelFxMagicInMs + 1500));
                                if (auto* task = SKSE::GetTaskInterface()) {
                                    task->AddTask([] { travelfx::ReleaseHeldTravel("safety timer"); });
                                }
                            }).detach();
                        }
                    } else if (std::strcmp(name, "Dialogue Menu") == 0) {
                        travelfx::g_dialogueClosedAt.store(NowMs());
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    TransitionExemptSink g_exemptSink;

    // Route C (v1.5.1): cancel the engine's own fade-to-black. It MUST be called from the main
    // thread at moments we know the main thread is alive -- the 25 ms watchdog relies on AddTask,
    // and the task pump dies ~0.6-0.75 s into the transition (measured), i.e. exactly before the
    // long part of the load. That is why the first version of route C did nothing.
    // v1.5.3: Suppressing LoadingMenu does NOT suppress the vanilla "Mist Menu", which is the
    // black/mist backdrop the engine shows during a load. It is a separate menu -- Clean Loading
    // Transitions hooks it for exactly this reason (MistMenu::PostDisplay). Log what is open and
    // try to drop the mist; the log tells us definitively what is on screen at the cut.
    void LogOpenMenus(const char* a_where)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return;
        }
        std::string open;
        for (const char* name : { "Loading Menu", "Mist Menu", "Fader Menu", "HUD Menu", "Main Menu" }) {
            if (ui->IsMenuOpen(name)) {
                if (!open.empty()) {
                    open += ", ";
                }
                open += name;
            }
        }
        logger::info("[Menus] open at {}: {}", a_where, open.empty() ? "none" : open.c_str());
    }

    void HideMenuIfOpen(const char* a_name)
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(a_name)) {
            return;
        }
        if (auto* q = RE::UIMessageQueue::GetSingleton()) {
            q->AddMessage(RE::BSFixedString(a_name), RE::UI_MESSAGE_TYPE::kHide, nullptr);
            logger::info("[NoBlack] posted Hide for '{}'", a_name);
        }
    }

    void StopEngineBlack(const char* a_where)
    {
        CacheForms();
        if (g_fadeImod) {
            RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
        }
        if (g_fadeHoldImod) {
            RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
        }
        logger::info("[NoBlack] stopped engine fade records ({})", a_where);
    }

    RE::UI_MESSAGE_RESULTS HookedLoadingMenuProcessMessage(RE::IMenu* a_this, RE::UIMessage& a_msg)
    {
        const auto msgType = a_msg.type.get();
        if (auto budget = g_msgTypeLogBudget.load(); budget > 0 &&
            g_msgTypeLogBudget.compare_exchange_strong(budget, budget - 1)) {
            logger::info("[Tier2] LoadingMenu ProcessMessage type={} (0=Update 1=Show 2=Reshow 3=Hide 4=ForceHide)",
                static_cast<std::uint32_t>(msgType));
        }
        // Global mode: catch transitions that LoadingFlow's door hook never saw (trigger volumes,
        // script MoveTo). Arming here captures the CURRENT cell as the "from" cell, which is correct
        // because the menu is requested before the cell actually changes.
        const bool wantsShow = (msgType == RE::UI_MESSAGE_TYPE::kShow || msgType == RE::UI_MESSAGE_TYPE::kReshow);
        // Travel effects (private build): a load the door hook did not start. Decide on a cover and,
        // if there is one, take the menu over for the whole load (long timeouts, see g_travelFxLoad).
        if (wantsShow && !g_suppressArmed.load() && g_config.seamless && g_config.suppressLoadingMenu && !voidoverlay::Failed() &&
            !voidoverlay::Active() && NowMs() - g_playerDoorPassedAt.load() < 4000) {
            // [Safe] a door the door logic passed on (opened with its key, for instance) is loading after all
            g_playerDoorPassedAt.store(0);
            logger::info("[Safe] load follows a door that was passed on; treating it as a door transition");
            ArmLoadingMenuSuppression("door passed on to the game, load followed", false);
        }
        if (g_config.travelFx && wantsShow && !g_suppressArmed.load()) {
            const char* why = "";
            const int kind = travelfx::KindForThisLoad(why);
            if (kind != voidoverlay::kFxNone) {
                g_travelFxLoad.store(true);
                ArmLoadingMenuSuppression("travel effect", true);
                if (g_suppressArmed.load()) {
                    voidoverlay::ArmTravelFx(kind, why);
                    g_menuWasSuppressed.store(true);
                    g_fallbackAt.store(NowMs() + static_cast<std::int64_t>(g_config.travelFxMaxHoldMs));
                    LogOpenMenus("travel effect");
                    HideMenuIfOpen("Mist Menu");
                    logger::info("[TravelFx] LoadingMenu suppressed for a travel load ({})", why);
                    { AnnounceLoad(true); return RE::UI_MESSAGE_RESULTS::kIgnore; }
                }
                g_travelFxLoad.store(false);
            } else {
                logger::info("[TravelFx] load left to the vanilla screen ({})", why);
                voidoverlay::Disarm("travel load not covered");
            }
        }
        if (g_config.globalMenuSuppression && !g_suppressArmed.load() && wantsShow) {
            if (NowMs() < g_exemptUntil.load()) {
                logger::info("[Tier2] global suppression skipped: exempt window (fast travel / load game)");
            } else {
                ArmLoadingMenuSuppression("global (menu requested outside the door hook)", true);
                if (g_suppressArmed.load()) {
                    g_menuWasSuppressed.store(true);
                    logger::info("[Tier2] LoadingMenu kShow suppressed by global mode");
                    { AnnounceLoad(true); return RE::UI_MESSAGE_RESULTS::kIgnore; }
                }
            }
        }
        // v1.6.7: late request after the transition already finished -- eat it, and make sure any
        // loading menu that did manage to open is dismissed.
        if (wantsShow && !g_suppressArmed.load() && NowMs() < g_graceSuppressUntil.load()) {
            logger::info("[Tier2] late LoadingMenu request suppressed (post-cut grace window)");
            HideMenuIfOpen("Loading Menu");
            { AnnounceLoad(true); return RE::UI_MESSAGE_RESULTS::kIgnore; }
        }
        if (g_suppressArmed.load() && g_suppressWanted.load() && wantsShow) {
            if (NowMs() <= g_suppressDeadline.load()) {
                if (!g_menuWasSuppressed.exchange(true)) {
                    // First suppression of this transition: the re-post fallback is measured from
                    // here, not from the door activation.
                    g_fallbackAt.store(NowMs() + static_cast<std::int64_t>(CoveredLoad() ? g_config.travelFxMaxHoldMs : g_config.suppressTimeoutMs));
                    // Raise the black NOW, roughly a second before the swap, so that the engine's
                    // scene-attach spin happens behind it instead of in front of it.
                    // v1.2.2: when a swap-time cover is configured, do NOT cover the whole load --
                    // the spin is only ~350 ms, so a long cover just reads as a hang.
                    if (g_config.coverAtSwapMs == 0) {
                        RaiseSwapBlack("menu suppressed");
                        BeginFreezeFrame("menu suppressed");
                    }
                }
                LogOpenMenus("menu suppression");
                HideMenuIfOpen("Mist Menu");
                StopEngineBlack("at menu suppression");
                logger::info("[Tier2] LoadingMenu suppressed (type={}, {} ms after activation)",
                    static_cast<std::uint32_t>(msgType), NowMs() - g_suppressStarted.load());
                { AnnounceLoad(true); return RE::UI_MESSAGE_RESULTS::kIgnore; }
            }
            logger::info("[Tier2] LoadingMenu let through: past the {} ms transition window",
                g_config.transitionWindowMs);
            // The vanilla menu owns the screen from here; stop drawing on it.
            voidoverlay::Disarm("menu let through past the window");
        }
        if (wantsShow) {
            easein::ShowHudNow();   // [Safe] a loading screen that is let through must be visible
            g_announcedOpen.store(false);   // [Compat] the real menu will raise its own events
        }
        return g_origLoadingProcessMessage ? g_origLoadingProcessMessage(a_this, a_msg)
                                           : RE::UI_MESSAGE_RESULTS::kPassOn;
    }

    // Hold the frame black for a beat after the cell swap, then fade in. This masks the pop-in of
    // geometry that finished streaming just after the cell became current -- the engine's own
    // fade-back can otherwise reveal half-built meshes. One-shot, bounded, main-thread work only.
    // Release the swap-black and fade back in. Must ALWAYS be able to run: a holding record that is
    // never released leaves the player permanently black with only the UI visible.
    void ReleaseSwapBlack()
    {
        CacheForms();
        if (g_fadeHoldImod) {
            RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
        }
        if (g_fadeImod) {
            RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
        }
        if (g_fadeBackImod) {
            RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeBackImod, g_config.envelopeFadeStrength, nullptr);
        }
        logger::info("[Visual] swap black released; fading back in");
    }

    // Bring the black up EARLY. The engine spins/scales the new scene into place during the first
    // frames after the swap, and those frames are visible the instant the view becomes the new cell.
    // Waiting for the cell change and only then going black misses them entirely (measured in-game:
    // the spin finishes, the view stabilises, and only afterwards does the black start). So the hold
    // is raised while the load is still running -- from the moment we suppress the loading menu.
    void RaiseSwapBlack(const char* a_reason)
    {
        if (!g_config.visualEnvelope || g_config.envelopeBlackHoldMs == 0) {
            return;
        }
        CacheForms();
        if (g_fadeHoldImod) {
            RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeHoldImod, g_config.envelopeFadeStrength, nullptr);
            logger::info("[Visual] swap black raised early ({})", a_reason);
        }
    }

    // Hold the frame black for a beat after the cell swap, then fade in. One-shot, bounded,
    // main-thread work only.
    void HoldBlackThenFadeIn(std::uint32_t a_holdMs)
    {
        const float strength = g_config.envelopeFadeStrength;
        CacheForms();
        // Prefer the engine's dedicated hold stage over re-triggering the fade record.
        if (g_fadeHoldImod) {
            RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeHoldImod, g_config.envelopeFadeStrength, nullptr);
        } else if (g_fadeImod) {
            RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeImod, g_config.envelopeFadeStrength, nullptr);
        }
        logger::info("[Visual] holding black for {} ms to mask pop-in (strength {:.2f})",
            a_holdMs, g_config.envelopeFadeStrength);
        std::thread([a_holdMs, strength]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(a_holdMs));
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([a_holdMs]() {
                    // The hold was already raised early (at suppression); this only releases it.
                    (void)a_holdMs;
                    ReleaseSwapBlack();
                });
            }
        }).detach();
    }

    // Fired exactly when the cell swap lands, to hide the hard cut from "door half open" to
    // "inside". Deliberately short: this is a cut mask, not the loading screen we just removed.
    // Black-family records (FadeToBlack / Hold) are released with FadeToBlackBack; any other record
    // is simply stopped after the window.
    void ApplyCutMask()
    {
        const std::uint32_t imodId = g_config.cutMaskImod;
        const std::uint32_t maskMs = g_config.cutMaskMs;
        const float         strength = g_config.envelopeFadeStrength;
        auto* rec = RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(imodId);
        if (!rec) {
            logger::warn("[Visual] cut mask record 0x{:08X} not found; skipped", imodId);
            return;
        }
        const bool  blackFamily = (imodId == 0x000F756D || imodId == 0x000F756E);
        const float fadeOut = g_config.cutMaskFadeOut;
        RE::ImageSpaceModifierInstanceForm::Trigger(rec, strength, nullptr);
        logger::info("[Visual] cut mask 0x{:08X} for {} ms ({}, fade-out {:.2f}s)", imodId, maskMs,
            blackFamily ? "black family" : "generic record", blackFamily ? 0.0f : fadeOut);
        std::thread([imodId, blackFamily, fadeOut, maskMs, strength]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(maskMs));
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([imodId, blackFamily, fadeOut, strength]() {
                    CacheForms();
                    if (blackFamily) {
                        // Same rule as HoldBlackThenFadeIn: never rely on a triggered record to
                        // clear an active one -- release the mask explicitly first.
                        if (g_fadeImod) {
                            RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
                        }
                        if (g_fadeHoldImod) {
                            RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
                        }
                        if (g_fadeBackImod) {
                            RE::ImageSpaceModifierInstanceForm::Trigger(g_fadeBackImod, strength, nullptr);
                        }
                        return;
                    }
                    if (auto* r = RE::TESForm::LookupByID<RE::TESImageSpaceModifier>(imodId)) {
                        (void)r;
                        // Papyrus documents RemoveCrossFade as "fading it out over the specified
                        // number of seconds"; CommonLib wraps it as StopCrossFade. A hard Stop()
                        // snaps the mask off, which reads as a flash on non-black records.
                        if (fadeOut > 0.0f) {
                            RE::ImageSpaceModifierInstanceForm::StopCrossFade(fadeOut);
                        } else {
                            RE::ImageSpaceModifierInstanceForm::Stop(r);
                        }
                    }
                });
            }
        }).detach();
    }

    // Always runs for a routed (real) transition: the timing is the whole point -- it tells us how
    // much of the perceived wait is actual cell I/O versus UI/fade overhead. Suppression itself is
    // captured here as a separate flag, so both modes are measured identically.
    void ArmLoadingMenuSuppression(const char* a_reason, bool a_isAutoCaveDoor, RE::FormID a_destCell)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* fromCell = player ? player->GetParentCell() : nullptr;
        g_suppressWanted.store(g_config.suppressLoadingMenu || g_config.globalMenuSuppression);
        // ---- route C (v1.5.0): always cancel the ENGINE's own fade-to-black ------------------
        // The engine raises FadeToBlackImod / FadeToBlackHoldImod when a load-door transition starts
        // and holds the screen black until it fades back in after the load. Measured on this setup
        // that is exactly the "it goes black for a moment" people report -- it survives with
        // every effect of ours switched off. We never add black of our own; we only STOP those two
        // records, so the last frame simply stays on screen until the new cell is there.
        // NOTE: STOP them, never try to cover them -- a Hold record that is never stopped leaves a
        // permanently black screen (that accident is in the archive, do not repeat it).
        g_noBlackWanted.store(true);
        presentfreeze::TraceArm();
        presentfreeze::ArmAutoGate();
        // v1.5.5: the Present hook is no longer used for drawing -- it exists solely to withhold the
        // engine's empty frames at the cut. Installed here because this is the first moment a swap
        // chain is guaranteed to exist.
        if (auto* sc = presentfreeze::CurrentSwapChain()) {
            presentfreeze::InstallOn(sc);
        }
        // Private build: hold the last frame over the load, dissolve once the new cell is drawn.
        // Only when the loading menu is going to be hidden -- otherwise we would cover the menu.
        if (g_travelFxLoad.load()) {
            g_suppressWanted.store(true);   // the travel effect needs the menu hidden regardless of the door setting
        } else if (g_suppressWanted.load()) {
            voidoverlay::Arm();
        }
        LogOpenMenus("door activation");
        HideMenuIfOpen("Mist Menu");
        StopEngineBlack("at door activation");
        g_voidStep.store(-1);
        g_probeWorldLastCell3D = 0;
        g_probeWorldLastMusic = 0;
        WorldProbeBegin();
        g_noBlackStartAt.store(NowMs());
        g_suppressFromCell.store(fromCell ? fromCell->GetFormID() : 0);
        g_suppressStarted.store(NowMs());
        g_suppressDeadline.store(NowMs() + static_cast<std::int64_t>(CoveredLoad() ? g_config.travelFxMaxHoldMs : g_config.transitionWindowMs));
        g_fallbackAt.store(0);   // only armed once we actually suppress a menu
        g_menuWasSuppressed.store(false);
        g_suppressArmed.store(true);

        logger::info("[Timers] in effect for this transition: postLoad={} ms, queueBudget={} ms, fadeToBlack={} s, preset={}",
            g_config.postLoadUpdateTimeMS, g_config.loadingQueuedPriorityBudgetMs,
            g_config.fadeToBlackFadeSeconds,
            g_config.postLoadUpdateTimeMS >= 1900.0f ? "stable" : (g_config.postLoadUpdateTimeMS <= 150.0f ? "fastest/balanced" : "custom"));
        logger::info("[Tier2] real transition started (cell 0x{:08X}, door {}, menu suppression {}, no-black {}, window {} ms, reason: {})",
            g_suppressFromCell.load(), a_isAutoCaveDoor ? "auto/frameless" : "manual",
            g_config.suppressLoadingMenu ? "ON" : "OFF",
            g_noBlackWanted.load() ? "FREEZE" : "fade",
            g_config.suppressTimeoutMs, a_reason);
        // One-shot watchdog. Bounded loop, posts at most one task per step, never re-queues itself
        // (the AddTask feedback loop is what froze this project twice before).
        const std::uint32_t myGen = ++g_suppressGen;
        std::thread([myGen]() {
            constexpr int kStepMs = 25;
            const int kMaxSteps = CoveredLoad() ? 2800 : 600;  // 70 s for covered loads, 15 s for plain doors
            bool fallbackPosted = false;
            for (int i = 0; i < kMaxSteps; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kStepMs));
                if (g_suppressGen.load() != myGen) {
                    return;   // a newer transition took over
                }
                if (!g_suppressArmed.load() && !g_probeArmed.load()) {
                    return;   // transition done AND assembly probe done
                }
                // Fallback re-post: measured from the moment we ACTUALLY suppressed a menu, because
                // the engine may ask for it a full second after the activation.
                const auto fbAt = g_fallbackAt.load();
                if (!fallbackPosted && fbAt != 0 && NowMs() > fbAt) {
                    fallbackPosted = true;
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([myGen]() {
                            if (g_suppressGen.load() != myGen || !g_menuWasSuppressed.load()) {
                                return;
                            }
                            // Stop suppressing from now on, otherwise the re-posted menu would be
                            // swallowed again by this same window and the player would see nothing.
                            g_suppressWanted.store(false);
                            easein::ShowHudNow();   // [Safe] or the loading screen would be invisible
                            voidoverlay::Disarm("menu re-posted (fallback)");
                            logger::warn("[Tier2] suppressed menu did not complete in time; re-posting the LoadingMenu (fallback)");
                            if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                                q->AddMessage(RE::BSFixedString(RE::LoadingMenu::MENU_NAME.data()),
                                    RE::UI_MESSAGE_TYPE::kShow, nullptr);
                            }
                        });
                    }
                }
                if (NowMs() > g_suppressDeadline.load()) {
                    logger::info("[Tier2] transition window elapsed without a cell change; stopping measurement");
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([] { AnnounceLoad(false); });   // [Compat]
                    }
                    g_suppressArmed.store(false);
                    g_travelFxLoad.store(false);
                    voidoverlay::Disarm("transition window elapsed without a cell change");
                    // Never leave our black up: if the transition never reports back, release it.
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask([myGen]() {
                            if (g_suppressGen.load() != myGen) {
                                return;
                            }
                            ReleaseSwapBlack();
                            EndFreezeFrame();
                        });
                    }
                    return;
                }
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([myGen]() {
                        if (g_suppressGen.load() != myGen) {
                            return;
                        }
                        if (!g_suppressArmed.load() && !g_probeArmed.load()) {
                            return;
                        }
                        // Probe first: it is independent of the transition state, and this step may be
                        // the very one that reports the transition as finished.
                        if (g_probeArmed.load()) {
                            ProbeSample();
                        }
                        if (!g_suppressArmed.load()) {
                            return;   // probe-only step; the transition itself already completed
                        }
                        // No-black mode: keep cancelling the engine's fade records for the whole
                        // transition window. The last presented frame then simply holds until the
                        // new cell shows up -- a freeze instead of a fade.
                        if (g_noBlackWanted.load() && NowMs() >= g_noBlackStartAt.load()) {
                            CacheForms();
                            // Only cancel the records that DARKEN the screen. Cancelling the fade-BACK
                            // as well makes the engine's own fade-in restart every 25 ms, which reads
                            // as strobing (the "scene spins into place" effect) right at the swap.
                            if (g_fadeImod) {
                                RE::ImageSpaceModifierInstanceForm::Stop(g_fadeImod);
                            }
                            if (g_fadeHoldImod) {
                                RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
                            }
                        }
                        // v1.2.3: keep the dead void moving (ramps an IMAD darkening in steps).
                        if (!g_noBlackWanted.load()) {
                            VoidRampStep();
                        }
                        WorldProbeStep();
                        auto* pl = RE::PlayerCharacter::GetSingleton();
                        if (!pl) {
                            return;
                        }
                        auto* cell = pl->GetParentCell();
                        if (cell && cell->GetFormID() != g_suppressFromCell.load()) {
                            const auto ms = NowMs() - g_suppressStarted.load();
                            g_cellLoadMs[cell->GetFormID()] = static_cast<std::uint32_t>(ms);
                            const bool wasSuppressed = g_menuWasSuppressed.exchange(false);
                            g_suppressArmed.store(false);
                            logger::info("[Tier2] real transition finished in {} ms (cell 0x{:08X} -> 0x{:08X}, menu {})",
                                ms, g_suppressFromCell.load(), cell->GetFormID(),
                                wasSuppressed ? "suppressed" : "shown");
                            // v1.4.0: this is the cut -- the new cell is current and the engine's
                            // ~350 ms scene spin starts here. Hand the overlay over to its pulse.
                            LogOpenMenus("cut");
                            g_graceSuppressUntil.store(NowMs() + 2500);
                            HideMenuIfOpen("Loading Menu");   // safety net: never leave it up
                            StopEngineBlack("at cut (fade-back raises black again)");
                            presentfreeze::HoldAtCut(g_config.holdAtCutMs);
                            presentfreeze::ArmHitchWindow();
                            voidoverlay::Cut();
                            AnnounceLoad(false);   // [Compat] the load is over
                            g_travelFxLoad.store(false);
                            // v1.4.12: the counter used to be bumped only on the frozen pocket path, so the
                            // menu always showed 0 even though transitions were happening.
                            ++g_transitionCount;
                            WorldProbeFinish();
                            if (g_config.probeAssembly) {
                                ProbeArm();
                            }
                            // v1.2.3: if the void was being dimmed, take it to full black across the
                            // spin and then fade into the finished cell. This is the transition the
                            // user asked for: dim over the dead window, black over the spin, fade in.
                            if (g_config.voidDarken > 0.01f) {
                                VoidFadeOutAtSwap(g_config.voidHoldMs);
                            }
                            // v1.2.2: cover the SWAP only, for at most uCoverAtSwapMs. The engine
                            // attaches the new scene over ~21 frames at 60 fps (~350 ms), which is
                            // the only part worth hiding; the load before it is static anyway.
                            if (g_config.coverAtSwapMs > 0 && g_config.voidDarken <= 0.01f) {
                                if (g_config.coverAtSwapMode == 2) {
                                    HoldBlackThenFadeIn(g_config.coverAtSwapMs);
                                } else if (g_config.freezeFramePresent || g_config.coverAtSwapMode == 1) {
                                    // Skip presentation from here; the display keeps the pre-spin frame.
                                    if (!presentfreeze::g_suppressPresent.load()) {
                                        auto* sc = presentfreeze::CurrentSwapChain();
                                        if (sc && presentfreeze::InstallOn(sc)) {
                                            presentfreeze::g_skippedFrames.store(0);
                                            presentfreeze::g_firstSkipLogged.store(false);
                                            presentfreeze::g_suppressStartAt.store(NowMs());
                                            presentfreeze::g_suppressPresent.store(true);
                                            logger::info("[Cover] swap cover ON for {} ms (freeze the presented image)", g_config.coverAtSwapMs);
                                            EndFreezeFrameWith(g_config.coverAtSwapMs);
                                        }
                                    }
                                }
                            }
                            if (g_noBlackWanted.load()) {
                                // v1.5.2: route C adds NO mask here. The old "cut mask" was a black
                                // image-space drip (sCutMaskImod) triggered right at the cut; with route
                                // C always on it fired on every single transition -- and THAT was the
                                // "black flash" people report, surviving every visual switch
                                // because it is part of the core path, not the visual layer.
                            } else if (g_config.visualEnvelope && g_config.envelopeBlackHoldMs > 0) {
                                HoldBlackThenFadeIn(g_config.envelopeBlackHoldMs);
                            } else {
                                // Safety net. A holding record that never gets released leaves the
                                // player permanently black with only the UI visible, so make sure
                                // nothing of ours is still holding the screen.
                                CacheForms();
                                if (g_fadeHoldImod) {
                                    RE::ImageSpaceModifierInstanceForm::Stop(g_fadeHoldImod);
                                }
                            }
                            // Always, on every path: never leave presentation suppressed.
                            EndFreezeFrame();
                        }
                    });
                }
            }
        }).detach();
    }

    void InstallLoadingMenuHook()
    {
        constexpr std::size_t kProcessMessageIdx = 0x04;
        constexpr std::uint32_t kPageExecuteReadWrite = 0x40;

        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_LoadingMenu[0] };
        auto** table = reinterpret_cast<void**>(vtbl.address());
        if (!table || !IsExecutableAddress(table[kProcessMessageIdx])) {
            logger::warn("[Tier2] LoadingMenu vtable[0x04] is not executable; menu suppression unavailable");
            return;
        }
        std::uint32_t oldProt = 0;
        if (REX::W32::VirtualProtect(&table[kProcessMessageIdx], sizeof(void*), kPageExecuteReadWrite, &oldProt)) {
            g_origLoadingProcessMessage = reinterpret_cast<ProcessMessage_t>(table[kProcessMessageIdx]);
            table[kProcessMessageIdx] = reinterpret_cast<void*>(&HookedLoadingMenuProcessMessage);
            REX::W32::VirtualProtect(&table[kProcessMessageIdx], sizeof(void*), oldProt, &oldProt);
            logger::info("[Tier2] LoadingMenu ProcessMessage hook installed at vtable[0x04]=0x{:X} (suppression {})",
                reinterpret_cast<std::uintptr_t>(g_origLoadingProcessMessage),
                g_config.suppressLoadingMenu ? "ENABLED" : "disabled");
        } else {
            logger::warn("[Tier2] could not make the LoadingMenu vtable writable");
        }
    }

    // =========================================================================
    //  Tier 3: proximity pre-warm
    //  Stands in for the "pin the last N interiors" idea the original design proposed: SE exposes no
    //  way to stop a cell from being detached (TESObjectCELL has IsAttached() and a cellDetached
    //  flag, but no Attach/Detach method), so keeping 3D trees alive is not reachable from the
    //  public surface. Instead we make the *re-load* cheap: while the player walks near a load
    //  door whose destination is routed to a real transition, demand that cell's models into the
    //  model DB ahead of time, using our own blueprint index (no need for the cell to be loaded).
    // =========================================================================
    std::atomic<bool>                            g_prewarmThreadStarted{ false };
    std::mutex                                   g_prewarmMutex;
    std::unordered_map<RE::FormID, std::int64_t> g_prewarmLastMs;
    std::unordered_set<std::string>              g_demandedPaths;   // never demand the same NIF twice
    std::unordered_map<RE::FormID, std::uint32_t> g_prewarmCount;   // per-cell, to honour the cap

    const char* ModelPathOf(RE::TESForm* a_form)
    {
        if (!a_form) {
            return nullptr;
        }
        using FT = RE::FormType;
        switch (a_form->GetFormType()) {
        case FT::Static:
            if (auto* x = a_form->As<RE::TESObjectSTAT>()) return x->GetModel();
            break;
        case FT::MovableStatic:
            if (auto* x = a_form->As<RE::BGSMovableStatic>()) return x->GetModel();
            break;
        case FT::Tree:
            if (auto* x = a_form->As<RE::TESObjectTREE>()) return x->GetModel();
            break;
        case FT::Activator:
            if (auto* x = a_form->As<RE::TESObjectACTI>()) return x->GetModel();
            break;
        case FT::Furniture:
            if (auto* x = a_form->As<RE::TESFurniture>()) return x->GetModel();
            break;
        case FT::Door:
            if (auto* x = a_form->As<RE::TESObjectDOOR>()) return x->GetModel();
            break;
        case FT::Flora:
            if (auto* x = a_form->As<RE::TESFlora>()) return x->GetModel();
            break;
        default:
            break;
        }
        return nullptr;
    }

    // Demands at most a slice of the cell's models, so a single frame never stalls: whatever does
    // not fit in the budget is picked up by the next tick (already-demanded paths are skipped).
    // Returns true when the cell is finished (all unique models handled or the cap was hit).
    bool PrewarmChunk(RE::FormID a_cellID, const BlueprintCell& a_bp)
    {
        constexpr std::int64_t kBudgetMs = 8;
        const auto t0 = NowMs();
        std::uint32_t demanded = 0;
        std::uint32_t noModel = 0;
        std::uint32_t pending = 0;

        for (const auto& r : a_bp.refs) {
            if (NowMs() - t0 > kBudgetMs) {
                pending = 1;   // more work left
                break;
            }
            {
                std::lock_guard<std::mutex> lk(g_prewarmMutex);
                if (g_prewarmCount[a_cellID] >= g_config.prewarmModelCap) {
                    break;
                }
            }
            const char* path = ModelPathOf(RE::TESForm::LookupByID(r.baseID));
            if (!path || !*path) {
                ++noModel;
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(g_prewarmMutex);
                if (!g_demandedPaths.insert(std::string(path)).second) {
                    continue;   // already resident from an earlier cell
                }
                ++g_prewarmCount[a_cellID];
            }
            RE::BSModelDB::DBTraits::ArgsType args{};
            RE::NiPointer<RE::NiNode> model;
            (void)RE::BSModelDB::Demand(path, model, args);
            ++demanded;
        }

        if (demanded > 0) {
            logger::info("[Tier3] prewarmed {} model(s) in {} ms for cell 0x{:08X} ({}, cap {}, refs {})",
                demanded, NowMs() - t0, a_cellID, pending ? "more pending" : "slice done",
                g_config.prewarmModelCap, a_bp.refs.size());
        }
        (void)noModel;
        return pending == 0;
    }

    void PrewarmTick()
    {
        if (!g_config.prewarmNearbyDoors) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        auto* cell = player->GetParentCell();
        if (!cell) {
            return;
        }
        const RE::NiPoint3 ppos = player->GetPosition();
        const float r2 = g_config.prewarmRadius * g_config.prewarmRadius;

        std::vector<RE::FormID> candidates;
        cell->ForEachReference([&](RE::TESObjectREFR* a_ref) -> RE::BSContainer::ForEachResult {
            auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
            if (!base || base->GetFormType() != RE::FormType::Door) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            auto* xTel = a_ref->extraList.GetByType<RE::ExtraTeleport>();
            if (!xTel || !xTel->teleportData) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const RE::NiPoint3 d = a_ref->GetPosition() - ppos;
            if (d.x * d.x + d.y * d.y + d.z * d.z > r2) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            auto linked = xTel->teleportData->linkedDoor.get();
            if (!linked) {
                linked = RE::TESObjectREFR::LookupByHandle(xTel->teleportData->linkedDoor.native_handle());
            }
            if (!linked) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            auto* dest = linked->GetParentCell();
            if (!dest) {
                dest = linked->GetSaveParentCell();
            }
            if (!dest) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto id = dest->GetFormID();
            // With pocket copies disabled (the default) EVERY door takes the engine's real
            // transition, so every destination cell is worth pre-warming. The old filter only kept
            // cells present in the exclude list -- in this mode that is just the 101 scripted cells,
            // so pre-warming did nothing for ordinary doors. That bug made Tier3 a no-op.
            if (!g_config.usePocketCopies || g_excludeCells.count(id) != 0) {
                candidates.push_back(id);
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        if (candidates.empty()) {
            return;
        }

        const auto snap = GetBlueprintSnapshot();
        if (!snap) {
            return;
        }
        const auto now = NowMs();
        for (const auto id : candidates) {
            {
                std::lock_guard<std::mutex> lk(g_prewarmMutex);
                const auto it = g_prewarmLastMs.find(id);
                if (it != g_prewarmLastMs.end() && now - it->second < 60000) {
                    continue;   // finished within the last minute
                }
            }
            const auto bpIt = snap->cells.find(id);
            if (bpIt == snap->cells.end() || bpIt->second.refs.empty()) {
                continue;
            }
            if (PrewarmChunk(id, bpIt->second)) {
                std::lock_guard<std::mutex> lk(g_prewarmMutex);
                g_prewarmLastMs[id] = now;   // only mark "done" once the cell is fully handled
            }
            return;   // one cell per tick keeps the frame cost bounded
        }
    }

    void StartPrewarmThread()
    {
        if (g_prewarmThreadStarted.exchange(true)) {
            return;
        }
        logger::info("[Tier3] prewarm poller started (radius {:.0f}, cap {}, 200 ms interval, enabled={})",
            g_config.prewarmRadius, g_config.prewarmModelCap, g_config.prewarmNearbyDoors ? "yes" : "no");
        std::thread([]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (!g_config.prewarmNearbyDoors) {
                    continue;   // do not even queue a task when the feature is off
                }
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() { PrewarmTick(); });
                }
            }
        }).detach();
    }

    bool HookedContActivate(RE::TESBoundObject* a_this, RE::TESObjectREFR* a_targetRef, RE::TESObjectREFR* a_activatorRef,
        std::uint8_t a_arg3, RE::TESBoundObject* a_object, std::int32_t a_targetCount)
    {
        if (a_targetRef && !g_bypassContainerHook.load()) {
            RE::FormID origContID = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                const auto it = g_clonedContToOriginalCont.find(a_targetRef->GetFormID());
                if (it != g_clonedContToOriginalCont.end()) {
                    origContID = it->second;
                }
            }
            if (origContID != 0) {
                if (auto* origContRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(origContID)) {
                    logger::info("[LF] Redirecting container activate 0x{:08X} -> original 0x{:08X}",
                        a_targetRef->GetFormID(), origContID);
                    g_bypassContainerHook.store(true);
                    const bool res = origContRef->ActivateRef(a_activatorRef, a_arg3, a_object, a_targetCount, false);
                    g_bypassContainerHook.store(false);
                    return res;
                }
            }
        }
        return g_origContActivate ? g_origContActivate(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount) : false;
    }

    // =========================================================================
    //  TESObjectDOOR::Activate Hook (vtable[0x37])
    // =========================================================================
    bool HookedDoorActivate(RE::TESBoundObject* a_this, RE::TESObjectREFR* a_targetRef, RE::TESObjectREFR* a_activatorRef,
        std::uint8_t a_arg3, RE::TESBoundObject* a_object, std::int32_t a_targetCount);

    // [EaseIn] When the player uses a load door, the HUD is hidden and the door is held back a few frames, so
    // the picture that gets held shows the world without the "Open" prompt. The HUD stays hidden until the
    // dissolve starts (whether the HUD is drawn into the frame or kept in a separate layer by the shader mod).
    namespace easein
    {
        struct Pending
        {
            bool                active = false;
            RE::TESBoundObject* door = nullptr;
            RE::ObjectRefHandle target;
            RE::ObjectRefHandle activator;
            std::uint8_t        arg3 = 0;
            RE::TESBoundObject* object = nullptr;
            std::int32_t        count = 0;
        } g_pending;                              // main thread
        bool              g_replaying = false;     // main thread
        std::atomic<bool> g_hudHidden{ false };
        std::atomic<int>  g_framesLeft{ 0 };

        void RestoreHud(const char* a_why)
        {
            if (!g_hudHidden.load()) {
                return;
            }
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([a_why] {
                    if (g_pending.active) {
                        return;   // a door is still waiting for its picture
                    }
                    if (g_hudHidden.exchange(false)) {
                        if (auto* ui = RE::UI::GetSingleton()) {
                            ui->ShowMenus(true);
                        }
                        logger::info("[EaseIn] HUD shown again ({})", a_why);
                    }
                });
            }
        }

        // Present thread, only while no transition is on screen: if this mod hid the HUD and nothing has claimed it
        // for a moment, bring it back. Whatever path was missed, menus cannot stay invisible.
        void IdleWatch()
        {
            static std::atomic<std::int64_t> s_since{ 0 };
            if (!g_hudHidden.load(std::memory_order_relaxed) || g_framesLeft.load(std::memory_order_relaxed) > 0) {
                s_since.store(0, std::memory_order_relaxed);
                return;
            }
            const std::int64_t now = NowMs();
            std::int64_t       since = s_since.load(std::memory_order_relaxed);
            if (since == 0) {
                s_since.store(now, std::memory_order_relaxed);
            } else if (now - since > 2500) {
                s_since.store(0, std::memory_order_relaxed);
                RestoreHud("nothing was using the hidden HUD");
            }
        }

        void ShowHudNow()
        {
            if (g_hudHidden.exchange(false)) {
                if (auto* ui = RE::UI::GetSingleton()) {
                    ui->ShowMenus(true);
                }
                logger::info("[EaseIn] HUD shown again (a loading screen is coming up)");
            }
        }

        void HideHudNow()
        {
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->IsShowingMenus()) {
                ui->ShowMenus(false);
                g_hudHidden.store(true);
            }
        }

        // Main thread: the clean picture exists, go through the door for real.
        void Finish(const char* a_why)
        {
            if (!g_pending.active) {
                return;
            }
            g_pending.active = false;
            g_framesLeft.store(0);
            const auto target = g_pending.target.get();
            const auto activator = g_pending.activator.get();
            if (target && activator) {
                g_replaying = true;
                HookedDoorActivate(g_pending.door, target.get(), activator.get(), g_pending.arg3, g_pending.object, g_pending.count);
                g_replaying = false;
            }
            logger::info("[EaseIn] door used after the HUD-free frames ({}); overlay {}", a_why, voidoverlay::Active() ? "armed" : "not armed");
            if (!voidoverlay::Active()) {
                if (NowMs() - g_playerDoorPassedAt.load() < 4000) {
                    // [Safe] the door was passed on to the game (opened with its key): the transition starts a moment later,
                    // when the load is requested. Keep the HUD away until then; bring it back if nothing comes.
                    mcm::After(1500, [] {
                        if (!voidoverlay::Active()) {
                            RestoreHud("no transition followed");
                        }
                    });
                } else {
                    RestoreHud("no transition followed");
                }
            }
        }

        // Present thread, while no overlay is up: counts the frames drawn with the HUD hidden.
        void OnIdlePresent()
        {
            int left = g_framesLeft.load(std::memory_order_relaxed);
            if (left <= 0) {
                return;
            }
            if (g_framesLeft.compare_exchange_strong(left, left - 1) && left == 1) {
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([] { Finish("frames drawn"); });
                }
            }
        }

        // Main thread, from the door hook. True = the activation was taken over and will be replayed.
        bool Begin(RE::TESBoundObject* a_this, RE::TESObjectREFR* a_targetRef, RE::TESObjectREFR* a_activatorRef, std::uint8_t a_arg3,
            RE::TESBoundObject* a_object, std::int32_t a_targetCount)
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                return false;
            }
            g_pending.active = true;
            g_pending.door = a_this;
            g_pending.target = a_targetRef->GetHandle();
            g_pending.activator = a_activatorRef->GetHandle();
            g_pending.arg3 = a_arg3;
            g_pending.object = a_object;
            g_pending.count = a_targetCount;
            if (ui->IsShowingMenus()) {
                ui->ShowMenus(false);
                g_hudHidden.store(true);
            }
            g_framesLeft.store(static_cast<int>(g_config.seamlessDoorDelayFrames));
            // Safety nets: the door opens even if no frame is drawn, and the HUD always comes back.
            std::thread([] {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([] { Finish("safety timer"); });
                }
                std::this_thread::sleep_for(std::chrono::seconds(25));
                if (!voidoverlay::Active()) {
                    RestoreHud("safety timer");
                }
            }).detach();
            return true;
        }
    }

    bool HookedDoorActivate(RE::TESBoundObject* a_this, RE::TESObjectREFR* a_targetRef, RE::TESObjectREFR* a_activatorRef,
        std::uint8_t a_arg3, RE::TESBoundObject* a_object, std::int32_t a_targetCount)
    {
        auto callOriginal = [&]() {
            return g_origDoorActivate ? g_origDoorActivate(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount) : false;
        };

        if (!g_config.enabled || !a_targetRef || !a_activatorRef) {
            return callOriginal();
        }
        // [EaseIn] a load door used by the player: take it over for a few HUD-free frames, then replay it.
        if (g_config.seamless && g_config.seamlessHideHud && !easein::g_replaying && !voidoverlay::Failed()) {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (pc && a_activatorRef == pc && a_targetRef->extraList.HasType<RE::ExtraTeleport>()) {
                if (easein::g_pending.active) {
                    return true;   // already on the way through this door
                }
                if ((!a_targetRef->IsLocked() || PlayerHasKeyFor(a_targetRef)) && !a_targetRef->IsActivationBlocked() && !voidoverlay::Active() &&
                    easein::Begin(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount)) {
                    return true;
                }
            }
        }
        // [Rules] doors with a transition of their own (lifts, wayshrines, portals, the rowboat): the effect is armed
        // here and then owns this load; the usual hold-and-dissolve stands back.
        if (g_config.travelFx && !voidoverlay::Active() && !voidoverlay::Failed()) {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (pc && a_activatorRef == pc && a_targetRef->extraList.HasType<RE::ExtraTeleport>() && !a_targetRef->IsLocked() &&
                !a_targetRef->IsActivationBlocked()) {
                float       dir = 1.0f;
                const char* why = "";
                const int   kind = rules::DoorStyle(a_this, dir, why);
                if (kind != voidoverlay::kFxNone) {
                    voidoverlay::SetFxDirection(dir);
                    travelfx::NotePending(kind, 8000, why);
                    voidoverlay::ArmTravelFx(kind, why);
                    mcm::After(2000, [] {
                        if (voidoverlay::TravelFxActive() && !voidoverlay::CutSeen() && !g_menuWasSuppressed.load()) {
                            ReleaseTravelCover("the door did not lead to a load");
                        }
                    });
                }
            }
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        // Diagnostic (v0.10.7): log door activations only when explicitly enabled -- NPCs activate
        // doors constantly (502 of 559 lines in one session), which drowns the real signal.
        if (g_config.logDoorActivations) {
            logger::info("[Door] activate ref=0x{:08X} base=0x{:08X} by={} locked={} state={} activity={}",
                a_targetRef->GetFormID(),
                a_this ? a_this->GetFormID() : 0,
                (player && a_activatorRef == player) ? "player" : "other",
                a_targetRef->IsLocked() ? "yes" : "no",
                a_targetRef->IsActivationBlocked() ? "blocked" : "ok",
                g_transitionInProgress.load() ? "transition" : "idle");
        }
        if (!player || a_activatorRef != player) {
            return callOriginal();
        }
        if (g_transitionInProgress.load()) {
            return true;
        }

        // Resolve source door (either a real door or a cloned pocket door)
        RE::FormID         sourceDoorID = a_targetRef->GetFormID();
        RE::TESObjectREFR* effectiveDoor = a_targetRef;
        bool               isClonedPocketDoor = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            auto it = g_clonedDoorToOriginalDoor.find(sourceDoorID);
            if (it != g_clonedDoorToOriginalDoor.end()) {
                sourceDoorID = it->second;
                isClonedPocketDoor = true;
                if (auto* origDoor = RE::TESForm::LookupByID<RE::TESObjectREFR>(sourceDoorID)) {
                    effectiveDoor = origDoor;
                }
            }
        }

        if (g_config.sneakForcesVanillaLoad && player->IsSneaking()) {
            logger::info("[LF] Player is sneaking; bypassing LoadingFlow for door 0x{:08X}", sourceDoorID);
            g_activeLoadingFlowID.store(0);
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        if (effectiveDoor->IsLocked()) {
            logger::info("[LF] Door 0x{:08X} is locked; delegating to vanilla lock handler", sourceDoorID);
            if (PlayerHasKeyFor(effectiveDoor) && effectiveDoor->extraList.HasType<RE::ExtraTeleport>()) {
                g_playerDoorPassedAt.store(NowMs());   // [Safe] it will open with the key and load
            }
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        // Extract teleport destination from live ExtraTeleport OR offline Blueprint door table
        RE::FormID         destCellID = 0;
        RE::TESObjectCELL* destCell = nullptr;
        bool               destIsInterior = false;
        RE::NiPoint3       telPos{};
        RE::NiPoint3       telRot{};
        bool               hasTel = false;

        if (auto* xTel = effectiveDoor->extraList.GetByType<RE::ExtraTeleport>(); xTel && xTel->teleportData) {
            telPos = xTel->teleportData->position;
            telRot = xTel->teleportData->rotation;
            auto linkedDoorPtr = xTel->teleportData->linkedDoor.get();
            if (!linkedDoorPtr) {
                linkedDoorPtr = RE::TESObjectREFR::LookupByHandle(xTel->teleportData->linkedDoor.native_handle());
            }
            if (linkedDoorPtr) {
                destCell = linkedDoorPtr->GetParentCell();
                if (!destCell) {
                    destCell = linkedDoorPtr->GetSaveParentCell();
                }
                if (destCell) {
                    destCellID = destCell->GetFormID();
                    destIsInterior = destCell->IsInteriorCell();
                    hasTel = true;
                }
            }
        }

        // P1-4: take one immutable Blueprint snapshot for the whole transition (no g_bpMutex held
        // while we call into the engine, and a Force Rebuild can safely swap it mid-transition).
        const auto bpSnap = GetBlueprintSnapshot();

        // Fallback to offline Blueprint door table when source door or linked door is not resolved in memory
        if (!hasTel && bpSnap) {
            const auto dIt = bpSnap->doors.find(sourceDoorID);
            if (dIt != bpSnap->doors.end()) {
                telPos = { dIt->second.telPos[0], dIt->second.telPos[1], dIt->second.telPos[2] };
                telRot = { dIt->second.telRot[0], dIt->second.telRot[1], dIt->second.telRot[2] };
                const auto linkedIt = bpSnap->doors.find(dIt->second.linkedDoorID);
                if (linkedIt != bpSnap->doors.end()) {
                    destCellID = linkedIt->second.parentCellID;
                    destIsInterior = (linkedIt->second.isParentInterior != 0);
                    destCell = RE::TESForm::LookupByID<RE::TESObjectCELL>(destCellID);
                    hasTel = true;
                }
            }
        }

        if (!hasTel || destCellID == 0) {
            if (a_targetRef->extraList.HasType<RE::ExtraTeleport>()) {
                g_playerDoorPassedAt.store(NowMs());   // [Safe] destination not resolved here, but it is a load door
            }
            return callOriginal();
        }

        RE::TESObjectCELL* playerHostCell = player->GetParentCell();
        RE::TESWorldSpace* playerHostWorld = player->GetWorldspace();
        if (!playerHostCell) {
            return callOriginal();
        }

        // ---------------------------------------------------------------------
        //  Case A: Destination is an EXTERIOR cell (Pocket Interior -> Exterior)
        // ---------------------------------------------------------------------
        if (!destIsInterior) {
            if (!isClonedPocketDoor || !g_config.allowPocketToExterior) {
                g_activeLoadingFlowID.store(0);
                // Private build: an exit to the world is a real transition like any other door; hold the last
                // frame over the (longer) load and dissolve into the exterior once it is drawn and steady.
                if (!isClonedPocketDoor && destCellID != playerHostCell->GetFormID()) {
                    ArmLoadingMenuSuppression("real transition to the exterior", IsAutoOrFramelessDoor(a_targetRef), destCellID);
                }
                return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
            }
            if (!playerHostWorld || (destCell && !destCell->IsAttached())) {
                logger::info("[LF] Exterior destination cell 0x{:08X} is not attached; falling back to vanilla load", destCellID);
                g_activeLoadingFlowID.store(0);
                return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
            }

            const bool isAutoCave = IsAutoOrFramelessDoor(a_targetRef);
            const std::uint32_t delayMs = isAutoCave ? (std::min)(g_config.doorOpenDelayMs, 120u) : g_config.doorOpenDelayMs;
            const RE::ObjectRefHandle doorHandle = a_targetRef->GetHandle();

            BeginDoorTransitionVisuals(a_targetRef, isAutoCave);
            ++g_transitionCount;

            auto finishExit = [destCellID, telPos, telRot, doorHandle, isAutoCave]() {
                const RE::FormID leftPocketID = g_activeLoadingFlowID.exchange(0);
                if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                    pl->SetPosition(telPos, true);
                    pl->SetHeading(telRot.z);
                }
                // P1-1: on the way out, put the NPCs of the pocket we just left back in their real cell.
                if (leftPocketID != 0) {
                    std::lock_guard<std::recursive_mutex> lk(g_mutex);
                    auto pkIt = g_pockets.find(leftPocketID);
                    if (pkIt != g_pockets.end()) {
                        TransferPocketActors(pkIt->second, false);
                    }
                }
                // L2: tell quest scripts we are back outside.
                FireFakeCellEvents(LookupCellOrNull(leftPocketID), LookupCellOrNull(destCellID), false);
                FinishDoorTransitionVisuals(doorHandle, isAutoCave);
                g_transitionInProgress.store(false);
                logger::info("[LF] >>> instant EXIT to Exterior cell 0x{:08X} pos=({:.1f},{:.1f},{:.1f}) yaw={:.2f}",
                    destCellID, telPos.x, telPos.y, telPos.z, telRot.z);
                if (g_config.showDebugNotifications) {
                    lfnotify::Show("Seamless Transitions: Instant Exit");
                }
            };

            if (delayMs == 0) {
                finishExit();
            } else {
                g_transitionInProgress.store(true);
                std::thread([delayMs, finishExit]() mutable {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                    if (auto* task = SKSE::GetTaskInterface()) {
                        task->AddTask(std::move(finishExit));
                    } else {
                        g_transitionInProgress.store(false);
                    }
                }).detach();
            }
            return true;
        }

        // ---------------------------------------------------------------------
        //  Case B: Destination is an INTERIOR cell (Ext->Int or Int->Int)
        // ---------------------------------------------------------------------
        if (g_excludeCells.count(destCellID) != 0) {
            // Hard exclude: real transition AND no menu suppression -- the per-cell escape hatch for
            // anything that misbehaves with a hidden menu.
            logger::info("[LF] Destination cell 0x{:08X} is in [ExcludeCells]; real transition, menu left alone", destCellID);
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        // Same-cell "transition": a mod door/stairs that teleports you elsewhere inside the SAME
        // cell. There is no cell change and therefore no loading screen to hide -- arming the
        // suppression/freeze machinery here would only cancel an unrelated fade.
        if (destCellID == playerHostCell->GetFormID()) {
            logger::info("[LF] Door 0x{:08X} teleports inside the same cell (0x{:08X}); leaving it vanilla",
                sourceDoorID, destCellID);
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        // Adaptive fallback: a cell we have already measured as slow gets the vanilla loading menu
        // instead of a long black gap. 0 disables the check.
        if (g_config.suppressMaxMs > 0) {
            const auto msIt = g_cellLoadMs.find(destCellID);
            if (msIt != g_cellLoadMs.end() && msIt->second > g_config.suppressMaxMs) {
                logger::info("[Tier2] cell 0x{:08X} last load measured {} ms (> uSuppressMaxMs {}); showing the vanilla menu",
                    destCellID, msIt->second, g_config.suppressMaxMs);
                return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
            }
        }

        // Master routing: with pocket copies disabled (the default) every door takes the engine's
        // own transition, so navmesh, NPCs, AI packages, combat, lighting and quest events are
        // exactly vanilla. LoadingFlow's only job here is to hide or mask the loading menu.
        if (!g_config.usePocketCopies) {
            ArmLoadingMenuSuppression("real transition (pocket copies disabled)", IsAutoOrFramelessDoor(a_targetRef), destCellID);
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        const bool isFromInterior = isClonedPocketDoor || playerHostCell->IsInteriorCell();
        if (isFromInterior && !g_config.allowInteriorToInterior) {
            ArmLoadingMenuSuppression("interior->interior disabled", IsAutoOrFramelessDoor(a_targetRef));
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        RE::NiPoint3 anchorXY = a_targetRef->GetPosition();
        const RE::NiPoint3 preEnterPos = player->GetPosition();
        const RE::NiPoint3 preEnterRot = player->GetAngle();
        const bool          isAutoCave = IsAutoOrFramelessDoor(a_targetRef);
        const std::uint32_t delayMs = isAutoCave ? (std::min)(g_config.doorOpenDelayMs, 120u) : g_config.doorOpenDelayMs;
        const RE::ObjectRefHandle doorHandle = a_targetRef->GetHandle();
        // L2: the cell the player is leaving (exterior, or the previous pocket's interior).
        RE::TESObjectCELL* fromCell = playerHostCell;

        // P0-1: the critical section below touches ONLY our own containers.
        // No engine calls, no `return`, and never finishEnter() inside it: finishEnter() takes this
        // same lock, so calling it under it (delayMs == 0) deadlocked the game thread outright, and
        // returning through the vanilla path while holding it re-entered the hooked Activate.
        RE::NiPoint3  destPocketPos{};
        RE::NiPoint3  destPocketRot = telRot;
        std::uint32_t slotIdx = 0;
        std::string   edidCopy;
        std::string   nameCopy;
        bool          fromBp = false;
        bool          pocketReady = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            if (isClonedPocketDoor && g_activeLoadingFlowID.load() != 0) {
                auto curIt = g_pockets.find(g_activeLoadingFlowID.load());
                if (curIt != g_pockets.end()) {
                    anchorXY.x = 0.5f * (curIt->second.boundMin.x + curIt->second.boundMax.x);
                    anchorXY.y = 0.5f * (curIt->second.boundMin.y + curIt->second.boundMax.y);
                }
            }
            // Build or retrieve the pocket immediately on T=0ms so its references are ready while the door cracks open!
            if (PocketInstance* pocket = GetOrCreatePocket(destCellID, destCell, playerHostCell, playerHostWorld, anchorXY, bpSnap)) {
                destPocketPos = telPos + pocket->pocketOrigin;
                slotIdx = pocket->slotIndex;
                edidCopy = pocket->interiorEditorID;
                nameCopy = pocket->interiorFullName;
                fromBp = pocket->builtFromBlueprint;
                // P1-2 / P1-3: remember where the player stood before the snap so Purge and Save
                // can put them back on solid ground instead of leaving them at Z=+26000.
                pocket->returnWorldPos = preEnterPos;
                pocket->returnWorldRot = preEnterRot;
                pocket->hasReturnWorldPos = true;
                g_lastExteriorReturnPos = preEnterPos;
                g_hasExteriorReturnPos.store(true);
                pocketReady = true;
            }
        }

        if (!pocketReady) {
            ArmLoadingMenuSuppression("pocket unavailable (quota / oversized cell)", IsAutoOrFramelessDoor(a_targetRef));
            return g_origDoorActivate(a_this, effectiveDoor, a_activatorRef, a_arg3, a_object, a_targetCount);
        }

        auto finishEnter = [destCellID, destPocketPos, destPocketRot, doorHandle, isAutoCave, isFromInterior, slotIdx, edidCopy, nameCopy, fromBp, fromCell]() {
            if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                g_activeLoadingFlowID.store(destCellID);
                pl->SetPosition(destPocketPos, true);
                pl->SetHeading(destPocketRot.z);
            }
            {
                std::lock_guard<std::recursive_mutex> lk(g_mutex);
                auto pkIt = g_pockets.find(destCellID);
                if (pkIt != g_pockets.end()) {
                    TransferPocketActors(pkIt->second, true);
                }
            }
            // L2: tell quest scripts we just walked into that interior.
            FireFakeCellEvents(fromCell, LookupCellOrNull(destCellID), true);
            FinishDoorTransitionVisuals(doorHandle, isAutoCave);
            g_transitionInProgress.store(false);

            logger::info("[LF] >>> instant ENTER [{}|{}] -> Slot #{} cell=0x{:08X} ({}) src={} pos=({:.1f},{:.1f},{:.1f}) yaw={:.2f}",
                isFromInterior ? "Int->Int" : "Ext->Int",
                isAutoCave ? "Cave/Auto" : "Door/Trapdoor",
                slotIdx, destCellID, edidCopy,
                fromBp ? "BLUEPRINT" : "LIVE",
                destPocketPos.x, destPocketPos.y, destPocketPos.z, destPocketRot.z);

            if (g_config.showDebugNotifications) {
                const auto msg = std::format("LoadingFlow: {} [Slot #{}]",
                    nameCopy.empty() ? edidCopy : nameCopy, slotIdx);
                lfnotify::Show(msg.c_str());
            }
        };

        BeginDoorTransitionVisuals(a_targetRef, isAutoCave);
        ++g_transitionCount;

        if (delayMs == 0) {
            finishEnter();
        } else {
            g_transitionInProgress.store(true);
            std::thread([delayMs, finishEnter]() mutable {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask(std::move(finishEnter));
                } else {
                    g_transitionInProgress.store(false);
                }
            }).detach();
        }
        return true;
    }

    // a_includeContainer: the container-activate hook only exists to redirect cloned pocket
    // containers to the real ones. With pocket copies disabled (v0.6.0+ default) that hook has
    // nothing to do, so we do not install it at all -- one less engine vtable patched, one less
    // thing that can go wrong.
    // Travel effects (private build): Black Books / Apocrypha books / Soul Cairn portals start the ink
    // the moment the player touches them, before the script fades the screen.
    void NoteInkActivation(RE::TESBoundObject* a_this, RE::TESObjectREFR* a_activatorRef, const char* a_what)
    {
        if (!g_config.travelFx || !a_this || !a_activatorRef) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || a_activatorRef != player) {
            return;
        }
        if (!travelfx::IsInkForm(a_this->GetFormID())) {
            return;
        }
        // [Styles] Dawnguard's Soul Cairn portals and Dragonborn's books have their own style settings
        const auto* file = a_this->GetFile(0);
        const bool  soul = file && _stricmp(file->fileName, "Dawnguard.esm") == 0;
        const int   kind = soul ? g_config.styleSoulCairn : g_config.styleBlackBook;
        if (kind == voidoverlay::kFxNone) {
            return;
        }
        travelfx::NotePending(kind, 15000, a_what);
        g_travelFxLoad.store(true);
        voidoverlay::ArmTravelFx(kind, a_what);
    }

    bool HookedActiActivate(RE::TESBoundObject* a_this, RE::TESObjectREFR* a_targetRef, RE::TESObjectREFR* a_activatorRef,
        std::uint8_t a_arg3, RE::TESBoundObject* a_object, std::int32_t a_targetCount)
    {
        NoteInkActivation(a_this, a_activatorRef, "activator touched (Black Book / portal)");
        return g_origActiActivate ? g_origActiActivate(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount) : false;
    }

    bool HookedBookActivate(RE::TESBoundObject* a_this, RE::TESObjectREFR* a_targetRef, RE::TESObjectREFR* a_activatorRef,
        std::uint8_t a_arg3, RE::TESBoundObject* a_object, std::int32_t a_targetCount)
    {
        NoteInkActivation(a_this, a_activatorRef, "Black Book read");
        return g_origBookActivate ? g_origBookActivate(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount) : false;
    }

    void InstallVtableHooks(bool a_includeContainer = true)
    {
        constexpr std::size_t kActivateIdx = 0x37;
        constexpr std::uint32_t kPageExecuteReadWrite = 0x40;

        if (g_config.travelFx) {
            auto hookOne = [&](REL::VariantID a_vtblId, Activate_t& a_orig, void* a_hook, const char* a_name) {
                REL::Relocation<std::uintptr_t> v{ a_vtblId };
                auto** vtbl = reinterpret_cast<void**>(v.address());
                if (vtbl && IsExecutableAddress(vtbl[kActivateIdx])) {
                    std::uint32_t oldProt = 0;
                    if (REX::W32::VirtualProtect(&vtbl[kActivateIdx], sizeof(void*), kPageExecuteReadWrite, &oldProt)) {
                        a_orig = reinterpret_cast<Activate_t>(vtbl[kActivateIdx]);
                        vtbl[kActivateIdx] = a_hook;
                        REX::W32::VirtualProtect(&vtbl[kActivateIdx], sizeof(void*), oldProt, &oldProt);
                        logger::info("[TravelFx] installed {}::Activate hook at vtable[0x37]=0x{:X}", a_name, reinterpret_cast<std::uintptr_t>(a_orig));
                    }
                } else {
                    logger::warn("[TravelFx] {} vtable[0x37] not executable; ink trigger hook skipped", a_name);
                }
            };
            hookOne(RE::VTABLE_TESObjectACTI[0], g_origActiActivate, reinterpret_cast<void*>(&HookedActiActivate), "TESObjectACTI");
            hookOne(RE::VTABLE_TESObjectBOOK[0], g_origBookActivate, reinterpret_cast<void*>(&HookedBookActivate), "TESObjectBOOK");
        }

        {
            REL::Relocation<std::uintptr_t> doorVtbl{ RE::VTABLE_TESObjectDOOR[0] };
            auto** vtbl = reinterpret_cast<void**>(doorVtbl.address());
            if (vtbl && IsExecutableAddress(vtbl[kActivateIdx])) {
                std::uint32_t oldProt = 0;
                if (REX::W32::VirtualProtect(&vtbl[kActivateIdx], sizeof(void*), kPageExecuteReadWrite, &oldProt)) {
                    g_origDoorActivate = reinterpret_cast<Activate_t>(vtbl[kActivateIdx]);
                    vtbl[kActivateIdx] = reinterpret_cast<void*>(&HookedDoorActivate);
                    REX::W32::VirtualProtect(&vtbl[kActivateIdx], sizeof(void*), oldProt, &oldProt);
                    logger::info("Installed TESObjectDOOR::Activate hook at vtable[0x37]=0x{:X} ({})",
                        reinterpret_cast<std::uintptr_t>(g_origDoorActivate),
                        IsAddressInTextSegment(reinterpret_cast<void*>(g_origDoorActivate)) ? "engine .text" : "chained");
                }
            } else {
                logger::error("TESObjectDOOR vtable[0x37] is not an executable address; hook aborted");
            }
        }

        if (!a_includeContainer) {
            logger::info("Container Activate hook skipped (pocket copies disabled)");
            return;
        }

        {
            REL::Relocation<std::uintptr_t> contVtbl{ RE::VTABLE_TESObjectCONT[0] };
            auto** vtbl = reinterpret_cast<void**>(contVtbl.address());
            if (vtbl && IsExecutableAddress(vtbl[kActivateIdx])) {
                std::uint32_t oldProt = 0;
                if (REX::W32::VirtualProtect(&vtbl[kActivateIdx], sizeof(void*), kPageExecuteReadWrite, &oldProt)) {
                    g_origContActivate = reinterpret_cast<Activate_t>(vtbl[kActivateIdx]);
                    vtbl[kActivateIdx] = reinterpret_cast<void*>(&HookedContActivate);
                    REX::W32::VirtualProtect(&vtbl[kActivateIdx], sizeof(void*), oldProt, &oldProt);
                    logger::info("Installed TESObjectCONT::Activate hook at vtable[0x37]=0x{:X}",
                        reinterpret_cast<std::uintptr_t>(g_origContActivate));
                }
            }
        }
    }

    // =========================================================================
    //  SKSE Menu Framework UI
    // =========================================================================
#if defined(LOADINGFLOW_WITH_MENU)
    std::atomic<float> g_origPostLoad{ -1.0f };
    std::atomic<int>   g_origQueueBudget{ -1 };
    std::atomic<bool>  g_origCaptured{ false };

    void ApplyEngineTimerTweaks();   // defined below; the menu can re-apply without a restart

    void __stdcall RenderMainMenu()
    {
        bool en = g_config.enabled;
        if (ImGuiMCP::Checkbox(L("menu.enable", "Enable LoadingFlow faster door loading"), &en)) {
            g_config.enabled = en;
            SaveConfig();
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.btn.save", "Save Config"))) {
            SaveConfig();
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(L("menu.btn.reload", "Reload INI"))) {
            LoadConfig();
            ResolveExcludedCells();
        }

        ImGuiMCP::Separator();
        const RE::FormID activeID = g_activeLoadingFlowID.load();
        if (activeID != 0) {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                L("menu.status.inPocket", "Current Status: INSIDE POCKET CELL (0x%08X) | Faster loads: %u"),
                activeID, g_transitionCount.load());
        } else {
            ImGuiMCP::TextColored(ImGuiMCP::ImVec4(0.7f, 0.85f, 1.0f, 1.0f),
                L("menu.status.normal", "Current Status: Normal World/Cell | Faster loads: %u"),
                g_transitionCount.load());
        }
        ImGuiMCP::Text(L("menu.blueprintStatus", "Auto-Indexed Blueprints: %zu interior cells / %zu doors (Hash: %08X, %s)"),
            g_bpCellCount.load(), g_bpDoorCount.load(),
            static_cast<std::uint32_t>(g_currentFingerprint.load() & 0xFFFFFFFFu),
            g_cacheHitOnStartup.load() ? "CACHE HIT" : "AUTO-BUILT");
        ImGuiMCP::SameLine();
        if (g_rebuildInProgress.load()) {
            ImGuiMCP::Text(L("menu.rebuild.busy", "Rebuilding... (background)"));
        } else if (ImGuiMCP::Button(L("menu.btn.rebuildIndex", "Force Rebuild Index"))) {
            // P1-4 / P0-1: never index 2500+ plugins on the render thread, and never do it while
            // holding the state mutex. Hand it to the SKSE task queue and keep it single-flight.
            bool expected = false;
            if (g_rebuildInProgress.compare_exchange_strong(expected, true)) {
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask([]() {
                        EnsureBlueprintsLoaded(true);
                        g_rebuildInProgress.store(false);
                    });
                } else {
                    g_rebuildInProgress.store(false);
                }
            }
        }

        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.sec.transitions", "-- Transitions --"));
        if (ImGuiMCP::Checkbox(L("menu.suppressLoadingMenu", "Tier2: hide the Loading Menu during real cell transitions (auto fallback)"), &g_config.suppressLoadingMenu)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.prewarmNearbyDoors", "Tier3: pre-warm models of nearby interiors while walking"), &g_config.prewarmNearbyDoors)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.sneakVanilla", "Hold Sneak while activating a door to force the vanilla load screen"), &g_config.sneakForcesVanillaLoad)) {
            SaveConfig();
        }
        if (ImGuiMCP::Checkbox(L("menu.debugNotify", "Show a top-left HUD notification on every transition (debug)"), &g_config.showDebugNotifications)) {
            SaveConfig();
        }

        ImGuiMCP::Separator();
        // ---- speed vs stability (v1.6.1) ---------------------------------------
        // These are ENGINE timers: they do not just change how the transition looks, they decide how
        // long scripts and streaming get before the player takes control. Some users want the fastest
        // possible door; others hit crashes/clutter with heavy mod lists. So the trade-off is exposed
        // here instead of being hard-coded -- and "Apply now" really applies it (engine settings are
        // otherwise only written at load time).
        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.sec.timers", "-- Speed vs stability (engine timers) --"));
        {
            auto applyPreset = [](const char* a_name, float a_postLoad, int a_queueBudget, float a_fade) {
                g_config.postLoadUpdateTimeMS = a_postLoad;
                g_config.loadingQueuedPriorityBudgetMs = a_queueBudget;
                g_config.fadeToBlackFadeSeconds = a_fade;
                SaveConfig();
                ApplyEngineTimerTweaks();
                logger::info("[Timers] preset '{}' applied: postLoad={} ms, queueBudget={} ms, fade={} s",
                    a_name, a_postLoad, a_queueBudget, a_fade);
            };
            if (ImGuiMCP::Button(L("menu.timers.stable", "Stable (restore the game's own values)"))) {
                // restore what the game shipped with (captured before our first write)
                g_config.postLoadUpdateTimeMS = g_origPostLoad.load();
                g_config.loadingQueuedPriorityBudgetMs = g_origQueueBudget.load();
                g_config.fadeToBlackFadeSeconds = 1.0f;   // vanilla fade-to-black, 1 s
                SaveConfig();
                ApplyEngineTimerTweaks();
                logger::info("[Timers] preset 'stable' restored engine originals");
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button(L("menu.timers.balanced", "Balanced (recommended)"))) {
                applyPreset("balanced", 800.0f, 20, 0.10f);
            }
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button(L("menu.timers.fast", "Fastest (may break heavy mods)"))) {
                applyPreset("fastest", 100.0f, 500, 0.10f);
            }
            ImGuiMCP::Text(g_config.postLoadUpdateTimeMS < 0.0f
                    ? "current: vanilla waits -- least risk, least speed-up"
                    : std::format("current: post-load wait {} ms, streaming budget {} ms", g_config.postLoadUpdateTimeMS, g_config.loadingQueuedPriorityBudgetMs).c_str());

            int postLoad = static_cast<int>(g_config.postLoadUpdateTimeMS);
            if (ImGuiMCP::SliderInt(L("menu.timers.postLoad", "Post-load script wait (ms, vanilla 2000, lower = faster but riskier)"), &postLoad, -1, 2000, "%d ms")) {
                g_config.postLoadUpdateTimeMS = static_cast<float>(postLoad);
                SaveConfig();
            }
            int queueBudget = g_config.loadingQueuedPriorityBudgetMs;
            if (ImGuiMCP::SliderInt(L("menu.timers.queue", "Background streaming budget (ms, vanilla 20)"), &queueBudget, -1, 1000, "%d ms")) {
                g_config.loadingQueuedPriorityBudgetMs = queueBudget;
                SaveConfig();
            }
            if (ImGuiMCP::Button(L("menu.timers.apply", "Apply engine timers now"))) {
                ApplyEngineTimerTweaks();
                logger::info("[Timers] re-applied from the menu (no restart needed)");
            }
            ImGuiMCP::Text(L("menu.timers.note", "Lower waits = faster doors, but scripts and mods get less time."));
        }

        if (ImGuiMCP::Checkbox(L("menu.globalSuppress", "Global: suppress the menu for non-door transitions too"), &g_config.globalMenuSuppression)) {
            SaveConfig();
        }

        ImGuiMCP::Separator();
        ImGuiMCP::Text(L("menu.sec.seamless", "-- Seamless door transition (private build) --"));
        if (ImGuiMCP::Checkbox(L("menu.seamless", "Hold the last frame over the load, then dissolve into the new cell"), &g_config.seamless)) {
            SaveConfig();
        }
        {
            int fadeMs = static_cast<int>(g_config.seamlessFadeMs);
            if (ImGuiMCP::SliderInt(L("menu.seamless.fade", "Dissolve length (ms)"), &fadeMs, 100, 2000, "%d ms")) {
                g_config.seamlessFadeMs = static_cast<std::uint32_t>(fadeMs);
                SaveConfig();
            }
            int minMs = static_cast<int>(g_config.seamlessMinAfterCutMs);
            if (ImGuiMCP::SliderInt(L("menu.seamless.min", "Earliest dissolve after the cell swap (ms, 0 = post-load wait + 100)"), &minMs, 0, 3000, "%d ms")) {
                g_config.seamlessMinAfterCutMs = static_cast<std::uint32_t>(minMs);
                SaveConfig();
            }
            int stableMs = static_cast<int>(g_config.seamlessStableMs);
            if (ImGuiMCP::SliderInt(L("menu.seamless.stable", "New picture must be steady for (ms)"), &stableMs, 0, 1000, "%d ms")) {
                g_config.seamlessStableMs = static_cast<std::uint32_t>(stableMs);
                SaveConfig();
            }
            int maxHold = static_cast<int>(g_config.seamlessMaxHoldMs);
            if (ImGuiMCP::SliderInt(L("menu.seamless.maxHold", "Dissolve anyway this long after the swap (ms)"), &maxHold, 300, 8000, "%d ms")) {
                g_config.seamlessMaxHoldMs = static_cast<std::uint32_t>(maxHold);
                SaveConfig();
            }
            ImGuiMCP::Text(L("menu.seamless.note", "Applies to the next door. The old frame is never darkened; the engine's spin happens underneath it."));
        }

        if (ImGuiMCP::Checkbox(L("menu.fgpause", "Pause frame generation while a hold or cover is on screen (fixes the one-frame flash)"), &g_config.pauseFrameGen)) {
            SaveConfig();
        }
        ImGuiMCP::Text(L("menu.sec.travelfx", "-- Travel effects (private build) --"));
        if (ImGuiMCP::Checkbox(L("menu.travelfx", "Cover non-door loads with an effect instead of the loading screen"), &g_config.travelFx)) {
            SaveConfig();
        }
        {
            bool eyesDefault = (g_config.travelFxDefault == 1);
            if (ImGuiMCP::Checkbox(L("menu.travelfx.default", "Unrecognised travel also gets the eyes (off = vanilla loading screen)"), &eyesDefault)) {
                g_config.travelFxDefault = eyesDefault ? 1 : 0;
                SaveConfig();
            }
            int inMs = static_cast<int>(g_config.travelFxInMs);
            if (ImGuiMCP::SliderInt(L("menu.travelfx.in", "Build-up length (ms)"), &inMs, 100, 3000, "%d ms")) {
                g_config.travelFxInMs = static_cast<std::uint32_t>(inMs);
                SaveConfig();
            }
            int outMs = static_cast<int>(g_config.travelFxOutMs);
            if (ImGuiMCP::SliderInt(L("menu.travelfx.out", "Clearing length (ms)"), &outMs, 100, 3000, "%d ms")) {
                g_config.travelFxOutMs = static_cast<std::uint32_t>(outMs);
                SaveConfig();
            }
            int maxHold = static_cast<int>(g_config.travelFxMaxHoldMs / 1000);
            if (ImGuiMCP::SliderInt(L("menu.travelfx.maxHold", "Give up and clear after (s)"), &maxHold, 5, 120, "%d s")) {
                g_config.travelFxMaxHoldMs = static_cast<std::uint32_t>(maxHold) * 1000u;
                SaveConfig();
            }
            ImGuiMCP::Text(L("menu.travelfx.note", "Eyes: boats, carriages, scripted travel. Ink: Black Books, Soul Cairn. Magic: map fast travel. Load/save/death keep the vanilla screen."));
        }
    }

    void RegisterMenu()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::warn("SKSEMenuFramework.dll not found; menu not registered (plugin still works)");
            return;
        }
        SKSEMenuFramework::SetSection("Seamless Transitions");
        SKSEMenuFramework::AddSectionItem("Seamless Transitions", RenderMainMenu);
        logger::info("Registered SKSEMenuFramework section: LoadingFlow");
    }
#else
    void RegisterMenu() {}
#endif

    // Engine transition timers live in the INI-backed setting collections under names of the form
    // "name:Section". This CommonLib version exposes only the raw BSSimpleList, so scan it.
    RE::Setting* FindIniSetting(const char* a_name)
    {
        auto scan = [&](RE::SettingCollectionList<RE::Setting>* a_coll) -> RE::Setting* {
            if (!a_coll) {
                return nullptr;
            }
            for (auto* s : a_coll->settings) {
                if (s && s->GetName() && std::strcmp(s->GetName(), a_name) == 0) {
                    return s;
                }
            }
            return nullptr;
        };
        if (auto* s = scan(RE::INISettingCollection::GetSingleton())) {
            return s;
        }
        return scan(RE::INIPrefSettingCollection::GetSingleton());
    }

    const char* SettingTypeName(RE::Setting::Type a_type)
    {
        switch (a_type) {
        case RE::Setting::Type::kBool: return "Bool";
        case RE::Setting::Type::kFloat: return "Float";
        case RE::Setting::Type::kInteger: return "SInt";
        case RE::Setting::Type::kUnsignedInteger: return "UInt";
        case RE::Setting::Type::kColorRGB: return "Color";
        case RE::Setting::Type::kColorRGBA: return "ColorA";
        case RE::Setting::Type::kString: return "String";
        default: return "Unknown";
        }
    }

    void SetIniFloat(const char* a_name, float a_value, bool a_onlyReport = false)
    {
        auto* s = FindIniSetting(a_name);
        if (!s) {
            logger::warn("[Speed] {} NOT FOUND (left unchanged)", a_name);
            return;
        }
        // NEVER write a union member without checking the setting's real type: writing data.b into
        // an integer setting only overwrites one byte and leaves garbage in the rest, which can make
        // the engine act on a nonsense value.
        if (s->GetType() != RE::Setting::Type::kFloat) {
            logger::warn("[Speed] {} is {} not Float (left unchanged)", a_name, SettingTypeName(s->GetType()));
            return;
        }
        const float old = s->data.f;
        if (a_onlyReport || a_value < 0.0f) {
            logger::info("[Speed] {} = {:.4f} (left at vanilla)", a_name, old);
            return;
        }
        s->data.f = a_value;
        logger::info("[Speed] {} {:.4f} -> {:.4f}", a_name, old, a_value);
    }

    void SetIniInt(const char* a_name, int a_value, bool a_onlyReport = false)
    {
        auto* s = FindIniSetting(a_name);
        if (!s) {
            logger::warn("[Speed] {} NOT FOUND (left unchanged)", a_name);
            return;
        }
        const auto t = s->GetType();
        if (t != RE::Setting::Type::kInteger && t != RE::Setting::Type::kUnsignedInteger) {
            logger::warn("[Speed] {} is {} not integer (left unchanged)", a_name, SettingTypeName(t));
            return;
        }
        const auto old = s->data.i;
        if (a_onlyReport || a_value < 0) {
            logger::info("[Speed] {} = {} (left at vanilla)", a_name, old);
            return;
        }
        s->data.i = a_value;
        logger::info("[Speed] {} {} -> {}", a_name, old, a_value);
    }

    // Try a bare name against several known INI sections, in order. SkyrimSE.exe contains
    // "iPreloadSizeLimit" exists in SkyrimSE.exe but its INI section is not documented, so probe.
    // (The matching ON/OFF switch bPreloadLinkedInteriors was removed -- it hangs save loading.)
    RE::Setting* FindIniSettingAuto(const char* a_name, char* a_outName, std::size_t a_outSize)
    {
        static const char* kSections[] = { "General", "BackgroundLoad", "Display", "Interface",
                                           "Papyrus", "Archive", "Audio", "Combat", "Menu" };
        for (const char* sec : kSections) {
            std::snprintf(a_outName, a_outSize, "%s:%s", a_name, sec);
            if (auto* s = FindIniSetting(a_outName)) {
                return s;
            }
        }
        std::snprintf(a_outName, a_outSize, "%s", a_name);
        return FindIniSetting(a_outName);
    }

    void SetIniIntAuto(const char* a_name, int a_value)
    {
        char full[160] = {};
        auto* s = FindIniSettingAuto(a_name, full, sizeof(full));
        if (!s) {
            logger::warn("[Preload] {} NOT FOUND in any known section (left unchanged)", a_name);
            return;
        }
        const auto t = s->GetType();
        if (t != RE::Setting::Type::kInteger && t != RE::Setting::Type::kUnsignedInteger) {
            logger::warn("[Preload] {} is {} (not integer) -- left unchanged", full, SettingTypeName(t));
            return;
        }
        const auto old = s->data.i;
        if (a_value < 0) {
            logger::info("[Preload] {} = {} (left at vanilla)", full, old);
            return;
        }
        s->data.i = a_value;
        logger::info("[Preload] {} {} -> {}", full, old, a_value);
    }

    // Shortens the engine's own transition timers. The big one is fFadeToBlackFadeSeconds: our log
    // measured a very stable ~1.0 s between pressing E and the engine even asking for the
    // LoadingMenu, and that value IS that setting's vanilla default.
    // v1.6.2: remember what the game actually shipped, so "Stable" can RESTORE it. Before this,
    // Stable wrote -1 ("leave alone"), which changed nothing after a previous preset had already
    // written 100/500 -- that is why the menu felt like it had no effect.
    void CaptureEngineTimerOriginals()
    {
        if (g_origCaptured.exchange(true)) {
            return;
        }
        if (auto* s = FindIniSetting("fPostLoadUpdateTimeMS:Papyrus"); s && s->GetType() == RE::Setting::Type::kFloat) {
            g_origPostLoad.store(s->GetFloat());
        }
        if (auto* s = FindIniSetting("iPostProcessMillisecondsLoadingQueuedPriority:BackgroundLoad"); s && s->GetType() == RE::Setting::Type::kInteger) {
            g_origQueueBudget.store(static_cast<int>(s->data.i));
        }
        logger::info("[Timers] captured engine originals: postLoad={} ms, queueBudget={} ms",
            g_origPostLoad.load(), g_origQueueBudget.load());
    }

    void ApplyEngineTimerTweaks()
    {
        CaptureEngineTimerOriginals();
        const bool on = g_config.tweakEngineTimers;
        logger::info("[Speed] engine transition timers ({}):", on ? "applying" : "report only");
        SetIniFloat("fFadeToBlackFadeSeconds:Interface", on ? g_config.fadeToBlackFadeSeconds : -1.0f, !on);
        SetIniFloat("fNormalDoorFadeSecs:General", on ? g_config.normalDoorFadeSecs : -1.0f, !on);
        SetIniFloat("fNormalDoorFadeWait:General", on ? g_config.normalDoorFadeWait : -1.0f, !on);
        SetIniFloat("fAutoDoorFadeSecs:General", on ? g_config.autoDoorFadeSecs : -1.0f, !on);
        SetIniFloat("fMinSecondsForLoadFadeIn:Interface", on ? g_config.minSecondsForLoadFadeIn : -1.0f, !on);
        SetIniFloat("fPostLoadUpdateTimeMS:Papyrus", on ? g_config.postLoadUpdateTimeMS : -1.0f, !on);
        SetIniFloat("fActorTeleportFadeSeconds:General", on ? g_config.actorTeleportFadeSeconds : -1.0f, !on);
        SetIniFloat("fFastTravelFadeSecs:General", on ? g_config.fastTravelFadeSecs : -1.0f, !on);
        SetIniInt("iPostProcessMillisecondsLoadingQueuedPriority:BackgroundLoad",
            on ? g_config.loadingQueuedPriorityBudgetMs : -1, !on);
        // Engine-side cell preloading. Both settings exist in SkyrimSE.exe; enabling them is the
        // only remaining lever on the load phase itself (our own measurements show the load phase
        // is workload-bound, and model-level pre-warming did nothing).
        SetIniIntAuto("iPreloadSizeLimit", g_config.preloadSizeLimit);
        logger::info("[Timers] effective now: postLoad={} ms, queueBudget={} ms, fadeToBlack={} s",
            g_config.postLoadUpdateTimeMS, g_config.loadingQueuedPriorityBudgetMs, g_config.fadeToBlackFadeSeconds);
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg)
    {        if (!a_msg) {
            return;
        }
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            logger::info("kDataLoaded: initializing LoadingFlow hooks & auto-verifying blueprint index");
            if (::GetModuleHandleW(L"LoadingFlow.dll")) {
                // both would hook the same doors and menus
                logger::error("LoadingFlow.dll is also loaded. Seamless Door and Travel Transitions replaces LoadingFlow; remove one of them. Staying inactive.");
                RE::DebugMessageBox("Seamless Door and Travel Transitions replaces LoadingFlow.\nBoth are installed, so this one stays switched off.\nPlease remove one of them.");
                g_config.enabled = false;
                break;
            }
            EnsureBlueprintsLoaded(false);
            ResolveExcludedCells();
            CacheForms();
            InstallVtableHooks(g_config.usePocketCopies);
            mcm::Install();   // [Styles]
            LogGraphicsEnvironment();
            if (g_config.probeGfx) {
                ProbeDumpGfxOnce();
            }
            // v1.3.1 step-2 probe: install the Present hook NOW (not only at a transition) so the
            // hook-chain order can be judged, and so frames during normal play can be dumped.
            if (g_config.capProbe || g_config.captureFrame) {
                auto* sc = presentfreeze::CurrentSwapChain();
                if (sc && presentfreeze::InstallOn(sc)) {
                    logger::info("[Cap] probe hook armed at kDataLoaded (swapchain {})", sc);
                } else {
                    logger::warn("[Cap] no swap chain at kDataLoaded; the probe will arm on the first transition instead");
                }
            }
            if (g_config.usePocketCopies) {
                logger::warn("============================================================");
                logger::warn("bUsePocketCopies = 1: FROZEN EXPERIMENTAL PATH.");
                logger::warn("The pocket-clone route was rejected (no NAVM => no NPC pathing) and");
                logger::warn("has never been verified in game. Set it back to 0.");
                logger::warn("============================================================");
            }
            if (g_config.usePocketCopies) {
                // Condition hooks exist only to make a pocket copy answer cell/location questions
                // correctly. In real-transition mode the engine answers them itself, so installing
                // them would be pure overhead on every condition evaluation.
                InstallConditionHooks();
            } else {
                logger::info("Condition function hooks skipped (real-transition mode: the engine answers natively)");
            }
            InstallLoadingMenuHook();
            travelfx::CacheForms();
            if (g_config.pauseFrameGen) {
                if (!fgpause::Install()) {
                    logger::info("[FGPause] frame generation DLL not loaded yet; will hook it when the first hold arms");
                }
            }
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_exemptSink);
                logger::info("[Tier2] menu sink registered (fast-travel / load-game exemption for global mode)");
            }
            ApplyEngineTimerTweaks();
            StartPrewarmThread();
            RegisterMenu();
            break;
        case SKSE::MessagingInterface::kSaveGame:
            // P1-3: never write a save file with the player standing at Z=+26000 inside a replica
            // whose geometry is not persisted. Put them back on the ground first, then let the save run.
            if (g_activeLoadingFlowID.load() != 0) {
                logger::warn("Save requested while inside a high-altitude Pocket: teleporting the player back to the "
                             "recorded ground position and purging temporary references before the save is written");
                PurgeAllPockets(true);
            }
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            travelfx::g_loadGamePending.store(a_msg->type == SKSE::MessagingInterface::kPreLoadGame);
            travelfx::g_pendingKind.store(voidoverlay::kFxNone);
            if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame) {
                ReleaseTravelCover("save load started");
            }
            g_travelFxLoad.store(false);
            PurgeAllPockets(false);
            // A load/new game kills any transition window that was still open; never leave the
            // overlay armed across it (it is bounded anyway, but a stale arm would draw over the
            // loading screen).
            voidoverlay::Disarm("load/new game");
            // Private build: the idle snapshot for the seamless hold needs the Present hook before
            // the first door, so install it as soon as a game is running.
            if (g_config.seamless && a_msg->type == SKSE::MessagingInterface::kPostLoadGame) {
                if (auto* sc = presentfreeze::CurrentSwapChain()) {
                    presentfreeze::InstallOn(sc);
                }
            }
            // Loading a save can re-read the game INIs and revert everything we applied at
            // kDataLoaded. The upstream transition mod re-applies here for exactly this reason.
            ApplyEngineTimerTweaks();
            break;
        default:
            break;
        }
    }
}

// Plugin version data is generated by add_commonlibsse_plugin (CMakeLists.txt) in this build.

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);

    auto path = logger::log_directory();
    if (!path) {
        return false;
    }
    *path /= "SeamlessTransitions.log"sv;

    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(PathText(*path, 0 /* system code page */), true);
    auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
    log->set_level(spdlog::level::info);
    log->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(log));
    spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");

    // Load the INI BEFORE logging the banner -- otherwise the banner prints defaults and lies about
    // the mode the game is actually running in.
    LoadConfig();
    if (auto* papyrus = SKSE::GetPapyrusInterface()) {
        papyrus->Register(mcm::RegisterPapyrus);   // [Styles] LoadingFlow_Native.Preview
    }

    logger::info("Seamless Door and Travel Transitions v1.1.2 (based on LoadingFlow 1.2.0) loading... [MODE: real transitions | hooks: door + LoadingMenu{} | "
                 "suppression {} (re-post after {} ms inside a {} ms window) | GLOBAL suppression {} | prewarm {} | "
                 "no-black {} (manual {}, walk-through {}) | cut mask 0x{:08X}/{} ms | freeze delay {} ms | "
                 "envelope {} ({} ms) | pocket copies {} | void overlay {} ({} motes, {} ms pulse)]",
        g_config.usePocketCopies ? " + condition + container" : " only",
        g_config.suppressLoadingMenu ? "ON" : "off",
        g_config.suppressTimeoutMs,
        g_config.transitionWindowMs,
        g_config.globalMenuSuppression ? "ON" : "off",
        g_config.prewarmNearbyDoors ? "ON" : "off",
        g_config.noBlackTransition ? "ON (freeze)" : "off",
        g_config.noBlackOnlyForManualDoors ? "yes" : "all doors",
        g_config.noBlackForWalkThrough ? "yes" : "no",
        g_config.cutMaskImod, g_config.cutMaskMs,
        g_config.freezeDelayMs,
        g_config.visualEnvelope ? "ON" : "off",
        g_config.envelopeBlackHoldMs,
        g_config.usePocketCopies ? "ON" : "off",
        g_config.voidOverlay ? "ON" : "off",
        g_config.overlayMotes,
        g_config.overlayPulseMs);

    if (auto* messaging = SKSE::GetMessagingInterface()) {
        messaging->RegisterListener(OnMessage);
    }

    return true;
}
