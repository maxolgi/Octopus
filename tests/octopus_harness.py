#!/usr/bin/env python3
"""
octopus_harness.py — driver/observer for a *running* Octopus engine.

Targets the hosted octopus_gui instance (the canonical local dev setup):
  * OSC input  -> UDP 127.0.0.1:8000   (index- and name-based addresses)
  * MIR output <- WebSocket ws://127.0.0.1:8089  (JSON frames)

MIR frame layout (170 bytes):
  2 sets (LHS=0, RHS=1) x 17 rows x 5 bytes
  byte0=blink  byte1=red  byte2=green  byte3=shine_green  byte4=shine_red
  bit b of a plane byte == LED position b (0-7) within that row.

The named-LED coordinate table is derived from the firmware's
firmware/OCT_OS/_OCT_Viewer/filler_functions.h::MIR_write_dot().
Step-pad coordinates use row_of(k)=k%11, column_of(k)=k/11-1 (the "mind
bending" HW logic in Intr_KEY_functions.h).
"""
import asyncio, json, socket, struct, threading, time
from collections import deque
import websockets

OSC_HOST = "127.0.0.1"
OSC_PORT = 8000
WS_URI   = "ws://127.0.0.1:8089"

# ---------------------------------------------------------------- OSC send
def osc_pack(address, *args):
    msg = address.encode("ascii") + b"\x00"
    while len(msg) % 4: msg += b"\x00"
    types = ","
    for a in args:
        types += "i" if isinstance(a, int) else ("f" if isinstance(a, float) else "s")
    msg += types.encode("ascii") + b"\x00"
    while len(msg) % 4: msg += b"\x00"
    for a in args:
        if isinstance(a, int):
            msg += struct.pack(">i", a)
        elif isinstance(a, float):
            msg += struct.pack(">f", a)
        else:
            s = str(a).encode("ascii") + b"\x00"
            while len(s) % 4: s += b"\x00"
            msg += s
    return msg

def send_osc(address, *args, host=OSC_HOST, port=OSC_PORT):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(osc_pack(address, *args), (host, port))
    s.close()

# ---------------------------------------------------------------- LED map
# name -> (set, row, bit)  from MIR_write_dot()
LED = {
    # circle core (zoom)  LHS row16
    "GRID": (0,16,0), "PAGE": (0,16,1), "STEP": (0,16,2),
    "MAP":  (0,16,3), "TRK":  (0,16,4), "PLAY": (0,16,5),
    # transport
    "REC": (0,14,7), "STP": (1,14,0), "PSE": (1,14,1),
    "P1":  (1,14,2), "P2":  (1,14,3), "P4":  (1,14,4),
    # mix strip (bottom row)
    "MIX": (0,10,0), "SEL": (0,10,1), "ATR": (0,10,2), "VOL": (0,10,3),
    "PAN": (0,10,4), "MOD": (0,10,5), "EXP": (0,10,6), "U0":  (0,10,7),
    "U1":  (1,10,0), "U2":  (1,10,1), "U3":  (1,10,2), "U4":  (1,10,3),
    "U5":  (1,10,4), "MUT": (1,10,5), "EDT": (1,10,6), "ESC": (1,10,7),
    # mutators (right column)
    "TGGL": (1,11,1), "SOLO": (1,11,2), "CLR": (1,11,3), "RND": (1,11,4),
    "FLT":  (1,11,5), "RMX": (1,12,0), "EFF": (1,12,1), "ZOOM": (1,12,2),
    "CPY":  (1,12,3), "PST": (1,12,4),
    # side bow (chord size + align)  RHS row16
    "CHORD0": (1,16,0), "CHORD1": (1,16,1), "CHORD2": (1,16,2),
    "CHORD3": (1,16,3), "CHORD4": (1,16,4), "CHORD5": (1,16,5),
    "CHORD6": (1,16,6), "ALN":   (1,16,7),
    # indicators
    "EDIT_IND": (1,12,5), "MIX_IND": (0,12,0),
    "TPO": (1,13,5), "CLOCK": (1,13,6),
}

# color plane byte index within a row
PLANE = {"blink": 0, "red": 1, "green": 2, "shine_green": 3, "shine_red": 4}

# key index constants (from keymap.md / defs_frontpanel.h)
KEY = {
    "GRID": 218, "PAGE": 219, "TRK": 220, "STEP": 227, "MAP": 228, "PLAY": 229,
    "REC": 223, "STP": 231, "PSE": 232, "P1": 241, "P2": 240, "P4": 250,
    "MUT": 164, "EDT": 175, "ESC": 186, "MIX": 21,
    "CHORD0": 258, "CHORD1": 257, "CHORD2": 256, "CHORD3": 255,
    "CHORD4": 254, "CHORD5": 253, "CHORD6": 252, "ALN": 251,
}
ROT = {"TPO": 0, "VEL": 1, "PIT": 2, "LEN": 3, "STA": 4, "POS": 5, "DIR": 6,
       "AMT": 7, "GRV": 8, "MCC": 9, "MCH": 10}
# zoom level constants (defs_general.h)
ZOOM = {"GRID": 2, "PAGE": 3, "TRACK": 4, "MAP": 5, "STEP": 6, "PLAY": 7}

def step_key(row, col):
    """OSC key index for a step pad at matrix row 0-9, col 0-15."""
    return 11 + col * 11 + row

def _bit(mir, set_, row, bit, color):
    return bool(mir[set_*85 + row*5 + PLANE[color]] & (1 << bit))

class Octopus:
    """Drives a running octopus_gui: OSC in (UDP), MIR out (WS)."""
    def __init__(self, ws_uri=WS_URI, osc_port=OSC_PORT, history=60):
        self.ws_uri = ws_uri
        self.osc_port = osc_port
        self.mir = None
        self.run_bit = None      # latest /transport int (G_run_bit)
        self._hist = deque(maxlen=200)
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = None
        self._connected = threading.Event()
        self._ws_error = None

    # ---- control (OSC out) ----
    def key(self, idx, press=True):
        send_osc("/key", idx, 1 if press else 0, port=self.osc_port)
    def key_name(self, name, press=True):
        send_osc(f"/key/{name}", 1 if press else 0, port=self.osc_port)
    def rotary(self, idx, direction, count=1):
        for _ in range(count):
            send_osc("/rotary", idx, direction, port=self.osc_port)
            time.sleep(0.02)
    def rot_name(self, name, direction, count=1):
        for _ in range(count):
            send_osc(f"/rotary/{name}", direction, port=self.osc_port)
            time.sleep(0.02)
    def transport(self, cmd):
        send_osc("/transport", cmd, port=self.osc_port)
    def tempo(self, bpm):
        send_osc("/tempo", bpm, port=self.osc_port)
    def zoom(self, level):
        send_osc("/zoom", level, port=self.osc_port)
    def save(self):
        send_osc("/save", port=self.osc_port)
    def quit(self):
        send_osc("/quit", port=self.osc_port)
    def settle(self, t=0.5):
        time.sleep(t)

    # ---- observation (WS in) ----
    def start(self, settle=1.5):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        self._connected.wait(timeout=10)
        if self._ws_error is not None:
            raise RuntimeError(f"WS connect failed: {self._ws_error}")
        time.sleep(settle)

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3)

    def _run(self):
        async def run():
            try:
                async with websockets.connect(self.ws_uri) as ws:
                    self._connected.set()
                    while not self._stop.is_set():
                        try:
                            m = await asyncio.wait_for(ws.recv(), timeout=0.5)
                        except asyncio.TimeoutError:
                            continue
                        d = json.loads(m)
                        a = d.get("address")
                        if a == "/mir":
                            b = bytes.fromhex(d["args"][0])
                            with self._lock:
                                self.mir = b
                                self._hist.append((time.time(), b))
                        elif a == "/transport":
                            with self._lock:
                                self.run_bit = d["args"][0]
            except Exception as e:
                self._ws_error = e
                self._connected.set()
        asyncio.run(run())

    def _snapshot(self):
        with self._lock:
            return self.mir, self.transport, list(self._hist)

    def get_mir(self):
        mir, _, _ = self._snapshot()
        return mir

    def capture_or(self, seconds=0.9):
        """Blink-safe capture: OR of every MIR frame received during a window.
        Blinking LEDs (red+green cleared on the blink-off phase) are thus
        reliably captured in at least one frame of the window. History is
        timestamped; frames are filtered by time (indexing a possibly-full
        deque by pre-window length would silently return the wrong tail)."""
        t0 = time.time()
        time.sleep(seconds)
        with self._lock:
            frames = [f for (t, f) in self._hist if t >= t0]
        if not frames:
            return self.get_mir()
        result = bytearray(len(frames[0]))
        for f in frames:
            for i in range(len(f)):
                result[i] |= f[i]
        return bytes(result)

    def led_or(self, name, color="red", seconds=0.9):
        """Blink-safe: True if the named LED is on in any frame of the window."""
        mir = self.capture_or(seconds)
        if mir is None:
            return False
        s, r, b = LED[name]
        return bool(mir[s*85 + r*5 + PLANE[color]] & (1 << b))

    def matrix_green_or(self, seconds=0.9):
        """Blink-safe green-plane bytes for the matrix region (rows 0-9, both sides)."""
        mir = self.capture_or(seconds)
        if mir is None:
            return None
        out = []
        for set_ in range(2):
            for row in range(10):
                out.append(mir[set_*85 + row*5 + PLANE["green"]])
        return tuple(out)

    def get_transport(self):
        with self._lock:
            return self.run_bit

    def transport_is(self, val, timeout=5.0):
        """Poll the run-bit directly (independent of MIR frames, which are
        diff-based and may not arrive if the display is stable)."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self._lock:
                tr = self.run_bit
            if tr == val:
                return True
            time.sleep(0.05)
        return False

    # ---- LED queries ----
    def ensure_edit_mode(self):
        """Stop transport, force GRID zoom, and ensure GRID_EDIT (not perform) mode.

        In GRID zoom the firmware shows: TRK green iff GRID_MIX (perform),
        PAGE green iff GRID_EDIT (edit). The PLAY key toggles the mode. We
        normalize to edit mode so that matrix key presses toggle STEPS (the
        manual's 'Step toggle (TGL)' behavior) rather than page play-state."""
        self.transport("stop"); self.settle(0.4)
        self.zoom(ZOOM["GRID"]); self.settle(0.6)
        # detect perform mode via TRK-green indicator (window-OR, blink-safe)
        for _ in range(2):
            if self.led_or("TRK", "green", 1.0) and not self.led_or("PAGE", "green", 1.0):
                self.key(KEY["PLAY"]); self.settle(0.2); self.key(KEY["PLAY"], False)
                self.settle(0.8)
            else:
                break

if __name__ == "__main__":
    o = Octopus()
    o.start()
    print("connected, transport:", o.get_transport())
    mir = o.get_mir()
    print("mir len:", len(mir) if mir else None)
    o.stop()
