# Genoqs Octopus/Nemo Linux Port — Implementation Plan

## 1. Architecture Overview

### 1.1 What stays, what goes, what's new

| Subsystem | Lines | Decision | Action |
|---|---:|---|---|
| `_OCT_objects/` (Phrase, PersistentV2, Page, Track, Step, Grid, MIDI_queue) | 9,483 | **Keep** | Copy into new tree; strip `diag_printf`, replace flash primitives |
| `_OCT_Player/` (sequencing engine) | 10,068 | **Keep** | Copy; stub `MIDI_send()` boundary, replace `play_UARTx_OUT.h` with ALSA |
| `_OCT_global/` (defs, types) | 4,663 | **Keep defs, rewrite runtime** | Port `defs_*.h`, `sts.h`, `types-general.h` verbatim; rewrite `variables.h` (replace eCos handles), `alarm_handlers.h`, `flash-block.c` |
| `_OCT_Viewer/` | 13,115 | **Keep logic, replace sink** | Keep `fill_*`/`show_*`/`MIR_write_dot` verbatim; replace `show_hwdriver.h` (`VIEWER_show_MIR`) with OSC emitter |
| `_OCT_exe_keys/` | 18,844 | **Keep** | Copy verbatim; replace `scan_keys` (hardware matrix) with OSC input dispatch into the same `executeKey()` pipeline |
| `_OCT_exe_rots/` | 4,016 | **Keep** | Copy verbatim; replace `G_scanRots` with OSC `/rotary` dispatch into `executeRot()` |
| `_OCT_interrupts/` | 3,217 | **Rewrite** | Delete `Intr_TMR.h`, `Intr_KEY.h`, `Intr_ROTARY.h`, `Intr_IN_UARTs.h`; replace with Linux equivalents (ALSA timer thread, OSC input thread, ALSA MIDI in) |
| `_OCT_init/` | 1,827 | **Rewrite** | Delete `OS_infrastructure.h` (eCos thread setup); keep `Init_memory.h` logic (struct zeroing, defaults); write new `init_threads()` |
| NEMO overlay (`NEMO_OS/`) | ~8,000 | **Keep** | Copy `_NEMO_*` files; the `#ifdef NEMO` system works as-is since we preserve the same include structure |

**New code to write (~3,000 lines):**
- `hal_linux.c` — eCos API shim (`cyg_*` → pthread/pipe/timerfd)
- `midi_alsa.c` — ALSA sequencer binding (replaces UART + MIDI mailbox)
- `osc_server.c` — liblo server (replaces key scan + rotary scan ISRs)
- `osc_render.c` — MIR → OSC bridge (replaces `VIEWER_show_MIR` hardware driver)
- `flash_file.c` — file-based persistence (replaces `flash-block.c`)
- `main_linux.c` — real `main()` entry point

### 1.2 Threading model

Original eCos threads → Linux pthreads:

| eCos thread | Linux replacement | Priority |
|---|---|---|
| Timer1 ISR + DSR → `kickSequencer` → `driveSequencer` | **Sequencer thread** — `SCHED_FIFO`, woken by ALSA queue tick callbacks via `eventfd` | Highest (RT) |
| `keyExecute_thread` (mbox → `executeKey`) | Called inline from OSC server thread | Normal |
| `rotExecute_thread` (sem → `executeRot`) | Called inline from OSC server thread | Normal |
| `showPage_thread` (sem → `Page_full_refresh`) | **Render thread** — 60 Hz `usleep` loop, calls `Page_full_refresh()` then emits OSC | Normal |
| `UART0/1_IN_thread` (mbox → MIDI interpreter) | ALSA sequencer input callback (in ALSA's own thread) | Normal |
| `UART0/1_OUT_thread` (mbox → UART writes) | **Eliminated** — ALSA `snd_seq_event_output` is synchronous and buffered | — |
| `flashOps_thread` | Called on demand from main thread | Normal |

The sequencer thread is the only one that needs real-time scheduling. It wakes on each ALSA queue tick (24 ppqn → but we need 48 ppqn, so we double-tick as the original does in EXT mode).

### 1.3 Timing model

| Concept | Original | Linux port |
|---|---|---|
| Master clock | 50 MHz hardware timer, `G_TIMER_REFILL = 62500000/BPM` | ALSA sequencer queue at PPQN=24, tempo via `snd_seq_queue_tempo_set_tempo(q, 60000000/BPM)` |
| Internal engine resolution | 48 PPQN (12 TTC per 16th) | Same — each ALSA tick fires `driveSequencer()` **twice** (24→48 doubling, exactly as `kickSequencer` does in EXT mode at `Intr_TMR.h:148`) |
| MIDI clock out | Sent on odd TTC values | `SND_SEQ_EVENT_CLOCK` — ALSA queue timer emits this automatically at 24 ppqn |
| External clock in | UART RX ISR → `G_midi_interpret_REALTIME` | ALSA `SND_SEQ_EVENT_CLOCK` subscription → same interpreter |
| `G_MIDI_timestamp` | Incremented per TTC tick | Same — our monotonic event scheduler clock |
| `G_TTC_abs_value` | Cycles 1..12 | Same |

### 1.4 MIDI I/O mapping

| Octopus concept | ALSA sequencer equivalent |
|---|---|
| UART0 (channels 1–16) | ALSA port "out_A" (physical MIDI out 1) |
| UART1 (channels 17–32) | ALSA port "out_B" (physical MIDI out 2) |
| Virtual channels (33–64) | ALSA port "out_VIRTUAL" (loopback to input) |
| `MIDI_send(type, val0, val1, val2)` | `snd_seq_ev_set_note / set_controller / set_pgmchange / set_pitchbend / set_chanpress` + `snd_seq_event_output` |
| `play_MIDI_queue(timestamp)` timestamped scheduling | `ev.time.tick = timestamp` + scheduled output |
| `Player_wait()` busy-poll | Eliminated (ALSA buffers internally) |
| Running status optimization | Eliminated (ALSA handles framing) |

---

## 2. Project Layout

```
octopus-linux/
├── Makefile
├── README.md
├── include/
│   └── hal_linux.h           # eCos shim declarations
├── src/
│   ├── main_linux.c           # main() entry point
│   ├── hal_linux.c            # cyg_* → pthread/pipe/timerfd shim
│   ├── midi_alsa.c            # ALSA sequencer binding
│   ├── osc_server.c           # liblo OSC input server
│   ├── osc_render.c           # MIR → OSC output bridge
│   └── flash_file.c           # file-based persistence
├── firmware/                    # ← git submodule (fork of genoqs-community/source)
│   ├── OCT_OS/                   #   5 files patched via patches/
│   │   ├── _OCT_global/          #   defs_*.h ported verbatim; variables.h, alarm_handlers.h rewritten
│   │   ├── _OCT_objects/         #   PersistentV2.c, Phrase.c ported; flash calls stubbed
│   │   ├── _OCT_Player/          #   play_*.h ported; MIDI_send() boundary rewired to ALSA
│   │   ├── _OCT_Viewer/          #   show_*.h, filler_*.h ported; show_hwdriver.h replaced
│   │   ├── _OCT_exe_keys/        #   key_*.h ported verbatim
│   │   ├── _OCT_exe_rots/        #   rot_*.h ported verbatim
│   │   └── _OCT_init/            #   Init_memory.h logic kept; OS_infrastructure.h deleted
│   └── NEMO_OS/                  #   Nemo firmware
│       ├── _NEMO_global/
│       ├── _NEMO_Viewer/
│       ├── _NEMO_exe_keys/
│       ├── _NEMO_exe_rots/
│   └── _NEMO_interrupts/
├── control_surfaces/          # ready-to-use OSC templates
│   ├── open_stage_control/
│   │   ├── octopus.json       # full 10×16 grid + transport + octave circle + rotaries
│   │   └── nemo.json          # 8-row variant
│   └── README.md
└── tests/
    ├── test_persistence.c     # load real .elf-embedded bank, verify checksum
    ├── test_player.c          # step → MIDI event verification
    ├── test_timing.c          # 48 ppqn tick accuracy
    └── test_osc_roundtrip.c   # OSC key press → LED state → OSC output
```

---

## 3. Module Specifications

### 3.1 `hal_linux.c` — eCos Compatibility Shim (~600 lines)

Provides the `cyg_*` API the existing code expects, backed by Linux primitives. This is the linchpin — it lets ~50k lines of original code compile and run unchanged.

| eCos function | Linux implementation |
|---|---|
| `cyg_thread_create(entry, data, …)` | `pthread_create` with configured stack size; store entry+data in a struct |
| `cyg_thread_resume()` / `cyg_thread_release()` | no-op (threads start immediately) / `pthread_join` |
| `cyg_mbox_create(handle, box)` | `int fds[2]; pipe(fds);` — mbox put = `write(fds[1], &ptr, 8)`, mbox get = `read(fds[0], &ptr, 8)`, mbox tryput = `write` with `O_NONBLOCK` |
| `cyg_mbox_put / get / tryget / peek` | pipe read/write with blocking/non-blocking semantics |
| `cyg_mutex_init / lock / unlock` | `pthread_mutex_init / lock / unlock` |
| `cyg_semaphore_init / post / wait / trywait` | `sem_init / post / wait / trywait` |
| `cyg_alarm_create(fn, alarm, handle)` | `timerfd_create(CLOCK_MONOTONIC, 0)` + a watcher thread that `read()`s the fd and calls `fn` |
| `cyg_alarm_initialize(handle, interval, trigger)` | `timerfd_settime` with `it_interval` |
| `cyg_clock_resolve_current / cyg_current_time()` | `clock_gettime(CLOCK_MONOTONIC)` → convert to eCos ticks (10 ms each: `nanoseconds / 10000000`) |
| `HAL_CLOCK_READ(&val)` | `clock_gettime(CLOCK_MONOTONIC)` → nanoseconds |
| `diag_printf(...)` | `vfprintf(stderr, ...)` guarded by a debug flag |
| `HAL_READ_UINT32 / HAL_WRITE_UINT32` | Only used for E7T registers — these calls are all in deleted files (interrupts, init, UART driver). If any survive in ported code, provide a stub that aborts. |
| `cyg_io_flash_*` / `flash_init` / `flash_read` / `flash_write` | Routed to `flash_file.c` |

**Critical detail:** the original code is header-heavy — most logic lives in `.h` files `#include`d into a single translation unit (`main.c` includes everything). We must preserve this structure or we'll hit thousands of "multiple definition" errors. `main_linux.c` will `#include` the same chain as the original `main.c`, with our replacements inserted at the right points.

### 3.2 `midi_alsa.c` — ALSA Sequencer Binding (~400 lines)

```
Functions exposed:
  void midi_alsa_init(int queue_ppqn);           // open seq client, create ports, alloc queue
  void midi_alsa_set_tempo(int bpm);             // snd_seq_queue_tempo
  void midi_alsa_start_queue(void);
  void midi_alsa_stop_queue(void);
  void midi_alsa_continue_queue(void);

  // Replaces MIDI_send() — called from the player engine
  void midi_alsa_send_event(int type, int channel, int val1, int val2, unsigned int timestamp);

  // Replaces play_MIDI_queue() — flush due events
  void midi_alsa_flush_queue(unsigned int current_timestamp);

  // Input subscription callback (replaces UART IN threads)
  void midi_alsa_set_input_callback(void (*fn)(int type, int channel, int val1, int val2));

  // Clock slave: subscribe to external clock source
  void midi_alsa_subscribe_clock(snd_seq_addr_t *source);
```

Port mapping:
- Channels 1–16 → ALSA port `"out_A"` (physical MIDI out 1)
- Channels 17–32 → ALSA port `"out_B"` (physical MIDI out 2)
- Channels 33–64 → ALSA port `"out_VIRTUAL"` (looped back to `"in_VIRTUAL"`)

The internal `MIDI_queue` (timestamped priority queue in `MIDI_queue.h`) is kept — it's portable C. We keep the queue logic, and `play_MIDI_queue()` pops events and calls `midi_alsa_send_event()` instead of writing UART bytes.

### 3.3 `osc_server.c` — OSC Input Server (~350 lines)

Uses liblo. Registers these method handlers, each of which feeds into the existing `executeKey()` / `executeRot()` pipeline:

**Input namespace (OSC → engine):**

```
/octopus/key          ii    (keyNdx 1..260, press:1/release:0)
                          → on press: G_pressed_keys[keyNdx]=keyNdx; executeKey(keyNdx);
                          → on release: G_pressed_keys[keyNdx]=0;

/octopus/rotary       ii    (rotNdx 0..20, direction: INC=2/DEC=1)
                          → executeRot((rotNdx<<2) | direction);

/octopus/tempo        i     (bpm 10..199)   → G_master_tempo=bpm; G_TIMER_REFILL_update();
/octopus/transport    s     ("start"|"stop"|"pause"|"continue")
/octopus/zoom         i     (zoom_level)    → G_zoom_level=level; Page_requestRefresh();
/octopus/load         s     (filepath)      → load PersistentV2 file
/octopus/save         s     (filepath)      → write PersistentV2 file
/octopus/clocksource  i     (0=OFF,1=INT,2=EXT)
```

The server runs in its own thread (liblo spawns it automatically). Key/rotary dispatch calls `executeKey`/`executeRot` under the scheduler mutex (same as original `keyExecute_thread`).

**Double-click handling:** the original uses a timer alarm for big-knob numeric entry. We preserve this via the `hal_linux.c` alarm shim (`timerfd`).

### 3.4 `osc_render.c` — MIR → OSC Output Bridge (~250 lines)

Replaces `show_hwdriver.h:VIEWER_show_MIR()`. After `Page_full_refresh()` fills the MIR, this module:

1. **Diffs** the current MIR against the previous frame (stored in a `prev_MIR[2][17][5]`).
2. For each changed logical index, emits:
   ```
   /octopus/led  iii  (index, red, green)
   ```
   where `red`/`green` encode the color plane bits (RED/BLINK → red; GREEN → green; SHINE alternates with blinker).

3. Emits lauflicht state:
   ```
   /octopus/lauflicht  iii  (pageNdx, row, col)
   ```

4. Emits transport/zoom indicators:
   ```
   /octopus/zoom_level  i
   /octopus/global_locator  i
   /octopus/tempo  i
   /octopus/blink  i   (master blinker phase — needed for shine/blink animation)
   ```

The render thread runs at ~60 Hz (matching `page_refreshAlarm_handler`'s frequency + blinker toggle). It calls `Page_full_refresh()` (which fills MIR via the unchanged fill/show pipeline), then calls `osc_render_emit()`.

**Bandwidth optimization:** the MIR is 170 bytes. A full frame is at most 260 OSC messages, but with diffing it's typically 5–30 messages per frame. On localhost this is negligible. For Wi-Fi tablet use, we add a bulk mode: `/octopus/mir  b` (170-byte blob) instead of individual messages.

### 3.5 `flash_file.c` — File-Based Persistence (~200 lines)

Replaces `flash-block.c`. The flash model: the Octopus has flash at `0x01900000`, organized as blocks of `FLASH_BLOCK_SIZE`. Pages and the grid are stored at computed offsets.

Our replacement maps the flash address space to a flat file:

```
octopus_state.bin   (size: GRID flash footprint ≈ 2 MB)
  offset 0x00000  → GridPersistentV2
  offset 0x80000  → Page 0 (PagePersistentV2)
  offset 0x80800  → Page 1
  ...             (up to 160 pages)
```

The existing `PersistentV2.c` import/export functions (`PersistentV2.c:137-1005`) are reused verbatim — they already handle magic numbers, version checks, and checksums. We only replace `flash_read(addr, buf, len)` and `flash_write(addr, buf, len)` with `pread(fileno, buf, len, addr - MY_FLASH_BASE)` and `pwrite(...)`.

**Bank import:** a separate function reads a sysex dump or a raw `GridPersistentV2` binary exported from real hardware, validates the header (magic `0xdead`/`0xbeaf`, version 2, checksum), and loads it into the runtime repositories.

### 3.6 `main_linux.c` — Entry Point (~150 lines)

```c
int main(int argc, char **argv) {
    // 1. Parse args (config file, OSC port, MIDI ports, NEMO flag)
    // 2. midi_alsa_init(24);          // open ALSA sequencer
    // 3. osc_server_init(port);       // start liblo server
    // 4. osc_render_init(host, port); // connect render target
    // 5. hal_linux_init();            // create pipes, mutexes, semaphores

    // 6. — equivalent to cyg_user_start() —
    Octopus_memory_init();             // zero repositories, init defaults
    midi_alsa_set_tempo(G_master_tempo);

    // 7. Load state from file (replaces Octopus_recall_flash)
    if (autoload_enabled) octopus_load_state("octopus_state.bin");

    // 8. Start threads
    start_sequencer_thread();          // SCHED_FIFO, wakes on ALSA queue ticks
    start_render_thread();             // 60 Hz refresh + OSC emit
    // OSC server thread is already running (liblo)
    // ALSA input callback is already registered

    // 9. Main loop: poll for load/save commands, CLI input
    while (running) { sleep(1); }

    // 10. Cleanup
    octopus_save_state("octopus_state.bin");
}
```

---

## 4. Implementation Phases

### Phase 0: Project Scaffold + HAL Shim (foundation)

**Goal:** Get the original code to compile under `gcc` on Linux without eCos.

**Steps:**
1. Create the directory structure above.
2. Add firmware submodule: `firmware/` → fork of `genoqs-community/source` (contains `OCT_OS/` and `NEMO_OS/`).
3. Write `hal_linux.c` with all `cyg_*`, `HAL_*`, `diag_printf` shims.
4. Write a minimal `main_linux.c` that calls `Octopus_memory_init()` and exits.
5. Write the Makefile (see §5).
6. **Iterate on compile errors** — this is where most of the work is. Expect:
   - Missing eCos headers (`cyg/kernel/kapi.h`) → provided by `hal_linux.h`
   - `E7T_*` register references in files we're keeping → stub with `abort()`
   - `flash_*` calls → stub with no-ops initially
   - `#include` path adjustments (the `BASELINE_PATH` in the original makefile points to `/home/genoqs/Dev/...` — fix to relative paths)

**Deliverable:** `make` produces a binary that runs `Octopus_memory_init()` without crashing. No MIDI, no OSC, no sound. This proves the HAL shim works.

**Verification:** binary exits cleanly; `valgrind` shows no leaks in the init path.

---

### Phase 1: ALSA MIDI Out + Transport (first sound)

**Goal:** Start/stop the sequencer, hear a metronome on MIDI out.

**Steps:**
1. Write `midi_alsa.c`: open sequencer client, create output port, allocate queue.
2. Wire `MIDI_send()` → `midi_alsa_send_event()`. Wire `play_MIDI_queue()` → `midi_alsa_flush_queue()`.
3. Write the **sequencer thread**: SCHED_FIFO priority, wakes on ALSA queue tick events (via `snd_seq_event_input` with the queue subscription), calls `driveSequencer()` twice per tick (24→48 ppqn doubling).
4. Stub `scan_keys()` to return FALSE (no key input yet).
5. Manually set one track to have a step at position 1, velocity 100, pitch 60.
6. Start the queue, verify MIDI note events appear on the ALSA output port.

**Deliverable:** Running the binary with `aconnect` to a softsynth (or `aseqdump -p <port>`) shows NOTE ON/OFF events at the configured tempo.

**Verification:** connect to a synthesizer, hear a repeating note at 120 BPM.

---

### Phase 2: OSC Input — Keys + Transport (interactive)

**Goal:** Control transport and toggle steps via OSC messages from a basic control surface.

**Steps:**
1. Write `osc_server.c` with liblo.
2. Register `/octopus/key ii` and `/octopus/transport s` handlers.
3. Wire key handler → `G_pressed_keys[keyNdx] = keyNdx; executeKey(keyNdx);`.
4. Test with `oscsend`: send `/octopus/transport "start"`, then `/octopus/key 241 1` (PLAY1), then `/octopus/key 11 1` (step at row 0, col 0 — should toggle it on).
5. Verify that toggled steps produce MIDI notes.

**Deliverable:** A minimal Open Stage Control JSON with just the transport buttons + a 10×16 grid of toggle buttons. Clicking a pad toggles a step; pressing play sequences it.

**Verification:** full round-trip: OSC click → `executeKey` → step toggle → `PLAYER_play_row` → `MIDI_NOTE_new` → ALSA output → audible note.

---

### Phase 3: OSC Output — LED Feedback (the display)

**Goal:** The control surface lights up to reflect sequencer state.

**Steps:**
1. Write `osc_render.c`: diff MIR, emit `/octopus/led iii` messages.
2. Replace `VIEWER_show_MIR()` with a call to `osc_render_emit()`.
3. Write the **render thread**: 60 Hz loop calling `Page_full_refresh()` then `osc_render_emit()`.
4. Implement the blinker timer (replaces `page_refreshAlarm_handler`): toggle `G_master_blinker` at ~7.5 Hz, call `Page_requestRefresh()`.
5. Verify: toggled steps show green LEDs, playing steps show the lauflicht (red running light), transport buttons reflect state.

**Deliverable:** The control surface now shows live step state and playhead position.

**Verification:** visual — the grid animates as the sequencer plays, lauflicht walks across rows.

---

### Phase 4: Rotary Encoders + Track/Step Attributes

**Goal:** All 21 rotaries work, editing VEL/PIT/LEN/STA/POS/DIR/AMT/GRV/MCC/MCH/TEMPOMUL.

**Steps:**
1. Register `/octopus/rotary ii` handler → `executeRot((rotNdx<<2) | direction)`.
2. Implement rotary quickturn acceleration (replaces `quickturnAlarm_handler` via `timerfd`).
3. Test each rotary in PAGE, TRACK, and STEP zoom levels.
4. Verify that attribute changes are reflected in MIDI output (e.g., changing pitch transposes notes, changing velocity changes dynamics).

**Deliverable:** Full rotary control over all step and track attributes.

**Verification:** rotate ROT_PIT in TRACK mode → all notes transpose; rotate ROT_VEL in STEP mode → selected step's velocity changes.

---

### Phase 5: Phrases, Strums, Chords, Morph

**Goal:** The expressive core — phrase expansion, chord stacking, strum patterns.

**Steps:**
1. Verify `Step_chord_player.h` works (it should — it's portable logic that was kept verbatim).
2. Test: set a step's `phrase_num` to a preset, verify multi-note expansion.
3. Test: add aux notes to a step (chord), verify chord playback.
4. Test: set strum level, verify strum timing.
5. Wire the octave-circle OSC keys for chord/pitch entry (`key_OCT_CIRCLE_chord_STEP.h`, `key_OCT_CIRCLE_xpose_STEP.h`).
6. Test morph (note: `play_morph.h:compute_morph()` is stubbed in the source — returns 0; document this as a known limitation).

**Deliverable:** Phrases, chords, and strums produce correct multi-note patterns.

**Verification:** set a step to phrase preset 5 (green delay) → hear echo pattern; set chord C-E-G → hear triad.

---

### Phase 6: Persistence — Save/Load + Bank Import

**Goal:** Save and restore state; import banks from real hardware.

**Steps:**
1. Write `flash_file.c`: map flash addresses to file offsets.
2. Wire `flash_read`/`flash_write` to `pread`/`pwrite`.
3. Test `Octopus_recall_flash()` / `Octopus_store_flash()` round-trip.
4. Write bank importer: read a raw `GridPersistentV2` binary (extracted from a sysex dump or copied from the `.elf`'s `FACTORY_RESTORE.txt`), validate header, load into repositories.
5. Add `/octopus/load s` and `/octopus/save s` OSC handlers.

**Deliverable:** State persists across restarts; hardware bank files load correctly.

**Verification:** create a pattern, save, restart, load → pattern is identical. Load a factory bank → plays correctly.

---

### Phase 7: Grid, Scenes, Track Chaining, Mixmaps

**Goal:** Multi-page grid operation, the full Octopus workflow.

**Steps:**
1. Test `zoomGRID`: the 16-scene × 9-bank page matrix. OSC keys for grid navigation.
2. Test page preselection and chain clock-preselect.
3. Test track chaining (`chain_data[HEAD/NEXT/PREV/PLAY]`).
4. Test CC mixmaps (`zoomMIXMAP`): CC mapping per track.
5. Test the MIX target strip (VOL/PAN/MOD/EXP/USR0-5).

**Deliverable:** Full grid operation — multiple pages playing, scene switching, track chains.

**Verification:** set up 2 pages in different banks, switch between them via grid keys → seamless transition.

---

### Phase 8: MIDI Clock Slave + MIDI Input (Recording)

**Goal:** Sync to external clock, record incoming MIDI notes.

**Steps:**
1. Implement clock slave: subscribe to external `SND_SEQ_EVENT_CLOCK` source, feed into `G_midi_interpret_REALTIME()`.
2. Implement the 8-pulse tempo averaging from `MIDI_IN_interpreter.h:122-167`.
3. Wire ALSA MIDI input → `G_midi_interpret_NOTE_ON/CONTROL/BENDER/PRESSURE`.
4. Test recording: arm a track (`G_track_rec_bit`), send MIDI notes in, verify they're recorded to the correct step positions.
5. Test `MIDICLOCK_START/STOP/CONTINUE` transport sync.

**Deliverable:** Engine follows external tempo, responds to external transport, records MIDI input.

**Verification:** connect a DAW as clock master → engine locks to DAW tempo; play notes on a keyboard → they appear in the armed track.

---

### Phase 9: Nemo Target + Control Surfaces

**Goal:** Both targets build and work; ready-to-use OSC templates.

**Steps:**
1. Add `NEMO=1` build flag to Makefile. Build the Nemo variant.
2. Resolve any Nemo-specific compile issues (Cadence feature, Wilson windowing, Scale zoom).
3. Build the full Open Stage Control JSONs for both Octopus and Nemo:
   - 10×16 (Octopus) / 8×16 (Nemo) grid of pads
   - Transport cluster (PLAY1/2/4, STOP, PAUSE, RECORD, ALIGN)
   - Octave circle (13 chromatic pads)
   - Scale selectors (10 pads)
   - Zoom cluster (GRID/PAGE/TRACK/STEP/MAP/PLAY)
   - 21 rotary encoders (rendered as vertical sliders or knobs)
   - Mix target strip
   - Status display (tempo, zoom level, blink indicator)
4. Test the complete control surface end-to-end with both targets.

**Deliverable:** Both `octopus` and `nemo` binaries work with their respective OSC templates.

**Verification:** full performance session — load a factory bank, play, edit steps, switch pages, record MIDI, all via the tablet control surface.

---

### Phase 10: Hardening + Documentation

**Goal:** Production-ready, documented, tested.

**Steps:**
1. Write `test_persistence.c` — load `FACTORY_RESTORE.txt` from the repo, verify all checksums pass.
2. Write `test_player.c` — set up known step states, verify exact MIDI event sequences.
3. Write `test_timing.c` — measure tick jitter over 60 seconds at various tempos, verify < 1 ms.
4. Write the README: build instructions, OSC namespace reference, control surface setup guide, bank import instructions.
5. Profile: identify any timing-sensitive paths that need optimization (the original has CPU-load monitoring and track-dropping; we may not need it on modern hardware but should verify).
6. Add signal handling (SIGINT → clean save + exit).

**Deliverable:** Complete, documented, tested system.

---

## 5. Build System & Dependencies

### 5.1 Makefile

```makefile
# Auto-detect Nemo target: make NEMO=1
NEMO ?= 0

CC = gcc
CFLAGS = -Wall -Wno-unused-function -O2 -g \
    -I include -I firmware/OCT_OS/_OCT_global -I firmware/OCT_OS/_OCT_objects \
    -I firmware/OCT_OS/_OCT_Player -I firmware/OCT_OS/_OCT_Viewer \
    -I firmware/OCT_OS/_OCT_exe_keys -I firmware/OCT_OS/_OCT_exe_rots \
    -I firmware/OCT_OS/_OCT_init -I firmware/OCT_OS/_OCT_interrupts

ifeq ($(NEMO),1)
    CFLAGS += -DNEMO -I firmware/NEMO_OS/_NEMO_global -I firmware/NEMO_OS/_NEMO_Viewer \
        -I firmware/NEMO_OS/_NEMO_exe_keys -I firmware/NEMO_OS/_NEMO_exe_rots -I firmware/NEMO_OS/_NEMO_interrupts
    TARGET = nemo
else
    TARGET = octopus
endif

LDLIBS = -lasound -llo -lpthread -lm

SRCS = src/main_linux.c src/hal_linux.c src/midi_alsa.c \
       src/osc_server.c src/osc_render.c src/flash_file.c \
       firmware/OCT_OS/_OCT_objects/PersistentV1.c \
       firmware/OCT_OS/_OCT_objects/PersistentV2.c \
       firmware/OCT_OS/_OCT_objects/Persistent.c \
       firmware/OCT_OS/_OCT_objects/Phrase.c \
       firmware/OCT_OS/_OCT_objects/Phrase-presets.c

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)
```

Note: most of the codebase is header-only (`.h` files included into `main_linux.c`), matching the original architecture. Only 5 `.c` files from the original are separately compiled, plus our 6 new `.c` files.

### 5.2 Dependencies

| Library | Package (Debian/Ubuntu) | Purpose |
|---|---|---|
| ALSA sequencer | `libasound2-dev` | MIDI I/O, queue timing |
| liblo | `liblo-dev` | OSC server |
| pthread | (glibc) | Threading |

**No GUI toolkit. No Qt, GTK, SDL.** The control surface is rendered externally.

### 5.3 Runtime requirements

- Linux kernel ≥ 3.0 (for `timerfd`, `eventfd`)
- ALSA sequencer module loaded (`snd-seq`)
- For real-time scheduling: `CAP_SYS_NICE` or `ulimit -r` (or run via `chrt`)
- A control surface renderer: Open Stage Control (free, Node.js), TouchOSC (paid, mobile), or any OSC client

---

## 6. OSC Namespace Contract (Complete Reference)

### 6.1 Input (control surface → engine)

| Address | Args | Maps to |
|---|---|---|
| `/octopus/key` | `ii` (index 1–260, press 1/release 0) | `executeKey(index)` |
| `/octopus/rotary` | `ii` (rotNdx 0–20, INC=2/DEC=1) | `executeRot((rotNdx<<2)\|dir)` |
| `/octopus/tempo` | `i` (bpm 10–199) | `G_master_tempo` + `G_TIMER_REFILL_update()` |
| `/octopus/transport` | `s` ("start"\|"stop"\|"pause"\|"continue") | transport state machine |
| `/octopus/zoom` | `i` (zoom level 0–14) | `G_zoom_level` + refresh |
| `/octopus/clocksource` | `i` (0=OFF, 1=INT, 2=EXT) | `G_clock_source` |
| `/octopus/load` | `s` (filepath) | load PersistentV2 |
| `/octopus/save` | `s` (filepath) | write PersistentV2 |

### 6.2 Output (engine → control surface)

| Address | Args | Frequency | Content |
|---|---|---|---|
| `/octopus/led` | `iii` (index, red 0/1, green 0/1) | on change | per-LED state |
| `/octopus/mir` | `b` (170-byte blob) | ~60 Hz (bulk mode) | full MIR frame |
| `/octopus/lauflicht` | `iii` (pageNdx, row 0–9, col 1–16) | on change | playhead position |
| `/octopus/global_locator` | `i` (0–16) | on change | global column cursor |
| `/octopus/zoom_level` | `i` | on change | current zoom |
| `/octopus/tempo` | `i` | on change | current BPM |
| `/octopus/blink` | `i` | ~7.5 Hz | master blinker phase (for shine/blink animation) |
| `/octopus/clocksource` | `i` | on change | current clock mode |
| `/octopus/transport` | `s` | on change | "playing"\|"stopped"\|"paused" |

### 6.3 Key index quick reference (Octopus)

| Region | Index range | OSC control surface element |
|---|---|---|
| Step matrix | 11–185 (via `11 + 11*col + row`) | 10×16 grid of pads |
| Mix target strip | 21,32,43,54,65,76,87,98,109,120,131,142,153,164,175,186 | 16 horizontal pads |
| Transport | 223(REC),231(STOP),232(PAUSE),241(PLAY1),240(PLAY2),250(PLAY4) | 6 transport buttons |
| Zoom cluster | 218(GRID),219(PAGE),220(TRACK),227(STEP),228(MAP),229(PLAY) | 6 zoom buttons |
| Octave circle | 212,211,210,209,208,217,225,226,235,236,237,238,239 | 13 chromatic pads |
| Scale selectors | 222,221,230,243,244,245,246,247,248,249 | 10 scale buttons |
| Big knob numerics | 201,200,199,198,197,207,206,216,215,224,233 | 11 numeric keys |
| Left button tool | 1–10 | 10 track selector pads |
| Right button tool | 187–196 | 10 mutator pads |

---

## 7. Risk Register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Header-soup compilation: 50k lines of `.h` files included into one TU may hit compiler limits or obscure errors | Medium | High | Phase 0 is dedicated to this; compile incrementally with `-fsyntax-only` first; use `-j1` to see error order |
| Hidden `E7T_*` register writes in "portable" files | Low | Medium | `grep -r 'E7T_\|HAL_WRITE\|HAL_READ'` all kept files; any hit is a stub with `abort()` |
| Timing jitter from non-RT kernel scheduling | Medium | High | Use `SCHED_FIFO` + `mlockall`; if still jittery, use ALSA queue scheduling (events pre-scheduled, not real-time thread) |
| `G_` globals cause thread-safety issues (original was single-threaded per ISR) | Medium | High | Preserve original mutex usage (`cyg_mutex` → `pthread_mutex`); the sequencer thread and key thread already use scheduler lock |
| PersistentV2 struct packing differs between ARM (original) and x86 (our target) | High | High | Add `__attribute__((packed))` to all Persistent structs; verify with `sizeof()` assertions against known ARM sizes from the `.elf` |
| Nemo `#ifdef NEMO` branches reference Octopus files with different include paths | Medium | Medium | Phase 9 starts with a clean compile of Nemo target; resolve paths case-by-case |
| `compute_morph()` is stubbed (returns 0) in source | Certain | Low | Document as known limitation; morph source-value capture works, only the interpolation is missing |
| ALSA queue at PPQN=24 + doubling = 48 may have edge cases at tempo extremes | Low | Medium | Test at MIN_TEMPO=10 and MAX_TEMPO=199; verify MIDI clock rate matches expected |

---

## 8. Estimated Effort by Phase

| Phase | Description | Est. lines new code | Key risk |
|---|---|---:|---|
| 0 | Scaffold + HAL shim | ~800 | Compilation of 50k-line header soup |
| 1 | ALSA MIDI out + transport | ~400 | Timing thread setup |
| 2 | OSC input (keys + transport) | ~350 | liblo integration |
| 3 | OSC output (LED feedback) | ~250 | MIR diffing efficiency |
| 4 | Rotary encoders + attributes | ~100 | Quickturn acceleration timer |
| 5 | Phrases, strums, chords | ~50 | Mostly verification, code is kept verbatim |
| 6 | Persistence + bank import | ~200 | Struct packing verification |
| 7 | Grid, scenes, chaining, mixmaps | ~100 | Mostly verification |
| 8 | MIDI clock slave + recording | ~300 | Tempo smoothing algorithm |
| 9 | Nemo target + control surfaces | ~150 + JSONs | Nemo compile + OSC template design |
| 10 | Hardening + documentation | ~400 | Test coverage |
| **Total** | | **~3,100** | |
