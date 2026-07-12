# Octopus UI Launcher

Native desktop app (egui) that launches the C engine, provides a MIDI device
picker, and embeds a web server serving the HTML control surface. The HTML files
are compiled in via `include_str!`, so no external files are needed at runtime.

## Build

```bash
cargo build --release    # produces octopus_ui / octopus_ui.exe
```

## Usage

### GUI mode (default)

```bash
./octopus_ui
```

Opens a native window with:
- **Variant picker** — Octopus or Nemo
- **MIDI device pickers** — output A, output B, input (populated by querying the engine via `--list-midi`)
- **OSC port** — defaults to 8000
- **Web port** — defaults to 8080 (WebSocket auto-set to port+1)
- **Start / Stop** — launches the engine binary and the embedded web server

Once started, open `http://localhost:8080` in a browser for the control surface.

### Headless mode (`--nogui`)

For use without a display (servers, CI, scripts):

```bash
./octopus_ui --nogui \
    --variant octopus \
    --osc-port 8000 \
    --http-port 8080 \
    --out-a 0 \
    --out-b 1 \
    --in -1
```

Launches the engine + web server, waits for Ctrl+C, then cleanly shuts down.
The WebSocket port is automatically `http-port + 1`.

### List MIDI devices

```bash
./octopus_ui --list-midi
```

Prints available ALSA (or winmm) MIDI ports, then exits.

## How it works

1. **Finds the engine binary** by searching: exe directory, `./build/`, `../build/`, current dir
2. **Launches the engine** with `--osc-port`, `--out-a`, `--out-b`, `--in` flags
3. **Starts the embedded web server** (`web_server.rs`) on HTTP 8080 + WebSocket 8081
4. The web server bridges browser WebSocket ↔ engine OSC (UDP 8000/9000)
5. On exit: sends `/quit` OSC to engine, kills the process, stops the web server

## Ports

| Port | Protocol | Purpose |
|---|---|---|
| 8080 | HTTP | Serves `web_gui.html` (Octopus) or `web_gui_nemo.html` (Nemo) |
| 8081 | WebSocket | Browser ↔ Rust bridge |
| 8000 | OSC (UDP) | Engine input |
| 9000 | OSC (UDP) | Engine output (MIR frames) |

## vs. the other bridges

| Feature | `octopus_ui` | `cli_ui` (`web_gui`) | `web_gui.py` |
|---|---|---|---|
| Launches engine | Yes | No | No |
| Native GUI | Yes (egui) | No | No |
| HTML bundled in binary | Yes (`include_str!`) | No (reads from disk) | No (reads from disk) |
| Python dependency | No | No | Yes |
| External files needed | None | `web_gui*.html` | `web_gui*.html` + Python |

## Dependencies

- `eframe` 0.29 (egui immediate-mode GUI)
- `tokio` (async runtime for web server)
- `tokio-tungstenite` (WebSocket)
- `serde_json` (WebSocket message parsing)
- `futures-util`
