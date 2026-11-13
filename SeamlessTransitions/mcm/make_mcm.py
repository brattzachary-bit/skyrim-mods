#!/usr/bin/env python3
"""Builds the MCM Helper package of Seamless Door and Travel Transitions into ./dist:
   SeamlessTransitions.esp (ESL-flagged: one quest carrying the config script and the player alias),
   Scripts/*.pex (written directly; the sources are in ./source), MCM/Config/SeamlessTransitions/{config.json,settings.ini}."""
import json, os, struct, time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "dist")
MOD = "SeamlessTransitions"

# ------------------------------------------------------------------ compiled scripts (.pex, big-endian)
class Pex:
    def __init__(self, source, name, parent):
        self.source, self.name, self.parent = source, name, parent
        self.strings, self.functions = [], []
    def s(self, text):
        if text not in self.strings:
            self.strings.append(text)
        return struct.pack(">H", self.strings.index(text))
    def ident(self, name):
        return b"\x01" + self.s(name)
    def function(self, name, rtype, params, flags, code=()):
        body = self.s(rtype) + self.s("") + struct.pack(">IB", 0, flags)
        body += struct.pack(">H", len(params)) + b"".join(self.s(n) + self.s(t) for n, t in params)
        body += struct.pack(">H", 0)                       # locals
        body += struct.pack(">H", len(code)) + b"".join(code)
        self.functions.append(self.s(name) + body)
    def standard_state_functions(self, with_handlers):
        integer0 = b"\x03" + struct.pack(">i", 0)
        self.function("GetState", "String", [], 0, [bytes([26]) + self.ident("::State")])
        self.function("GotoState", "None", [("newState", "String")], 0, [
            bytes([23]) + self.ident("onEndState") + self.ident("self") + self.ident("::NoneVar") + integer0,
            bytes([13]) + self.ident("::State") + self.ident("newState"),
            bytes([23]) + self.ident("onBeginState") + self.ident("self") + self.ident("::NoneVar") + integer0])
        if with_handlers:                                  # scripts without a parent define the two handlers themselves
            self.function("onBeginState", "None", [], 0)
            self.function("onEndState", "None", [], 0)
    def build(self):
        name = self.s(self.name)
        obj = self.s(self.parent) + self.s("") + struct.pack(">I", 0) + self.s("")
        obj += struct.pack(">H", 0) + struct.pack(">H", 0)                 # variables, properties
        obj += struct.pack(">H", 1) + self.s("") + struct.pack(">H", len(self.functions)) + b"".join(self.functions)
        flags = self.s("hidden") + b"\x00" + self.s("conditional") + b"\x01"
        def wstr(t):
            return struct.pack(">H", len(t)) + t.encode("latin1")
        out = struct.pack(">IBBHQ", 0xFA57C0DE, 3, 2, 1, int(time.time()))
        out += wstr(self.source) + wstr("SeamlessTransitions") + wstr("generated")
        out += struct.pack(">H", len(self.strings)) + b"".join(wstr(t) for t in self.strings)
        out += b"\x00"                                                     # no debug info
        out += struct.pack(">H", 2) + flags
        out += struct.pack(">H", 1) + name + struct.pack(">I", len(obj) + 4) + obj
        return out

def scripts():
    native = Pex("SeamlessTransitions_Native.psc", "SeamlessTransitions_Native", "")
    native.function("Preview", "None", [("aiStyle", "Int")], 3)            # global | native
    native.standard_state_functions(True)
    config = Pex("SeamlessTransitions_MCM.psc", "SeamlessTransitions_MCM", "MCM_ConfigBase")
    config.standard_state_functions(False)
    return {"SeamlessTransitions_Native.pex": native.build(), "SeamlessTransitions_MCM.pex": config.build()}

# ------------------------------------------------------------------ plugin (little-endian)
def sub(tag, data):
    return tag + struct.pack("<H", len(data)) + data
def record(tag, formid, flags, body):
    return tag + struct.pack("<IIIIHH", len(body), flags, formid, 0, 44, 0) + body
def group(label, body):
    return b"GRUP" + struct.pack("<I", 24 + len(body)) + label + struct.pack("<iHHI", 0, 0, 0, 0) + body
def wstr(t):
    return struct.pack("<H", len(t)) + t.encode("latin1")

def plugin():
    quest = 0x01000800                                    # one master, so our own records are index 01
    vmad = struct.pack("<hhH", 5, 2, 1)
    vmad += wstr("SeamlessTransitions_MCM") + b"\x00" + struct.pack("<H", 1)
    vmad += wstr("ModName") + b"\x02\x01" + wstr("Seamless Transitions")   # string property, edited
    vmad += b"\x02" + struct.pack("<H", 0) + wstr("")                       # quest fragments: none
    vmad += struct.pack("<H", 1)                                            # one alias with scripts
    vmad += struct.pack("<HhI", 0, 0, quest)                                # alias 0 of this quest
    vmad += struct.pack("<hhH", 5, 2, 1) + wstr("SKI_PlayerLoadGameAlias") + b"\x00" + struct.pack("<H", 0)
    body = sub(b"EDID", b"SeamlessTransitionsMCM\0") + sub(b"VMAD", vmad) + sub(b"FULL", b"Seamless Transitions\0")
    body += sub(b"DNAM", struct.pack("<HBBIi", 0x0111, 0, 0xFF, 0, 0))      # start game enabled, run once, allow repeated stages
    body += sub(b"NEXT", b"") + sub(b"ANAM", struct.pack("<I", 1))
    body += sub(b"ALST", struct.pack("<I", 0)) + sub(b"ALID", b"PlayerAlias\0") + sub(b"FNAM", struct.pack("<I", 0))
    body += sub(b"ALFR", struct.pack("<I", 0x14)) + sub(b"VTCK", struct.pack("<I", 0)) + sub(b"ALED", b"")
    hedr = sub(b"HEDR", struct.pack("<fiI", 1.71, 2, 0x801))
    tes4 = record(b"TES4", 0, 0x200, hedr + sub(b"CNAM", b"Seamless Transitions\0") +
                  sub(b"SNAM", b"Config page (MCM Helper) for Seamless Door and Travel Transitions.\0") +
                  sub(b"MAST", b"Skyrim.esm\0") + sub(b"DATA", struct.pack("<Q", 0)))
    return tes4 + group(b"QUST", record(b"QUST", quest, 0, body))

# ------------------------------------------------------------------ config page
STYLES = ["Vanilla loading screen", "Eyes", "Fade to black", "Light", "Drowsy", "Ink", "Soul mist", "Wipe", "Dream", "Dissolve (like doors)"]
SHORT = ["VANILLA", "EYES", "FADE", "LIGHT", "DROWSY", "INK", "MIST", "WIPE", "DREAM", "DISSOLVE"]
DOOR_STYLES = ["Hold and dissolve"] + STYLES[1:9]
DOOR_SHORT = ["DISSOLVE"] + SHORT[1:9]
AS_DOOR = ["Like other doors"] + STYLES[1:]
AS_DOOR_SHORT = ["DOOR"] + SHORT[1:]
NO_CHANGE = ["No change"] + STYLES[1:]
NO_CHANGE_SHORT = ["SAME"] + SHORT[1:]
SLEEP = ["Off"] + STYLES[1:]
SLEEP_SHORT = ["OFF"] + SHORT[1:]

DEFAULTS = {
    "Doors": {"iDoor": 0, "bAnnounceLoads": 1, "bSeamless": 1, "bHideHud": 1, "iDissolveMs": 600, "iEaseInMs": 4000, "fPushInPct": 0.8, "fDriftPct": 0.3},
    "Travel": {"bEnabled": 1, "iFastTravel": 1, "iCarriage": 2, "iOther": 1, "iBlackBook": 5, "iSoulCairn": 6, "iSleep": 4,
               "iEffectMs": 450, "iFastTravelMs": 1000},
    "Places": {"iLift": 7, "iPortal": 3, "iCaptive": 4, "iBoat": 2, "iDream": 8},
    "Hotkey": {"iKeyStart": 0, "iKeyEnd": 0, "iKeyEndDelayMs": 0},
    "FrameGeneration": {"bPause": 1, "bPauseDlss": 0, "bHoldPresents": 1},
}

def toggle(id_, text, help_):
    return {"id": id_, "text": text, "type": "toggle", "help": help_, "valueOptions": {"sourceType": "ModSettingBool"}}
def slider(id_, text, help_, lo, hi, step, fmt, kind="ModSettingInt"):
    return {"id": id_, "text": text, "type": "slider", "help": help_,
            "valueOptions": {"min": lo, "max": hi, "step": step, "formatString": fmt, "sourceType": kind}}
def style(id_, text, help_, options=STYLES, short=SHORT):
    return {"id": id_, "text": text, "type": "enum", "help": help_,
            "valueOptions": {"options": options, "shortNames": short, "sourceType": "ModSettingInt"}}
def preview(text, index):
    return {"text": text, "type": "text", "help": "Plays this transition where you stand, as soon as the menu is closed. Nothing is loaded.",
            "valueOptions": {"value": "PLAY"},
            "action": {"type": "CallGlobalFunction", "script": "SeamlessTransitions_Native", "function": "Preview", "params": ["{i}%d" % index]}}
def header(text):
    return {"text": text, "type": "header"}

def config():
    doors = [
        header("Doors"),
        toggle("bSeamless:Doors", "Seamless doors", "The last view is held while the next place loads and then dissolves into it. Off: a plain cut."),
        style("iDoor:Doors", "Door transition", "What ordinary load doors use. Hold and dissolve is the default; any other transition can stand in for it. Lifts, portals and the rowboat have their own setting on the Places page.", DOOR_STYLES, DOOR_SHORT),
        toggle("bHideHud:Doors", "Hide the HUD for the transition", "The HUD disappears a few frames before the door opens, so the held picture has no prompt or crosshair in it, and returns with the new place."),
        slider("iDissolveMs:Doors", "Dissolve length", "How long the held picture takes to dissolve into the new place.", 100, 2000, 50, "{0} ms"),
        header("Push-in"),
        slider("fPushInPct:Doors", "Push-in amount", "How far the held picture moves forward while it eases in. 0 switches it off.", 0, 5, 0.1, "{1}%", "ModSettingFloat"),
        slider("iEaseInMs:Doors", "Push-in ease time", "How long the push-in takes to build up. Longer is gentler.", 0, 10000, 250, "{0} ms"),
        slider("fDriftPct:Doors", "Drift per second", "Slow forward movement that continues for as long as the picture is held. 0 switches it off.", 0, 2, 0.05, "{2}%", "ModSettingFloat"),
        header("Compatibility"),
        toggle("bAnnounceLoads:Doors", "Tell other mods about loads", "Other mods (DynDOLOD, for one) wait for the loading screen to know you have travelled. With this on they are told even though no loading screen appears. Leave on unless another mod reacts badly."),
        header("Frame generation"),
        toggle("bPause:FrameGeneration", "Pause frame generation", "Switches FidelityFX frame generation (Community Shaders) off while a transition is on screen. Prevents a flash of the new place."),
        toggle("bPauseDlss:FrameGeneration", "Pause DLSS frame generation (experimental)", "Does the same for NVIDIA DLSS frame generation. Untested by the author: switch it on if you use DLSS frame generation and see a flash of the new place, and off again if it causes trouble."),
        toggle("bHoldPresents:FrameGeneration", "Hold frames across the swap", "No frames are shown between the moment the new place is swapped in and the start of the dissolve. Leave on unless transitions look stuck."),
    ]
    travel = [
        header("Which transition for what"),
        toggle("bEnabled:Travel", "Travel transitions", "Covers loads that are not doors with a transition in place of the loading screen. Every picker below offers every transition, including Dissolve, the door look. Loading a save, quitting and dying always use the normal screen."),
        style("iFastTravel:Travel", "Fast travel", "The transition builds over the place you leave before the journey starts, and clears on arrival."),
        style("iCarriage:Travel", "Carriages, boats, scripted travel", "Used when a load follows a conversation."),
        style("iOther:Travel", "Other loads during play", "Anything not recognised: walk-in entrances, teleports, quest moves."),
        style("iBlackBook:Travel", "Black Books and Apocrypha", "Reading a Black Book, and leaving Apocrypha."),
        style("iSoulCairn:Travel", "Soul Cairn", "Entering and leaving the Soul Cairn."),
        style("iSleep:Travel", "Waking up", "Plays the second half of a transition when you wake from sleep. No load is involved.", SLEEP, SLEEP_SHORT),
        slider("iFastTravelMs:Travel", "Fast travel speed", "How long the fast travel transition takes to close over the place you leave, and the same again to open on arrival.", 200, 4000, 50, "{0} ms"),
        slider("iEffectMs:Travel", "Speed of the others", "How long the other transitions take to close, and the same again to open. Drowsy takes twice as long; ink and mist clear at half speed.", 100, 3000, 50, "{0} ms"),
    ]
    places = [
        header("Places and situations"),
        style("iLift:Places", "Dwemer lifts", "The dark travels down the view when the lift goes up, and up the view when it goes down.", AS_DOOR, AS_DOOR_SHORT),
        style("iPortal:Places", "Portals", "The Forgotten Vale wayshrines and paragon portals, the gate to Misty Grove, the Ebonmere in the Twilight Sepulcher, and leaving Sovngarde, Skuldafn or Azura's Star.", AS_DOOR, AS_DOOR_SHORT),
        style("iBoat:Places", "Rowboat to Castle Volkihar", "The crossing by rowboat.", AS_DOOR, AS_DOOR_SHORT),
        style("iDream:Places", "Dreams and other minds", "Entering and leaving Pelagius's mind, and the Dreamstride in Nightcaller Temple.", NO_CHANGE, NO_CHANGE_SHORT),
        style("iCaptive:Places", "Jail, abduction, blackout", "How your eyes open when you arrive in a jail, wake in the Abandoned Shack or in the Temple of Dibella after a night to remember; and the whole transition when you are moved while asleep or black out in the Underforge.", NO_CHANGE, NO_CHANGE_SHORT),
    ]
    key_help = "For other mods with a hotkey worth pressing around a load (a rendering or frame generation feature that should be off while the area changes). Pick a key the game does not use. The number is a DirectInput key code, the same numbers SkyUI key bindings use. Examples: F1-F10 = 59-68, F11 = 87, F12 = 88, Insert = 210, Home = 199, End = 207, Page Up = 201, Page Down = 209, numpad 0-9 = 82, 79, 80, 81, 75, 76, 77, 71, 72, 73. 0 = no key."
    hotkeys = [
        header("Tap a key around every transition"),
        slider("iKeyStart:Hotkey", "Key when a transition starts", key_help, 0, 255, 1, "{0}"),
        slider("iKeyEnd:Hotkey", "Key when a transition ends", key_help + " Use the same key as above for a simple off-and-on toggle.", 0, 255, 1, "{0}"),
        slider("iKeyEndDelayMs:Hotkey", "Wait before the end key", "Extra time after the transition has cleared before the end key is tapped.", 0, 5000, 100, "{0} ms"),
    ]
    previews = [header("Preview"), preview("Door dissolve", 0)] + [preview(STYLES[i], i) for i in range(1, 9)]
    return {
        "modName": MOD, "displayName": "Seamless Transitions", "minMcmVersion": 9, "cursorFillMode": "topToBottom",
        "pages": [{"pageDisplayName": "Doors", "cursorFillMode": "topToBottom", "content": doors},
                  {"pageDisplayName": "Travel", "cursorFillMode": "topToBottom", "content": travel},
                  {"pageDisplayName": "Places", "cursorFillMode": "topToBottom", "content": places},
                  {"pageDisplayName": "Hotkey", "cursorFillMode": "topToBottom", "content": hotkeys},
                  {"pageDisplayName": "Preview", "cursorFillMode": "topToBottom", "content": previews}],
    }

def settings_ini():
    lines = ["; Defaults of the Seamless Door and Travel Transitions config page. Do not edit: your own values are kept in",
             "; Data\\MCM\\Settings\\SeamlessTransitions.ini.", ""]
    for section, values in DEFAULTS.items():
        lines.append("[%s]" % section)
        lines += ["%s=%s" % kv for kv in values.items()]
        lines.append("")
    return "\r\n".join(lines)

def main():
    def put(rel, data):
        path = os.path.join(OUT, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data if isinstance(data, bytes) else data.encode("utf-8"))
    put(MOD + ".esp", plugin())
    for name, data in scripts().items():
        put("Scripts/" + name, data)
    for name in os.listdir(os.path.join(HERE, "source")):
        put("Scripts/Source/" + name, open(os.path.join(HERE, "source", name), "rb").read())
    put("MCM/Config/%s/config.json" % MOD, json.dumps(config(), indent=2))
    put("MCM/Config/%s/settings.ini" % MOD, settings_ini())
    print("written to", OUT)

if __name__ == "__main__":
    main()
