#!/usr/bin/env python3
"""ClaudeStick bridge: the Stick's API, hosted on your own machine.

Run it on a computer that stays on at home. Your Claude Code hooks talk to it instead of to
the Stick, and the Stick (or your phone) connects to it from any network, so you can approve,
deny, talk to and stop your home sessions from work.

  python3 bridge/claudestick_bridge.py                   # listens on 0.0.0.0:7890
  python3 bridge/claudestick_bridge.py --show-token      # print the token and exit
  python3 bridge/claudestick_bridge.py --help

Put it behind a reverse proxy with HTTPS before exposing it to the internet (see docs/REMOTE.md).
Standard library only, Python 3.8+.
"""
import argparse
import hashlib
import hmac
import ipaddress
import json
import math
import os
import secrets
import socket
import ssl
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

VERSION = "0.9.3"

PRESENT_S = 30           # a client (Stick or phone) that polled this recently counts as "here"
STALE_S = 15             # the hook stopped polling a request: drop it
SESSION_IDLE_S = 30 * 60
MAX_SESSIONS, MAX_APPROVALS, MAX_STOPS, MAX_MACROS, MAX_LEDGER = 16, 32, 16, 10, 256
LITE_SESSIONS, LITE_APPROVALS, LITE_CHOICES, LITE_DETAIL = 6, 8, 4, 800   # what the Stick has room for
MAX_JSON = 256 * 1024
MAX_AUDIO = 4 * 1024 * 1024
MAX_DRAIN = 8 * 1024 * 1024   # most of a refused body we'll read before closing
MAX_CONNECTIONS = 64
FAIL_LIMIT, FAIL_WINDOW_S, LOCKOUT_S = 10, 60, 300
PAIR_TTL_S, PAIR_MAX_FAILS, PAIR_FAIL_LIMIT = 300, 3, 5   # a pairing code: 5 minutes, then 3 wrong tries kill it; 5 wrong tries lock an address

DEFAULT_MACROS = [
    {"label": "Continue", "text": "Continue."},
    {"label": "Keep going", "text": "Keep going until the task is fully done. Only stop if you are truly blocked."},
    {"label": "Next phase", "text": "That phase looks good. Proceed to the next phase of the plan."},
    {"label": "Run tests + fix", "text": "Run the test suite and fix any failures before continuing."},
    {"label": "Summarize", "text": "Summarize what you did, what's left, and any risks, in under 10 bullets."},
    {"label": "Commit", "text": "Commit the current work with a clear, conventional commit message."},
]
DEFAULT_CONFIG = {
    "permission_wait_s": 20, "remote_permission_wait_s": 600, "stop_wait_s": 600, "stop_menu": "remote",
    "questions_on_device": True, "remote": "on", "public_url": "", "ntfy_url": "", "ntfy_token": "",
    "stt_url": "", "stt_model": "whisper-1", "stt_language": "", "stt_key": "", "macros": DEFAULT_MACROS,
}
SECRETS = ("ntfy_token", "stt_key")
PUBLIC_PAGES = ("/", "/setup", "/health", "/install.py", "/claudestick_hook.py")


# ------------------------------------------------------------------ config

def _clamp(v, lo, hi):
    return max(lo, min(hi, v))


def clean_macros(raw):
    out = []
    for m in raw if isinstance(raw, list) else []:
        if isinstance(m, dict) and isinstance(m.get("label"), str) and isinstance(m.get("text"), str):
            label, text = m["label"].strip()[:28], m["text"].strip()[:1000]
            if label and text:
                out.append({"label": label, "text": text})
        if len(out) >= MAX_MACROS:
            break
    return out


def merge_config(conf, patch):
    """Apply the settings in `patch` that are valid; ignore the rest. Returns True if anything changed."""
    before = json.dumps(conf, sort_keys=True)
    for k in ("permission_wait_s", "remote_permission_wait_s", "stop_wait_s"):
        v = patch.get(k)
        if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v):
            conf[k] = _clamp(int(v), 1 if k == "permission_wait_s" else 5, 3600)
    if patch.get("stop_menu") in ("remote", "always", "never"):
        conf["stop_menu"] = patch["stop_menu"]
    if patch.get("remote") in ("off", "on", "auto"):
        conf["remote"] = patch["remote"]
    if isinstance(patch.get("questions_on_device"), bool):
        conf["questions_on_device"] = patch["questions_on_device"]
    for k in ("public_url", "ntfy_url", "ntfy_token", "stt_url", "stt_model", "stt_language", "stt_key"):
        if isinstance(patch.get(k), str):
            conf[k] = patch[k].strip()[:300]
    if isinstance(patch.get("macros"), list):
        conf["macros"] = clean_macros(patch["macros"]) or list(DEFAULT_MACROS)
    return json.dumps(conf, sort_keys=True) != before


def redact(conf):
    out = {k: v for k, v in conf.items() if k not in SECRETS}
    for k in SECRETS:
        out[k + "_set"] = bool(conf.get(k))
    return out


class Store:
    """The bridge's identity (token, id) and settings, kept in one directory (private to the user)."""

    def __init__(self, path):
        self.path = path
        os.makedirs(path, mode=0o700, exist_ok=True)
        self.ident = self._read("bridge.json") or {}
        self.config = json.loads(json.dumps(DEFAULT_CONFIG))
        merge_config(self.config, self._read("config.json") or {})

    def _read(self, name):
        try:
            with open(os.path.join(self.path, name)) as f:
                v = json.load(f)
            return v if isinstance(v, dict) else None
        except (OSError, ValueError):
            return None

    def _write(self, name, data):
        dst = os.path.join(self.path, name)
        tmp = dst + ".tmp"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            json.dump(data, f, indent=2)
            f.write("\n")
        os.replace(tmp, dst)

    def ensure_identity(self, rotate=False):
        """Create the token and device id on first run. Returns True if a new token was made."""
        made = False
        if rotate or not self.ident.get("token"):
            self.ident["token"] = secrets.token_hex(16)
            made = True
        if not self.ident.get("id"):
            self.ident["id"] = secrets.token_hex(6)
        if made or not os.path.exists(os.path.join(self.path, "bridge.json")):
            self._write("bridge.json", self.ident)
        return made

    def save_config(self):
        self._write("config.json", self.config)


# -------------------------------------------------------------------- core

class Core:
    """Everything the Stick does with sessions, approvals and Done cards, minus the hardware."""

    def __init__(self, store, mono=time.monotonic, wall=time.time):
        self.store, self.mono, self.wall = store, mono, wall
        self.lock = threading.RLock()
        self.sessions, self.approvals, self.stops = {}, {}, {}
        self.ledger, self.cur_day = {}, ""
        self.limits, self.limits_at = {}, 0.0
        self.pinned = ""
        self.next_id = 1_000_000          # far from the Stick's own ids, so both can run side by side
        self.last_stick = self.last_kbd = 0.0
        self.clients = {}                 # who polled /state, and when
        self.v, self._key = 0, ""
        self.hook_sha = ""
        self.pair = None                  # the one active pairing code: {"code", "exp", "fails"}

    @property
    def conf(self):
        return self.store.config

    # -- helpers
    def _nid(self):
        self.next_id += 1
        return self.next_id - 1

    def _session(self, body):
        sid = str(body.get("session") or "unknown")[:128]
        s = self.sessions.get(sid)
        if s is None:
            if len(self.sessions) >= MAX_SESSIONS:   # make room: drop the quietest one
                oldest = min(self.sessions, key=lambda k: self.sessions[k]["seen"])
                del self.sessions[oldest]
            s = self.sessions[sid] = {"project": sid[:8], "state": "", "detail": "", "since": self.mono(),
                                      "stop": False, "inject": "", "plan_done_n": 0}
        if isinstance(body.get("project"), str) and body["project"]:
            s["project"] = body["project"][:60]
        s["seen"] = self.mono()
        return sid, s

    def present(self):
        now = self.mono()
        return any(now - t < PRESENT_S for t in self.clients.values())

    def remote_now(self):
        r = self.conf["remote"]
        want = r == "on" or (r == "auto" and self.last_stick > self.last_kbd)
        return bool(want and self.present())

    def _note_stick(self):
        self.last_stick = self.mono()

    def _note_kbd(self):
        self.last_kbd = self.mono()

    def _expire(self):
        now = self.mono()
        for table in (self.approvals, self.stops):
            for k in [k for k, r in table.items() if now - r["poll"] > STALE_S]:
                del table[k]
        for k in [k for k, s in self.sessions.items() if now - s["seen"] > SESSION_IDLE_S]:
            del self.sessions[k]
        if self.pinned and self.pinned not in self.sessions:
            self.pinned = ""
        for k in [k for k, t in self.clients.items() if now - t > 3600]:
            del self.clients[k]

    def _text_to(self, sid, text):
        """Answer the session's open Done/Question card with `text`, or queue it for its next hook call."""
        for r in self.stops.values():
            if r["result"] is None and r["session"] == sid:
                r["custom"], r["result"] = text, "custom"
                return True
        if sid in self.sessions:
            self.sessions[sid]["inject"] = text
            return True
        return False

    # -- what hooks send
    def event(self, b):
        with self.lock:
            sid, s = self._session(b)
            self._extras(sid, s, b)
            if b.get("typed"):
                self._note_kbd()
            ev = str(b.get("event", ""))
            if ev == "end":
                self.sessions.pop(sid, None)
                if self.pinned == sid:
                    self.pinned = ""
                return {"stop": False, "inject": None, "remote": self.remote_now()}
            if s["state"] != ev:
                s["since"] = self.mono()
            s["state"], s["detail"] = ev[:24], str(b.get("detail", ""))[:400]
            out = {"stop": False, "inject": None, "remote": self.remote_now()}
            if b.get("can_stop") and s["stop"]:
                out["stop"], s["stop"] = True, False
            elif b.get("can_inject") and s["inject"]:
                out["inject"], s["inject"] = s["inject"], ""
            return out

    def _extras(self, sid, s, b):
        if b.get("plan_done"):
            s["plan_done_n"] += 1
        t = b.get("todo")
        if isinstance(t, dict):
            items = [[str(i[0])[:1], str(i[1])[:120]] for i in (t.get("items") or [])[:12]
                     if isinstance(i, list) and len(i) >= 2]
            s["todo"] = {"done": int(t.get("done") or 0), "total": int(t.get("total") or 0),
                         "skip": int(t.get("skip") or 0), "now": str(t.get("now") or "")[:120], "items": items}
        u = b.get("usage")
        if isinstance(u, dict):
            s["usage"] = {k: int(u.get(k) or 0) for k in ("in", "out", "cr", "cw", "ctx")}
            s["usage"]["model"] = str(u.get("model") or "")[:40]
            day = u.get("day")
            if isinstance(day, str) and day:
                self.cur_day = max(self.cur_day, day)
                self.ledger[(sid, day)] = int(u.get("today") or 0)
                if len(self.ledger) > MAX_LEDGER:   # forget the oldest days first
                    for k in sorted(self.ledger, key=lambda k: k[1])[:len(self.ledger) - MAX_LEDGER]:
                        del self.ledger[k]

    def usage(self, b):
        with self.lock:
            s = self.sessions.get(str(b.get("session") or ""))   # never creates a session
            if s is not None:
                for k in ("ctx_pct", "cost"):
                    v = b.get(k)
                    if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v):
                        s[k] = v
                if isinstance(b.get("model"), str):
                    s["model_name"] = b["model"][:40]
            for k in ("five", "seven"):
                w = b.get(k)
                if isinstance(w, dict) and isinstance(w.get("pct"), (int, float)) and math.isfinite(w["pct"]):
                    self.limits[k] = {"pct": w["pct"], "resets": int(w.get("resets") or 0)}
                    self.limits_at = self.mono()
            return {"ok": True}

    def permission_new(self, b):
        with self.lock:
            if len(self.approvals) >= MAX_APPROVALS:
                return 503, {"error": "queue full"}
            sid, s = self._session(b)
            s["state"], s["since"] = "approve", self.mono()
            risk = str(b.get("risk") or "")
            i = self._nid()
            self.approvals[i] = {
                "tool": str(b.get("tool") or "tool")[:40], "detail": str(b.get("detail") or "")[:1500],
                "project": s["project"], "session": sid, "created": self.mono(), "poll": self.mono(),
                "always": "" if risk == "high" else str(b.get("always") or "")[:200],
                "risk": risk[:8], "risk_why": str(b.get("risk_why") or "")[:120], "reason": "",
                "decision": "skip" if self.pinned and self.pinned != sid else "pending"}
            return 200, {"id": i, "remote": self.remote_now()}

    def permission_poll(self, i, delete=False):
        with self.lock:
            a = self.approvals.get(i)
            if a is None:
                return {"state": "gone"}
            a["poll"] = self.mono()
            if delete:
                del self.approvals[i]
                return {"state": "cancelled"}
            if a["decision"] == "pending":
                return {"state": "pending"}
            del self.approvals[i]
            out = {"state": a["decision"]}
            if a["decision"] == "deny" and a["reason"]:
                out["reason"] = a["reason"]
            return out

    def stop_new(self, b):
        with self.lock:
            if len(self.stops) >= MAX_STOPS:
                return 503, {"error": "queue full"}
            sid, s = self._session(b)
            kind = "question" if b.get("kind") == "question" else "done"
            s["state"], s["since"] = kind, self.mono()
            if isinstance(b.get("macros"), list):
                opts = clean_macros(b["macros"])
            else:   # Claude's own options first (remote mode), then your macros
                opts = clean_macros(b.get("options")) + ([] if kind == "question" else clean_macros(self.conf["macros"]))
            i = self._nid()
            self.stops[i] = {"kind": kind, "session": sid, "project": s["project"], "macros": opts[:MAX_MACROS + 4],
                             "title": str(b.get("title") or "")[:300], "last": str(b.get("last") or "")[:600],
                             "created": self.mono(), "poll": self.mono(), "result": None, "custom": ""}
            if self.pinned and self.pinned != sid:
                self.stops[i]["result"] = "dismiss"
            return 200, {"id": i, "remote": self.remote_now()}

    def stop_poll(self, i, delete=False):
        with self.lock:
            r = self.stops.get(i)
            if r is None:
                return {"state": "gone"}
            r["poll"] = self.mono()
            if delete:
                del self.stops[i]
                return {"state": "cancelled"}
            if r["result"] is None:
                return {"state": "pending"}
            del self.stops[i]
            if r["result"] == "dismiss":
                return {"state": "dismiss"}
            if r["result"] == "custom":
                return {"state": "custom", "text": r["custom"]}
            return {"state": "macro", "text": r["macros"][r["result"]]["text"]}

    # -- what the Stick and the phone page send
    def act(self, b):
        a, rid = b.get("action"), b.get("id")
        idx = b.get("index") if isinstance(b.get("index"), int) else -1
        with self.lock:
            if a not in ("remote", "pin"):
                self._note_stick()
            if a in ("approve", "deny", "keyboard", "always"):
                ap = self.approvals.get(rid)
                if not ap or ap["decision"] != "pending":
                    return False
                d = {"approve": "allow", "deny": "deny", "keyboard": "skip", "always": "always"}[a]
                if d == "always" and not ap["always"]:
                    d = "allow"
                if d == "skip":
                    self._note_kbd()
                if d == "deny" and isinstance(b.get("text"), str) and b["text"]:
                    ap["reason"] = b["text"][:500]
                ap["decision"], ap["poll"] = d, self.mono()   # the hook has STALE_S to pick it up
                return True
            if a in ("choose", "dismiss"):
                r = self.stops.get(rid)
                if not r or r["result"] is not None:
                    return False
                if a == "dismiss":
                    r["result"] = "dismiss"
                elif 0 <= idx < len(r["macros"]):
                    r["result"] = idx
                else:
                    return False
                r["poll"] = self.mono()
                return True
            if a in ("say", "macro", "stop"):
                sid = b.get("session") or max(self.sessions, key=lambda k: self.sessions[k]["seen"], default=None)
                if sid not in self.sessions:
                    return False
                if a == "stop":
                    self.sessions[sid]["stop"] = True
                    return True
                macros = self.conf["macros"]
                text = str(b.get("text") or "")[:2000] if a == "say" else (macros[idx]["text"] if 0 <= idx < len(macros) else "")
                return bool(text.strip()) and self._text_to(sid, text)
            if a == "pin":
                self.pinned = b.get("session") if b.get("session") in self.sessions else ""
                return True
            if a == "remote":
                order = ["off", "on", "auto"]
                want = b.get("setting")
                self.conf["remote"] = want if want in order else order[(order.index(self.conf["remote"]) + 1) % 3]
                self.store.save_config()
                return True
        return False

    def poll(self, client, want_v=None, lite=False):
        """GET /state. With ?v=N the answer is tiny when nothing changed since version N.
        `lite` is the Stick's view: it only has room for a few sessions, prompts and cards."""
        with self.lock:
            self._expire()
            if client:
                self.clients[client] = self.mono()
            snap = self._snapshot()
            key = json.dumps(snap, sort_keys=True, separators=(",", ":"))
            if key != self._key:
                self.v, self._key = self.v + 1, key
            out = {"v": self.v, "now": int(self.wall())}
            if want_v is not None and want_v == self.v:
                out["same"] = True
                return out
            now = self.mono()
            snap = json.loads(key)   # a copy we can decorate
            for s in snap["sessions"]:
                s["since_s"] = int(now - self.sessions[s["id"]]["since"])
            if "limits" in snap:
                snap["limits"]["age_s"] = int(now - self.limits_at)
            for a in snap["approvals"]:
                a["age_s"] = int(now - self.approvals[a["id"]]["created"])
            if lite:
                snap["sessions"] = snap["sessions"][-LITE_SESSIONS:]
                snap["approvals"] = snap["approvals"][:LITE_APPROVALS]
                snap["choices"] = snap["choices"][:LITE_CHOICES]
                for a in snap["approvals"]:
                    a["detail"] = a["detail"][:LITE_DETAIL]
                for c in snap["choices"]:
                    c["last"], c["title"] = c["last"][:LITE_DETAIL], c["title"][:LITE_DETAIL]
            out.update(snap)
            return out

    def _snapshot(self):
        sessions = []
        for sid, s in self.sessions.items():
            o = {"id": sid, "project": s["project"], "state": s["state"], "detail": s["detail"],
                 "plan_done_n": s["plan_done_n"]}
            if s.get("todo", {}).get("total"):
                o["todo"] = s["todo"]
            if s.get("usage"):
                o["usage"] = s["usage"]
            if "ctx_pct" in s:
                o["ctx_pct"] = s["ctx_pct"]
            if s.get("model_name"):
                o["model_name"] = s["model_name"]
            sessions.append(o)
        snap = {
            "bridge": 1, "remote": self.remote_now(), "remote_setting": self.conf["remote"], "pinned": self.pinned,
            "sessions": sessions,
            "tokens_today": sum(t for (_, day), t in self.ledger.items() if day == self.cur_day),
            "approvals": [{"id": i, "tool": a["tool"], "detail": a["detail"], "project": a["project"],
                           "session": a["session"], "src": "wifi", "always": a["always"], "risk": a["risk"],
                           "risk_why": a["risk_why"]}
                          for i, a in sorted(self.approvals.items()) if a["decision"] == "pending"],
            "choices": [{"id": i, "kind": r["kind"], "project": r["project"], "session": r["session"],
                         "title": r["title"], "last": r["last"], "options": [m["label"] for m in r["macros"]]}
                        for i, r in sorted(self.stops.items()) if r["result"] is None],
            "macros": [m["label"] for m in self.conf["macros"]],
        }
        if self.limits:
            snap["limits"] = {k: dict(v) for k, v in self.limits.items()}
        return snap

    # -- pairing codes: a short one-time code that a client trades for the real token
    def pair_new(self):
        """Make a code (replacing any earlier one). Only an authenticated caller gets here, so a code
        can only exist because the owner asked for it, and an attacker never gets to guess against one
        they didn't see created: each code allows PAIR_MAX_FAILS wrong tries in total, then it's gone."""
        with self.lock:
            code = f"{secrets.randbelow(10 ** 6):06d}"
            self.pair = {"code": code, "exp": self.mono() + PAIR_TTL_S, "fails": 0}
            return {"code": code, "expires_in": PAIR_TTL_S}

    def pair_claim(self, raw):
        """The real token for the right, unexpired code (which is then spent); otherwise None."""
        digits = "".join(c for c in raw if c.isdigit()) if isinstance(raw, str) else ""
        with self.lock:
            p = self.pair
            if p and self.mono() > p["exp"]:
                p = self.pair = None
            if not p:
                return None
            if len(digits) == 6 and hmac.compare_digest(digits.encode(), p["code"].encode()):
                self.pair = None
                return self.store.ident["token"]
            p["fails"] += 1
            if p["fails"] >= PAIR_MAX_FAILS:
                self.pair = None
            return None

    # -- settings
    def config_get(self, secrets_too=False):
        with self.lock:
            return json.loads(json.dumps(self.conf if secrets_too else redact(self.conf)))

    def config_post(self, patch, secrets_too=False):
        with self.lock:
            if merge_config(self.conf, patch):
                self.store.save_config()
            return self.config_get(secrets_too)


# ------------------------------------------------------------ rate limiting

class FailGuard:
    """A lockout for addresses that keep getting something wrong (`limit` misses in `window` s -> `lockout` s)."""

    def __init__(self, limit=FAIL_LIMIT, window=FAIL_WINDOW_S, lockout=LOCKOUT_S, mono=time.monotonic):
        self.limit, self.window, self.lockout, self.mono = limit, window, lockout, mono
        self.fails = {}   # ip -> [count, window start, locked until]
        self.lock = threading.Lock()

    def locked(self, ip):
        with self.lock:
            f = self.fails.get(ip)
            return max(0, int(f[2] - self.mono())) + 1 if f and f[2] > self.mono() else 0

    def fail(self, ip):
        """Count a miss. Returns True when this address just got locked out."""
        now = self.mono()
        with self.lock:
            if len(self.fails) > 2048:
                self.fails = {k: v for k, v in self.fails.items() if v[2] > now or now - v[1] < self.window}
            f = self.fails.setdefault(ip, [0, now, 0.0])
            if now - f[1] > self.window:
                f[0], f[1] = 0, now
            f[0] += 1
            if f[0] >= self.limit:
                f[0], f[2] = 0, now + self.lockout
                return True
            return False


class AuthGuard(FailGuard):
    """Constant-time token check, plus the lockout."""

    def __init__(self, token, mono=time.monotonic):
        super().__init__(mono=mono)
        self.token = token.encode()

    def ok(self, supplied):
        return bool(supplied) and hmac.compare_digest(supplied.encode("utf-8", "replace"), self.token)


# --------------------------------------------------------------------- HTTP

def find_asset(*candidates):
    here = os.path.dirname(os.path.abspath(__file__))
    for c in candidates:
        for base in (os.path.join(here, ".."), here):
            p = os.path.normpath(os.path.join(base, c))
            if os.path.isfile(p):
                return p
    return None


def read_asset(*candidates):
    p = find_asset(*candidates)
    if not p:
        return None
    with open(p, "rb") as f:
        return f.read()


def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))   # no packets are sent
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


def multipart(wav, conf):
    b = "----claudestick" + uuid.uuid4().hex
    parts = [f'--{b}\r\nContent-Disposition: form-data; name="file"; filename="speech.wav"\r\n'
             f'Content-Type: audio/wav\r\n\r\n'.encode(), wav,
             f'\r\n--{b}\r\nContent-Disposition: form-data; name="model"\r\n\r\n{conf["stt_model"] or "whisper-1"}'.encode(),
             f'\r\n--{b}\r\nContent-Disposition: form-data; name="response_format"\r\n\r\njson'.encode()]
    if conf["stt_language"]:
        parts.append(f'\r\n--{b}\r\nContent-Disposition: form-data; name="language"\r\n\r\n{conf["stt_language"]}'.encode())
    parts.append(f"\r\n--{b}--\r\n".encode())
    return b"".join(parts), "multipart/form-data; boundary=" + b


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 64
    allow_reuse_address = True

    def __init__(self, addr, handler, core, guard, trust_proxy=False, tls=None, verbose=False, pair_guard=None):
        self.core, self.guard, self.trust_proxy, self.tls, self.verbose = core, guard, trust_proxy, tls, verbose
        self.pair_guard = pair_guard or FailGuard(PAIR_FAIL_LIMIT, FAIL_WINDOW_S, LOCKOUT_S, mono=guard.mono)
        self.slots = threading.BoundedSemaphore(MAX_CONNECTIONS)
        super().__init__(addr, handler)

    def get_request(self):
        sock, addr = super().get_request()
        if self.tls:   # handshake happens on first read, in the connection's own thread
            sock = self.tls.wrap_socket(sock, server_side=True, do_handshake_on_connect=False)
        return sock, addr

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):   # too many open connections: refuse this one
            self.shutdown_request(request)
            return
        super().process_request(request, client_address)

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()

    def handle_error(self, request, client_address):
        # scanners, dropped phones and failed TLS handshakes are routine: no stack traces
        e = sys.exc_info()[1]
        if self.verbose or not isinstance(e, (OSError, ssl.SSLError)):
            sys.stderr.write(f"claudestick-bridge: {client_address[0]}: {type(e).__name__}: {e}\n")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"   # keep-alive: the Stick polls every couple of seconds
    timeout = 65
    server_version = "ClaudeStickBridge"
    sys_version = ""

    def log_message(self, fmt, *args):   # never log the query string: it can carry the token
        if self.server.verbose:
            sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    # -- plumbing
    def client_ip(self):
        ip = self.client_address[0]
        if self.server.trust_proxy:
            fwd = self.headers.get("X-Forwarded-For", "").split(",")[-1].strip()   # the entry our proxy added
            try:
                ip = str(ipaddress.ip_address(fwd))
            except ValueError:
                pass
        return ip

    def query(self):
        return {k: v[0] for k, v in parse_qs(urlparse(self.path).query).items()}

    def send(self, obj, code=200, ctype="application/json", extra=()):
        data = obj if isinstance(obj, bytes) else obj.encode() if isinstance(obj, str) else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Referrer-Policy", "no-referrer")   # the page URL can carry the token
        self.send_header("X-Content-Type-Options", "nosniff")
        if self.close_connection:
            self.send_header("Connection", "close")
        if ctype.startswith("text/html"):
            self.send_header("X-Frame-Options", "DENY")
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def authed(self):
        ip = self.client_ip()
        wait = self.server.guard.locked(ip)
        if wait:
            self.close_connection = True
            self.send({"error": "too many failed attempts"}, 429, extra=[("Retry-After", str(wait))])
            return False
        if self.server.guard.ok(self.headers.get("X-Token") or self.query().get("t", "")):
            return True
        if self.server.guard.fail(ip):
            sys.stderr.write(f"claudestick-bridge: {ip} locked out for {LOCKOUT_S}s after repeated bad tokens\n")
        self.close_connection = True   # the body (if any) was not read
        self.send({"error": "bad token"}, 401)
        return False

    def discard(self, n):
        """Read and drop (some of) a body we refused, so the client can finish sending and read our answer:
        closing with unread data resets the connection and the client may never see the 413."""
        deadline = time.monotonic() + 10
        left = min(n, MAX_DRAIN)
        try:
            self.connection.settimeout(5)
            while left > 0 and time.monotonic() < deadline:
                chunk = self.rfile.read(min(65536, left))
                if not chunk:
                    break
                left -= len(chunk)
        except OSError:
            pass

    def body(self, limit=MAX_JSON):
        """The request body, or None after sending an error."""
        if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
            self.close_connection = True
            self.send({"error": "chunked bodies are not supported"}, 411)
            return None
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = -1
        if n < 0:
            self.close_connection = True
            self.send({"error": "bad length"}, 400)
            return None
        if n > limit:
            self.close_connection = True
            self.send({"error": "too large"}, 413)
            self.discard(n)
            return None
        return self.rfile.read(n) if n else b""

    def json_body(self, limit=MAX_JSON):
        raw = self.body(limit)
        if raw is None:
            return None
        try:
            v = json.loads(raw or b"{}")
            if isinstance(v, dict):
                return v
        except ValueError:
            pass
        self.send({"error": "bad json"}, 400)
        return None

    # -- GET
    def do_GET(self):
        p, q = urlparse(self.path).path.rstrip("/") or "/", self.query()
        core = self.server.core
        if p == "/health":
            hook = read_asset("hooks/claudestick_hook.py", "claudestick_hook.py")
            return self.send({"ok": True, "fw": VERSION, "bridge": True, "name": "ClaudeStick bridge",
                              "id": core.store.ident["id"], "host": "", "auth": True, "pair": True,
                              "hook": hashlib.sha256(hook).hexdigest() if hook else ""})
        if p in ("/", "/setup", "/install.py", "/claudestick_hook.py"):
            name = {"/": ("firmware/phone_page.html", "phone_page.html", "text/html; charset=utf-8"),
                    "/setup": ("firmware/setup_page.html", "setup_page.html", "text/html; charset=utf-8"),
                    "/install.py": ("install.py", "install.py", "text/x-python"),
                    "/claudestick_hook.py": ("hooks/claudestick_hook.py", "claudestick_hook.py", "text/x-python")}[p]
            data = read_asset(name[0], name[1])
            return self.send(data, ctype=name[2]) if data else self.send({"error": "missing file"}, 404)
        if not self.authed():
            return
        if p == "/state":
            v = q.get("v")
            client = "".join(c for c in (self.headers.get("X-Client") or "phone") if c.isalnum())[:12] + \
                "@" + self.client_ip()
            return self.send(core.poll(client, int(v) if v and v.lstrip("-").isdigit() else None, q.get("n") == "1"))
        if p == "/config":
            return self.send(core.config_get(q.get("secrets") == "1"))
        if p == "/setup/info":
            c = core.config_get()
            return self.send({"done": True, "bridge": True, "ap": False, "fw": VERSION, "port": self.server.server_port,
                              "ip": lan_ip(), "public_url": c.get("public_url", ""), "token": self.server.guard.token.decode(),
                              "wifi": {"connected": True, "ssid": "", "ip": lan_ip()}, "remote": c["remote"], "buddies": []})
        if p in ("/permission", "/stop"):
            i = int(q["id"]) if q.get("id", "").isdigit() else 0
            return self.send(core.permission_poll(i) if p == "/permission" else core.stop_poll(i))
        self.send({"error": "not found"}, 404)

    def do_DELETE(self):
        p, q = urlparse(self.path).path.rstrip("/"), self.query()
        if not self.authed():
            return
        i = int(q["id"]) if q.get("id", "").isdigit() else 0
        if p == "/permission":
            return self.send(self.server.core.permission_poll(i, delete=True))
        if p == "/stop":
            return self.send(self.server.core.stop_poll(i, delete=True))
        self.send({"error": "not found"}, 404)

    # -- POST
    def do_POST(self):
        p, q = urlparse(self.path).path.rstrip("/"), self.query()
        core = self.server.core
        if p == "/pair":   # the one POST that takes no token: a one-time code stands in for it
            return self.pair()
        if not self.authed():
            return
        if p == "/stt":
            return self.stt()
        b = self.json_body()
        if b is None:
            return
        if p == "/pair/new":
            return self.send(core.pair_new())
        if p == "/act":
            if not b:   # query-string form, used by ntfy notification buttons
                b = {"action": q.get("action"), "session": q.get("session")}
                if q.get("id", "").isdigit():
                    b["id"] = int(q["id"])
            ok = core.act(b)
            return self.send({"ok": ok}, 200 if ok else 404)
        if p == "/config":
            return self.send(core.config_post(b, q.get("secrets") == "1"))
        routes = {"/event": core.event, "/usage": core.usage}
        if p in routes:
            return self.send(routes[p](b))
        if p in ("/permission", "/stop"):
            code, out = (core.permission_new if p == "/permission" else core.stop_new)(b)
            return self.send(out, code)
        self.send({"error": "not found"}, 404)

    def pair(self):
        """Trade a pairing code for the token. Wrong, expired and absent codes all look the same."""
        ip, guard = self.client_ip(), self.server.pair_guard
        wait = guard.locked(ip)
        if wait:
            self.close_connection = True
            return self.send({"error": "too many wrong codes"}, 429, extra=[("Retry-After", str(wait))])
        b = self.json_body(1024)
        if b is None:
            return
        token = self.server.core.pair_claim(b.get("code"))
        if token is None:
            if guard.fail(ip):
                sys.stderr.write(f"claudestick-bridge: {ip} locked out of pairing for {LOCKOUT_S}s after wrong codes\n")
            return self.send({"error": "wrong or expired pairing code"}, 403)
        sys.stderr.write(f"claudestick-bridge: paired a device from {ip}\n")
        self.send({"ok": True, "token": token, "id": self.server.core.store.ident["id"]})

    def stt(self):
        """Speech to text for the Stick: a WAV in, {"text": ...} out, using the server set in the bridge's settings."""
        conf = self.server.core.config_get(True)
        wav = self.body(MAX_AUDIO)
        if wav is None:
            return
        if not conf["stt_url"]:
            return self.send({"error": "voice isn't set up on the bridge: add a transcription URL in its settings"}, 503)
        if len(wav) < 44:
            return self.send({"error": "no audio"}, 400)
        data, ctype = multipart(wav, conf)
        req = urllib.request.Request(conf["stt_url"], data=data, method="POST", headers={"Content-Type": ctype})
        if conf["stt_key"]:
            req.add_header("Authorization", "Bearer " + conf["stt_key"])
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                text = json.loads(r.read(1 << 20) or b"{}").get("text", "")
        except urllib.error.HTTPError as e:
            return self.send({"error": f"speech server answered {e.code}"}, 502)
        except (OSError, ValueError) as e:
            return self.send({"error": f"speech server: {type(e).__name__}"}, 502)
        self.send({"text": str(text).strip()})


# --------------------------------------------------------------------- main

def make_server(store, host="0.0.0.0", port=7890, trust_proxy=False, tls_cert=None, tls_key=None, verbose=False,
                core=None, guard=None):
    ctx = None
    if tls_cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.load_cert_chain(tls_cert, tls_key)
    core = core or Core(store)
    guard = guard or AuthGuard(store.ident["token"])
    return Server((host, port), Handler, core, guard, trust_proxy, ctx, verbose)


def pair_command(store, port, ip):
    """Ask the bridge that's running on this machine for a pairing code (it needs the token, which we hold)."""
    body = json.dumps({}).encode()
    headers = {"X-Token": store.ident["token"], "Content-Type": "application/json"}
    for scheme, handler in (("http", urllib.request.HTTPHandler()),
                            ("https", urllib.request.HTTPSHandler(context=ssl._create_unverified_context()))):   # loopback only
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), handler)
        try:
            with opener.open(urllib.request.Request(f"{scheme}://127.0.0.1:{port}/pair/new", data=body, headers=headers), timeout=5) as r:
                out = json.loads(r.read())
        except urllib.error.HTTPError as e:
            print(f"the bridge refused ({e.code}); is this the data directory it is using? (--data)", file=sys.stderr)
            return 1
        except (OSError, ValueError):
            continue
        code = out["code"]
        print(f"pairing code: {code[:3]} {code[3:]}   (single use, valid {out['expires_in'] // 60} minutes, dies after {PAIR_MAX_FAILS} wrong tries)\n"
              f"  Stick:    its setup page -> remote access -> pairing code (with the bridge's address)\n"
              f"  phone:    open the bridge's address and enter it\n"
              f"  computer: curl -s http://{ip}:{port}/install.py | python3 - --host {ip} --port {port} --pair {code}")
        return 0
    print(f"no bridge answering on port {port}: start it first (or pass --port)", file=sys.stderr)
    return 1


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="0.0.0.0", help="address to listen on (default 0.0.0.0)")
    p.add_argument("--port", type=int, default=7890, help="port (default 7890)")
    p.add_argument("--data", default=os.environ.get("CLAUDESTICK_BRIDGE_DIR", os.path.expanduser("~/.claudestick-bridge")),
                   help="where the token and settings live (default ~/.claudestick-bridge)")
    p.add_argument("--trust-proxy", action="store_true",
                   help="behind a reverse proxy: use the client address it puts in X-Forwarded-For (only when it is the only way in)")
    p.add_argument("--tls-cert", help="serve HTTPS directly with this certificate (PEM; or use a reverse proxy)")
    p.add_argument("--tls-key", help="private key for --tls-cert")
    p.add_argument("--show-token", action="store_true", help="print the token and the install command, then exit")
    p.add_argument("--rotate-token", action="store_true", help="make a new token (every client must be set up again)")
    p.add_argument("--pair", action="store_true",
                   help="ask the running bridge for a one-time 6-digit pairing code (5 minutes) and print how to use it, then exit")
    p.add_argument("-v", "--verbose", action="store_true", help="log every request (never the token)")
    a = p.parse_args(argv)

    store = Store(a.data)
    made = store.ensure_identity(rotate=a.rotate_token)
    token = store.ident["token"]
    ip = lan_ip()
    install = f"curl -s http://{ip}:{a.port}/install.py | python3 - --host {ip} --port {a.port} --token {token}"
    if a.show_token:
        print(f"token: {token}\n{install}")
        return 0
    if a.pair:
        return pair_command(store, a.port, ip)
    if bool(a.tls_cert) != bool(a.tls_key):
        p.error("--tls-cert and --tls-key go together")
    srv = make_server(store, a.host, a.port, a.trust_proxy, a.tls_cert, a.tls_key, a.verbose)
    scheme = "https" if a.tls_cert else "http"
    print(f"ClaudeStick bridge {VERSION} on {scheme}://{a.host}:{a.port}  (data: {a.data})")
    if made:
        print(f"\nfirst run: your token is\n  {token}\nset up Claude Code on a machine with:\n  {install}\n"
              f"then point the Stick (or your phone) at this bridge. See docs/REMOTE.md.")
    else:
        print("token: run with --show-token to print it again")
    print("no typing the token: run with --pair (while this is running) for a one-time 6-digit code")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
