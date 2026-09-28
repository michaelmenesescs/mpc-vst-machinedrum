"""Phase 3 of the "Monomodule way" skin plan (HANDOFF.md): hand-authored UI structure. Unlike the
font/icon atlases, page layout has no ROM-derived shortcut - this is manually encoded from navigating
the real screens (bit-exact captures via tools/mdtrace), the same role Monomodule's own SpecData.cpp
plays for the Monomachine.

Key finding this session: the SYN page and the AMP/EFX page are **the same physical widget** - an
identical 4-col x 2-row dial grid, just with different label text and different bound parameters
(confirmed by diffing the two captured screens: dial positions are pixel-identical, only the label
text band above each dial and which parameter each dial reads differ). So there's one grid geometry
here (DIAL_GRID), reused by both screens listed in SCREENS. `tools/mdtrace/build_dial_atlas.py`'s
`dial_crop_for()` already encodes the same per-cell geometry independently for its icon crop; the two
should eventually share one source of truth (not merged yet - keep in sync by hand for now, note the
duplication if either changes).

Label positions (LABEL_BAND below) were found by diffing the AMP/EFX and SYN page captures at the
label row: identical vertical band (y=3-7 of the LCD), each column ~20px wide starting at x=52 - i.e.
the same column pitch as the dial grid below it, just one geometry higher up. Not yet pixel-verified
per individual column edge (this is an approximate band, not confirmed against a third page) - refine
before Phase 4 needs exact multi-line text wrapping.
"""

# Shared geometry for the 8-dial grid (both SCREENS below use this unchanged).
DIAL_GRID = {
    "cols": 4, "rows": 2,
    "dial_x0": 52, "dial_col_pitch": 21, "dial_w": 14,
    "dial_y0": 14, "dial_row_pitch": 31, "dial_h": 12,
    "label_y0": 3, "label_h": 5,           # label text band, above the dial row divider at y~13
    "label_col_pitch": 20,                  # approx - see module docstring, not yet independently verified
}

SCREENS = {
    "amp_fx": {
        "reach": ["SynthesisEffectsRouting"],  # panel: action(s) from home to reach this screen
        "grid": DIAL_GRID,
        # Fixed labels, same for every machine (this is the mixer DSP's own per-track FX page, not
        # per-machine) - order matches gen_params.py's FX_PARAMS and HostModel's raw params 8-16.
        "labels": ["AMD", "AMF", "EQF", "EQG", "FLTF", "FLTW", "FLTQ", "SRR"],
    },
    "syn": {
        "reach": [],  # this is the default/home page - no navigation needed
        "grid": DIAL_GRID,
        # Labels are per-machine and NOT hand-transcribed: pull MachineInfo::params[0..7] for
        # whatever machine is currently assigned to the track being rendered (engine/MachineRunner.h).
        # Confirmed by example: the default kit's track 1 machine (TRX UW) showed
        # PTCH DEC RAMP HOLD / TICK NOIS DIRT DIST, which is exactly that machine's own SYN1-8 names.
        "labels": "dynamic:MachineInfo.params",
    },
}

# Not modeled yet (out of scope for the current Phase 3 pass, or actively parked - see HANDOFF.md):
#   - The LEV bar-meter (top-left of both screens) - parked per explicit direction; not blocking
#     Phase 3/4, which can render AMP/EFX and SYN pages without it.
#   - KIT name box / breadcrumb line (bottom-left) - static chrome, not a per-machine control; not
#     needed for the compositor's job of drawing accurate per-machine dial labels.
#   - Any screen beyond these two (LFO page, routing page, etc. - existence not yet confirmed; see
#     HANDOFF.md's page-navigation notes, panel:DataPageForward/Backward had no visible effect on the
#     AMP/EFX screen when tried).
