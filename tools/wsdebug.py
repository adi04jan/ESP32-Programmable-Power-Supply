#!/usr/bin/env python3
"""Wireless debugger for Voltbench — tails the device log over WebSocket.

The firmware side already exists: DbgPrint fans every log byte out to USB
serial, telnet :23, and the /logws WebSocket (view-only).  This is just the
host-side client, so a session on the LAN can watch the device live without
a USB cable.

  python tools/wsdebug.py                          # tail 30 s, then exit
  python tools/wsdebug.py -t 0                     # tail until Ctrl-C
  python tools/wsdebug.py -c "set_voltage 1 5"     # send a command, watch the effect
  python tools/wsdebug.py -c "toggle 1 1" -t 60 --host 192.168.1.42
  python tools/wsdebug.py -t 0 -o vb.log           # collector: reconnects, logs to disk

Commands go over /ws (the same dispatcher the browser uses -> apply_command),
so anything the web UI can do works here.  /logws is unauthenticated and
read-only; only -c needs the panel password.
"""
import argparse, json, sys, time, urllib.parse, urllib.request

import websocket  # websocket-client


def login(host, password):
    """POST /api/login -> token.  Rotates the token, so call at most once."""
    body = urllib.parse.urlencode({"pass": password}).encode()
    with urllib.request.urlopen(f"http://{host}/api/login", body, timeout=8) as r:
        return json.load(r)["token"]


def send_command(host, password, cmd):
    """cmd is 'action [output [value]]', e.g. 'set_voltage 1 5' or 'toggle 1 1'."""
    parts = cmd.split()
    msg = {"action": parts[0],
           "output": int(parts[1]) if len(parts) > 1 else 0,
           "value": float(parts[2]) if len(parts) > 2 else 0.0}

    with urllib.request.urlopen(f"http://{host}/api/info", timeout=8) as r:
        if json.load(r).get("require_login"):
            msg["token"] = login(host, password)

    ws = websocket.WebSocket()
    ws.connect(f"ws://{host}/ws", timeout=8)
    ws.send(json.dumps(msg))
    ws.settimeout(5)
    try:
        print(f"[cmd] {msg['action']} -> {ws.recv()}", flush=True)
    except websocket.WebSocketTimeoutException:
        print(f"[cmd] {msg['action']} sent, no status reply within 5 s", flush=True)
    ws.close()


def tail(host, seconds, out=None):
    """Stream /logws until the deadline, reconnecting for as long as it lasts.

    Reconnection is the whole point of the collector mode: the interesting
    moment is the device going away, and a client that dies with it records
    nothing about the recovery (or the absence of one).
    """
    sink = open(out, "a", encoding="utf-8", buffering=1) if out else None

    def emit(msg):
        line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}"
        print(line, flush=True)
        if sink:
            sink.write(line + "\n")

    deadline = time.time() + seconds if seconds else None
    try:
        while deadline is None or time.time() < deadline:
            ws = websocket.WebSocket()
            try:
                ws.connect(f"ws://{host}/logws", timeout=8)
            except (OSError, websocket.WebSocketException) as e:
                emit(f"[wsdebug] {host} unreachable ({e}) - retry in 5 s")
                time.sleep(5)
                continue

            ws.settimeout(1)
            emit(f"[wsdebug] connected to ws://{host}/logws")
            buf = ""
            try:
                while deadline is None or time.time() < deadline:
                    try:
                        buf += ws.recv()
                    except websocket.WebSocketTimeoutException:
                        continue
                    # Frames are raw ring chunks, not line-aligned — rebuffer.
                    *lines, buf = buf.split("\n")
                    for line in lines:
                        emit(line.rstrip())
            except (OSError, websocket.WebSocketException) as e:
                emit(f"[wsdebug] stream dropped ({e}) - reconnecting")
            finally:
                if buf.strip():
                    emit(buf.rstrip())
                ws.close()
    except KeyboardInterrupt:
        pass
    finally:
        if sink:
            sink.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="voltbench.local")
    ap.add_argument("-c", "--cmd", help="'action [output [value]]' sent over /ws first")
    ap.add_argument("-p", "--password", default="admin")
    ap.add_argument("-t", "--seconds", type=int, default=30,
                    help="tail duration; 0 = until Ctrl-C (default: 30)")
    ap.add_argument("-o", "--out", help="also append to this file (collector mode)")
    a = ap.parse_args()

    try:
        if a.cmd:
            send_command(a.host, a.password, a.cmd)
        tail(a.host, a.seconds, a.out)
    except OSError as e:
        # ConnectionRefused / DNS failure / device rebooting mid-OTA all land here.
        sys.exit(f"[wsdebug] {a.host}: {e}")


if __name__ == "__main__":
    main()
