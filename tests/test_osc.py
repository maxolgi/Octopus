#!/usr/bin/env python3
"""
test_osc.py — Minimal OSC test client for the Octopus Linux port.
Sends OSC messages over UDP to control the sequencer.
"""
import socket
import struct
import sys
import time

HOST = "127.0.0.1"
PORT = 8000

def osc_pack(address, *args):
    """Pack an OSC message into bytes."""
    # Address string (null-terminated, padded to 4 bytes)
    msg = address.encode('ascii') + b'\x00'
    while len(msg) % 4 != 0:
        msg += b'\x00'
    
    # Type tag string
    types = ','
    for a in args:
        if isinstance(a, int):
            types += 'i'
        elif isinstance(a, float):
            types += 'f'
        elif isinstance(a, str):
            types += 's'
    
    msg += types.encode('ascii') + b'\x00'
    while len(msg) % 4 != 0:
        msg += b'\x00'
    
    # Arguments
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

def send(address, *args):
    """Send an OSC message."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.sendto(osc_pack(address, *args), (HOST, PORT))
    sock.close()
    print(f"  → {address} {args}")

if __name__ == '__main__':
    print(f"Sending OSC to {HOST}:{PORT}")
    
    # Step 1: Start the sequencer
    print("\n1. Start sequencer")
    send("/octopus/transport", "start")
    time.sleep(2)
    
    # Step 2: Toggle some steps on the grid
    # Page is at zoomPAGE (default). Matrix key indices: 11 + col*11 + row
    # Step at row 0, col 0 = index 11
    # Step at row 0, col 4 = index 55
    # Step at row 1, col 0 = index 12
    print("\n2. Toggle step at row 0, col 0 (key 11)")
    send("/octopus/key", 11, 1)  # press
    send("/octopus/key", 11, 0)  # release
    time.sleep(1)
    
    print("\n3. Toggle step at row 0, col 4 (key 55)")
    send("/octopus/key", 55, 1)
    send("/octopus/key", 55, 0)
    time.sleep(1)
    
    print("\n4. Toggle step at row 0, col 8 (key 99)")
    send("/octopus/key", 99, 1)
    send("/octopus/key", 99, 0)
    time.sleep(1)
    
    print("\n5. Change tempo to 140 BPM")
    send("/octopus/tempo", 140)
    time.sleep(3)
    
    print("\n6. Change tempo back to 120 BPM")
    send("/octopus/tempo", 120)
    time.sleep(2)
    
    # Step 3: Stop
    print("\n7. Stop sequencer")
    send("/octopus/transport", "stop")
    time.sleep(1)
    
    print("\nDone.")
