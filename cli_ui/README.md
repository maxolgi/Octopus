# Web GUI Bridge (Standalone Rust)

Standalone WebSocket ↔ OSC bridge that serves the HTML control surface in a
browser. No native GUI, no engine launching — start the engine separately, then
run this bridge. Functionally identical to `web_gui.py` but with no Python
dependency.

Reads `web_gui.html` and `web_gui_nemo.html` from the current directory at
runtime (the files are NOT embedded — keep them next to the binary).

## Build

```bash
cargo build --release    # produces web_gui / web_gui.exe
```

## Usage

```bash
# 1. Start the engine
./build/octopus &
# (or ./build/nemo & for Nemo)

# 2. Start the bridge
./web_gui

# 3. Open in browser
# http://localhost:8080        (Octopus)
# http://localhost:8080/nemo   (Nemo)
```

## How it works

1. **HTTP server** (port 8080) — serves `web_gui.html` at `/` and
   `web_gui_nemo.html` at `/nemo`
2. **WebSocket server** (port 8081) — accepts browser connections
3. **OSC bridge** — forwards browser WebSocket messages to the engine via UDP
   (port 8000), and forwards engine OSC output (port 9000) back to the browser
4. The browser interprets incoming OSC MIR frames into LED grid updates

## Ports

| Port | Protocol | Purpose |
|---|---|---|
| 8080 | HTTP | Serves `web_gui.html` / `web_gui_nemo.html` |
| 8081 | WebSocket | Browser ↔ Rust bridge |
| 8000 | OSC (UDP) | Engine input (bridge → engine) |
| 9000 | OSC (UDP) | Engine output (engine → bridge) |

All ports are hardcoded and must match the engine defaults.

## vs. the other bridges

| Feature | `web_gui` | `octopus_ui` | `web_gui.py` |
|---|---|---|---|
| Launches engine | No | Yes | No |
| Native GUI | No | Yes (egui) | No |
| HTML bundled in binary | No (reads from disk) | Yes (`include_str!`) | No (reads from disk) |
| Python dependency | No | No | Yes |
| External files needed | `web_gui*.html` | None | `web_gui*.html` + Python |

## Dependencies

- `tokio` (async runtime)
- `tokio-tungstenite` (WebSocket)
- `serde_json` (WebSocket message parsing)
- `futures-util`
