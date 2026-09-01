# AGENTS.md

Genoqs Octopus MIDI sequencer firmware ported from eCos/ARM to Linux and Windows.
C engine (ALSA/winmm MIDI, custom OSC over UDP) hosted in-process by a Rust
workspace that serves the HTML control surface (egui GUI + headless CLI).

## Firmware submodule

The original firmware source lives in a git submodule at `firmware/` pointing to a fork of [genoqs-community/source](https://github.com/genoqs-community/source). It contains `OCT_OS/` (Octopus firmware) and `NEMO_OS/` (Nemo firmware). Five files are patched with `#ifdef __linux__` / `#ifdef _WIN32` guards via `patches/` (applied to the fork).

**First-time clone:**
```bash
git clone --recurse-submodules <repo-url>
# Or after a regular clone:
git submodule update --init
```

**Setting up a new fork** (if the submodule is not yet configured):
```bash
./scripts/setup-firmware.sh git@github.com:YOURUSER/source.git
```

## Build

### C engine (Linux)
```bash
make              # build/octopus (standalone) + build/liboctopus.a (static lib for Rust hosts)
make clean
```
Requires: `gcc`, `libasound2-dev`. Links: `-lasound -lpthread -lm`.

### C engine (Windows)
```powershell
powershell -ExecutionPolicy Bypass -File build_win.ps1          # standalone octopus.exe
powershell -ExecutionPolicy Bypass -File build_win.ps1 -Lib     # build\liboctopus.a (for Rust hosts)
powershell -ExecutionPolicy Bypass -File build_win.ps1 -Clean
```
Requires: MSYS2 ucrt64 gcc in PATH (`C:\msys64\ucrt64\bin`). Links: `-lwinmm -lws2_32 -lwinpthread -lm -lkernel32 -lavrt`.

### Rust workspace (octopus_gui; --no-gui for headless)
```bash
cargo build --release    # produces octopus_gui (windowed + --no-gui headless mode)
```
Root Cargo workspace: `engine/` (FFI wrapper + web server, compiles the C engine into `build/liboctopus.a` via `build.rs`), `gui/` (egui desktop app with `--no-gui` headless mode). The engine runs **in-process** — no subprocess, no OSC UDP loopback on port 9000: WebSocket input is injected via `oct_send_osc()`, engine output frames (MIR/transport) arrive through a C callback (`oct_set_frame_callback`) and are broadcast to WebSocket clients. The HTML control surface is embedded via `include_str!` (`/` → web_gui.html, `/modern` → web_gui_modern.html).

On Windows the Rust binaries must be built with the `x86_64-pc-windows-gnu` target (the C objects are MinGW-built): `rustup target add x86_64-pc-windows-gnu && cargo build --release --target x86_64-pc-windows-gnu`.

### Full release
```bash
powershell -ExecutionPolicy Bypass -File build_release.ps1   # Windows
bash build_release.sh                                        # Linux
```
Outputs to `dist/`: `octopus`, `octopus_gui`, HTML files.

## Architecture: single translation unit

**Critical:** `src/engine.c` `#include`s all firmware `.h` files AND several `.c` files into one translation unit, matching the original firmware's architecture. The original `.c` files in `firmware/OCT_OS/_OCT_objects/` (PersistentV1.c, PersistentV2.c, Persistent.c, Phrase.c, Phrase-presets.c) and `firmware/OCT_OS/_OCT_global/flash-block.c` are `#include`d directly — they are NOT compiled separately. Do not add them to the build sources or you will get multiple-definition errors.

Only the files in `src/` are compiled as separate object files:
- `engine.c` — the single firmware TU + hosted engine API (`include/engine_api.h`: `oct_engine_start/stop`, `oct_send_osc`, `oct_save_state`, `oct_set_frame_callback`), sequencer thread, state load/save
- `engine_main.c` — thin standalone `main()` (arg parsing + start/wait/stop); NOT part of `liboctopus.a`
- `hal_linux.c` — eCos API shim (`cyg_*` → pthread/pipe/timerfd/Win32)
- `midi_alsa.c` (Linux) / `midi_winmm.c` (Windows) — MIDI backend + `midi_enum_devices()` for the GUI picker
- `osc_server.c` — OSC input server (raw UDP, no liblo) + shared dispatcher used by `oct_send_osc()`
- `osc_render.c` — MIR → OSC output bridge + 60 Hz render thread (UDP mode standalone, callback mode hosted)
- `flash_file.c` — file-based persistence

The engine is one-shot per process: `oct_engine_start()` initializes the firmware once; `oct_engine_stop()` is final (the firmware cannot be re-initialized). `/quit` sets `main_running=0` but does NOT signal the process when hosted (`g_oct_hosted` flag).

## C language and compiler flags

- Standard: **gnu89** (not C99+). Many warnings suppressed: `-Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-implicit-int -Wno-int-conversion`.
- Cross-platform via `#ifdef _WIN32` / `#else` blocks throughout `src/`.
- Nemo variant via `#ifdef NEMO` — adds Nemo-specific include paths and track count changes.

## eCos compatibility shim

`include/hal_linux.h` + `src/hal_linux.c` provide all eCos types, macros, and `cyg_*` functions the firmware expects. This is the linpin that lets ~50k lines of original firmware compile unchanged. When the firmware needs a new eCos primitive, add it to the shim — don't modify firmware logic.

Key mappings: `cyg_thread_*` → pthread, `cyg_mbox_*` → pipe/ring-buffer, `cyg_mutex_*` → pthread_mutex, `cyg_semaphore_*` → sem_init/post/wait, `cyg_alarm_*` → timerfd/WaitableTimer, `diag_printf` → vfprintf(stderr).

**HANDLE conflict:** The firmware defines `HANDLE` as 5 (a display mode constant in `defs_general.h`). Windows code that needs the Win32 HANDLE type uses `void*` instead. Never `#include <windows.h>` in a file that also includes firmware headers without this awareness.

## Firmware modifications (minimal, guarded)

20 files modified from the original firmware, all via preprocessor guards. Patches are stored in `patches/` (files `01`–`05` are the core port patches, `06` covers Nemo build compat + additional Linux/Windows guards) and applied to the `firmware/` submodule (a fork of `genoqs-community/source`).

**Core Linux/Windows port patches (`patches/01`–`05`):**
1. `OCT_OS/_OCT_global/includes-declarations.h` — swaps eCos headers for `hal_linux.h`
2. `OCT_OS/_OCT_init/Init_memory.h:638` — NULL guard in `PAGE_init()`
3. `OCT_OS/_OCT_interrupts/cpu-load.c:16` — disables CPU load check on Linux/Windows
4. `OCT_OS/_OCT_Player/play_MIDI.h:80` — `#ifdef __linux__` routes `MIDI_send()` to ALSA
5. `OCT_OS/_OCT_Viewer/show_hwdriver.h:36` — `#ifndef __linux__` suppresses hardware `VIEWER_show_MIR()`

**Nemo build + additional port patches (`patches/06`):**
6. `OCT_OS/_OCT_global/defs_functions.h` — define `KEY/LED_RANDOMIZE` under `#ifdef NEMO`
7. `OCT_OS/_OCT_global/includes-definitions.h` — add Nemo helper functions (`page_is_chain_follow`, `track_get_window_shift`) under `#ifdef NEMO`
8. `OCT_OS/_OCT_init/OS_infrastructure.h` — wrap page refresh alarm creation in `#if !defined(__linux__) && !defined(_WIN32)`
9. `OCT_OS/_OCT_interrupts/Intr_KEY_functions.h` — fix `memset(MIR, 0, 204)` → `memset(MIR, 0, sizeof(MIR))`
10. `OCT_OS/_OCT_interrupts/Intr_TMR.h` — move MIDI clock to sequencer thread on Linux/Windows; add `g_tick_ns` precompute
11–18. `OCT_OS/_OCT_exe_keys/key_GRID.h`, `key_MAP.h`, `key_PAGE_sel_NONE.h`, `key_PAGE_sel_NONE_BIRDSEYE.h`, `key_PAGE_sel_STEP.h`, `key_PAGE_sel_TRACK.h`, `key_STEP.h` — wrap `KEY_RANDOMIZE` cases in `#ifndef NEMO`; wrap `KEY_MIXTGT_USR1-4` in `#ifndef NEMO` (fixes duplicate case values)
12. `OCT_OS/_OCT_interrupts/Intr_KEY_GRID.h` — wrap `KEY_RANDOMIZE` in `#ifndef NEMO`
13. `OCT_OS/_OCT_Player/play_functions.h`, `play_play.h` — minor whitespace

When modifying firmware files, use `#ifdef __linux__` / `#ifdef _WIN32` / `#ifndef __linux__` guards, not unconditional edits. Generate patches with `diff -u` from the submodule root and store in `patches/`.

## OSC protocol

Custom OSC implementation over raw UDP (no liblo dependency). Ports (standalone engine): **8000** (input), **9000** (output). In hosted mode (Rust GUI/CLI) port 8000 still listens for external control surfaces, but output frames go to the in-process callback instead of UDP 9000.

Addresses work both with and without `/octopus/` prefix — the dispatcher in `osc_server.c` matches `/key`, `/rotary`, `/transport`, etc. directly. Name-based addresses also supported: `/key/REC 1`, `/rotary/VEL 2`, etc. Name tables are in `osc_server.c` (`key_name_table`, `rot_name_table`). Full key/rotary index map: see `keymap.md`.

Key dispatch acquires `cyg_scheduler_lock()` / `cyg_scheduler_unlock()` to protect firmware globals — all OSC handlers run under this lock.

## Web GUI

Two ways to drive the engine from the browser HTML control surface (`web_gui.html`, `/modern` → `web_gui_modern.html`):

### 1. Rust host (in-process engine) — `octopus_gui`
```bash
cargo build --release
./target/release/octopus_gui             # egui desktop app: MIDI picker + Start
./target/release/octopus_gui --no-gui    # headless: engine + web server, Ctrl+C to quit
```
Both serve HTTP 8088 + WebSocket 8089 with the HTML embedded via `include_str!`. WebSocket input goes straight into the engine via `oct_send_osc()`; engine output frames arrive via the C frame callback and are broadcast to WebSocket clients. The OSC UDP input listener (port 8000) still runs in-process for external control surfaces and test scripts. The GUI starts the engine automatically on launch and can switch MIDI devices live via OSC (`/midi/out_a` etc.); the engine is one-shot per process, so after Stop the app must be relaunched.

### 2. Standalone engine + Python bridge (legacy)
```bash
./build/octopus &
python3 web_gui.py    # requires: pip3 install websockets
```
UDP OSC loopback on port 9000 (engine → bridge). Reads HTML files at runtime from the script directory.

### HTML control surface

The HTML files use inline `<style>` blocks — there is no `static/css/` directory and no external CSS files. CSS class names are specific to each HTML file (e.g., `.sbtn`, `.rb`, `.pad-btn`, `.bcell`, `.led`). The Rust hosts embed the files at compile time (rebuild required after changes); `web_gui.py` reads them from disk.

## State persistence

Auto-loads `octopus_state.bin` on startup (path overridable via `--state` / engine opts). Saving is explicit: `--autosave` (standalone/CLI), the `/save` OSC address, GRID+PGM on the control surface, or the GUI's "Save state on stop" option. Uses a simple tag-length-value format (tags: `GRID`, `PAGE`) wrapping the firmware's PersistentV2 export/import functions. The in-memory flash buffer (`hal_flash_base`, 1MB) in `hal_linux.c` is a separate mechanism used by the firmware's internal flash API.

## Sequencer thread timing

- Linux: `SCHED_FIFO` priority 80 + `mlockall` + `clock_nanosleep(TIMER_ABSTIME)` + 300μs busy-wait
- Windows: MMCSS "Pro Audio" task + `THREAD_PRIORITY_TIME_CRITICAL` + `Sleep()` + 2ms `QueryPerformanceCounter` busy-wait
- 48 PPQN: each tick calls `driveSequencer()` once, MIDI clock sent before scheduler lock

## Windows cleanup order (critical)

On Windows, shutdown order matters to avoid zombie processes:
1. Set `sequencer_running = 0` and `main_running = 0`
2. `Sleep(50)` — let sequencer thread exit its loop
3. `midi_cleanup()` — close winmm handles (prevents DLL detach hang)
4. Save state
5. `TerminateProcess()` — skips DllMain that would hang on open handles (standalone binary only — never in hosted mode, it would kill the GUI)

Do not `pthread_join` the sequencer thread on Windows — MMCSS cleanup can hang.

## Testing

No automated test suite. Tests are manual Python integration scripts in `tests/`:
```bash
./build/octopus &                    # start engine first
python3 tests/test_osc.py            # basic OSC: transport, step toggle, tempo
python3 tests/test_phase4.py         # rotary encoders, track attributes, zoom
```
These send OSC to port 8000 and optionally listen on 9000 for MIR frames (standalone engine only — the hosted Rust binaries deliver frames via WebSocket instead). `octopus_gui --no-gui` runs the same UDP listener in-process, so the tests work against it too. Verify MIDI output with `aseqdump -p <client>:0` (Linux) or a MIDI monitor (Windows).

## Key reference docs in repo

- `keymap.md` — complete key/rotary index map, OSC addresses, MIR LED format
- `octopus.txt` — Genoqs Octopus reference manual (CE v5.30)
- `README.md` — quick start, OSC namespace, MIDI connections, build targets
- `control_surfaces/open_stage_control/README.md` — Open Stage Control session setup
