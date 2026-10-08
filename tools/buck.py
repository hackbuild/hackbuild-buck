#!/usr/bin/env python3
"""Talk to a BUCK over MQTT, CLASP, or USB serial.

  python3 tools/buck.py say "hello deer"                  MQTT, standard library only
  python3 tools/buck.py say "hello deer" --via clasp      needs: pip install websockets
  python3 tools/buck.py watch                             print status as it changes
  python3 tools/buck.py console                           serial console, needs: pip install pyserial

--id picks the BUCK (default heatsync). --voice sam|elf|robot|stuffy|oldlady|et
sends a voice change before the text.
"""
import argparse
import asyncio
import glob
import os
import socket
import struct
import sys
import threading
import time

MQTT_HOST = "relay.clasp.chat"
MQTT_PORT = 1883
CLASP_URL = "wss://relay.clasp.to"
ROOT = "hackbuild/buck"


# ---------------------------------------------------------------- MQTT (stdlib)

def _rl(n):
    out = b""
    while True:
        b = n & 0x7F
        n >>= 7
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out


def _s(x):
    x = x.encode()
    return struct.pack(">H", len(x)) + x


def _pkt(first, body):
    return bytes([first]) + _rl(len(body)) + body


def _mqtt_connect(client_id):
    sock = socket.create_connection((MQTT_HOST, MQTT_PORT), 10)
    sock.sendall(_pkt(0x10, _s("MQTT") + bytes([4, 2]) + struct.pack(">H", 60) + _s(client_id)))
    if sock.recv(4)[:1] != b"\x20":
        sys.exit("broker refused the connection")
    return sock


def _mqtt_packets(sock):
    buf = b""
    while True:
        data = sock.recv(4096)
        if not data:
            return
        buf += data
        while len(buf) >= 2:
            mult, length, i = 1, 0, 1
            while True:
                if i >= len(buf):
                    break
                b = buf[i]
                length += (b & 0x7F) * mult
                mult *= 128
                i += 1
                if not b & 0x80:
                    break
            if len(buf) < i + length:
                break
            yield buf[0], buf[i:i + length]
            buf = buf[i + length:]


def mqtt_say(bid, text, voice):
    sock = _mqtt_connect(f"buck-cli-{os.getpid()}")
    if voice:
        sock.sendall(_pkt(0x30, _s(f"{ROOT}/{bid}/voice") + voice.encode()))
    sock.sendall(_pkt(0x30, _s(f"{ROOT}/{bid}/say") + text.encode()))
    sock.sendall(b"\xe0\x00")
    sock.close()
    print(f"sent to {ROOT}/{bid}/say on mqtt://{MQTT_HOST}")


def mqtt_watch(bid):
    sock = _mqtt_connect(f"buck-watch-{os.getpid()}")
    sock.sendall(_pkt(0x82, struct.pack(">H", 1) + _s(f"{ROOT}/{bid}/status/#") + b"\x00"))
    print(f"watching {ROOT}/{bid}/status/# (stored values first), ctrl-c to stop")

    def ping():
        while True:
            time.sleep(30)
            sock.sendall(b"\xc0\x00")

    threading.Thread(target=ping, daemon=True).start()
    for first, body in _mqtt_packets(sock):
        if first >> 4 == 3:
            tl = struct.unpack(">H", body[:2])[0]
            topic = body[2:2 + tl].decode(errors="replace")
            print(f"{time.strftime('%H:%M:%S')}  {topic.rsplit('/', 1)[-1]:9} {body[2 + tl:].decode(errors='replace')}")


# ---------------------------------------------------------------- CLASP

def _frame(payload, qos=1):
    return bytes([0x53, (qos << 6) | 1]) + struct.pack(">H", len(payload)) + payload


def _hello(name):
    return _frame(bytes([0x01, 1, 0xE0]) + _s(name) + _s(""))


def _emit(addr, text):
    return _frame(bytes([0x20, 1 << 5]) + _s(addr) + b"\x01\x08" + _s(text))


def _subscribe(sid, pattern):
    return _frame(bytes([0x10]) + struct.pack(">I", sid) + _s(pattern) + b"\xff\x00")


def _decode(f):
    """(type, address, value) for PUBLISH and SET with simple values, else (type, None, None)."""
    off = 12 if f[1] & 0x20 else 4
    p = f[off:]
    t = p[0]
    if t not in (0x20, 0x21):
        return t, None, None
    flags = p[1]
    al = struct.unpack(">H", p[2:4])[0]
    addr = p[4:4 + al].decode(errors="replace")
    q = p[4 + al:]
    if t == 0x20:
        if not q or q[0] != 1:
            return t, addr, None
        vt, q = q[1], q[2:]
    else:
        vt = flags & 0x0F
    if vt == 0x08:
        n = struct.unpack(">H", q[:2])[0]
        return t, addr, q[2:2 + n].decode(errors="replace")
    if vt == 0x01:
        return t, addr, bool(q[0])
    if vt == 0x07:
        return t, addr, round(struct.unpack(">d", q[:8])[0], 2)
    return t, addr, None


async def clasp_say(bid, text, voice):
    import websockets
    async with websockets.connect(CLASP_URL, subprotocols=["clasp"]) as ws:
        await ws.send(_hello("buck cli"))
        await ws.recv()  # WELCOME
        if voice:
            await ws.send(_emit(f"/{ROOT}/{bid}/voice", voice))
        await ws.send(_emit(f"/{ROOT}/{bid}/say", text))
        await asyncio.sleep(0.5)
    print(f"sent to /{ROOT}/{bid}/say on {CLASP_URL}")


async def clasp_watch(bid):
    import websockets
    async with websockets.connect(CLASP_URL, subprotocols=["clasp"]) as ws:
        await ws.send(_hello("buck watch"))
        await ws.recv()
        await ws.send(_subscribe(1, f"/{ROOT}/{bid}/status/**"))
        print(f"watching /{ROOT}/{bid}/status/** on {CLASP_URL}, ctrl-c to stop")
        async for m in ws:
            t, addr, val = _decode(m)
            if addr and not addr.endswith("/jaw"):
                print(f"{time.strftime('%H:%M:%S')}  {addr.rsplit('/', 1)[-1]:9} {val}")


# ---------------------------------------------------------------- serial

def console():
    import serial
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    port = os.environ.get("BUCK_PORT") or (ports[0] if ports else None)
    if not port:
        sys.exit("no BUCK on USB: plug it in or set BUCK_PORT")
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.1
    s.rts = False   # RTS high with DTR low resets the C3, so RTS stays low and DTR is left alone
    s.open()

    def reader():
        while True:
            d = s.read(4096)
            if d:
                sys.stdout.write(d.decode(errors="replace"))
                sys.stdout.flush()

    threading.Thread(target=reader, daemon=True).start()
    print(f"{port}: type to talk, /help for commands, ctrl-d to quit")
    for line in sys.stdin:
        s.write(line.encode())


def main():
    ap = argparse.ArgumentParser(description="Talk to a BUCK")
    ap.add_argument("cmd", choices=["say", "watch", "console"])
    ap.add_argument("text", nargs="*")
    ap.add_argument("--id", default="heatsync", help="which BUCK (default heatsync)")
    ap.add_argument("--via", choices=["mqtt", "clasp"], default="mqtt")
    ap.add_argument("--voice", help="sam, elf, robot, stuffy, oldlady or et")
    a = ap.parse_args()
    if a.cmd == "console":
        return console()
    if a.cmd == "say":
        text = " ".join(a.text)
        if not text:
            sys.exit("say what?")
        if a.via == "mqtt":
            mqtt_say(a.id, text, a.voice)
        else:
            asyncio.run(clasp_say(a.id, text, a.voice))
        return
    try:
        if a.via == "mqtt":
            mqtt_watch(a.id)
        else:
            asyncio.run(clasp_watch(a.id))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
