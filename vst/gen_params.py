#!/usr/bin/env python3
"""Writes params.json for Machinedrum One (VST index = order; append only, never reorder once shipped).
Keys match vst/engine.cpp. V1: per track (0-15), machine (a raw OS machine id, not a named option list -
the id table is decoded from the user's own firmware at runtime, see docs/FIRMWARE.md, so this repo can't
commit real names for it), level and pan. Plus two globals, tempo and max_voices (HostModel::
setMaxActiveVoices, HANDOFF.md "adjustable voice cap")."""
import json

params = []
sections = []
for t in range(16):
    keys = []
    params.append({"key": "track%d_machine" % t, "name": "T%d Machine" % (t + 1), "min": 0, "max": 191,
                    "default": 0, "display": "int"})
    keys.append("track%d_machine" % t)
    params.append({"key": "track%d_level" % t, "name": "T%d Level" % (t + 1), "min": 0, "max": 127,
                    "default": 100, "display": "int"})
    keys.append("track%d_level" % t)
    params.append({"key": "track%d_pan" % t, "name": "T%d Pan" % (t + 1), "min": 0, "max": 127,
                    "default": 64, "display": "int"})
    keys.append("track%d_pan" % t)
    sections.append(("TRACK %d" % (t + 1), keys))

params.append({"key": "tempo", "name": "Tempo", "min": 30, "max": 300, "default": 120, "display": "int"})
params.append({"key": "max_voices", "name": "Max Voices", "min": 1, "max": 16, "default": 16, "display": "int"})
sections.append(("GLOBAL", ["tempo", "max_voices"]))

json.dump({"name": "Machinedrum One", "params": params,
           "sections": [{"label": l, "keys": k} for l, k in sections]},
          open("params.json", "w"), indent=1)
