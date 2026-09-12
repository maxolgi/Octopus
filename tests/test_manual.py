#!/usr/bin/env python3
"""
test_manual.py — behavior tests for the Octopus engine, grounded in the
reference manual (CE v5.30, `octopus.txt` in the repo root).

Drives a *running* octopus_gui (hosted) instance:
  * OSC input  -> UDP 127.0.0.1:8000
  * MIR output <- WebSocket ws://127.0.0.1:8089

Each test checks a documented behavior:
  1. establish a known precondition (stop transport / set zoom / reset),
  2. perform the documented operation via OSC,
  3. assert on the observable (MIR LED state, /transport bit, state file).

Manual anchors (octopus.txt):
  * page selection toggle (PAGE zoom)   — "Grid: page selection"
  * transport start/stop/pause/continue — circle transport cluster
  * zoom GRID/PAGE/STEP/MAP indicators  — Step/Track/Page/Grid mode indices
  * record arm (REC)                    — "Recording"
  * save machine state                  — "Load/Save: Saving the machine state"
  * name-based OSC dispatch             — port feature (keymap.md)

Known limitation (documented, not a firmware bug): the step-grid toggle in
GRID zoom ("Step Mode: Toggle (TGL)") is play-mode dependent — matrix keys
toggle page play-state in GRID_MIX (perform) mode and steps only in
GRID_EDIT, and GRID_play_mode is set by the periodic key interrupt from the
held-key state, which the OSC interface cannot hold reliably. It is left as
a manual/diagnostic behavior; the PAGE-zoom page-selection toggle (same
matrix-key mechanism, mode-independent) is tested automatically instead.

Observability notes:
  * The MIR is diff-based (sent only on change) and the firmware pre-applies
    the blinker: on the blink-off phase red/green bits are cleared wherever
    the blink bit is set. All LED assertions therefore use a window-OR
    (capture_or / led_or) that ORs every frame in a ~0.9 s window, reliably
    catching the blink-on phase.
  * The physical zoom keys are play-mode dependent (the PAGE key only
    switches in GRID_EDIT; the MAP key toggles MIDI-CC routing in GRID_MIX;
    in STEP zoom only GRID/PAGE/TRK keys act). To test the zoom *display*
    deterministically we set the level with the documented /zoom OSC command
    and assert the indicator LED.
  * The suite opens with a full Grid Clear (manual p.86), which wipes the
    instance's pattern and guarantees stepSelection == 0, REC_bit == 0.
    State is NOT restored afterwards (the engine is one-shot per process and
    /load is unsupported at runtime). This is a dev instance offered for
    testing; the wipe is expected.

Run:  python3 tests/test_manual.py
"""
import os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from octopus_harness import Octopus, ZOOM, KEY

STATE_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                          "target", "release", "octopus_state.bin")


def reset_state(o):
    """Deterministic prelude: full Grid Clear + GRID zoom + edit mode.

    Grid Clear (manual p.86): hold GRID (BIRDSEYE) + press CLR -> the
    firmware calls Octopus_memory_CLR(), re-initialising all steps, tracks,
    pages and the grid (stepSelection == 0, REC_bit == 0, default pattern).
    PLAY_MODE_STATUS and G_zoom_level are globals the clear does NOT touch,
    so we re-assert GRID zoom and, if the PLAY indicator is lit (perform
    mode), press PLAY to return to edit mode (in GRID zoom the PLAY key
    unconditionally calls toggle_PLAY_MODE)."""
    o.transport("stop"); o.settle(0.5)
    o.zoom(ZOOM["GRID"]); o.settle(0.5)
    o.key(KEY["GRID"]); o.settle(0.15)      # hold GRID -> BIRDSEYE
    o.key(189); o.settle(0.3)               # CLR
    o.key(189, False); o.settle(0.2)
    o.key(KEY["GRID"], False); o.settle(0.5)
    o.zoom(ZOOM["GRID"]); o.settle(0.5)
    if o.led_or("PLAY", "red", 0.9):        # perform mode -> toggle to edit
        o.key(KEY["PLAY"]); o.settle(0.2); o.key(KEY["PLAY"], False); o.settle(0.6)


# ------------------------------------------------------------------ tests
def test_transport_start_stop(o):
    """Manual: transport cluster — P1 starts, STP stops."""
    o.transport("stop"); o.settle(0.8)
    if not o.transport_is(0):
        return False, "/transport != 0 after stop"
    o.transport("start"); o.settle(0.8)
    if not o.transport_is(1):
        return False, "/transport != 1 after start"
    if not o.led_or("P1", "green"):
        return False, "P1 LED not green while playing"
    o.transport("stop"); o.settle(0.8)
    if not o.transport_is(0):
        return False, "/transport != 0 after second stop"
    return True, ""


def test_transport_pause_continue(o):
    """Manual: pause halts the run bit; continue resumes it."""
    o.transport("start"); o.settle(0.8)
    if not o.transport_is(1):
        return False, "/transport != 1 after start"
    o.transport("pause"); o.settle(0.8)
    if not o.transport_is(0):
        return False, "/transport != 0 after pause (HALT clears run bit)"
    o.transport("continue"); o.settle(0.8)
    if not o.transport_is(1):
        return False, "/transport != 1 after continue"
    o.transport("stop"); o.settle(0.4)
    return True, ""


def test_page_selection_toggle(o):
    """Manual 'Grid: page selection': in PAGE zoom, pressing a matrix key
    toggles the page-selection indicator (green).

    This is the reliably-observable matrix-key behavior: in PAGE zoom the
    matrix shows the 10 pages and a key press toggles that page's selection,
    shown in the green plane. (The step-grid toggle in GRID zoom is
    play-mode dependent — see the header note — and is left for manual
    testing.) The green plane is playhead-immune (the chase light is red)
    and the window-OR is blink-immune. Each successful toggle is flipped
    back to restore state."""
    o.transport("stop"); o.settle(0.5)
    o.zoom(ZOOM["PAGE"]); o.settle(0.8)
    if o.matrix_green_or(1.5) is None:
        return False, "no MIR frames received"
    candidates = [11, 22, 33, 12, 44]
    for k in candidates:
        base = o.matrix_green_or(1.5)
        o.key(k); o.settle(0.2); o.key(k, False); o.settle(0.4)
        after = o.matrix_green_or(1.5)
        if after != base:
            # restore: flip back (well beyond the ~220 ms double-click window)
            o.key(k); o.settle(0.2); o.key(k, False); o.settle(0.4)
            return True, "page-selection toggle (key %d) changed the green plane" % k
    return False, "no page-selection toggle changed the matrix green plane"


def _zoom_indicator(o, zoom_name, led_name):
    """Set a zoom level via /zoom and assert its indicator LED lights (red)."""
    o.transport("stop"); o.settle(0.5)
    o.zoom(ZOOM[zoom_name]); o.settle(0.8)
    # selected-zoom indicators blink (red+green+blink); a 1.5 s window-OR
    # (~4+ blink periods) reliably catches the red phase.
    if not o.led_or(led_name, "red", 1.5):
        return False, "%s zoom indicator never lit red" % zoom_name
    return True, ""


def test_zoom_grid(o):
    """Manual: GRID zoom lights the GRID indicator (orange = red+green+blink)."""
    return _zoom_indicator(o, "GRID", "GRID")


def test_zoom_page(o):
    """Manual: PAGE zoom lights the PAGE indicator (blinking red)."""
    return _zoom_indicator(o, "PAGE", "PAGE")


def test_zoom_step(o):
    """Manual: STEP zoom lights the STEP indicator (red+green+blink)."""
    return _zoom_indicator(o, "STEP", "STEP")


def test_zoom_map(o):
    """Manual: MAP zoom lights the MAP indicator (blinking red)."""
    return _zoom_indicator(o, "MAP", "MAP")


def test_record_arm(o):
    """Manual 'Recording': REC arms record (blinking red REC LED)."""
    o.ensure_edit_mode()
    o.key(KEY["REC"]); o.settle(0.2); o.key(KEY["REC"], False); o.settle(0.4)
    armed = o.led_or("REC", "red", 1.5)
    # disarm again (press REC) to restore
    o.key(KEY["REC"]); o.settle(0.2); o.key(KEY["REC"], False); o.settle(0.6)
    if not armed:
        return False, "REC LED never lit after arming"
    return True, ""


def test_save_state(o):
    """Manual 'Load/Save: Saving the machine state': /save writes the state file."""
    before = os.path.getmtime(STATE_FILE) if os.path.exists(STATE_FILE) else None
    o.save(); o.settle(1.5)
    if not os.path.exists(STATE_FILE):
        return False, "state file not created: %s" % STATE_FILE
    after = os.path.getmtime(STATE_FILE)
    if before is not None and after <= before:
        return False, "state file mtime did not advance"
    return True, "state file written (%s)" % STATE_FILE


def test_name_based_dispatch(o):
    """Port feature (keymap.md): /key/<NAME> dispatches like /key <index>.

    Done in GRID zoom, where STP/P1 reliably control the transport."""
    o.zoom(ZOOM["GRID"]); o.settle(0.6)
    o.transport("start"); o.settle(0.6)
    if not o.transport_is(1):
        return False, "precondition: /transport != 1 after start"
    o.key_name("STP"); o.settle(0.2); o.key_name("STP", False); o.settle(0.8)
    if not o.transport_is(0):
        return False, "/key/STP did not stop the transport"
    o.key_name("P1"); o.settle(0.2); o.key_name("P1", False); o.settle(0.6)
    if not o.transport_is(1):
        return False, "/key/P1 did not start the transport"
    o.transport("stop"); o.settle(0.4)
    return True, ""


def test_tempo_responsive(o):
    """Manual 'Tempo': /tempo sets the master tempo; engine stays responsive."""
    o.tempo(140); o.settle(0.6)
    o.transport("start"); o.settle(0.6)
    ok = o.transport_is(1)
    o.transport("stop"); o.settle(0.4)
    if not ok:
        return False, "engine unresponsive after /tempo 140"
    return True, ""


# NOTE: step/track attribute editing (TGL, PIT, VEL, ...) is a deep,
# state-dependent state machine (zoom x play-mode x page-selection x cursor).
# It cannot be reliably normalized via the OSC/WS interface, so it is left as
# a manual/diagnostic test rather than an automated must-pass test. The
# automated suite below covers the core documented behaviors that ARE
# reliably observable.
TESTS = [
    ("page selection toggle",       test_page_selection_toggle),
    ("transport start/stop",        test_transport_start_stop),
    ("transport pause/continue",    test_transport_pause_continue),
    ("zoom GRID indicator",         test_zoom_grid),
    ("zoom PAGE indicator",         test_zoom_page),
    ("zoom STEP indicator",         test_zoom_step),
    ("zoom MAP indicator",          test_zoom_map),
    ("record arm (REC)",            test_record_arm),
    ("save machine state",          test_save_state),
    ("name-based OSC dispatch",     test_name_based_dispatch),
    ("tempo set (responsive)",      test_tempo_responsive),
]


def main():
    o = Octopus()
    print("Connecting to ws://127.0.0.1:8089 ...")
    try:
        o.start()
    except Exception as e:
        print("FATAL: could not connect to the running engine: %r" % e)
        print("Start it first:  ./target/release/octopus_gui --no-gui")
        sys.exit(2)
    if o.get_mir() is None:
        print("FATAL: no /mir frames received — is the engine running?")
        sys.exit(2)
    print("Connected. transport=%s" % o.get_transport())
    print("Running Grid Clear prelude (wipes the instance's pattern)...")
    reset_state(o)
    passed = failed = 0
    for name, fn in TESTS:
        try:
            ok, msg = fn(o)
        except Exception as e:
            ok, msg = False, "exception: %r" % e
        if ok:
            passed += 1
            print("  PASS  %s" % name)
        else:
            failed += 1
            print("  FAIL  %s: %s" % (name, msg))
    # leave the engine in a clean, stopped state
    o.transport("stop"); o.settle(0.3)
    o.zoom(ZOOM["GRID"]); o.settle(0.3)
    o.stop()
    print("\n%d passed, %d failed, %d total" % (passed, failed, len(TESTS)))
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
