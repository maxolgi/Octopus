#!/usr/bin/env python3
"""
build_sessions.py - Generate Open Stage Control session files for the
Octopus and Nemo Linux ports.

Outputs:
  octopus.json  - 10-row Octopus grid
  nemo.json     - 8-row Nemo grid

Each session mirrors the hardware front panel:
  - step grid matrix (with LED feedback via the custom module)
  - attribute column (left) + mutator column (right)
  - left MIX rotaries + right EDIT rotaries
  - mix-target strip
  - circle: notes, scale nav, zoom, transport, scales, big-knob,
    chain, program, chord
  - transport header + tempo knob

Run:  python3 build_sessions.py
"""
import json
import os

ENGINE = "127.0.0.1:8000"

# Shared accent / colour defaults
LED_BTN = {
    "type": "button",
    "mode": "push",
    "decoupled": True,
    "on": 1,
    "off": 0,
    "address": "/key",
    "alphaFillOn": 1,
    "alphaFillOff": 0.1,
    "colorWidget": "auto",
    "width": 34,
    "height": 34,
}


def btn(key, label, w=34, h=34):
    """A decoupled push button that sends /key <key> <1|0> and lights from /mir."""
    b = dict(LED_BTN)
    b.update({
        "id": "k_%d" % key,
        "preArgs": [key],
        "label": label,
        "width": w,
        "height": h,
        "target": ENGINE,
    })
    return b


def rot(index, label):
    """A knob that emits discrete /rotary <index> <1=dec|2=inc> on turn."""
    script = (
        "if(locals.p===undefined){locals.p=value}"
        "else if(value!==locals.p){"
        "send('%s','/rotary',%d,value>locals.p?2:1);locals.p=value}"
    ) % (ENGINE, index)
    return {
        "type": "knob",
        "id": "rot_%s" % label,
        "label": label,
        "bypass": True,
        "sensitivity": 1,
        "default": 0.5,
        "range": {"min": 0, "max": 1},
        "width": 42,
        "height": 54,
        "onValue": script,
        "colorWidget": "#8899bb",
    }


# ------------------------------------------------------------------ data maps
NOTES = [212, 211, 210, 209, 208, 217, 225, 226, 235, 236, 237, 238, 239]
NOTE_LBL = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", "Cu"]
SCALENAV = [(221, "MOD"), (222, "SEL"), (230, "CAD")]
ZOOM = [(218, "GRID"), (219, "PAGE"), (220, "TRK"), (227, "STEP"), (228, "MAP"), (229, "PLAY")]
TRANSPORT = [(223, "REC"), (231, "STOP"), (232, "PSE"), (241, "P1"), (240, "P2"), (250, "P4"), (251, "ALN")]
SCALES = [(243, "MY"), (244, "PEN"), (245, "WHL"), (246, "MAJ"), (247, "MIN"), (248, "DIM"), (249, "CHR")]
BIGKNOB = [(201, "1"), (200, "2"), (199, "3"), (198, "4"), (197, "5"),
           (207, "6"), (206, "7"), (216, "8"), (215, "9"), (224, "100"), (233, "200")]
CHAIN = [(205, "CH1"), (204, "CH2"), (203, "CH3"), (202, "CH4")]
CHANNEL = [(213, "CHN"), (214, "FLW")]
PROGRAM = [(242, "PGM"), (234, "TPO")]
CHORD = [(258, "0"), (257, "1"), (256, "2"), (255, "3"), (254, "4"), (253, "5"), (252, "6")]
ATTR = [(1, "VEL"), (2, "PIT"), (3, "LEN"), (4, "STR"), (5, "POS"),
        (6, "DIR"), (7, "AMT"), (8, "GRV"), (9, "MCC"), (10, "MCH")]
MUT = [(187, "TGL"), (188, "SOL"), (189, "CLR"), (190, "RND"), (191, "FLT"),
       (192, "RMX"), (193, "EFF"), (194, "ZOM"), (195, "CPY"), (196, "PST")]
MIX = [(21, "MIX"), (32, "SEL"), (43, "ATR"), (54, "VOL"), (65, "PAN"), (76, "MOD"),
       (87, "EXP"), (98, "U0"), (109, "U1"), (120, "U2"), (131, "U3"), (142, "U4"),
       (153, "U5"), (164, "MUT"), (175, "EDT"), (186, "ESC")]


def step_matrix(rows, id_prefix):
    """Build the step-grid matrix widget for `rows` tracks (10 octopus / 8 nemo).

    Cell $ fills top-to-bottom, left-to-right; top row = sequencer row (rows-1).
    key = 11 + col*11 + row ; col<8 -> MIR set 0, else set 1.
    The #{...} expression is evaluated by Open Stage Control per cell.
    """
    top = rows - 1
    preargs_expr = f"#{{11 + ($ % 16) * 11 + {top} - ($ - $ % 16) / 16}}"
    pw, ph = 26, 26
    return {
        "type": "matrix",
        "id": "grid",
        "layout": "grid",
        "gridTemplate": 16,
        "quantity": rows * 16,
        "widgetType": "button",
        "traversing": "smart",
        "scroll": False,
        "colorBg": "#1a1a1a",
        "width": 16 * pw + 8,
        "height": rows * ph + 4,
        "props": {
            "id": "%s_#{$}" % id_prefix,
            "address": "/key",
            "preArgs": [preargs_expr],
            "mode": "push",
            "decoupled": True,
            "on": 1,
            "off": 0,
            "label": False,
            "width": pw,
            "height": ph,
            "alphaFillOn": 1,
            "alphaFillOff": 0.08,
            "target": ENGINE,
        },
    }


def vpanel(id_, widgets, label=None, colorBg=None, scroll=False, layout="vertical",
           gridTemplate=None, width=None, height=None, expand=None):
    p = {"type": "panel", "id": id_, "layout": layout, "scroll": scroll, "widgets": widgets}
    if label:
        p["label"] = label
    if colorBg:
        p["colorBg"] = colorBg
    if gridTemplate is not None:
        p["gridTemplate"] = gridTemplate
    if width is not None:
        p["width"] = width
    if height is not None:
        p["height"] = height
    if expand is not None:
        p["expand"] = expand
    return p


def hbtn_row(id_, items, w=34, h=34):
    """Horizontal row of LED buttons."""
    return vpanel(id_, [btn(k, l, w, h) for k, l in items], layout="horizontal")


def build_session(rows, id_prefix, title):
    pw, ph = 36, 36
    grid_w = 16 * pw + 8           # 584
    matrix_h = rows * ph + 4       # 264 (octopus) / 212 (nemo)
    rot_w = 46
    col_w = 34
    msect_w = rot_w + col_w + grid_w + col_w + rot_w  # 984

    # ---- matrix section (left half of the body) ----
    # left MIX rotaries: R9..R0 -> indices 11..20 (top to bottom)
    left_rots = [rot(20 - r, "R%d" % r) for r in range(rows)]            # R9..R0
    # right EDIT rotaries: VEL..MCH -> indices 1..10 (top to bottom)
    right_rots = [rot(i + 1, ATTR[i][1]) for i in range(rows)]           # VEL..MCH
    attr_col = vpanel("attr_col", [btn(k, l, 30, 30) for k, l in ATTR[:rows]],
                      colorBg="#222", width=col_w, height=matrix_h)
    mut_col = vpanel("mut_col", [btn(k, l, 30, 30) for k, l in MUT[:rows]],
                     colorBg="#222", width=col_w, height=matrix_h)

    matrix_section = vpanel("matrix_section", [
        vpanel("rot_left", left_rots, colorBg="#222", width=rot_w, height=matrix_h),
        attr_col,
        step_matrix(rows, id_prefix),
        mut_col,
        vpanel("rot_right", right_rots, colorBg="#222", width=rot_w, height=matrix_h),
    ], layout="horizontal", colorBg="#222", width=msect_w, height=matrix_h)

    # ---- circle section (right half of the body) ----
    circle = vpanel("circle", [
        hbtn_row("notes", [(k, l) for k, l in zip(NOTES, NOTE_LBL)], 30, 30),
        hbtn_row("scalenav", SCALENAV, 30, 30),
        vpanel("zoom", [btn(k, l, 36, 32) for k, l in ZOOM], layout="grid", gridTemplate=3),
        hbtn_row("transport", TRANSPORT, 34, 34),
        hbtn_row("scales", SCALES, 30, 30),
        hbtn_row("bigknob", BIGKNOB, 30, 30),
        hbtn_row("chain", CHAIN + CHANNEL, 30, 30),
        hbtn_row("program", PROGRAM, 30, 30),
        hbtn_row("chord", [("chord_%s" % l,) and (k, l) for k, l in CHORD], 30, 30),
    ], colorBg="#1e1e2e", scroll=True, width=440, height=matrix_h)

    body = vpanel("body", [matrix_section, circle],
                  layout="horizontal", colorBg="#181818", width=msect_w + 448, height=matrix_h)

    # ---- transport header ----
    header = vpanel("header", [
        {"type": "text", "id": "title", "value": title,
         "width": 100, "height": 40, "css": "font-size:16px;font-weight:bold;color:#ccc;"},
        rot(0, "TPO"),
        {
            "type": "input", "id": "tempo", "label": "BPM", "numeric": True,
            "address": "/tempo", "target": ENGINE, "width": 80, "height": 40,
            "default": 120, "align": "center", "unit": " bpm",
            "validation": "^([1-9][0-9]?|1[0-9]{2})$",
        },
        {"type": "button", "id": "save_btn", "label": "SAVE",
         "address": "/save", "target": ENGINE, "mode": "tap", "on": 1,
         "width": 60, "height": 40, "colorWidget": "#aa6622"},
    ], layout="horizontal", colorBg="#262626", height=50)

    # ---- mix strip (bottom) ----
    mix_strip = vpanel("mix_strip", [btn(k, l, 38, 38) for k, l in MIX],
                       layout="horizontal", height=44)

    # ---- root ----
    root = {
        "type": "root",
        "id": "root",
        "linkId": "",
        "colorBg": "#101010",
        "layout": "vertical",
        "scroll": True,
        "widgets": [header, body, mix_strip],
    }
    # OSC session files wrap the root widget in {type:"session", content:{root}}.
    # version 1.99.99 disables all legacy converters (which expect data.session).
    return {
        "type": "session",
        "version": "1.99.99",
        "content": root,
    }


def main():
    out = os.path.dirname(os.path.abspath(__file__))
    for rows, prefix, title, fname in [
        (10, "pad", "OCTOPUS", "octopus.json"),
        (8, "npad", "NEMO", "nemo.json"),
    ]:
        session = build_session(rows, prefix, title)
        path = os.path.join(out, fname)
        with open(path, "w") as f:
            json.dump(session, f, indent=2)
        print("wrote %s" % path)


if __name__ == "__main__":
    main()
