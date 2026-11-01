"""The bridge, end to end: the same HTTP API as the Stick, plus auth, persistence and the STT proxy."""
import http.client
import json
import os
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, ROOT + "/bridge")
import claudestick_bridge as br  # noqa: E402

FAILED = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (("  | " + str(extra)[:200]) if extra and not cond else ""))
    if not cond:
        FAILED.append(name)
    return cond


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        self.t += 0.001   # every reading is later than the last, like a real monotonic clock
        return self.t


TOKEN = "t" * 32
data = tempfile.mkdtemp()
store = br.Store(data)
store.ident["token"] = TOKEN
store.ensure_identity()
check("the bridge defaults to remote 'on' (it is only ever on while a Stick or phone is connected)", store.config["remote"] == "on")
store.config["remote"] = "auto"   # the tests below exercise the auto rule
clock = Clock()
core = br.Core(store, mono=clock, wall=lambda: 1_800_000_000 + clock.t)
guard = br.AuthGuard(TOKEN, mono=clock)
srv = br.make_server(store, "127.0.0.1", 0, core=core, guard=guard)
PORT = srv.server_port
threading.Thread(target=srv.serve_forever, daemon=True).start()


def call(method, path, body=None, token=TOKEN, headers=None, raw=None, port=None):
    c = http.client.HTTPConnection("127.0.0.1", port or PORT, timeout=10)
    h = dict(headers or {})
    if token:
        h["X-Token"] = token
    data_ = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    if data_ is not None:
        h.setdefault("Content-Type", "application/json")
    try:
        c.request(method, path, body=data_, headers=h)
    except (BrokenPipeError, ConnectionResetError):   # the server answered (413) before we finished sending
        pass
    r = c.getresponse()
    out = r.read()
    try:
        j = json.loads(out)
    except ValueError:
        j = None
    c.close()
    return r.status, j, r


def get(path, **k):
    return call("GET", path, **k)[:2]


def post(path, body=None, **k):
    return call("POST", path, body or {}, **k)[:2]


def press(action, **kw):
    return post("/act", {"action": action, **kw})


def state(client="stick", v=None):
    return get("/state" + (f"?v={v}" if v is not None else ""), headers={"X-Client": client})[1]


# ----------------------------------------------------------------- identity, auth, hygiene
check("token + id are created once and persisted", len(store.ident["id"]) == 12 and
      json.load(open(data + "/bridge.json"))["token"] == TOKEN)
check("data dir is private (0700), files 0600", oct(os.stat(data).st_mode & 0o777) == "0o700" and
      oct(os.stat(data + "/bridge.json").st_mode & 0o777) == "0o600")
s2 = br.Store(data)
check("reopening keeps the same identity", s2.ident == store.ident)
made = br.Store(tempfile.mkdtemp())
check("a fresh store makes a 32-hex token", made.ensure_identity() and len(made.ident["token"]) == 32)

st, j = get("/health", token="")
check("/health is public and says it's a bridge", st == 200 and j["bridge"] is True and j["id"] == store.ident["id"] and
      len(j["hook"]) == 64, j)
for page, needle in [("/", "ClaudeStick"), ("/setup", "setup"), ("/install.py", "Install"), ("/claudestick_hook.py", "def ")]:
    c = http.client.HTTPConnection("127.0.0.1", PORT)
    c.request("GET", page)
    r = c.getresponse()
    body = r.read().decode()
    check(f"{page} is served without a token", r.status == 200 and needle in body, r.status)
    c.close()
check("served hook matches the hook in the repo (self-update sees no difference)",
      j["hook"] == __import__("hashlib").sha256(open(ROOT + "/hooks/claudestick_hook.py", "rb").read()).hexdigest())
check("no token -> 401", get("/state", token="")[0] == 401)
check("wrong token -> 401", get("/state", token="x" * 32)[0] == 401)
check("same length wrong token -> 401", get("/state", token="u" * 32)[0] == 401)
check("token as ?t= works", get("/state?t=" + TOKEN, token="")[0] == 200)
st, j, r = call("GET", "/state", headers={"X-Client": "stick"})
check("security headers", r.getheader("Referrer-Policy") == "no-referrer" and r.getheader("Cache-Control") == "no-store" and
      r.getheader("X-Content-Type-Options") == "nosniff")
check("no Server banner with versions", "Python" not in (r.getheader("Server") or ""), r.getheader("Server"))

# keep-alive: several requests on one connection, and a rejected POST doesn't poison the next request
c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
for _ in range(3):
    c.request("GET", "/state", headers={"X-Token": TOKEN, "X-Client": "stick"})
    r = c.getresponse()
    r.read()
check("keep-alive: 3 requests, one connection", r.status == 200)
c.close()
c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
c.request("POST", "/event", body=b'{"session":"x"}', headers={"X-Token": "nope", "Content-Type": "application/json"})
r = c.getresponse()
r.read()
check("bad token on a POST: 401 and the connection is closed (body not left behind)", r.status == 401 and r.will_close)
c.close()

# body limits
check("oversized JSON -> 413", post("/event", raw=b"{" + b" " * (br.MAX_JSON + 10) + b"}")[0] == 413)
check("malformed JSON -> 400", post("/event", raw=b"{nope")[0] == 400)
check("JSON that isn't an object -> 400", post("/event", raw=b"[1,2]")[0] == 400)
c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
c.putrequest("POST", "/event")
c.putheader("X-Token", TOKEN)
c.putheader("Transfer-Encoding", "chunked")
c.endheaders()
c.send(b"2\r\n{}\r\n0\r\n\r\n")
r = c.getresponse()
r.read()
check("chunked request bodies are refused (411)", r.status == 411)
c.close()
check("unknown route -> 404", get("/nope")[0] == 404 and post("/nope")[0] == 404)

# lockout: ten bad guesses lock the address out, even for the right token; it expires
for _ in range(br.FAIL_LIMIT):
    get("/state", token="bad" * 8)
st, j, r = call("GET", "/state")
check("10 bad tokens lock the address out (429 + Retry-After), even with the right token",
      st == 429 and int(r.getheader("Retry-After")) > 0, st)
check("public pages still load while locked out", get("/health", token="")[0] == 200)
clock.t += br.LOCKOUT_S + 2
check("lockout expires", get("/state")[0] == 200)

# behind a proxy the lockout is per real client, not per proxy
psrv = br.make_server(store, "127.0.0.1", 0, trust_proxy=True, core=core, guard=br.AuthGuard(TOKEN, mono=clock))
threading.Thread(target=psrv.serve_forever, daemon=True).start()
for _ in range(br.FAIL_LIMIT):
    call("GET", "/state", token="bad" * 8, headers={"X-Forwarded-For": "203.0.113.9"}, port=psrv.server_port)
check("proxy: the attacker is locked out",
      call("GET", "/state", headers={"X-Forwarded-For": "203.0.113.9"}, port=psrv.server_port)[0] == 429)
check("proxy: another client behind the same proxy is fine",
      call("GET", "/state", headers={"X-Forwarded-For": "198.51.100.4"}, port=psrv.server_port)[0] == 200)
check("proxy: a spoofed leading X-Forwarded-For entry doesn't help",
      call("GET", "/state", headers={"X-Forwarded-For": "198.51.100.4, 203.0.113.9"}, port=psrv.server_port)[0] == 429)
psrv.shutdown()

# ----------------------------------------------------------------- pairing codes
def pair(code, port=None):
    return call("POST", "/pair", {"code": code}, token="", port=port)


check("/health says the bridge can pair", get("/health", token="")[1].get("pair") is True)
check("a code can't be made without the token", call("POST", "/pair/new", {}, token="")[0] == 401)
check("...nor with a wrong one", call("POST", "/pair/new", {}, token="x" * 32)[0] == 401)
st, j = post("/pair/new")
code = j["code"]
check("a code is 6 digits, valid for 5 minutes", st == 200 and len(code) == 6 and code.isdigit() and j["expires_in"] == br.PAIR_TTL_S, j)
st, j, _ = pair("000000" if code != "000000" else "111111")
check("a wrong code is refused (403), without saying why", st == 403 and j == {"error": "wrong or expired pairing code"}, j)
st, j, _ = pair(code)
check("the right code, with no token at all, returns the real token", st == 200 and j["token"] == TOKEN and j["id"] == store.ident["id"], j)
st, j, _ = pair(code)
check("...and it's single use: the same code again is refused", st == 403, j)
check("the token it returned really works", get("/state", token=j.get("token") or TOKEN)[0] == 200)

st, j = post("/pair/new")
code = j["code"]
check("spaces and a hyphen are ignored", pair(code[:3] + " - " + code[3:])[0] == 200)
st, j = post("/pair/new")
c1 = j["code"]
st, j = post("/pair/new")
c2 = j["code"]
if c1 != c2:
    check("a new code replaces the old one", pair(c1)[0] == 403 and pair(c2)[0] == 200)
else:   # one in a million: same code twice
    check("a new code replaces the old one", pair(c2)[0] == 200)
clock.t += br.LOCKOUT_S + 5   # each group below starts with a clean per-address count
check("junk is just a wrong code: numbers, nulls, lists, a missing field",
      all(call("POST", "/pair", body, token="")[0] == 403 for body in ({"code": 123456}, {"code": None}, {"code": [1]}, {})))
clock.t += br.LOCKOUT_S + 5
check("malformed JSON to /pair -> 400", call("POST", "/pair", raw=b"{nope", token="")[0] == 400)
check("an oversized /pair body is refused (413), not read into memory", call("POST", "/pair", raw=b" " * 5000, token="")[0] == 413)

# three wrong tries burn the code, even for the right one afterwards
clock.t += br.LOCKOUT_S + 5
st, j = post("/pair/new")
code = j["code"]
wrongs = [w for w in ("111111", "222222", "333333", "444444") if w != code][:br.PAIR_MAX_FAILS]
for w in wrongs:
    pair(w)
st, j, _ = pair(code)
check("after 3 wrong tries the code is dead, even if you then get it right", st == 403, j)
check("a spent code can't be revived: it has to be made again by the token holder", post("/pair/new")[0] == 200)

# expiry
st, j = post("/pair/new")
code = j["code"]
clock.t += br.PAIR_TTL_S + 1
check("an expired code is refused", pair(code)[0] == 403)
clock.t += br.LOCKOUT_S + 5
core.pair = None
st, j, _ = pair("123456")
check("with no active code, every guess just fails (and looks like any other wrong code)", st == 403 and j == {"error": "wrong or expired pairing code"}, j)

# per-address lockout on /pair, independent of the token lockout
clock.t += br.LOCKOUT_S + 5
core.pair = None
for _ in range(br.PAIR_FAIL_LIMIT):
    pair("999999")
st, j, r = call("POST", "/pair", {"code": "999999"}, token="")
check("wrong codes from one address lock pairing out (429 + Retry-After)", st == 429 and int(r.getheader("Retry-After")) > 0, st)
post("/pair/new")
st, j, _ = pair(core.pair["code"])
check("...even for the right code, while locked out", st == 429)
check("the lockout is only for /pair: the token still works from that address", get("/state")[0] == 200)
clock.t += br.LOCKOUT_S + 5
st, j = post("/pair/new")
check("the lockout expires", pair(j["code"])[0] == 200)

# nothing secret in the logs
import io, contextlib
buf = io.StringIO()
with contextlib.redirect_stderr(buf):
    st, j = post("/pair/new")
    pair(j["code"])
    for _ in range(br.PAIR_FAIL_LIMIT):
        pair("888888")
log = buf.getvalue()
check("the log says a device paired but never the code or the token", "paired a device" in log and TOKEN not in log and j["code"] not in log and "888888" not in log, log)
clock.t += br.LOCKOUT_S + 5

# the CLI asks the running bridge for a code
import subprocess
r = subprocess.run([sys.executable, ROOT + "/bridge/claudestick_bridge.py", "--pair", "--data", data, "--port", str(PORT)],
                   capture_output=True, text=True, timeout=20)
check("--pair prints a code and how to use it", r.returncode == 0 and "pairing code:" in r.stdout and "--pair " in r.stdout and
      "Stick:" in r.stdout and TOKEN not in r.stdout, r.stdout + r.stderr)
digits = "".join(c for c in r.stdout.split("pairing code:")[1].split("(")[0] if c.isdigit())
check("...and that code works", pair(digits)[0] == 200)
r = subprocess.run([sys.executable, ROOT + "/bridge/claudestick_bridge.py", "--pair", "--data", data, "--port", "1"],
                   capture_output=True, text=True, timeout=20)
check("--pair with no bridge running says so", r.returncode == 1 and "no bridge answering" in r.stderr, r.stderr)

# ----------------------------------------------------------------- sessions, events
HOOK = {"session": "s1", "project": "api-server@main"}
st, j = post("/event", {**HOOK, "event": "working", "detail": "Edit main.py", "can_inject": True})
check("event: shape matches the Stick's", st == 200 and j == {"stop": False, "inject": None, "remote": False}, j)
s = state()
check("session shows up with project/state/detail",
      s["sessions"][0]["project"] == "api-server@main" and s["sessions"][0]["state"] == "working" and
      s["sessions"][0]["detail"] == "Edit main.py", s)
check("state carries the bridge flag, version and epoch", s["bridge"] == 1 and s["v"] >= 1 and s["now"] > 1_700_000_000, s)

# remote gating: auto follows where you last answered, but only while someone is polling
post("/event", {**HOOK, "event": "working", "typed": True})
check("typing in the terminal -> desk", state()["remote"] is False)
st, j = post("/permission", {**HOOK, "tool": "Bash", "detail": "npm run build", "always": "npm run *"})
aid = j["id"]
check("permission: id is far from the Stick's own ids", aid >= 1_000_000, j)
check("permission: desk mode asks for a short wait", j["remote"] is False)
check("approval is listed for the Stick", state()["approvals"][0]["id"] == aid and
      state()["approvals"][0]["always"] == "npm run *" and state()["approvals"][0]["session"] == "s1")
check("permission poll: pending", get(f"/permission?id={aid}")[1] == {"state": "pending"})
check("approve", press("approve", id=aid) == (200, {"ok": True}))
check("approve again: not pending any more -> 404", press("approve", id=aid)[0] == 404)
check("permission poll: allow, then gone", get(f"/permission?id={aid}")[1] == {"state": "allow"} and
      get(f"/permission?id={aid}")[1] == {"state": "gone"})
check("answering on the Stick turns remote on (auto)", state()["remote"] is True)
check("...and the hooks are told", post("/event", {**HOOK, "event": "working"})[1]["remote"] is True)
clock.t += br.PRESENT_S + 1
check("nobody polling for 30 s -> remote is off, so the terminal asks", post("/event", {**HOOK, "event": "working"})[1]["remote"] is False)
state()
check("polling again brings it back", post("/event", {**HOOK, "event": "working"})[1]["remote"] is True)
post("/event", {**HOOK, "event": "working", "typed": True})
check("typing again turns it off", post("/event", {**HOOK, "event": "working"})[1]["remote"] is False)
check("'on' = always remote while a client is here",
      press("remote", setting="on")[0] == 200 and post("/event", {**HOOK, "event": "working"})[1]["remote"] is True)
check("'off' wins", press("remote", setting="off")[0] == 200 and post("/event", {**HOOK, "event": "working"})[1]["remote"] is False)
check("remote setting is saved to disk", json.load(open(data + "/config.json"))["remote"] == "off")
press("remote", setting="auto")
check("remote with no setting cycles", press("remote")[0] == 200 and core.conf["remote"] == "off")
press("remote", setting="auto")

# approval variants
def new_approval(**kw):
    return post("/permission", {**HOOK, "tool": "Bash", "detail": "x", **kw})[1]["id"]


i = new_approval(always="git status *")
press("always", id=i)
check("always -> 'always'", get(f"/permission?id={i}")[1] == {"state": "always"})
i = new_approval()
press("always", id=i)
check("always when none offered -> plain allow", get(f"/permission?id={i}")[1] == {"state": "allow"})
i = new_approval(risk="high", risk_why="rm -rf", always="rm *")
a = state()["approvals"][0]
check("high risk: no always offered, risk fields kept", a["always"] == "" and a["risk"] == "high" and a["risk_why"] == "rm -rf", a)
press("deny", id=i, text="not that directory")
check("deny with a reason passes the reason back", get(f"/permission?id={i}")[1] == {"state": "deny", "reason": "not that directory"})
i = new_approval()
press("keyboard", id=i)
check("answer-at-keyboard -> skip, and flips auto remote off", get(f"/permission?id={i}")[1] == {"state": "skip"} and state()["remote"] is False)
i = new_approval()
check("hook gave up: DELETE removes it", call("DELETE", f"/permission?id={i}")[1] == {"state": "cancelled"} and
      get(f"/permission?id={i}")[1] == {"state": "gone"})
i = new_approval()
clock.t += br.STALE_S + 1
state()
check("a hook that stopped polling is dropped after 15 s", get(f"/permission?id={i}")[1] == {"state": "gone"} and not state()["approvals"])
i = new_approval()
press("approve", id=i)
clock.t += br.STALE_S - 2
check("a decision waits for the hook's next poll", get(f"/permission?id={i}")[1] == {"state": "allow"})
check("approvals queue is capped", all(new_approval() for _ in range(br.MAX_APPROVALS)) and
      post("/permission", {**HOOK, "tool": "Bash"})[0] == 503)
for a in state()["approvals"]:
    call("DELETE", f"/permission?id={a['id']}")

# pinning: other sessions fall through to their terminals
post("/event", {"session": "s2", "project": "other", "event": "working"})
press("pin", session="s1")
i2 = post("/permission", {"session": "s2", "tool": "Bash", "detail": "y"})[1]["id"]
check("pinned: other session's approval is skipped", get(f"/permission?id={i2}")[1] == {"state": "skip"})
j = post("/stop", {"session": "s2", "kind": "done", "last": "done"})[1]
check("pinned: other session's Done card is dismissed", get(f"/stop?id={j['id']}")[1] == {"state": "dismiss"})
check("pin shows in state", state()["pinned"] == "s1")
press("pin", session="")
check("unpin", state()["pinned"] == "")
check("pin an unknown session -> unpinned", press("pin", session="ghost")[0] == 200 and state()["pinned"] == "")

# ----------------------------------------------------------------- Done / Question cards
j = post("/stop", {**HOOK, "kind": "done", "last": "All tests pass", "title": "Done"})[1]
sid_ = j["id"]
c = state()["choices"][0]
check("done card lists Claude's macros", c["kind"] == "done" and c["options"][0] == "Continue" and len(c["options"]) == 6, c)
check("stop poll: pending", get(f"/stop?id={sid_}")[1] == {"state": "pending"})
press("choose", id=sid_, index=2)
check("choose a macro -> its text", get(f"/stop?id={sid_}")[1] == {"state": "macro", "text": "That phase looks good. Proceed to the next phase of the plan."})
check("...and then it's gone", get(f"/stop?id={sid_}")[1] == {"state": "gone"})
j = post("/stop", {**HOOK, "kind": "done", "last": "x"})[1]
check("choose out of range -> 404", press("choose", id=j["id"], index=99)[0] == 404)
press("dismiss", id=j["id"])
check("dismiss", get(f"/stop?id={j['id']}")[1] == {"state": "dismiss"})
j = post("/stop", {**HOOK, "kind": "question", "title": "Which DB?", "options": [{"label": "Postgres", "text": "Use Postgres"},
                                                                                {"label": "SQLite", "text": "Use SQLite"}]})[1]
c = state()["choices"][0]
check("question: only Claude's options, none of your macros", c["kind"] == "question" and c["options"] == ["Postgres", "SQLite"], c)
check("question sets the session state", state()["sessions"][0]["state"] == "question")
press("choose", id=j["id"], index=1)
check("question answer", get(f"/stop?id={j['id']}")[1] == {"state": "macro", "text": "Use SQLite"})
j = post("/stop", {**HOOK, "kind": "done", "macros": [{"label": "Only", "text": "only this"}]})[1]
check("explicit macros replace the defaults", state()["choices"][0]["options"] == ["Only"])
press("say", session="s1", text="please also update the docs")
check("say answers the open card", get(f"/stop?id={j['id']}")[1] == {"state": "custom", "text": "please also update the docs"})

# text and macros without a card: delivered on the next hook call, stop flag too
check("say with no card queues it", press("say", session="s1", text="  go on  ")[0] == 200)
st, j = post("/event", {**HOOK, "event": "working", "can_inject": True})
check("queued text arrives with the next event", j["inject"] == "  go on  ", j)
check("...once", post("/event", {**HOOK, "event": "working", "can_inject": True})[1]["inject"] is None)
press("macro", session="s1", index=0)
check("macro queues its text", post("/event", {**HOOK, "event": "working", "can_inject": True})[1]["inject"] == "Continue.")

# a queued message can be seen and taken back
press("say", session="s1", text="queued for the next step " + "x" * 100)
q1 = [x for x in state()["sessions"] if x["id"] == "s1"][0]
check("a queued message shows in the state (clipped to 60)", q1.get("queued", "").startswith("queued for the next step") and len(q1["queued"]) == 60, q1)
q2 = [x for x in get("/state?n=1", headers={"X-Client": "stick"})[1]["sessions"] if x["id"] == "s1"][0]
check("...in the Stick's trimmed view too", "queued" in q2, q2)
check("unsend takes it back", press("unsend", session="s1")[0] == 200)
check("...nothing is delivered afterwards", post("/event", {**HOOK, "event": "working", "can_inject": True})[1]["inject"] is None)
check("...and the state stops showing it", "queued" not in [x for x in state()["sessions"] if x["id"] == "s1"][0])
check("unsend with nothing queued is harmless", press("unsend", session="s1")[0] == 200)
check("unsend for an unknown session -> 404", press("unsend", session="ghost")[0] == 404)

# send now: a queued message jumps ahead of the next tool call
press("say", session="s1", text="stop and look at this")
check("a queued message isn't handed over before a tool runs", post("/event", {**HOOK, "event": "tool", "can_stop": True, "can_preempt": True})[1]["inject"] is None)
check("send now with nothing queued -> 404", press("sendnow", session="s2")[0] == 404)
check("send now marks it urgent in the state", press("sendnow", session="s1")[0] == 200 and
      [x for x in state()["sessions"] if x["id"] == "s1"][0].get("urgent") is True)
j = post("/event", {**HOOK, "event": "tool", "can_stop": True, "can_preempt": True})[1]
check("...then the next tool call gets it", j["inject"] == "stop and look at this", j)
check("...once, and the urgent flag is gone", post("/event", {**HOOK, "event": "working", "can_inject": True})[1]["inject"] is None and
      "urgent" not in [x for x in state()["sessions"] if x["id"] == "s1"][0])
press("say", session="s1", text="new text")
press("sendnow", session="s1")
press("say", session="s1", text="replacement")
check("a newer message isn't urgent unless you say so", post("/event", {**HOOK, "event": "tool", "can_stop": True, "can_preempt": True})[1]["inject"] is None)
press("unsend", session="s1")
press("say", session="s1", text="x"); press("sendnow", session="s1"); press("unsend", session="s1")
check("unsend clears urgency too", post("/event", {**HOOK, "event": "tool", "can_preempt": True})[1]["inject"] is None)

# watch: a hook waiting out an idle spell picks text up, without counting as activity
press("say", session="s1", text="wake up")
idle0 = [x for x in state()["sessions"] if x["id"] == "s1"][0]["idle_s"]
clock.t += 5
check("watch hands over queued text once", post("/watch", {"session": "s1", "project": "p"})[1].get("inject") == "wake up" and
      post("/watch", {"session": "s1"})[1].get("inject") is None)
idle1 = [x for x in state()["sessions"] if x["id"] == "s1"][0]["idle_s"]
check("...and doesn't make the session look active", idle1 >= idle0 + 5, (idle0, idle1))
clock.t += br.SESSION_IDLE_S - 100
post("/watch", {"session": "s1"})
clock.t += 200
check("...but keeps an idle session listed", any(x["id"] == "s1" for x in state()["sessions"]))
check("watch for an unknown session lists it", post("/watch", {"session": "fresh-one", "project": "fresh"})[1].get("inject") is None and
      any(x["id"] == "fresh-one" for x in state()["sessions"]))
post("/event", {"session": "fresh-one", "event": "end"})

# which session was active last: idle_s counts from each hook's last report
post("/event", {"session": "idle-a", "project": "a", "event": "working"})
clock.t += 6
post("/event", {"session": "idle-b", "project": "b", "event": "working"})
clock.t += 3
ids = {x["id"]: x for x in state()["sessions"]}
check("each session reports how long since its hook last spoke", ids["idle-a"]["idle_s"] >= 9 and 3 <= ids["idle-b"]["idle_s"] < 6, (ids["idle-a"], ids["idle-b"]))
post("/event", {"session": "idle-a", "event": "end"})
post("/event", {"session": "idle-b", "event": "end"})
check("macro index out of range -> 404", press("macro", session="s1", index=99)[0] == 404)
check("empty say -> 404", press("say", session="s1", text="   ")[0] == 404)
check("say to an unknown session -> 404", press("say", session="ghost", text="hi")[0] == 404)
check("say with no session goes to the most recent one", press("say", text="hello")[0] == 200)
post("/event", {"session": "s2", "event": "working"})
press("stop", session="s1")
check("stop flag is only handed to a hook that can stop",
      post("/event", {**HOOK, "event": "working"})[1]["stop"] is False and
      post("/event", {**HOOK, "event": "working", "can_stop": True})[1]["stop"] is True and
      post("/event", {**HOOK, "event": "working", "can_stop": True})[1]["stop"] is False)
check("unknown action -> 404", press("explode")[0] == 404)

# ----------------------------------------------------------------- usage, to-do, plan done, tokens
post("/event", {**HOOK, "event": "working", "todo": {"done": 1, "total": 3, "skip": 0, "now": "Write tests",
                                                    "items": [["c", "Plan"], ["i", "Write tests"], ["p", "Ship"]]},
                "usage": {"in": 10, "out": 20, "cr": 300, "cw": 40, "ctx": 5000, "model": "sonnet", "day": "2026-10-30", "today": 1234}})
post("/event", {"session": "s2", "event": "working", "usage": {"in": 1, "out": 1, "day": "2026-10-30", "today": 100}})
s = state()
s1 = [x for x in s["sessions"] if x["id"] == "s1"][0]
check("to-do passes through", s1["todo"]["done"] == 1 and s1["todo"]["items"][1] == ["i", "Write tests"], s1)
check("usage passes through", s1["usage"]["cr"] == 300 and s1["usage"]["model"] == "sonnet", s1)
check("tokens today = sum over sessions", s["tokens_today"] == 1334, s["tokens_today"])
post("/event", {**HOOK, "event": "working", "usage": {"day": "2026-10-30", "today": 2000}})
check("a session's day total is replaced, not added", state()["tokens_today"] == 2100)
post("/event", {**HOOK, "event": "working", "usage": {"day": "2026-10-31", "today": 5}})
check("a new day starts the count over", state()["tokens_today"] == 5)
before = [x for x in state()["sessions"] if x["id"] == "s1"][0]["plan_done_n"]
post("/event", {**HOOK, "event": "working", "plan_done": True})
check("plan_done bumps a counter the Stick compares", [x for x in state()["sessions"] if x["id"] == "s1"][0]["plan_done_n"] == before + 1)
post("/usage", {"session": "s1", "ctx_pct": 42.5, "cost": 1.25, "model": "Opus 5.5",
                "five": {"pct": 61, "resets": 1_800_003_600}, "seven": {"pct": 12, "resets": 1_800_500_000}})
s = state()
s1 = [x for x in s["sessions"] if x["id"] == "s1"][0]
check("usage: per-session ctx % and model name", s1["ctx_pct"] == 42.5 and s1["model_name"] == "Opus 5.5", s1)
check("usage: account limits with reset times", s["limits"]["five"] == {"pct": 61, "resets": 1_800_003_600} and
      s["limits"]["seven"]["pct"] == 12, s.get("limits"))
clock.t += 90
check("limits report their age", state()["limits"]["age_s"] == 90)
n = len(state()["sessions"])
post("/usage", {"session": "nobody", "ctx_pct": 1})
check("usage never creates a session", len(state()["sessions"]) == n)
post("/usage", {"session": "s1", "ctx_pct": "NaN", "five": {"pct": float("nan")}})
check("junk usage values are ignored", state()["limits"]["five"]["pct"] == 61)
post("/event", {"session": "s2", "event": "end"})
check("end removes the session", [x["id"] for x in state()["sessions"]] == ["s1"])

# ----------------------------------------------------------------- state versions
s = state()
v = s["v"]
clock.t += 5
check("nothing changed -> ?v= answers {same:true}, tiny", state(v=v) == {"v": v, "now": s["now"] + 5, "same": True}, state(v=v))
check("time passing alone (since_s, ages) does not bump the version", state()["v"] == v)
post("/event", {**HOOK, "event": "waiting", "detail": "x"})
s = state(v=v)
check("a change bumps v and sends the full state", s["v"] == v + 1 and "sessions" in s and "same" not in s, s.get("v"))
check("state at the current v is 'same'", state(v=s["v"]).get("same") is True)
check("a stale v gets the full state", "sessions" in state(v=1))
check("garbage v is ignored", "sessions" in get("/state?v=abc")[1])
check("since_s counts up", [x for x in state()["sessions"] if x["id"] == "s1"][0]["since_s"] >= 0)

# the Stick asks for a trimmed view (?n=1): its newest few sessions, oldest few prompts, shorter text
for k in range(10):
    post("/event", {"session": f"lite{k}", "event": "working"})
    post("/permission", {"session": f"lite{k}", "tool": "Bash", "detail": "x" * 2000})
lite = get("/state?n=1")[1]
full = get("/state")[1]
check("lite state: newest 6 sessions, oldest 8 approvals, clipped detail",
      len(lite["sessions"]) == br.LITE_SESSIONS and lite["sessions"][-1]["id"] == "lite9" and
      len(lite["approvals"]) == br.LITE_APPROVALS and len(lite["approvals"][0]["detail"]) == br.LITE_DETAIL, (len(lite["sessions"]), len(lite["approvals"])))
check("the full view still has everything (the phone page)", len(full["approvals"]) >= 10 and len(full["approvals"][0]["detail"]) > br.LITE_DETAIL)
check("both views share one version number", lite["v"] == full["v"])
for a_ in full["approvals"]:
    call("DELETE", f"/permission?id={a_['id']}")
for k in range(10):
    post("/event", {"session": f"lite{k}", "event": "end"})

# ----------------------------------------------------------------- sessions come and go
clock.t += br.SESSION_IDLE_S + 1
state()
check("idle sessions expire after 30 min", state()["sessions"] == [])
for k in range(br.MAX_SESSIONS + 4):
    clock.t += 1
    post("/event", {"session": f"many{k}", "event": "working"})
check("session table is bounded, oldest dropped", len(state()["sessions"]) == br.MAX_SESSIONS and
      state()["sessions"][-1]["id"] == f"many{br.MAX_SESSIONS + 3}")

# ----------------------------------------------------------------- settings
c = get("/config")[1]
check("config: secrets are hidden by default", "stt_key" not in c and "ntfy_token" not in c and c["stt_key_set"] is False, c)
c = post("/config", {"stt_key": "sk-secret", "ntfy_token": "tk_secret", "stt_url": "http://x:8000/v1/audio/transcriptions",
                     "public_url": "https://stick.example.com", "permission_wait_s": 999999, "remote_permission_wait_s": 1,
                     "stop_menu": "weird", "remote": "on", "bogus": 1, "stt_model": 5})[1]
check("config: secrets stay hidden in the reply, flags say they're set", "sk-secret" not in json.dumps(c) and c["stt_key_set"], c)
check("config: numbers clamped, junk ignored", c["permission_wait_s"] == 3600 and c["remote_permission_wait_s"] == 5 and
      c["stop_menu"] == "remote" and c["remote"] == "on" and "bogus" not in c and c["stt_model"] == "whisper-1", c)
check("config: hooks can ask for the secrets", get("/config?secrets=1")[1]["stt_key"] == "sk-secret")
check("config: saved to disk (0600) and reloaded",
      br.Store(data).config["public_url"] == "https://stick.example.com" and
      oct(os.stat(data + "/config.json").st_mode & 0o777) == "0o600")
m = [{"label": f"m{i}", "text": f"text {i}"} for i in range(15)] + [{"label": "", "text": "x"}, {"label": "y"}, "junk"]
c = post("/config", {"macros": m})[1]
check("config: macros capped at 10, empty/invalid ones dropped", len(c["macros"]) == 10 and c["macros"][0]["label"] == "m0", len(c["macros"]))
check("config: an empty macro list restores the defaults", len(post("/config", {"macros": []})[1]["macros"]) == 6)
check("state lists macro labels", state()["macros"][0] == "Continue")
post("/config", {"remote": "auto"})

# ----------------------------------------------------------------- speech to text proxy
SEEN = {}


class Stt(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        SEEN.update(auth=self.headers.get("Authorization"), ctype=self.headers.get("Content-Type"), body=body)
        code = SEEN.get("code", 200)
        out = json.dumps({"text": "  hello from the stick  "} if code == 200 else {"error": "x"}).encode()
        self.send_response(code)
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)


stt = ThreadingHTTPServer(("127.0.0.1", 0), Stt)
threading.Thread(target=stt.serve_forever, daemon=True).start()
wav = b"RIFF" + b"\0" * 80
post("/config", {"stt_url": "", "stt_key": ""})
check("stt: not configured -> 503 with a clear message", call("POST", "/stt", raw=wav, headers={"Content-Type": "audio/wav"})[0] == 503)
post("/config", {"stt_url": f"http://127.0.0.1:{stt.server_port}/v1/audio/transcriptions", "stt_key": "sk-secret",
                 "stt_model": "tiny", "stt_language": "en"})
st, j, _ = call("POST", "/stt", raw=wav, headers={"Content-Type": "audio/wav"})
check("stt: a WAV in, text out", st == 200 and j == {"text": "hello from the stick"}, (st, j))
check("stt: upstream got a multipart with file, model, language and the key",
      SEEN["auth"] == "Bearer sk-secret" and SEEN["ctype"].startswith("multipart/form-data; boundary=") and
      b'name="file"; filename="speech.wav"' in SEEN["body"] and b"\r\n\r\ntiny" in SEEN["body"] and
      b'name="language"\r\n\r\nen' in SEEN["body"] and wav in SEEN["body"], SEEN.get("body", b"")[:200])
SEEN["code"] = 500
check("stt: upstream error -> 502", call("POST", "/stt", raw=wav, headers={"Content-Type": "audio/wav"})[0] == 502)
SEEN["code"] = 200
check("stt: a tiny body is rejected", call("POST", "/stt", raw=b"x")[0] == 400)
check("stt: oversized audio -> 413", call("POST", "/stt", raw=b"\0" * (br.MAX_AUDIO + 1))[0] == 413)
check("stt: needs the token", call("POST", "/stt", raw=wav, token="")[0] == 401)
stt.shutdown()
post("/config", {"stt_url": "http://127.0.0.1:1/x"})
check("stt: unreachable server -> 502, not a crash", call("POST", "/stt", raw=wav)[0] == 502)

# ----------------------------------------------------------------- setup page data
st, j = get("/setup/info")
check("setup/info: bridge flag, token, lan address", st == 200 and j["bridge"] and j["done"] and j["token"] == TOKEN and
      j["port"] == PORT and j["ip"], j)
check("setup/info needs the token", get("/setup/info", token="")[0] == 401)

# ----------------------------------------------------------------- TLS
if shutil.which("openssl"):
    d = tempfile.mkdtemp()
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", d + "/k.pem", "-out", d + "/c.pem",
                    "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1"],
                   capture_output=True, check=True)
    tsrv = br.make_server(store, "127.0.0.1", 0, tls_cert=d + "/c.pem", tls_key=d + "/k.pem", core=core, guard=guard)
    threading.Thread(target=tsrv.serve_forever, daemon=True).start()
    ctx = ssl.create_default_context(cafile=d + "/c.pem")
    c = http.client.HTTPSConnection("127.0.0.1", tsrv.server_port, context=ctx, timeout=10)
    for _ in range(2):   # twice: keep-alive over TLS
        c.request("GET", "/state", headers={"X-Token": TOKEN})
        r = c.getresponse()
        body = json.loads(r.read())
    check("tls: serves HTTPS with keep-alive", r.status == 200 and "sessions" in body)
    c.close()
    plain = http.client.HTTPConnection("127.0.0.1", tsrv.server_port, timeout=3)
    try:
        plain.request("GET", "/health")
        plain.getresponse().read()
        ok = False
    except (OSError, http.client.HTTPException):
        ok = True
    check("tls: plain HTTP to the TLS port is refused", ok)
    tsrv.shutdown()
else:
    print("skip tls (openssl not installed)")

# ----------------------------------------------------------------- connection cap and a stuck client
socks = []
import socket  # noqa: E402
small = br.make_server(store, "127.0.0.1", 0, core=core, guard=guard)
small.slots = threading.BoundedSemaphore(2)
threading.Thread(target=small.serve_forever, daemon=True).start()
for _ in range(2):
    s_ = socket.create_connection(("127.0.0.1", small.server_port))
    socks.append(s_)   # idle connections holding the two slots
time.sleep(0.2)
s3 = socket.create_connection(("127.0.0.1", small.server_port))
s3.settimeout(3)
s3.sendall(b"GET /health HTTP/1.1\r\nHost: x\r\n\r\n")
try:
    got = s3.recv(100)
except OSError:
    got = b""
check("connection cap: the third connection is refused", got == b"")
for s_ in socks + [s3]:
    s_.close()
small.shutdown()

# ----------------------------------------------------------------- the version matches the firmware
fw = __import__("re").search(r'FW_VERSION\s*=\s*"([^"]+)"', open(ROOT + "/firmware/src/main.cpp").read()).group(1)
check(f"bridge version == firmware version ({fw})", br.VERSION == fw, (br.VERSION, fw))

srv.shutdown()
print("\nbridge: %d failed" % len(FAILED) if FAILED else "\nbridge: all passed")
sys.exit(1 if FAILED else 0)
