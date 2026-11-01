#!/usr/bin/env python3
"""ClaudeStick telemetry collector: receives the opt-in usage reports described in docs/TELEMETRY.md.

  python3 telemetry/claudestick_telemetry.py                 # listens on 0.0.0.0:7895
  python3 telemetry/claudestick_telemetry.py --summary        # print aggregate stats and exit
  python3 telemetry/claudestick_telemetry.py --show-token     # print the /stats token and exit

A Stick only sends a report when its owner has explicitly turned the setting on (off by
default) and only after they've pointed it at a destination (also blank by default — see
firmware/setup_page.html and docs/TELEMETRY.md for exactly what a report contains and
what it never contains: no tool/approval/macro text, no commands, no code, no audio, no
token, no SSID, no URL you configured).

Standard library only, Python 3.8+. Put it behind a reverse proxy for HTTPS if it's
reachable from the internet (see docs/REMOTE.md for the same kind of setup with the bridge).
"""
import argparse
import hmac
import ipaddress
import json
import os
import secrets
import socket
import sqlite3
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

VERSION = "0.9.4"   # kept in step with FW_VERSION; tools/verify_release.py checks this like the bridge's

MAX_BODY = 4096                 # a report is a few hundred bytes; generous headroom, not unbounded
RATE_WINDOW_S, RATE_MAX = 600, 20   # per address: 20 reports / 10 min is far above one real device's 6 h cadence

# Every field a report may carry, with a type check and (for strings) a max length. Anything
# else in the body is dropped — this is the one place that enforces "never anything you typed".
FIELDS = {
    "device_id": (str, 64), "fw": (str, 24), "uptime_s": (int, None), "boot_epoch": (int, None),
    "reset_reason": (str, 40), "safe_mode": (bool, None),
    "approvals": (int, None), "denies": (int, None), "always_allow": (int, None), "danger_cards": (int, None),
    "macro_taps": (int, None), "done_choose": (int, None), "done_dismiss": (int, None),
    "voice_ok": (int, None), "voice_fail": (int, None), "sessions_seen": (int, None),
    "remote_on_s": (int, None), "remote_off_s": (int, None),
    "battery_pct": (int, None), "battery_charging": (bool, None), "battery_drain_pct_h": ((int, float), None),
    "bridge_configured": (bool, None), "bt_on": (bool, None), "remote_mode": (str, 8),
    "text_style": (str, 16), "buddy": (str, 24), "tilt_on": (bool, None), "flip_on": (bool, None),
    "wifi_networks": (int, None),
}


def clean(body):
    """What actually gets stored: only known fields, right type, strings clipped. device_id and fw required."""
    if not isinstance(body, dict):
        return None
    out = {}
    for k, (types, maxlen) in FIELDS.items():
        v = body.get(k)
        if v is None:
            continue
        if types is bool:
            if not isinstance(v, bool):
                continue
        elif isinstance(v, bool):   # bools are ints in Python/JSON; never accept one where a number was wanted
            continue
        elif not isinstance(v, types):
            continue
        if isinstance(v, str):
            v = v[:maxlen]
        if isinstance(v, int) and not isinstance(v, bool):
            v = max(-1 << 31, min(v, 1 << 31))   # a device can't send a number that breaks storage
        out[k] = v
    if not out.get("device_id") or not out.get("fw"):
        return None
    return out


class Store:
    def __init__(self, path):
        self.path = path
        os.makedirs(path, mode=0o700, exist_ok=True)
        self.db = sqlite3.connect(os.path.join(path, "telemetry.db"), check_same_thread=False)
        self.db.execute("CREATE TABLE IF NOT EXISTS reports (id INTEGER PRIMARY KEY, device_id TEXT, fw TEXT, "
                        "received_at REAL, remote TEXT, payload TEXT)")
        self.db.execute("CREATE INDEX IF NOT EXISTS idx_device ON reports (device_id, received_at)")
        self.db.commit()
        self.lock = threading.Lock()

    def ident_token(self):
        p = os.path.join(self.path, "stats_token.json")
        try:
            with open(p) as f:
                return json.load(f)["token"]
        except (OSError, ValueError, KeyError):
            tok = secrets.token_hex(16)
            fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                json.dump({"token": tok}, f)
            return tok

    def store(self, report, remote):
        with self.lock:
            self.db.execute("INSERT INTO reports (device_id, fw, received_at, remote, payload) VALUES (?,?,?,?,?)",
                            (report["device_id"], report["fw"], time.time(), remote, json.dumps(report)))
            self.db.commit()

    def stats(self):
        with self.lock:
            cur = self.db.execute("SELECT device_id, fw, received_at, payload FROM reports")
            rows = cur.fetchall()
        devices = {}
        fw_hist = {}
        totals = {}
        for device_id, fw, received_at, payload in rows:
            d = devices.setdefault(device_id, {"first_seen": received_at, "last_seen": received_at, "reports": 0, "fw": fw})
            d["first_seen"] = min(d["first_seen"], received_at)
            d["last_seen"] = max(d["last_seen"], received_at)
            d["reports"] += 1
            d["fw"] = fw   # newest row wins by table order (insertion order == id order)
            fw_hist[fw] = fw_hist.get(fw, 0) + 1
            try:
                p = json.loads(payload)
            except ValueError:
                continue
            for k, v in p.items():
                if isinstance(v, (int, float)) and not isinstance(v, bool) and k in FIELDS and FIELDS[k][0] in (int, (int, float)):
                    totals[k] = totals.get(k, 0) + v
        now = time.time()
        return {
            "reports_total": len(rows),
            "devices_total": len(devices),
            "devices_active_7d": sum(1 for d in devices.values() if now - d["last_seen"] < 7 * 86400),
            "fw_versions": fw_hist,
            "totals": totals,
        }


def summary_text(s):
    lines = [f"reports:        {s['reports_total']}", f"devices:        {s['devices_total']}",
             f"active (7 d):   {s['devices_active_7d']}", "firmware versions:"]
    for fw, n in sorted(s["fw_versions"].items(), key=lambda x: -x[1]):
        lines.append(f"  {fw:<10} {n}")
    lines.append("totals across every report on file:")
    for k, v in sorted(s["totals"].items()):
        lines.append(f"  {k:<20} {v}")
    return "\n".join(lines)


class RateLimit:
    def __init__(self, mono=time.monotonic):
        self.mono = mono
        self.hits = {}
        self.lock = threading.Lock()

    def allow(self, ip):
        now = self.mono()
        with self.lock:
            if len(self.hits) > 4096:
                self.hits = {k: v for k, v in self.hits.items() if now - v[0] < RATE_WINDOW_S}
            win_start, n = self.hits.get(ip, (now, 0))
            if now - win_start > RATE_WINDOW_S:
                win_start, n = now, 0
            n += 1
            self.hits[ip] = (win_start, n)
            return n <= RATE_MAX


def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "ClaudeStickTelemetry"
    sys_version = ""

    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def client_ip(self):
        ip = self.client_address[0]
        if self.server.trust_proxy:
            fwd = self.headers.get("X-Forwarded-For", "").split(",")[-1].strip()
            try:
                ip = str(ipaddress.ip_address(fwd))
            except ValueError:
                pass
        return ip

    def send(self, obj, code=200):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def do_GET(self):
        p, q = urlparse(self.path).path.rstrip("/") or "/", {k: v[0] for k, v in parse_qs(urlparse(self.path).query).items()}
        if p == "/health":
            return self.send({"ok": True, "service": "claudestick-telemetry", "version": VERSION})
        if p == "/stats":
            tok = self.headers.get("X-Token") or q.get("t", "")
            if not hmac.compare_digest(tok.encode("utf-8", "replace"), self.server.token.encode()):
                return self.send({"error": "bad token"}, 401)
            return self.send(self.server.store.stats())
        self.send({"error": "not found"}, 404)

    def do_POST(self):
        p = urlparse(self.path).path.rstrip("/")
        if p != "/report":
            return self.send({"error": "not found"}, 404)
        ip = self.client_ip()
        if not self.server.limiter.allow(ip):
            self.close_connection = True
            return self.send({"error": "too many reports from this address"}, 429)
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = -1
        if n < 0:
            self.close_connection = True
            return self.send({"error": "bad length"}, 400)
        if n > MAX_BODY:
            self.close_connection = True
            return self.send({"error": "too large"}, 413)
        raw = self.rfile.read(n) if n else b""
        try:
            body = json.loads(raw or b"{}")
        except ValueError:
            return self.send({"error": "bad json"}, 400)
        report = clean(body)
        if not report:
            return self.send({"error": "missing device_id/fw, or nothing recognized"}, 400)
        self.server.store.store(report, ip)
        self.send({"ok": True})


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, store, token, trust_proxy=False, verbose=False):
        self.store, self.token, self.trust_proxy, self.verbose = store, token, trust_proxy, verbose
        self.limiter = RateLimit()
        super().__init__(addr, Handler)

    def handle_error(self, request, client_address):
        # a dropped connection or a half-sent request is routine on a public endpoint: no stack trace
        e = sys.exc_info()[1]
        if self.verbose or not isinstance(e, (OSError, ssl.SSLError)):
            sys.stderr.write(f"claudestick-telemetry: {client_address[0]}: {type(e).__name__}: {e}\n")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=7895)
    p.add_argument("--data", default=os.environ.get("CLAUDESTICK_TELEMETRY_DIR", os.path.expanduser("~/.claudestick-telemetry")))
    p.add_argument("--trust-proxy", action="store_true", help="behind a reverse proxy: rate-limit by X-Forwarded-For")
    p.add_argument("--show-token", action="store_true", help="print the /stats token and exit")
    p.add_argument("--summary", action="store_true", help="print aggregate stats from the database and exit (no server)")
    p.add_argument("-v", "--verbose", action="store_true")
    a = p.parse_args(argv)

    store = Store(a.data)
    if a.summary:
        print(summary_text(store.stats()))
        return 0
    token = store.ident_token()
    if a.show_token:
        print(f"token: {token}\ncurl 'http://{lan_ip()}:{a.port}/stats?t={token}'")
        return 0
    srv = Server((a.host, a.port), store, token, a.trust_proxy, a.verbose)
    print(f"ClaudeStick telemetry collector {VERSION} on http://{a.host}:{a.port}  (data: {a.data})")
    print(f"stats: run with --show-token, or GET /stats?t=<token>")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
