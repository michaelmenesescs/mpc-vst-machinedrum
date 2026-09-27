#!/usr/bin/env python3
"""Writes params.json for Machinedrum One (VST index = order; append only, never reorder once shipped).
Keys match vst/engine.cpp. Per track (0-15): machine (a raw OS machine id, not a named option list - the
id table is decoded from the user's own firmware at runtime, see docs/FIRMWARE.md, so this repo can't
commit real names for it), vol, pan, and the mixer DSP's 9 per-track FX params (AMD/AMF/EQF/EQG/FLTF/
FLTW/FLTQ/SRR/DIST - HostModel raw params 8-16, confirmed against the real hardware's own AMP/EFX page,
see HANDOFF.md "real screen navigation confirmed"). Defaults for these give a clean, filter-open,
EQ-flat starting point (matching tools/mdrender/mdrender.cpp's demo kit), not the raw all-zero default,
which would otherwise leave every track's filter effectively closed.

Not yet exposed: SYN1-8 (their good defaults are per-machine, set by HostModel::setMachine() itself;
a flat VST default would stomp them the moment a machine is assigned - needs "has the user actually
touched this knob" tracking in engine.cpp first, not done yet) and DEL/REV/LFOS/LFOD/LFOM (send
levels and LFO routing - straightforward to add the same way as the FX params below, just not done
yet for scope). Plus two globals, tempo and max_voices (HostModel::setMaxActiveVoices, HANDOFF.md
"adjustable voice cap")."""
import json

# key, name, default - HostModel raw param 8-16, in the real hardware's own AMP/EFX page order
FX_PARAMS = [
    ("amd", "AMD", 0), ("amf", "AMF", 0), ("eqf", "EQF", 64), ("eqg", "EQG", 64),
    ("fltf", "FLTF", 0), ("fltw", "FLTW", 127), ("fltq", "FLTQ", 0),
    ("srr", "SRR", 0), ("dist", "DIST", 0),
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
    sections.append(("TRACK %d" % (t + 1), keys))

params.append({"key": "tempo", "name": "Tempo", "min": 30, "max": 300, "default": 120, "display": "int"})
params.append({"key": "max_voices", "name": "Max Voices", "min": 1, "max": 16, "default": 16, "display": "int"})
sections.append(("GLOBAL", ["tempo", "max_voices"]))

json.dump({"name": "Machinedrum One", "params": params,
           "sections": [{"label": l, "keys": k} for l, k in sections]},
          open("params.json", "w"), indent=1)
