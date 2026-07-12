# Octopus/Nemo Linux Port (Cuttlefish v6)

A faithful port of the Genoqs Octopus and Nemo MIDI sequencer firmware to Linux,
using ALSA for MIDI I/O and OSC for the control surface.

This is the **cuttlefish-v6** branch, built on the [octopus-cuttlefish-6.0.0](https://github.com/genoqs-community/source/tree/octopus-cuttlefish-6.0.0)
firmware. It adds SoloRec, MIDI slave clock, human record mode, Cut/Undo/Checkpoint,
and transpose/strum fixes on top of the standard CE firmware.

The `main` branch builds against the standard CE v0.0.5.30 firmware.

## Branches

| Branch | Firmware | Features |
|---|---|---|
| `main` | CE v0.0.5.30 (master) | Standard Octopus/Nemo |
| `cuttlefish-v6` | octopus-cuttlefish-6.0.0 | SoloRec, MIDI slave, recording, Cut/Undo |

Switch between them:
```bash
git checkout main              # or cuttlefish-v6
git submodule update --init    # updates firmware to match
```

## Quick Start

```bash
# Clone with firmware submodule
git clone --recurse-submodules <repo-url>
cd Octopus

# Build (requires: gcc, libasound2-dev)
make                    # build Octopus
make NEMO=1             # build Nemo variant

# Run the engine
./build/octopus &

# Start the web GUI
python3 web_gui.py

# Open in browser
# http://localhost:8080
```

If you cloned without `--recurse-submodules`, initialize the firmware submodule:
```bash
git submodule update --init
```

## Architecture

The original firmware source is a git submodule at `firmware/` (a fork of
[genoqs-community/source](https://github.com/genoqs-community/source) with
20 `#ifdef __linux__`/`_WIN32`/`NEMO` patches in `patches/`).

The entire 65,000-line firmware compiles and runs on Linux via an eCos
compatibility shim (`hal_linux.c`) that maps `cyg_*` APIs to pthreads, pipes,
POSIX semaphores, and timerfd. No original firmware logic is modified (only
20 minimal guarded patches for NULL safety, CPU-load stub, ALSA/winmm routing, and Nemo build compat).

### Modules

| File | Purpose |
|---|---|
| `src/main_linux.c` | Entry point, sequencer thread, auto-save/load |
| `src/hal_linux.c` | eCos API shim (pthread, pipe, timerfd) |
| `src/midi_alsa.c` | ALSA sequencer I/O (2 out ports + 1 in port) |
| `src/osc_server.c` | OSC UDP input server (dependency-free) |
| `src/osc_render.c` | MIR → OSC output bridge + 60 Hz render thread |
| `include/hal_linux.h` | eCos type/macro declarations |
| `web_gui.py` | WebSocket ↔ OSC bridge for browser control surface |
| `web_gui.html` | Browser-based grid UI |

### OSC Namespace

**Input (control surface → engine):**

| Address | Args | Maps to |
|---|---|---|
| `/octopus/key` | `ii` (index 1–260, press 1/release 0) | `executeKey(index)` |
| `/octopus/rotary` | `ii` (id 0–20, INC=2/DEC=1) | `executeRot()` |
| `/octopus/transport` | `s` ("start"/"stop"/"pause"/"continue") | transport state |
| `/octopus/tempo` | `i` (bpm 10–199) | `G_master_tempo` |
| `/octopus/zoom` | `i` (level 0–14) | `G_zoom_level` |
| `/octopus/clocksource` | `i` (0=OFF, 1=INT, 2=EXT) | `G_clock_source` |
| `/octopus/quit` | — | clean shutdown |

**Output (engine → control surface):**

| Address | Args | Content |
|---|---|---|
| `/octopus/mir` | `b` (170-byte blob) | Full MIR LED framebuffer |
| `/octopus/blink` | `i` | Master blinker phase |
| `/octopus/transport` | `i` | Playing (1) / stopped (0) |

### Key Index Reference

| Region | Index | Formula |
|---|---|---|
| Step matrix | 11–185 | `11 + col * 11 + row` (row 0–9, col 0–15) |
| Transport | 241=PLAY, 231=STOP, 232=PAUSE, 223=RECORD | |
| Zoom | 218=GRID, 219=PAGE, 220=TRACK, 227=STEP | |
| Track selectors | 1–10 | track 0–9 |
| Rotary encoders | 0–20 | TEMPO, VEL, PIT, LEN, STA, POS, DIR, AMT, GRV, MCC, MCH |

## MIDI Connections

The engine creates ALSA sequencer client with ports:
- `out_A` — channels 1–16 (physical MIDI out 1)
- `out_B` — channels 17–32 (physical MIDI out 2)
- `in` — MIDI input (clock slave, recording)

```bash
# Auto-connected to Midi Through for testing.
# Connect to a synth:
aconnect <octopus_client>:0 <synth_client>:<port>

# Monitor:
aseqdump -p <octopus_client>:0

# External clock source:
aconnect <clock_source>:<port> <octopus_client>:2
```

## Web GUI

There are **three interchangeable web GUI bridges**, all serving the same HTML
control surface (`web_gui.html` / `web_gui_nemo.html`) on the same ports
(HTTP 8080, WebSocket 8081, OSC 8000/9000). Only one should be running at a
time. They differ only in runtime dependencies and whether they also launch the
engine:

| Bridge | Language | Launches engine? | HTML files | Path |
|---|---|---|---|---|
| **Rust UI launcher** | Rust (egui) | Yes | Bundled in binary | [`ui/`](ui/README.md) |
| **Rust standalone bridge** | Rust | No | Reads from disk | [`cli_ui/`](cli_ui/README.md) |
| **Python bridge** | Python | No | Reads from disk | `web_gui.py` |

### Rust UI launcher — `ui/` → `octopus_ui`

Native desktop app with a MIDI device picker. Launches the engine automatically
and embeds the HTML control surface (no external files needed). See
[`ui/README.md`](ui/README.md).

```bash
cd ui && cargo build --release
./ui/target/release/octopus_ui
# Or headless:
./ui/target/release/octopus_ui --nogui --variant octopus
```

### Rust standalone bridge — `cli_ui/` → `cli_ui`

Same WebSocket ↔ OSC bridge as `web_gui.py` but no Python dependency. The engine
must be started separately. See [`cli_ui/README.md`](cli_ui/README.md).

```bash
./build/octopus &
cd cli_ui && cargo build --release
./cli_ui/target/release/cli_ui
```

### Python bridge — `web_gui.py`

```bash
pip3 install websockets
./build/octopus &
python3 web_gui.py
```

The HTML control surface is the same for all three — changes to
`web_gui.html` / `web_gui_nemo.html` apply everywhere. The Rust UI launcher
requires a rebuild since it embeds them at compile time.

## Persistence

State auto-saves on exit to `octopus_state.bin` and auto-loads on startup.
Uses the firmware's PersistentV2 serialization format (PersPageExport/Import).

## Testing

```bash
# Basic MIDI verification
./build/octopus &
python3 tests/test_osc.py    # OSC commands
aseqdump -p 128:0            # watch MIDI output

# Phase 4 test (rotary encoders, track attributes)
python3 tests/test_phase4.py
```

## Build Targets

```bash
make            # Octopus (10 tracks × 16 steps)
make NEMO=1     # Nemo (8 visible tracks, Cadence, Wilson windowing)
make clean
```

## Modifications to Original Firmware

### Port patches — `patches/01`–`06` (shared with `main` branch)

Same 20 files as the `main` branch (see `main` branch README for full list). Patch 04 (`play_MIDI.h`) has a v6-specific variant in `patches/07` due to whitespace differences.

### v6 play_MIDI variant — `patches/07`

- `play_MIDI.h` — v6-specific `#ifdef __linux__` guard (same logic, different context lines)

### v6 SoloRec bug fixes — `patches/08`

7 NULL-guard and overflow fixes for SoloRec stability:
- `play__master.h` — NULL guard in `PLAYER_dispatch` (primary crash fix), div-by-zero guard in `advance_page_locators`
- `Solorec.h` — array overflow in `Solorec_init`, NULL guards in `clearRec` and `applyEffects`
- `key_SOLOREC.h` — NULL guard in page cluster selection
- `key_cluster_operation.h` — NULL guard in `stop_solo_rec`

### HAL shim fix (main repo, not firmware)

- `src/hal_linux.c` — alarm timing: compute relative delay as `trigger - cyg_current_time()` instead of `trigger * 10`. Without this, double-click, quick-turn, edit, and SoloRec effect timers fire ~60x too late.

Patches are stored in `patches/`.

## License

GPL v2 (inherited from the original Octopus_OS firmware).
