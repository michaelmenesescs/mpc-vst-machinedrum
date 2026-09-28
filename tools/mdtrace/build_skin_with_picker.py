#!/usr/bin/env python3
"""Builds the Machinedrum One skin the normal way (shadow_skin.build(), same as
mpc-vst-plugins' own write_skin()), then INJECTS the machine picker - duplicating
mpc-vst-monomodule's own mk_skin.py technique of calling shadow_skin.py's low-level primitives
directly (ss._local/_placed/_button/_bounds), since the generic layout.conf widgets (`popup`,
`enum_h/v`) don't scale to 131 options (see HANDOFF.md, "Machine picker: duplicating Monomodule's
own design"). This is a thin addition on top of the normal pipeline, not a parallel rewrite of it -
`shadow_skin.build()` already returns the (componentDefinitions, tabs, qlinkMap) Python structures
`write_skin()` would otherwise serialize directly; this script injects into them first.

Per track, adds:
  - A machine-bar field (build_machine_bar.py's per-machine images, one shown at a time via
    IndexedEnabling on track%d_machine) that toggles track%d_machine__open on tap.
  - The static picker_panel.png (build_machine_picker.py) as one image, shown only while
    track%d_machine__open is set (IndexedEnabling on the hidden open flag - see gen_params.py's
    popup_of mechanism, which keeps this flag local to the wrapper for free).
  - One transparent touch button per machine (from build_machine_picker.py's manifest.json rects),
    bound to track%d_machine directly (a radio-group Button component, same mechanism as a layout
    `popup`'s own option buttons) - tapping one sets that machine and the wrapper auto-closes the
    open flag (popup_picked(), also for free).

usage: build_skin_with_picker.py <layout.conf> <params.json> <skin_dir> <art_bin>
                                  <picker_manifest.json> <bar_manifest_dir>
  Both build_machine_picker.py's picker_panel.png and build_machine_bar.py's bar_<id>.png files must
  already be copied directly into <skin_dir> by the caller (this script only references their bare
  filenames, matching where shadow_skin.build() itself expects image= paths to resolve from - it
  doesn't run the capture pipeline itself).
  picker_manifest.json  build_machine_picker.py's manifest.json (machine id -> {rect, ...})
  bar_manifest_dir      build_machine_bar.py's output dir (bar_manifest.json + bar_<id>.png files)
"""
import json
import os
import sys

sys.path.insert(0, "/home/sam/mpc-vst/tools")
import shadow_skin as ss  # noqa: E402

NUM_MACHINE_OPTIONS = 192  # track%d_machine's declared max (191) + 1, gen_params.py
CLEAR_PNG = "clear.png"  # a 1x1 transparent PNG - the caller must ensure this exists in skin_dir


def enabling(index, key, i, n):
    return "IndexedEnabling/%d/%d/Parameter %d" % (i, n, index[key])


def inject_machine_picker(comps, tabs, index, picker_manifest, bar_manifest, panel_w, panel_h):
    """Mutates comps (dict) and tabs (list of {"tab": name, "kids": [...]}) in place - see
    shadow_skin.build()'s own docstring for their exact shape."""
    machines = picker_manifest["machines"]  # {"28": {"rect": [x,y,w,h], ...}, ...}
    BAR_X, BAR_Y, BAR_W, BAR_H = 20, 15, 51, 13  # matches build_machine_bar.py's own bar geometry
    PANEL_X, PANEL_Y = 0, 90  # picker overlays the whole tab below the header

    # Each machine needs its OWN local def (not one shared def) since a Button's buttonId (bid) is
    # baked into the component definition, not the placement - see the per-machine loop below.
    # One shared local def for the bar field (tap toggles the open flag).
    bar_field_key = "mdMachineBarField"
    if bar_field_key not in comps:
        comps[bar_field_key] = ss._local(
            bar_field_key,
            [ss._action("Mouse Down", "Toggle Switch"), ss._action("Enter Pressed", "Toggle Switch")],
            [ss._focus(BAR_W, BAR_H)])
    # One shared local def per machine's bar image (IndexedEnabling on track%d_machine - the SAME
    # image asset is reused across every track, only the placement's bound parameter differs).
    bar_image_key = {}
    for mid, info in bar_manifest.items():
        key = "mdBarImg_%s" % mid
        bar_image_key[mid] = key
        if key not in comps:
            comps[key] = ss._local(key, [], [ss._sub(
                "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": info["file"]},
                ss._bounds(0, 0, info["w"], info["h"]), "Image")])
    # One shared local def for the static picker panel image.
    panel_key = "mdPickerPanel"
    if panel_key not in comps:
        comps[panel_key] = ss._local(panel_key, [], [ss._sub(
            "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": "picker_panel.png"},
            ss._bounds(0, 0, panel_w, panel_h), "Image")])

    for t in range(16):
        tab_name = "TRACK %d" % (t + 1)
        page = next(p for p in tabs if p["tabName"] == tab_name)
        kids = comps[page["componentName"]]["value"]["componentsData"]
        open_key = "track%d_machine__open" % t
        machine_key = "track%d_machine" % t
        open_i = index[open_key]
        machine_i = index[machine_key]

        # Bar field: toggles the open flag, positioned over the bar images.
        kids.append(ss._placed(bar_field_key, "Machine picker T%d" % (t + 1), open_i,
                                BAR_X, BAR_Y + ss.Y_OFF, BAR_W, BAR_H, focus="Yes"))
        # One bar image per machine, shown only when it's the current one.
        for mid, key in bar_image_key.items():
            c = ss._placed(key, "Machine bar T%d %s" % (t + 1, bar_manifest[mid]["name"]), machine_i,
                            BAR_X, BAR_Y + ss.Y_OFF, bar_manifest[mid]["w"], bar_manifest[mid]["h"], focus="No")
            c["bounds"]["showWhenDataModelInvalid"] = "Show"
            c["bounds"]["additionalInvalidatingHandles"] = [enabling(index, machine_key, int(mid), NUM_MACHINE_OPTIONS)]
            kids.append(c)

        shown = enabling(index, open_key, 1, 2)
        panel_c = ss._placed(panel_key, "Machine list T%d" % (t + 1), open_i,
                              PANEL_X, PANEL_Y + ss.Y_OFF, panel_w, panel_h, focus="No")
        panel_c["bounds"]["showWhenDataModelInvalid"] = "Show"
        panel_c["bounds"]["additionalInvalidatingHandles"] = [shown]
        kids.append(panel_c)

        for mid, minfo in machines.items():
            rx, ry, rw, rh = minfo["rect"]
            per_mid_key = "mdPickOpt_%s" % mid
            if per_mid_key not in comps:
                comps[per_mid_key] = ss._local(
                    per_mid_key, [ss._action("Mouse Down", "Q-Link")],
                    [ss._button(CLEAR_PNG, CLEAR_PNG, int(mid), NUM_MACHINE_OPTIONS, rw, rh)])
            c = ss._placed(per_mid_key, "Machine %s T%d" % (minfo["name"], t + 1), machine_i,
                            PANEL_X + rx, PANEL_Y + ry + ss.Y_OFF, rw, rh, focus="No")
            c["bounds"]["showWhenDataModelInvalid"] = "Show"
            c["bounds"]["additionalInvalidatingHandles"] = [shown]
            kids.append(c)


def main() -> int:
    if len(sys.argv) < 7:
        print(__doc__, file=sys.stderr)
        return 2
    layout_path, params_path, skin_dir, art_bin, picker_manifest_path, bar_dir = sys.argv[1:7]
    params = json.load(open(params_path))["params"]
    index = {p["key"]: i for i, p in enumerate(params)}

    from PIL import Image
    os.makedirs(skin_dir, exist_ok=True)
    comps_list, tabs, qmap = ss.build(layout_path, params, skin_dir, art_bin, lambda a, b: Image.open(a).save(b))
    # build() returns componentDefinitions as a flat list (each {"key":..., "value":...}, the same
    # shape ss._local() itself produces); a dict keyed by "key" is far more useful for the lookups
    # and additions below - flattened back to a list just before writing TUI.json.
    comps = {c["key"]: c for c in comps_list}

    picker_manifest = json.load(open(picker_manifest_path))
    bar_manifest = json.load(open(os.path.join(bar_dir, "bar_manifest.json")))
    inject_machine_picker(comps, tabs, index, picker_manifest, bar_manifest,
                          picker_manifest["panel_w"], picker_manifest["panel_h"])

    tui = {"pageData": {
        "version": 1,
        "componentDefinitions": {"version": 2, "importFiles": [
            ss.AKAI + "Generic/Generic Knob Overlay.json", ss.AKAI + "Generic/Generic Menu Overlay.json"],
                                 "localComponentDefinitions": list(comps.values())},
        "info": {"version": 1, "type": "CompleteDescription"},
        "tabs": tabs}}
    json.dump(tui, open(os.path.join(skin_dir, "TUI.json"), "w"), indent=1)
    print(f"wrote {os.path.join(skin_dir, 'TUI.json')} with machine picker injected "
          f"({len(picker_manifest['machines'])} machines x {len(tabs)} tracks)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
