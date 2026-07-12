#!/usr/bin/env python3
"""
web_gui.py — WebSocket ↔ OSC bridge for the Octopus Linux port.

Serves the HTML control surface on HTTP port 8080 and bridges WebSocket
connections (port 8081) to the Octopus engine's UDP OSC ports.

Usage:
    ./build/octopus &          # start the engine
    python3 web_gui.py         # start the web GUI bridge
    # Open http://localhost:8080 in a browser
"""
import asyncio
import json
import struct
import socket
import threading
import http.server
import functools
import os

try:
    import websockets
except ImportError:
    print("Error: pip3 install websockets")
    exit(1)

ENGINE_HOST = "127.0.0.1"
ENGINE_OSC_IN = 8000
ENGINE_OSC_OUT = 9000
WEB_HTTP_PORT = 8080
WEB_WS_PORT = 8081

HTML_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'web_gui.html')
NEMO_HTML_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'web_gui_nemo.html')

# ============================================================
# OSC pack/unpack
# ============================================================

def osc_pack(address, *args):
    msg = address.encode() + b'\x00'
    while len(msg) % 4: msg += b'\x00'
    types = ','
    for a in args:
        if isinstance(a, int): types += 'i'
        elif isinstance(a, str): types += 's'
    msg += types.encode() + b'\x00'
    while len(msg) % 4: msg += b'\x00'
    for a in args:
        if isinstance(a, int): msg += struct.pack('>i', a)
        elif isinstance(a, str):
            s = a.encode() + b'\x00'
            while len(s) % 4: s += b'\x00'
            msg += s
    return msg

def osc_unpack(data):
    addr_end = data.index(b'\x00')
    address = data[:addr_end].decode('ascii')
    pos = (addr_end + 4) & ~3
    if pos >= len(data) or data[pos:pos+1] != b',':
        return address, []
    type_end = data.index(b'\x00', pos)
    types = data[pos+1:type_end].decode('ascii')
    pos = (type_end + 4) & ~3
    args = []
    for t in types:
        if t == 'i':
            args.append(struct.unpack('>i', data[pos:pos+4])[0])
            pos += 4
        elif t == 'b':
            blen = struct.unpack('>i', data[pos:pos+4])[0]
            pos += 4
            args.append(data[pos:pos+blen].hex())
            pos += (blen + 3) & ~3
        elif t == 's':
            send_ = data.index(b'\x00', pos)
            args.append(data[pos:send_].decode('ascii'))
            pos = (send_ + 4) & ~3
    return address, args

# ============================================================
# OSC UDP socket
# ============================================================

osc_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
osc_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
osc_sock.bind(('127.0.0.1', ENGINE_OSC_OUT))
osc_sock.setblocking(False)

connected_clients = set()

async def listen_osc():
    """Forward OSC from engine to WebSocket clients."""
    loop = asyncio.get_running_loop()
    while True:
        try:
            data, addr = await loop.sock_recvfrom(osc_sock, 4096)
            address, args = osc_unpack(data)
            msg = json.dumps({"address": address, "args": args})
            if connected_clients:
                await asyncio.gather(
                    *[c.send(msg) for c in list(connected_clients)],
                    return_exceptions=True
                )
        except BlockingIOError:
            await asyncio.sleep(0.001)
        except Exception:
            await asyncio.sleep(0.001)

async def handle_websocket(websocket):
    """Handle browser WebSocket connection."""
    connected_clients.add(websocket)
    try:
        async for message in websocket:
            try:
                cmd = json.loads(message)
                ctype = cmd.get('type', '')
                if ctype == 'key':
                    osc_sock.sendto(osc_pack('/key', cmd['index'], 1 if cmd.get('press', True) else 0),
                                    (ENGINE_HOST, ENGINE_OSC_IN))
                elif ctype == 'rotary':
                    osc_sock.sendto(osc_pack('/rotary', cmd['index'], cmd['direction']),
                                    (ENGINE_HOST, ENGINE_OSC_IN))
                elif ctype == 'transport':
                    osc_sock.sendto(osc_pack('/transport', cmd['cmd']),
                                    (ENGINE_HOST, ENGINE_OSC_IN))
                elif ctype == 'tempo':
                    osc_sock.sendto(osc_pack('/tempo', cmd['bpm']),
                                    (ENGINE_HOST, ENGINE_OSC_IN))
                elif ctype == 'zoom':
                    osc_sock.sendto(osc_pack('/zoom', cmd['level']),
                                    (ENGINE_HOST, ENGINE_OSC_IN))
                elif ctype == 'quit':
                    osc_sock.sendto(osc_pack('/quit'),
                                    (ENGINE_HOST, ENGINE_OSC_IN))
            except Exception:
                pass
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        connected_clients.discard(websocket)

# ============================================================
# HTTP server (runs in a separate thread)
# ============================================================

class HTTPHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        html_file = HTML_FILE
        if self.path == '/nemo' or self.path == '/nemo/':
            html_file = NEMO_HTML_FILE
        elif self.path != '/' and self.path != '/index.html':
            self.send_response(404)
            self.end_headers()
            return
        try:
            with open(html_file, 'r', encoding='utf-8') as f:
                content = f.read()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.end_headers()
            self.wfile.write(content.encode())
        except FileNotFoundError:
            self.send_response(404)
            self.end_headers()
    def log_message(self, *args):
        pass

def run_http():
    server = http.server.HTTPServer(('0.0.0.0', WEB_HTTP_PORT), HTTPHandler)
    server.serve_forever()

# ============================================================
# Main
# ============================================================

async def main():
    # Start HTTP server in background thread
    http_thread = threading.Thread(target=run_http, daemon=True)
    http_thread.start()

    # Start WebSocket server and OSC listener
    async with websockets.serve(handle_websocket, "0.0.0.0", WEB_WS_PORT):
        print(f"Web GUI: http://localhost:{WEB_HTTP_PORT}")
        print(f"WebSocket: ws://localhost:{WEB_WS_PORT}")
        print(f"OSC bridge: engine:{ENGINE_OSC_IN}->engine:{ENGINE_OSC_OUT}")
        await listen_osc()

if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nShutting down.")
