#!/usr/bin/env python3
"""Writes params.json for Machinedrum One (VST index = order; append only, never reorder once shipped).
Keys match vst/engine.cpp. Per track (0-15): machine (a raw OS machine id, not a named option list - the
id table is decoded from the user's own firmware at runtime, see docs/FIRMWARE.md, so this repo can't
commit real names for it), vol, pan, the AMP/EFX page's 8 params (AMD/AMF/EQF/EQG/FLTF/FLTW/FLTQ/SRR -
HostModel raw params 8-15), and the ROUTE page's 6 not-otherwise-exposed params (DIST/DEL/REV/LFOS/
LFOD/LFOM - raw 16, 19-23). This 8+6 split (not one 9-param FX page) matches real hardware: the
`SynthesisEffectsRouting` panel button actually cycles THREE pages (SYN -> AMP/EFX "TFX" -> ROUTE
"ROUT" -> back to SYN), each an 8-dial grid; DIST is ROUTE's own first dial, not a 9th AMP/EFX param -
see HANDOFF.md, "found the third per-track page: ROUTE, and a real DIST placement bug". VOL/PAN are
also real ROUTE-page citizens on hardware, but keep their own existing keys/knobs rather than
duplicating them under ROUTE. FX_PARAMS' defaults give a clean, filter-open, EQ-flat starting point
(matching tools/mdrender/mdrender.cpp's demo kit), not the raw all-zero default, which would otherwise
leave every track's filter effectively closed; ROUTE_PARAMS default to 0 (matching real hardware's own
send-level/LFO-off defaults).

Not yet exposed: SYN1-8 (their good defaults are per-machine, set by HostModel::setMachine() itself;
a flat VST default would stomp them the moment a machine is assigned - needs "has the user actually
touched this knob" tracking in engine.cpp first, not done yet). Plus two globals, tempo and max_voices
(HostModel::setMaxActiveVoices, HANDOFF.md "adjustable voice cap")."""
import json

# key, name, default - HostModel raw params 8-15, in the real hardware's own AMP/EFX page order
FX_PARAMS = [
    ("amd", "AMD", 0), ("amf", "AMF", 0), ("eqf", "EQF", 64), ("eqg", "EQG", 64),
    ("fltf", "FLTF", 0), ("fltw", "FLTW", 127), ("fltq", "FLTQ", 0), ("srr", "SRR", 0),
]
# key, name, default - HostModel raw params 16, 19-23, in the real hardware's own ROUTE page order
ROUTE_PARAMS = [
    ("dist", "DIST", 0), ("del", "DEL", 0), ("rev", "REV", 0),
    ("lfos", "LFOS", 0), ("lfod", "LFOD", 0), ("lfom", "LFOM", 0),
]

params = []
sections = []
for t in range(16):
    keys = []
    params.append({"key": "track%d_machine" % t, "name": "T%d Machine" % (t + 1), "min": 0, "max": 191,
                    "default": 0, "display": "int"})
    keys.append("track%d_machine" % t)
    params.append({"key": "track%d_vol" % t, "name": "T%d Vol" % (t + 1), "min": 0, "max": 127,
                    "default": 100, "display": "int"})
    keys.append("track%d_vol" % t)
    params.append({"key": "track%d_pan" % t, "name": "T%d Pan" % (t + 1), "min": 0, "max": 127,
                    "default": 64, "display": "int"})
    keys.append("track%d_pan" % t)
    for key, label, default in FX_PARAMS:
        params.append({"key": "track%d_%s" % (t, key), "name": "T%d %s" % (t + 1, label), "min": 0, "max": 127,
                        "default": default, "display": "int"})
        keys.append("track%d_%s" % (t, key))
    for key, label, default in ROUTE_PARAMS:
        params.append({"key": "track%d_%s" % (t, key), "name": "T%d %s" % (t + 1, label), "min": 0, "max": 127,
                        "default": default, "display": "int"})
        keys.append("track%d_%s" % (t, key))
    sections.append(("TRACK %d" % (t + 1), keys))

params.append({"key": "tempo", "name": "Tempo", "min": 30, "max": 300, "default": 120, "display": "int"})
params.append({"key": "max_voices", "name": "Max Voices", "min": 1, "max": 16, "default": 16, "display": "int"})
sections.append(("GLOBAL", ["tempo", "max_voices"]))

json.dump({"name": "Machinedrum One", "params": params,
           "sections": [{"label": l, "keys": k} for l, k in sections]},
          open("params.json", "w"), indent=1)
