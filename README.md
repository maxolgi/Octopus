# Octopus/Nemo Linux Port

A faithful port of the Genoqs Octopus and Nemo MIDI sequencer firmware to Linux,
using ALSA for MIDI I/O and OSC for the control surface.

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

20 files modified in the `firmware/` submodule (all minimal, guarded by `#ifdef`). Split across two patch files:

### Core port — `patches/01`–`05`

- `includes-declarations.h` — swap eCos headers for `hal_linux.h`
- `Init_memory.h` — NULL guard in `PAGE_init()` (ARM hardware masked this bug)
- `cpu-load.c` — disable CPU load check on Linux/Windows (no hardware countdown timer)
- `play_MIDI.h` — `#ifdef __linux__` routing `MIDI_send()` to ALSA
- `show_hwdriver.h` — `#ifndef __linux__` suppressing hardware `VIEWER_show_MIR()`

### Nemo build + additional — `patches/06`

- `defs_functions.h` — define `KEY/LED_RANDOMIZE` under `#ifdef NEMO`
- `includes-definitions.h` — add Nemo helper functions under `#ifdef NEMO`
- `OS_infrastructure.h` — wrap alarm creation in `#if !linux && !_WIN32`
- `Intr_KEY_functions.h` — fix `memset(MIR, 0, 204)` → `sizeof(MIR)`
- `Intr_TMR.h` — move MIDI clock to sequencer thread; add `g_tick_ns` precompute
- `key_GRID.h`, `key_MAP.h`, `key_STEP.h` — wrap `KEY_RANDOMIZE` in `#ifndef NEMO`
- `key_PAGE_sel_NONE.h` — wrap `KEY_MIXTGT_USR1–4` in `#ifndef NEMO` (fixes duplicate case values)
- `key_PAGE_sel_NONE_BIRDSEYE.h`, `key_PAGE_sel_STEP.h`, `key_PAGE_sel_TRACK.h` — wrap `KEY_RANDOMIZE` in `#ifndef NEMO`
- `Intr_KEY_GRID.h` — wrap `KEY_RANDOMIZE` in `#ifndef NEMO`
- `play_functions.h`, `play_play.h` — minor whitespace

Patches are stored in `patches/`. See `scripts/setup-firmware.sh` for fork setup.

## License

GPL v2 (inherited from the original Octopus_OS firmware).
