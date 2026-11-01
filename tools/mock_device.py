#!/usr/bin/env python3
"""Simulated ClaudeStick: same HTTP API as the v0.5 firmware, no hardware needed.

  python3 tools/mock_device.py --token test123 [--port 7880]

Pages: phone remote http://localhost:<port>/?t=<token>, setup/settings http://localhost:<port>/setup
Type at the prompt to press "buttons":
  a      approve top card / send selected option      A  always-allow top card
  n TXT  deny top card and tell Claude why (hold-A voice on the Stick)
  b      deny top card / next option                  k  answer at keyboard
  d      dismiss the Done/Question card               s  stop the latest session
  m N    queue macro N for the latest session         t TEXT  send free text (like voice/phone)
  r      cycle remote setting (off/on/auto)           ls show state
"""
import argparse
import hashlib
import json
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

LOCK = threading.Lock()
TOKEN = ""
_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.join(_HERE, "..")
try:
    INDEX_HTML = open(os.path.join(_REPO, "firmware/phone_page.html")).read()
except OSError:
    INDEX_HTML = "<p>phone page not found</p>"
try:
    SETUP_HTML = open(os.path.join(_REPO, "firmware/setup_page.html")).read()
except OSError:
    SETUP_HTML = "<p>setup page not found</p>"

BUDDIES = ["capybara", "duck", "goose", "blob", "cat", "dragon", "octopus", "owl", "penguin", "turtle", "snail",
           "ghost", "axolotl", "cactus", "robot", "rabbit", "mushroom", "chonk"]
DEFAULT_MACROS = [
    {"label": "Continue", "text": "Continue."},
    {"label": "Keep going", "text": "Keep going until the task is fully done. Only stop if you are truly blocked."},
    {"label": "Next phase", "text": "That phase looks good. Proceed to the next phase of the plan."},
    {"label": "Run tests + fix", "text": "Run the test suite and fix any failures before continuing."},
    {"label": "Summarize", "text": "Summarize what you did, what's left, and any risks, in under 10 bullets."},
    {"label": "Commit", "text": "Commit the current work with a clear, conventional commit message."},
]
CONF = {"permission_wait_s": 20, "remote_permission_wait_s": 600, "stop_wait_s": 600, "stop_menu": "remote",
        "questions_on_device": True, "remote": "auto", "ntfy_url": "", "ntfy_token": "", "stt_url": "",
        "stt_model": "whisper-1", "stt_key": "", "stt_language": "", "update_url": "", "telemetry": False,
        "telemetry_url": "", "macros": list(DEFAULT_MACROS)}
S = {"sessions": {}, "approvals": {}, "stops": {}, "next": 1, "log": [], "pinned": "",
     "last_stick": 0.0, "last_kbd": 0.0, "ota_bytes": 0}
SETUP = {"nets": [], "bridge": {"url": "", "tok": "", "up": False, "error": ""}, "done": False, "ap": False, "ssid": "", "connected": True, "ip": "127.0.0.1", "name": "Claude-MOCK",
         "buddy": 0, "volume": 120, "tilt": True, "wake": True, "bt_on": True, "tz": "", "tz_name": "", "clock24": False, "font": 1}
DEVICE_ID, MDNS_HOST = "a1b2c3d4e5f6", "claudestick-e5f6.local"


def remote_now():
    r = CONF["remote"]
    return r == "on" or (r == "auto" and S["last_stick"] > S["last_kbd"])


def note_stick():
    S["last_stick"] = time.time()


def note_kbd():
    S["last_kbd"] = time.time()


def nid():
    S["next"] += 1
    return S["next"] - 1


def session(body):
    sid = body.get("session", "unknown")
    s = S["sessions"].setdefault(sid, {"project": sid[:8], "state": "", "detail": "", "stop": False, "inject": "", "urgent": False})
    if body.get("project"):
        s["project"] = body["project"]
    s["seen"] = time.time()
    return sid, s


def deliver(sid, text):
    pend = [r for r in S["stops"].values() if r["result"] is None and r["session"] == sid]
    if pend:
        pend[0]["custom"], pend[0]["result"] = text, "custom"
    elif sid in S["sessions"]:
        S["sessions"][sid]["inject"], S["sessions"][sid]["urgent"] = text, False
    else:
        return False
    return True


def act(b):
    """Same semantics as POST /act in the firmware."""
    a, rid, idx = b.get("action"), b.get("id"), b.get("index", -1)
    with LOCK:
        if a not in ("remote", "pin"):
            note_stick()
        if a in ("approve", "deny", "keyboard", "always"):
            ap = S["approvals"].get(rid)
            if not ap or ap["decision"] != "pending":
                return False
            d = {"approve": "allow", "deny": "deny", "keyboard": "skip", "always": "always"}[a]
            if d == "always" and not ap.get("always"):
                d = "allow"
            if d == "skip":
                note_kbd()
            if d == "deny" and b.get("text"):
                ap["reason"] = b["text"]
            ap["decision"] = d
            return True
        if a in ("choose", "dismiss"):
            r = S["stops"].get(rid)
            if not r or r["result"] is not None:
                return False
            if a == "dismiss":
                r["result"] = "dismiss"
            elif 0 <= idx < len(r["macros"]):
                r["result"] = idx
            else:
                return False
            return True
        if a in ("say", "macro", "stop", "unsend", "sendnow"):
            sid = b.get("session") or max(S["sessions"], key=lambda k: S["sessions"][k].get("seen", 0), default=None)
            if sid not in S["sessions"]:
                return False
            if a == "stop":
                S["sessions"][sid]["stop"] = True
                return True
            if a == "unsend":
                S["sessions"][sid]["inject"], S["sessions"][sid]["urgent"] = "", False
                return True
            if a == "sendnow":
                if not S["sessions"][sid].get("inject"):
                    return False
                S["sessions"][sid]["urgent"] = True
                return True
            m = CONF["macros"]
            text = b.get("text", "") if a == "say" else (m[idx]["text"] if 0 <= idx < len(m) else "")
            return bool(text) and deliver(sid, text)
        if a == "pin":
            S["pinned"] = b.get("session") if b.get("session") in S["sessions"] else ""
            return True
        if a == "remote":
            order = ["off", "on", "auto"]
            CONF["remote"] = b.get("setting") or order[(order.index(CONF["remote"]) + 1) % 3]
            return True
    return False


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, obj, code=200, ctype="application/json"):
        data = obj.encode() if isinstance(obj, str) else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def q(self):
        return {k: v[0] for k, v in parse_qs(urlparse(self.path).query).items()}

    def ok_auth(self):
        if TOKEN and self.headers.get("X-Token") != TOKEN and self.q().get("t") != TOKEN:
            self.reply({"error": "bad token"}, 401)
            return False
        return True

    def setup_auth(self):
        if SETUP["done"] and self.q().get("t") != TOKEN and self.headers.get("X-Token") != TOKEN:
            self.reply({"error": "setup finished"}, 401)
            return False
        return True

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        if raw and self.headers.get("Content-Type", "").startswith("multipart/"):
            return {"_raw": raw}
        return json.loads(raw or b"{}")

    # ---------------------------------------------------------------- GET
    def do_GET(self):
        p, q = urlparse(self.path).path, self.q()
        if p == "/health":
            hook = hashlib.sha256(open(os.path.join(_REPO, "hooks/claudestick_hook.py"), "rb").read()).hexdigest()
            return self.reply({"ok": True, "fw": "0.8.1", "name": SETUP["name"], "id": DEVICE_ID, "host": MDNS_HOST,
                               "hook": hook, "auth": bool(TOKEN)})
        if p == "/":
            return self.reply(INDEX_HTML, ctype="text/html; charset=utf-8")
        if p == "/setup":
            return self.reply(SETUP_HTML, ctype="text/html; charset=utf-8")
        if p in ("/install.py", "/claudestick_hook.py"):
            f = "install.py" if p == "/install.py" else "hooks/claudestick_hook.py"
            return self.reply(open(os.path.join(_REPO, f)).read(), ctype="text/x-python")
        if p.startswith("/setup/"):
            if not self.setup_auth():
                return
            if p == "/setup/info":
                return self.reply({"done": SETUP["done"], "ap": SETUP["ap"], "fw": "0.8.1", "port": self.server.server_port,
                                   "tz": SETUP["tz"], "tz_name": SETUP["tz_name"], "clock24": SETUP["clock24"],
                                   "clock": SETUP.get("clock", ""), "font": SETUP["font"],
                                   "wifi": {"ssid": SETUP["ssid"], "connected": SETUP["connected"], "ip": SETUP["ip"],
                                            "saved": [n["s"] for n in SETUP["nets"]] or ([SETUP["ssid"]] if SETUP["ssid"] else [])},
                                   "link": {"url": SETUP["bridge"]["url"], "set": bool(SETUP["bridge"]["url"]),
                                              "up": SETUP["bridge"]["up"], "error": SETUP["bridge"]["error"]},
                                   "name": SETUP["name"], "bt": SETUP["name"], "buddy": SETUP["buddy"],
                                   "volume": SETUP["volume"], "tilt": SETUP["tilt"], "wake": SETUP["wake"], "bt_on": SETUP["bt_on"], "remote": CONF["remote"],
                                   "telemetry": CONF["telemetry"], "telemetry_available": bool(CONF["telemetry_url"]),
                                   "buddies": BUDDIES, "token": TOKEN, "ip": SETUP["ip"]})
            if p == "/setup/scan":
                return self.reply([{"ssid": "home-wifi", "rssi": -48, "secure": True},
                                   {"ssid": "neighbors", "rssi": -80, "secure": False}])
        if not self.ok_auth():
            return
        rid = int(q.get("id", "0") or 0)
        with LOCK:
            if p == "/config":
                return self.reply(CONF)
            if p == "/state":
                return self.reply({
                    "remote": remote_now(), "remote_setting": CONF["remote"], "pinned": S["pinned"],
                    "sessions": [{"id": k, "project": v["project"], "state": v["state"], "detail": v["detail"],
                                  **({"todo": v["todo"]} if v.get("todo", {}).get("total") else {}),
                                  **({"queued": v["inject"][:60]} if v.get("inject") else {}),
                                  **({"urgent": True} if v.get("inject") and v.get("urgent") else {}),
                                  **({"usage": v["usage"]} if v.get("usage") else {}),
                                  **({"ctx_pct": v["u_ctx_pct"]} if "u_ctx_pct" in v else {}),
                                  **({"model_name": v["u_model"]} if "u_model" in v else {})}
                                 for k, v in S["sessions"].items()],
                    "tokens_today": sum(t for (sid, day), t in S.setdefault("ledger", {}).items()
                                        if day == S.get("day")),
                    **({"limits": S["limits"]} if S.get("limits") else {}),
                    "approvals": [{"id": i, "tool": a["tool"], "detail": a["detail"], "project": a["project"], "src": "wifi",
                                   "always": a.get("always", ""), "risk": a.get("risk", ""), "risk_why": a.get("risk_why", "")}
                                  for i, a in S["approvals"].items() if a["decision"] == "pending"],
                    "choices": [{"id": i, "kind": r["kind"], "project": r["project"], "title": r["title"],
                                 "last": r["last"], "options": [m["label"] for m in r["macros"]]}
                                for i, r in S["stops"].items() if r["result"] is None],
                    "macros": [m["label"] for m in CONF["macros"]],
                })
            if p == "/permission":
                a = S["approvals"].get(rid)
                if not a:
                    return self.reply({"state": "gone"})
                st = a["decision"]
                if st != "pending":
                    del S["approvals"][rid]
                return self.reply({"state": st, **({"reason": a["reason"]} if st == "deny" and a.get("reason") else {})})
            if p == "/stop":
                r = S["stops"].get(rid)
                if not r:
                    return self.reply({"state": "gone"})
                if r["result"] is None:
                    return self.reply({"state": "pending"})
                del S["stops"][rid]
                if r["result"] == "dismiss":
                    return self.reply({"state": "dismiss"})
                if r["result"] == "custom":
                    return self.reply({"state": "custom", "text": r["custom"]})
                return self.reply({"state": "macro", "text": r["macros"][r["result"]]["text"]})
        self.reply({"error": "not found"}, 404)

    def do_DELETE(self):
        if not self.ok_auth():
            return
        p, q = urlparse(self.path).path, self.q()
        with LOCK:
            (S["approvals"] if p == "/permission" else S["stops"]).pop(int(q.get("id", "0")), None)
        self.reply({"state": "cancelled"})

    # --------------------------------------------------------------- POST
    def do_POST(self):
        p, q = urlparse(self.path).path, self.q()
        if p.startswith("/setup/"):
            b = self.body()
            if not self.setup_auth():
                return
            if p == "/setup/wifi":
                SETUP.update(ssid=b.get("ssid", ""), connected=b.get("pass") == "goodpass")
                return self.reply({"ok": True})
            if p == "/setup/wifi/add":
                ssid_, pw = str(b.get("ssid", "")), str(b.get("pass", ""))
                if not ssid_ or len(ssid_) > 32 or len(pw) > 63:
                    return self.reply({"error": "need a network name (32 characters at most) and a password (63 at most)"}, 400)
                if not SETUP["nets"] and SETUP["ssid"]:
                    SETUP["nets"].append({"s": SETUP["ssid"], "p": ""})
                for n in SETUP["nets"]:
                    if n["s"] == ssid_:
                        n["p"] = pw
                        return self.reply({"ok": True})
                if len(SETUP["nets"]) >= 4:
                    return self.reply({"error": "four networks is the limit: forget one first"}, 409)
                SETUP["nets"].append({"s": ssid_, "p": pw})
                return self.reply({"ok": True})
            if p == "/setup/wifi/forget":
                SETUP["nets"] = [n for n in SETUP["nets"] if n["s"] != b.get("ssid")]
                return self.reply({"ok": True})
            if p == "/setup/bridge":
                url = str(b.get("url", "")).strip().rstrip("/")
                if not url:
                    SETUP["bridge"] = {"url": "", "tok": "", "up": False, "error": ""}
                    return self.reply({"ok": True, "set": False})
                if not re.fullmatch(r"https?://[^/\s@?]+", url):
                    return self.reply({"error": "the address should look like https://stick.example.com"}, 400)
                code = re.sub(r"\D", "", str(b.get("code", "")))
                if str(b.get("code", "")).strip():
                    if len(code) != 6:
                        return self.reply({"error": "a pairing code is 6 digits"}, 400)
                    if code != "123456":
                        return self.reply({"ok": False, "set": False, "error": "that pairing code is wrong or has expired (they last 5 minutes and stop working after 3 wrong tries)"})
                    tok = "goodtoken"
                else:
                    tok = str(b.get("token", "")).strip() or (SETUP["bridge"]["tok"] if SETUP["bridge"]["url"] == url else "")
                if not tok:
                    return self.reply({"error": "paste the bridge's token, or enter a pairing code"}, 400)
                good = tok == "goodtoken"
                SETUP["bridge"] = {"url": url, "tok": tok, "up": good, "error": "" if good else "the bridge rejected that token"}
                return self.reply({"ok": good, "set": True, "error": SETUP["bridge"]["error"]})
            if p == "/setup/preview":
                SETUP["buddy"] = b.get("buddy", 0)
                if isinstance(b.get("font"), int):
                    SETUP["font"] = b["font"]
                print(f"[preview] buddy -> {BUDDIES[SETUP['buddy']]}")
                return self.reply({"ok": True})
            if p == "/setup/time":
                tz = b.get("tz")
                if isinstance(tz, str):
                    if tz and not re.fullmatch(r"[A-Za-z0-9,.:/+<>-]{1,63}", tz):
                        return self.reply({"error": "bad time zone"}, 400)
                    SETUP["tz"], SETUP["tz_name"] = tz, str(b.get("tz_name", ""))[:40]
                if isinstance(b.get("clock24"), bool):
                    SETUP["clock24"] = b["clock24"]
                SETUP["clock"] = "8:59p" if not SETUP["clock24"] else "20:59"
                SETUP["epoch"] = b.get("epoch")
                return self.reply({"ok": True, "clock": SETUP["clock"], "tz": SETUP["tz"], "tz_name": SETUP["tz_name"],
                                   "clock24": SETUP["clock24"]})
            if p == "/setup/save":
                name = (b.get("name") or SETUP["name"]).strip()
                if not name.startswith("Claude"):
                    name = "Claude-" + name
                restart = name != SETUP["name"]
                SETUP.update(name=name[:24], buddy=b.get("buddy", 0), volume=b.get("volume", 120),
                             tilt=bool(b.get("tilt")), done=True)
                if isinstance(b.get("wake"), bool):
                    SETUP["wake"] = b["wake"]
                if isinstance(b.get("font"), int):
                    SETUP["font"] = b["font"]
                if isinstance(b.get("bt"), bool) and b["bt"] != SETUP["bt_on"]:
                    SETUP["bt_on"], restart = b["bt"], True   # Bluetooth starts at boot
                if isinstance(b.get("remote"), str):
                    CONF["remote"] = b["remote"]
                if isinstance(b.get("telemetry"), bool):
                    CONF["telemetry"] = b["telemetry"]
                return self.reply({"ok": True, "restart": restart, "token": TOKEN, "bt": SETUP["name"],
                                   "ip": SETUP["ip"], "port": self.server.server_port})
        if not self.ok_auth():
            return
        b = self.body()
        if p == "/act":
            if not b:
                b = {"action": q.get("action"), "session": q.get("session")}
                if q.get("id"):
                    b["id"] = int(q["id"])
            ok = act(b)
            return self.reply({"ok": ok}, 200 if ok else 404)
        if p == "/config":
            with LOCK:
                for k, v in b.items():
                    if k in CONF:
                        CONF[k] = v
                return self.reply(CONF)
        if p == "/ota":
            S["ota_bytes"] = len(b.get("_raw", b""))
            print(f"[ota] received {S['ota_bytes']} bytes")
            return self.reply({"ok": True})
        if p == "/watch":   # a hook waiting out an idle spell: has text arrived? (doesn't count as activity)
            with LOCK:
                sid = b.get("session", "unknown")
                s = S["sessions"].get(sid)
                if s is None:
                    sid, s = session(b)
                out = {"inject": None, "remote": remote_now()}
                if s["inject"]:
                    out["inject"], s["inject"], s["urgent"] = s["inject"], "", False
                return self.reply(out)
        with LOCK:
            sid, s = session(b)
            S["log"].append((p, b))
            if p == "/event":
                if b.get("typed"):
                    note_kbd()
                ev = b.get("event", "")
                if ev == "end":
                    S["sessions"].pop(sid, None)
                    if S["pinned"] == sid:   # a tie to a session that's gone would skip everyone else's prompts
                        S["pinned"] = ""
                    return self.reply({"stop": False, "inject": None, "remote": remote_now()})
                s["state"], s["detail"] = ev, b.get("detail", "")
                if isinstance(b.get("todo"), dict):
                    s["todo"] = b["todo"]
                if isinstance(b.get("usage"), dict):
                    u = b["usage"]
                    s["usage"] = {k: u.get(k) for k in ("in", "out", "cr", "cw", "ctx", "model")}
                    if u.get("day"):
                        S["day"] = max(S.get("day", ""), u["day"])
                        S.setdefault("ledger", {})[(sid, u["day"])] = u.get("today", 0)
                out = {"stop": False, "inject": None, "remote": remote_now()}
                if b.get("can_stop") and s["stop"]:
                    out["stop"], s["stop"] = True, False
                elif (b.get("can_inject") or (b.get("can_preempt") and s.get("urgent"))) and s["inject"]:
                    out["inject"], s["inject"], s["urgent"] = s["inject"], "", False
                return self.reply(out)
            if p == "/usage":
                S.setdefault("limits", {})
                for k in ("five", "seven"):
                    if isinstance(b.get(k), dict):
                        S["limits"][k] = {"pct": b[k].get("pct"), "resets": b[k].get("resets", 0)}
                s2 = S["sessions"].get(sid)
                if s2 is not None:
                    for k in ("ctx_pct", "cost", "model"):
                        if k in b:
                            s2["u_" + k] = b[k]
                return self.reply({"ok": True})
            if p == "/permission":
                i = nid()
                s["state"] = "approve"
                risk = b.get("risk", "")
                S["approvals"][i] = {"tool": b.get("tool"), "detail": b.get("detail"), "project": s["project"],
                                     "always": "" if risk == "high" else b.get("always", ""), "risk": risk,
                                     "risk_why": b.get("risk_why", ""),
                                     "decision": "skip" if S["pinned"] and S["pinned"] != sid else "pending"}
                print(f"\n[{'DANGER ' if risk else ''}APPROVE?] #{i} {s['project']} {b.get('tool')}: "
                      f"{b.get('detail')}  always={b.get('always')!r}  (a/A/b/k)")
                return self.reply({"id": i, "remote": remote_now()})
            if p == "/stop":
                i = nid()
                kind = b.get("kind", "done")
                if b.get("macros"):
                    macros = b["macros"]
                else:
                    macros = list(b.get("options") or []) + ([] if kind == "question" else CONF["macros"])
                s["state"] = "question" if kind == "question" else "done"
                S["stops"][i] = {"macros": macros, "result": None, "sel": 0, "project": s["project"], "kind": kind,
                                 "title": b.get("title", ""), "last": b.get("last", ""), "session": sid, "custom": ""}
                if S["pinned"] and S["pinned"] != sid:
                    S["stops"][i]["result"] = "dismiss"
                print(f"\n[{kind.upper()}] #{i} {s['project']}: {b.get('title') or b.get('last', '')}")
                for n, m in enumerate(macros):
                    print(f"   {n}: {m['label']}")
                return self.reply({"id": i, "remote": remote_now()})
        self.reply({"error": "not found"}, 404)


def press(cmd):
    parts = cmd.split()
    if not parts:
        return
    c = parts[0]
    with LOCK:
        pend = sorted(i for i, a in S["approvals"].items() if a["decision"] == "pending")
        stops = sorted(i for i, r in S["stops"].items() if r["result"] is None)
    if c == "n" and pend:   # deny with a reason, like hold-A voice on the Stick
        return act({"action": "deny", "id": pend[0], "text": cmd[2:]})
    if c in ("a", "A", "b", "k") and pend:
        return act({"action": {"a": "approve", "A": "always", "b": "deny", "k": "keyboard"}[c], "id": pend[0]})
    if c in ("a", "b", "d") and stops:
        with LOCK:
            r = S["stops"][stops[0]]
            if c == "b":
                r["sel"] = (r["sel"] + 1) % len(r["macros"])
                print("   >", r["macros"][r["sel"]]["label"])
                return
        return act({"action": "choose" if c == "a" else "dismiss", "id": stops[0], "index": r["sel"]})
    if c == "s":
        return act({"action": "stop"})
    if c == "m" and len(parts) > 1:
        return act({"action": "macro", "index": int(parts[1])})
    if c == "t":
        return act({"action": "say", "text": cmd[2:]})
    if c == "r":
        act({"action": "remote"})
        print("remote setting:", CONF["remote"], "-> effective", remote_now())
        return
    if c == "ls":
        print(json.dumps({k: S[k] for k in ("sessions", "approvals", "stops")}, indent=1, default=str))


def main():
    global TOKEN
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=7880)
    p.add_argument("--token", default="")
    a = p.parse_args()
    TOKEN = a.token
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print(f"mock ClaudeStick on :{a.port}  token={TOKEN or '(none)'}")
    try:
        while True:
            press(input("> ").strip())
    except (EOFError, KeyboardInterrupt):
        pass


if __name__ == "__main__":
    main()
