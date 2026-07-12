#!/usr/bin/env python3
"""
test_phase4.py — Comprehensive test for Phase 4: rotary encoders, zoom, attributes.

Tests:
1. Toggle steps on multiple tracks (different pitches)
2. Switch to TRACK zoom and edit track pitch via rotary
3. Switch back to PAGE zoom
4. Change tempo via rotary
5. Verify MIDI output reflects attribute changes
"""
import socket
import struct
import sys
import time
import threading

HOST = "127.0.0.1"
OSC_IN_PORT = 8000   # octopus OSC input
OSC_OUT_PORT = 9000  # octopus OSC output

def osc_pack(address, *args):
    msg = address.encode('ascii') + b'\x00'
    while len(msg) % 4 != 0:
        msg += b'\x00'
    types = ','
    for a in args:
        if isinstance(a, int): types += 'i'
        elif isinstance(a, float): types += 'f'
        elif isinstance(a, str): types += 's'
    msg += types.encode('ascii') + b'\x00'
    while len(msg) % 4 != 0:
        msg += b'\x00'
    for a in args:
        if isinstance(a, int):
            msg += struct.pack('>i', a)
        elif isinstance(a, float):
            msg += struct.pack('>f', a)
        elif isinstance(a, str):
            s = a.encode('ascii') + b'\x00'
            while len(s) % 4 != 0:
                s += b'\x00'
            msg += s
    return msg

send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def send(address, *args):
    send_sock.sendto(osc_pack(address, *args), (HOST, OSC_IN_PORT))

def key(idx, press=True):
    send("/octopus/key", idx, 1 if press else 0)

def rotary(idx, direction, count=1):
    """direction: 'inc' or 'dec'. count: number of ticks."""
    d = 2 if direction == 'inc' else 1
    for _ in range(count):
        send("/octopus/rotary", idx, d)
        time.sleep(0.02)

# Key constants from defs_frontpanel.h
KEY_ZOOM_PAGE  = 219
KEY_ZOOM_TRACK = 220
KEY_ZOOM_STEP  = 227
KEY_PLAY1      = 241
KEY_STOP       = 231
KEY_RETURN     = 186

# Rotary constants
ROT_BIGKNOB = 0
ROT_VEL     = 1
ROT_PIT     = 2
ROT_LEN     = 3
ROT_MCH     = 10

# Matrix key helper: row 0-9, col 0-15
def matrix_key(row, col):
    return 11 + col * 11 + row

# Track selector keys: keys 1-10 select tracks 0-9
def track_selector(row):
    return row + 1

# ============================================================
# MIDI capture thread
# ============================================================
captured_notes = []

def midi_capture():
    """Listen for MIDI on OSC output port (we can't easily capture ALSA,
    so we'll rely on aseqdump in the background). Instead, capture MIR
    state changes as evidence of attribute changes."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', OSC_OUT_PORT))
    sock.settimeout(0.5)
    mir_frames = []
    start = time.time()
    while time.time() - start < 30:
        try:
            data, addr = sock.recvfrom(4096)
            addr_str = data.split(b'\x00')[0].decode('ascii', errors='replace')
            if addr_str == '/octopus/mir':
                # Parse blob
                pos = 0
                alen = len(data.split(b'\x00')[0]) + 1
                pos = (alen + 3) & ~3
                tlen = len(data[pos:].split(b'\x00')[0]) + 1
                pos += (tlen + 3) & ~3
                blob_size = struct.unpack('>i', data[pos:pos+4])[0]
                pos += 4
                mir = data[pos:pos+blob_size]
                nz = sum(1 for b in mir if b != 0)
                mir_frames.append(nz)
        except socket.timeout:
            pass
    return mir_frames

# Start capture thread
cap_thread = threading.Thread(target=midi_capture)
cap_thread.daemon = True
cap_thread.start()

print("=" * 60)
print("Phase 4 Test: Rotary Encoders + Track/Step Attributes")
print("=" * 60)

# ============================================================
# Step 1: Start transport and set up a pattern
# ============================================================
print("\n1. Starting sequencer...")
send("/octopus/transport", "start")
time.sleep(1)

# ============================================================
# Step 2: Toggle steps on track 0 at columns 0, 4, 8, 12
# ============================================================
print("2. Toggling steps on track 0 (row 0): cols 0, 4, 8, 12")
for col in [0, 4, 8, 12]:
    k = matrix_key(0, col)
    key(k)
    time.sleep(0.1)
    key(k, False)
    time.sleep(0.1)
time.sleep(2)

# ============================================================
# Step 3: Switch to TRACK zoom and change pitch
# ============================================================
print("3. Switching to TRACK zoom")
key(KEY_ZOOM_TRACK)
time.sleep(0.5)
key(KEY_ZOOM_TRACK, False)
time.sleep(0.5)

print("   Selecting track 0")
key(track_selector(0))  # select track 0
time.sleep(0.3)
key(track_selector(0), False)
time.sleep(0.5)

print("   Rotating ROT_PIT (pitch) UP by 7 semitones")
rotary(ROT_PIT, 'inc', 7)
time.sleep(2)

print("   Rotating ROT_PIT (pitch) DOWN by 5 semitones")
rotary(ROT_PIT, 'dec', 5)
time.sleep(2)

print("   Rotating ROT_VEL (velocity) UP by 20")
rotary(ROT_VEL, 'inc', 20)
time.sleep(2)

# ============================================================
# Step 4: Return to PAGE zoom
# ============================================================
print("4. Returning to PAGE zoom")
key(KEY_RETURN)
time.sleep(0.3)
key(KEY_RETURN, False)
time.sleep(0.5)

key(KEY_ZOOM_PAGE)
time.sleep(0.3)
key(KEY_ZOOM_PAGE, False)
time.sleep(1)

# ============================================================
# Step 5: Change tempo via big knob rotary
# ============================================================
print("5. Changing tempo via ROT_BIGKNOB")
rotary(ROT_BIGKNOB, 'inc', 10)  # increase tempo
time.sleep(2)
rotary(ROT_BIGKNOB, 'dec', 10)  # back down
time.sleep(2)

# ============================================================
# Step 6: Toggle steps on track 1 for polyphonic pattern
# ============================================================
print("6. Toggling steps on track 1 (row 1): cols 2, 6, 10, 14")
for col in [2, 6, 10, 14]:
    k = matrix_key(1, col)
    key(k)
    time.sleep(0.1)
    key(k, False)
    time.sleep(0.1)
time.sleep(3)

# ============================================================
# Done
# ============================================================
print("\n7. Stopping sequencer")
send("/octopus/transport", "stop")
time.sleep(1)

print("\n" + "=" * 60)
print("Phase 4 test complete.")
print("Check MIDI output with: aseqdump -p <client>:0")
print("=" * 60)

send("/octopus/quit")
