"""The real Claude Code hook, installed from a bridge, talking to a bridge: approvals, Done cards, voice text, usage."""
import http.client
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, ROOT + "/bridge")
import claudestick_bridge as br  # noqa: E402

FAILED = []
TOKEN = "b" * 32


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (("  | " + str(extra)[:200]) if extra and not cond else ""))
    if not cond:
        FAILED.append(name)
    return cond


# --- a bridge on the real clock, an ntfy sink, and a fake "Stick" that polls so the bridge sees a client
store = br.Store(tempfile.mkdtemp())
store.ident["token"] = TOKEN
store.ensure_identity()
core = br.Core(store)
srv = br.make_server(store, "127.0.0.1", 0, core=core)
PORT = srv.server_port
threading.Thread(target=srv.serve_forever, daemon=True).start()
NT = []


class Ntfy(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        NT.append({"title": self.headers.get("Title"), "actions": self.headers.get("Actions"),
                   "click": self.headers.get("Click"), "auth": self.headers.get("Authorization")})
        self.rfile.read(int(self.headers["Content-Length"]))
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()


ntfy = ThreadingHTTPServer(("127.0.0.1", 0), Ntfy)
threading.Thread(target=ntfy.serve_forever, daemon=True).start()

STICK = {"on": False}


def stick_loop():
    while True:
        if STICK["on"]:
            core.poll("stick@test")
        time.sleep(0.3)


threading.Thread(target=stick_loop, daemon=True).start()
home = tempfile.mkdtemp()
work = tempfile.mkdtemp()
env = dict(os.environ, HOME=home)


def api(method, path, body=None):
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
    c.request(method, path, body=json.dumps(body).encode() if body is not None else None,
              headers={"X-Token": TOKEN, "Content-Type": "application/json"})
    r = c.getresponse()
    out = json.loads(r.read() or b"{}")
    c.close()
    return out


def press(**kw):
    return api("POST", "/act", kw)


# --- install straight from the bridge: no checkout needed on the machine running Claude Code
r = subprocess.run(["bash", "-c", f"curl -s http://127.0.0.1:{PORT}/install.py | python3 - --host 127.0.0.1 --port {PORT} --token {TOKEN}"],
                   cwd=work, env=env, capture_output=True, text=True)
local = json.load(open(home + "/.claudestick/config.json"))
check("install: config holds only connection + identity", set(local) <= {"host", "port", "token", "device_id", "mdns_host",
                                                                         "added_todo_env", "statusline_chain"}, local)
check("install: the bridge's id is recorded (so the hook can find it again)", local.get("device_id") == store.ident["id"], local)
check("install: reachable, token accepted", "token OK" in r.stdout and "device OK" in r.stdout, r.stdout + r.stderr)
check("install: 8 hooks", len(json.load(open(home + "/.claude/settings.json"))["hooks"]) == 8)
HOOK = home + "/.claudestick/claudestick_hook.py"
check("the installed hook is the bridge's copy", open(HOOK, "rb").read() == open(ROOT + "/hooks/claudestick_hook.py", "rb").read())

# --- install with a one-time pairing code instead of the token
def install_with(extra, home_dir, extra_env=None):
    e = dict(os.environ, HOME=home_dir, **(extra_env or {}))
    return subprocess.run(["bash", "-c", f"curl -s http://127.0.0.1:{PORT}/install.py | python3 - {extra}"],
                          cwd=work, env=e, capture_output=True, text=True), e


home_p = tempfile.mkdtemp()
code = api("POST", "/pair/new")["code"]
r, _ = install_with(f"--host 127.0.0.1 --port {PORT} --pair '{code[:3]} {code[3:]}'", home_p)
lp = json.load(open(home_p + "/.claudestick/config.json")) if os.path.exists(home_p + "/.claudestick/config.json") else {}
check("pair install: a spaced code is accepted and the token ends up in the local config", lp.get("token") == TOKEN and "token OK" in r.stdout, r.stdout + r.stderr)
check("pair install: the code is not stored anywhere", code not in open(home_p + "/.claudestick/config.json").read())
home_q = tempfile.mkdtemp()
r, _ = install_with(f"--host 127.0.0.1 --port {PORT} --pair {code}", home_q)
check("pair install: a code works once", r.returncode != 0 and "wrong or has expired" in (r.stdout + r.stderr) and not os.path.exists(home_q + "/.claudestick/config.json"), r.stdout + r.stderr)
r, _ = install_with(f"--host 127.0.0.1 --port {PORT} --pair 12345", home_q)
check("pair install: a short code is refused (nothing installed)", r.returncode != 0 and not os.path.exists(home_q + "/.claudestick/config.json"), r.stdout + r.stderr)
r, _ = install_with(f"--host 127.0.0.1 --port {PORT} --pair 123456 --token {TOKEN}", home_q)
check("pair install: --pair and --token together are refused", r.returncode != 0 and "not both" in (r.stdout + r.stderr), r.stdout + r.stderr)

# --- a bridge on https (self-signed here): install by code, then the hook talks to it
cert_dir = tempfile.mkdtemp()
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", cert_dir + "/k.pem", "-out", cert_dir + "/c.pem",
                "-days", "2", "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost"], capture_output=True, check=True)
tls_srv = br.make_server(store, "127.0.0.1", 0, core=core, tls_cert=cert_dir + "/c.pem", tls_key=cert_dir + "/k.pem")
TLS_PORT = tls_srv.server_port
threading.Thread(target=tls_srv.serve_forever, daemon=True).start()
HTTPS = f"https://127.0.0.1:{TLS_PORT}"
home_s = tempfile.mkdtemp()
trust = {"SSL_CERT_FILE": cert_dir + "/c.pem"}
code = api("POST", "/pair/new")["code"]
r = subprocess.run(["python3", ROOT + "/install.py", "--host", HTTPS, "--pair", code], cwd=work, env=dict(os.environ, HOME=home_s),
                   capture_output=True, text=True)
check("https: an untrusted certificate is refused, never skipped", r.returncode != 0 and not os.path.exists(home_s + "/.claudestick/config.json"), r.stdout + r.stderr)
code = api("POST", "/pair/new")["code"]
r = subprocess.run(["python3", ROOT + "/install.py", "--host", HTTPS, "--pair", code], cwd=work, env=dict(os.environ, HOME=home_s, **trust),
                   capture_output=True, text=True)
ls = json.load(open(home_s + "/.claudestick/config.json")) if os.path.exists(home_s + "/.claudestick/config.json") else {}
check("https: installs by code over a trusted certificate and keeps the full address as the host", ls.get("host") == HTTPS and ls.get("token") == TOKEN and "token OK" in r.stdout,
      (ls, r.stdout + r.stderr))
env_s = dict(os.environ, HOME=home_s, **trust)
tp_s = home_s + "/t.jsonl"
open(tp_s, "w").write("")
p = subprocess.Popen([sys.executable, home_s + "/.claudestick/claudestick_hook.py"], env=env_s, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
p.stdin.write(json.dumps({"session_id": "hs1", "cwd": work, "transcript_path": tp_s, "hook_event_name": "UserPromptSubmit", "prompt": "over https"}))
p.stdin.close()
p.wait(timeout=20)
seen = False
for _ in range(80):
    seen = any(x["id"] == "hs1" for x in core.poll(None)["sessions"])
    if seen:
        break
    time.sleep(0.1)
check("https: the hook reaches the bridge and registers its session", seen, core.poll(None))
tls_srv.shutdown()

repo = os.path.join(work, "api-server")
os.makedirs(repo)
git_env = {**env, "GIT_AUTHOR_NAME": "a", "GIT_AUTHOR_EMAIL": "a@b", "GIT_COMMITTER_NAME": "a", "GIT_COMMITTER_EMAIL": "a@b"}
subprocess.run(["git", "init", "-q", "-b", "fix-auth", repo])
subprocess.run(["git", "-C", repo, "commit", "-q", "--allow-empty", "-m", "x"], env=git_env)
tp = home + "/t.jsonl"


def transcript(t):
    open(tp, "w").write(json.dumps({"type": "assistant", "message": {"content": [{"type": "text", "text": t}]}}) + "\n")


transcript("Done with phase 1.")


def start(ev, sid="h1"):
    ev = {"session_id": sid, "cwd": repo, "transcript_path": tp, **ev}
    p = subprocess.Popen([sys.executable, HOOK], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    p.stdin.write(json.dumps(ev))
    p.stdin.close()
    return p


def finish(p, timeout=20):
    out = p.stdout.read()
    err = p.stderr.read()
    p.wait(timeout=timeout)
    return (json.loads(out) if out.strip() else None), err


def wait_for(fn, timeout=8):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.1)
    return None


def snap():
    return core.poll(None)   # reading the state without counting as a connected client


def pending():
    return snap()["approvals"]


# --- settings (as the setup page saves them) reach the hook, secrets included
api("POST", "/config", {"permission_wait_s": 2, "ntfy_url": f"http://127.0.0.1:{ntfy.server_port}/claude",
                        "ntfy_token": "tk_x", "public_url": "https://stick.example.com/", "remote": "on"})
finish(start({"hook_event_name": "SessionStart", "source": "startup"}))
st = snap()
check("session appears with its git branch", [x["project"] for x in st["sessions"] if x["id"] == "h1"] == ["api-server@fix-auth"], st["sessions"])
dc = json.load(open(home + "/.claudestick/device_config.json"))
check("hook fetched the settings including ntfy secrets", dc.get("ntfy_token") == "tk_x" and dc.get("public_url") == "https://stick.example.com/", dc)

# --- nobody is connected: remote is off, so the terminal asks after a short wait (never hangs for 10 minutes)
p = start({"hook_event_name": "PermissionRequest", "tool_name": "Bash", "tool_input": {"command": "npm test"}})
t0 = time.time()
o, _ = finish(p)
check("no Stick connected: falls back to the terminal prompt within seconds", o is None and time.time() - t0 < 10, (o, time.time() - t0))
check("...and leaves no stale approval behind", wait_for(lambda: not pending(), 3))

# --- the Stick connects (from work): approvals now wait for it
STICK["on"] = True
time.sleep(0.6)
core.conf["permission_wait_s"] = 2
check("with the Stick connected, remote is on", snap()["remote"] is True)
p = start({"hook_event_name": "PermissionRequest", "tool_name": "Bash", "tool_input": {"command": "npm run build"}})
a = wait_for(pending)
check("approval reaches the Stick, with the project name", a and a[0]["project"] == "api-server@fix-auth" and a[0]["tool"] == "Bash", a)
wait_for(lambda: NT and NT[-1]["actions"])   # the hook posts the ntfy push right after it files the approval
check("ntfy push has Approve/Always/Deny buttons that point at the public URL",
      NT and NT[-1]["actions"].count("http,") >= 2 and
      all(x.split(",")[2].strip().startswith("https://stick.example.com/act?action=") for x in NT[-1]["actions"].split(";")) and
      NT[-1]["click"].startswith("https://stick.example.com/?t="), NT[-1:])
# press the ntfy "Approve" button the way the phone would (against the local bridge instead of the public name)
btn = NT[-1]["actions"].split(";")[0].split(",")[2].strip()
q = parse_qs(urlparse(btn).query)
check("button URL carries action, id and token", q["action"] == ["approve"] and q["t"] == [TOKEN] and q["id"] == [str(a[0]["id"])], q)
req = urllib.request.Request(f"http://127.0.0.1:{PORT}/act?" + urlparse(btn).query, method="POST")
check("pressing the button approves", json.loads(urllib.request.urlopen(req).read()) == {"ok": True})
o, _ = finish(p)
check("hook gets 'allow'", (o or {}).get("hookSpecificOutput", {}).get("decision", {}).get("behavior") == "allow", o)

p = start({"hook_event_name": "PermissionRequest", "tool_name": "Bash", "tool_input": {"command": "git status --short"}})
a = wait_for(pending)
press(action="always", id=a[0]["id"])
o, _ = finish(p)
perm = (o or {}).get("hookSpecificOutput", {}).get("decision", {})
check("always-allow becomes a rule", perm.get("updatedPermissions", [{}])[0].get("rules", [{}])[0].get("ruleContent") == "git status", perm)

p = start({"hook_event_name": "PermissionRequest", "tool_name": "Bash", "tool_input": {"command": "rm -rf build"}})
a = wait_for(pending)
check("risky command carries its warning to the Stick", a and a[0]["risk"] == "high" and a[0]["always"] == "", a)
press(action="deny", id=a[0]["id"], text="keep the build folder")
o, _ = finish(p)
d = (o or {}).get("hookSpecificOutput", {}).get("decision", {})
check("deny + spoken reason reaches Claude", d.get("behavior") == "deny" and "keep the build folder" in d.get("message", ""), o)

p = start({"hook_event_name": "PermissionRequest", "tool_name": "Bash", "tool_input": {"command": "make"}})
a = wait_for(pending)
press(action="keyboard", id=a[0]["id"])
o, _ = finish(p)
check("'answer at keyboard' hands the prompt back to the terminal", o is None, o)

# --- Done card: Claude's own options + your macros, answered with text typed on the phone
transcript("Backups migrated.\n\nOptions: Run real backup | Add retention")
p = start({"hook_event_name": "Stop"})
c = wait_for(lambda: snap()["choices"])
check("Done card = Claude's options then your macros", c and c[0]["options"][:3] == ["Run real backup", "Add retention", "Continue"], c)
press(action="say", session="h1", text="use postgres")
o, _ = finish(p)
check("text sent from the phone/Stick answers the Done card", (o or {}).get("reason", "").endswith("use postgres"), o)

p = start({"hook_event_name": "Stop"})
c = wait_for(lambda: snap()["choices"])
press(action="choose", id=c[0]["id"], index=len(c[0]["options"]) - 1)
o, _ = finish(p)
check("choosing a macro continues Claude with its text", "Commit the current work" in (o or {}).get("reason", ""), o)

# --- stop and queued text arrive on the next tool call
press(action="stop", session="h1")
o, _ = finish(start({"hook_event_name": "PreToolUse", "tool_name": "Bash", "tool_input": {"command": "ls"}}))
check("Stop from the Stick halts Claude", (o or {}).get("continue") is False, o)
press(action="say", session="h1", text="also run the linter")
o, _ = finish(start({"hook_event_name": "PostToolUse", "tool_name": "Bash", "tool_input": {}, "tool_response": {}}))
check("text queued mid-run is delivered after the next tool", "also run the linter" in json.dumps(o or {}), o)

# --- "send now": the next tool call is held back and the message handed over
finish(start({"hook_event_name": "UserPromptSubmit", "prompt": "go"}, sid="p1"))
press(action="say", session="p1", text="change of plan")
o, _ = finish(start({"hook_event_name": "PreToolUse", "tool_name": "Bash", "tool_input": {"command": "ls"}}, sid="p1"))
check("a queued message doesn't hold up the next tool", o is None, o)
press(action="sendnow", session="p1")
o, _ = finish(start({"hook_event_name": "PreToolUse", "tool_name": "Bash", "tool_input": {"command": "ls"}}, sid="p1"))
hso = (o or {}).get("hookSpecificOutput", {})
check("send now holds the next tool back and hands over the message", hso.get("permissionDecision") == "deny" and
      "change of plan" in hso.get("permissionDecisionReason", ""), o)
o, _ = finish(start({"hook_event_name": "PreToolUse", "tool_name": "Bash", "tool_input": {"command": "ls"}}, sid="p1"))
check("...once", o is None, o)

# --- the idle-spell watcher: a Stop hook (async + asyncRewake) that wakes the session when text arrives
wenv = dict(env, CLAUDESTICK_WATCH_POLL="0.3")


def watcher(sid):
    w = subprocess.Popen([sys.executable, HOOK, "--watch"], env=wenv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True)
    w.stdin.write(json.dumps({"session_id": sid, "cwd": repo, "transcript_path": tp, "hook_event_name": "Stop"}))
    w.stdin.close()
    return w


finish(start({"hook_event_name": "UserPromptSubmit", "prompt": "hi"}, sid="w1"))
w = watcher("w1")
time.sleep(1.0)
check("the watcher keeps running while nothing arrives", w.poll() is None)
press(action="say", session="w1", text="please also check the logs")
rc = w.wait(timeout=10)
err = w.stderr.read()
check("text from the Stick wakes it: exit code 2, the text on stderr", rc == 2 and "please also check the logs" in err and "ClaudeStick" in err, (rc, err))
w = watcher("w1")
time.sleep(0.8)
finish(start({"hook_event_name": "UserPromptSubmit", "prompt": "next"}, sid="w1"))
rc = w.wait(timeout=10)
check("a new prompt makes it stand down quietly", rc == 0 and w.stderr.read() == "", rc)
w_old = watcher("w1")
time.sleep(0.8)
w_new = watcher("w1")
try:
    old_rc = w_old.wait(timeout=10)
except subprocess.TimeoutExpired:
    old_rc = None
check("a newer watcher replaces the older one", old_rc is not None and w_new.poll() is None, (old_rc, w_new.poll()))
finish(start({"hook_event_name": "SessionEnd"}, sid="w1"))
try:
    end_rc = w_new.wait(timeout=10)
except subprocess.TimeoutExpired:
    end_rc = None
check("the session ending stops its watcher", end_rc is not None, end_rc)
w = watcher("w2")
time.sleep(0.8)
check("the watcher keeps a quiet session listed on the bridge", any(x["id"] == "w2" for x in snap()["sessions"]), snap()["sessions"])
w.kill()
w.wait()

# --- the installer adds the watcher only where Claude Code can run it in the background
def install_settings(extra, claude_version=None):
    h = tempfile.mkdtemp()
    e = dict(os.environ, HOME=h)
    if claude_version:
        fake = tempfile.mkdtemp()
        open(fake + "/claude", "w").write(f"#!/bin/sh\necho '{claude_version} (Claude Code)'\n")
        os.chmod(fake + "/claude", 0o755)
        e["PATH"] = fake + os.pathsep + e["PATH"]
    r = subprocess.run(["bash", "-c", f"curl -s http://127.0.0.1:{PORT}/install.py | python3 - --host 127.0.0.1 --port {PORT} --token {TOKEN} {extra}"],
                       cwd=work, env=e, capture_output=True, text=True)
    st = json.load(open(h + "/.claude/settings.json")) if os.path.exists(h + "/.claude/settings.json") else {}
    stops = [x for g in st.get("hooks", {}).get("Stop", []) for x in g.get("hooks", [])]
    return h, e, r, [x for x in stops if x.get("asyncRewake")]


h1, e1, r1, wk = install_settings("", "2.1.200")
check("install: a recent Claude Code gets the Stop watcher (async + asyncRewake, --watch)", len(wk) == 1 and wk[0].get("async") is True and
      wk[0]["command"].endswith("--watch"), (wk, r1.stdout))
check("install: it is the second Stop hook, next to the normal one", len(json.load(open(h1 + "/.claude/settings.json"))["hooks"]["Stop"]) == 2)
r1b = subprocess.run(["bash", "-c", f"curl -s http://127.0.0.1:{PORT}/install.py | python3 - --uninstall"], cwd=work, env=e1, capture_output=True, text=True)
check("uninstall removes it too", "asyncRewake" not in open(h1 + "/.claude/settings.json").read(), r1b.stdout + r1b.stderr)
check("install: an old Claude Code doesn't (it would hold up every Stop)", not install_settings("", "2.0.5")[3])
h3, e3, r3, wk3 = install_settings("", "no idea")   # a `claude` that doesn't say what version it is
check("install: unknown version -> not installed, and it says how to ask for it", not wk3 and "--wake" in r3.stdout, r3.stdout)
check("install: --wake installs it regardless", len(install_settings("--wake", "2.0.5")[3]) == 1)
check("install: --no-wake skips it even on a recent version", not install_settings("--no-wake", "2.1.200")[3])

# --- status line: plan limits and context % reach the bridge
DATA = {"session_id": "h1", "workspace": {"current_dir": repo}, "context_window": {"used_percentage": 42.5, "context_window_size": 200000},
        "cost": {"total_cost_usd": 1.84}, "model": {"display_name": "Opus 5.5"},
        "rate_limits": {"five_hour": {"used_percentage": 63.2, "resets_at": 1793398040},
                        "seven_day": {"used_percentage": 28, "resets_at": 1793663600}}}
r = subprocess.run([sys.executable, HOOK, "--statusline"], input=json.dumps(DATA), env=env, capture_output=True, text=True, timeout=10)
st = wait_for(lambda: snap().get("limits"))
check("status line forwards the 5h/7d limits", st and round(st["five"]["pct"]) == 63 and round(st["seven"]["pct"]) == 28, st)
s1 = [s for s in snap()["sessions"] if s["id"] == "h1"][0]
check("...and context % + model name", s1.get("ctx_pct") == 42.5 and s1.get("model_name") == "Opus 5.5", s1)

# --- the Stick disconnects mid-run: new prompts stop waiting for it
STICK["on"] = False
core.clients.clear()
o, _ = finish(start({"hook_event_name": "UserPromptSubmit", "prompt": "hello"}))
check("nobody connected -> remote off again for new prompts", snap()["remote"] is False)

srv.shutdown()
print("\nbridge hooks: %d failed" % len(FAILED) if FAILED else "\nbridge hooks: all passed")
sys.exit(1 if FAILED else 0)
