#!/usr/bin/env python3
"""ClaudeStick hook for Claude Code (stdlib only).

One script handles every hook event. It reads the event JSON on stdin, talks to
the StickS3 over HTTP on your LAN, and prints a decision when the device gives
one. If the device is unreachable it exits silently and Claude Code behaves
exactly as if the hook weren't installed.

Local config:  ~/.claudestick/config.json  (host, port, token, device_id; override path
               with CLAUDESTICK_CONFIG). Everything else (macros, waits, alerts) lives on
               the Stick and is cached in ~/.claudestick/device_config.json.
Disable for one shell: CLAUDESTICK=0     Label a session: CLAUDESTICK_NAME=whatever
"""
import concurrent.futures
import hashlib
import json
import os
import re
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

CONFIG_PATH = os.environ.get("CLAUDESTICK_CONFIG", os.path.expanduser("~/.claudestick/config.json"))
DEVICE_CFG = os.path.join(os.path.dirname(CONFIG_PATH), "device_config.json")
TMP = tempfile.gettempdir()
UID = str(os.getuid()) if hasattr(os, "getuid") else os.environ.get("USERNAME", "u")
DOWN_FILE = os.path.join(TMP, f"claudestick-down-{UID}")
DISCOVER_FILE = os.path.join(TMP, f"claudestick-discover-{UID}")
MODE_DIR = os.path.join(TMP, f"claudestick-mode-{UID}")
SESS_DIR = os.path.join(os.path.dirname(CONFIG_PATH), "sessions")   # per-session to-do list + token totals
DOWN_BACKOFF_S = 20
DISCOVER_BACKOFF_S = 120
DEVICE_CFG_TTL_S = 60
POLL_S = 0.4
USAGE_EVERY_S = 20          # PostToolUse sends token totals at most this often
TODO_MAX = 12

DEFAULT_MACROS = [
    {"label": "Continue", "text": "Continue."},
    {"label": "Keep going", "text": "Keep going until the task is fully done. Only stop if you are truly blocked."},
    {"label": "Next phase", "text": "That phase looks good. Proceed to the next phase of the plan."},
    {"label": "Run tests + fix", "text": "Run the test suite and fix any failures before continuing."},
    {"label": "Summarize", "text": "Summarize what you did, what's left, and any risks, in under 10 bullets."},
    {"label": "Commit", "text": "Commit the current work with a clear, conventional commit message."},
]

REMOTE_ON = (
    "[ClaudeStick remote mode ON] The user is away from the keyboard, steering this session from a 2-button "
    "handheld with a tiny screen (and sometimes a phone). Until told otherwise: "
    "(1) When you need a decision or clarification, use the AskUserQuestion tool with 2-4 options whose labels are "
    "at most 20 characters; never ask open-ended questions in prose. "
    "(2) Keep end-of-turn messages to 2-3 short sentences. "
    "(3) End every turn with one final line exactly in the form `Options: <first> | <second> | <third>` listing up to "
    "3 concrete next steps, each at most 30 characters. "
    "(4) Keep going autonomously through low-risk steps instead of stopping to check in, and batch risky actions so "
    "approvals are few."
)
REMOTE_OFF = (
    "[ClaudeStick remote mode OFF] The user is back at the keyboard. Return to normal formatting, ask questions however "
    "suits best, and stop adding the `Options:` line."
)

# Things that get a red DANGER card on the Stick and need a deliberate hold to allow.
RISKY = [
    (r"\brm\s+(-[a-zA-Z]*[rR][a-zA-Z]*f|-[a-zA-Z]*f[a-zA-Z]*[rR]|--recursive\b.*--force|--force\b.*--recursive)", "recursive force delete"),
    (r"\bgit\s+push\b.*(\s-f\b|--force(?!-with-lease))", "force push"),
    (r"\bgit\s+reset\s+--hard\b", "discards local changes"),
    (r"\bgit\s+clean\s+-[a-zA-Z]*f", "deletes untracked files"),
    (r"\bgit\s+(branch\s+-D|push\s+\S+\s+--delete|push\s+\S+\s+:)", "deletes a branch"),
    (r"\b(zfs|zpool)\s+destroy\b", "destroys ZFS data"),
    (r"\b(pct|qm)\s+destroy\b", "destroys a Proxmox guest"),
    (r"\b(pvesm\s+free|lvremove|vgremove|pvremove)\b", "deletes storage"),
    (r"\bdd\s+.*\bof=/dev/", "writes a raw device"),
    (r"\b(mkfs(\.\w+)?|wipefs|sgdisk\s+--zap|fdisk|parted)\b", "repartitions / formats a disk"),
    (r">\s*/dev/(sd|nvme|hd|vd)", "writes a raw device"),
    (r"\bchmod\s+-R\s+0?777\b", "world-writable permissions"),
    (r"\b(curl|wget)\b[^|]*\|\s*(sudo\s+)?(ba|z)?sh\b", "pipes a download into a shell"),
    (r"\b(DROP\s+(TABLE|DATABASE|SCHEMA)|TRUNCATE\s+TABLE)\b", "drops database data"),
    (r"\bkubectl\s+delete\b", "deletes Kubernetes resources"),
    (r"\bterraform\s+(destroy|apply\s+.*-auto-approve)\b", "infrastructure change without review"),
    (r"\bdocker\s+(system|volume)\s+prune\b", "prunes Docker data"),
    (r"\b(shutdown|reboot|poweroff|halt)\b", "power action"),
    (r"\b(iptables\s+-F|nft\s+flush)\b", "flushes firewall rules"),
    (r"\bsystemctl\s+(stop|disable|mask)\s+(sshd?|networking|pve\w*|wings|docker)\b", "stops a critical service"),
]
SENSITIVE_PATHS = re.compile(r"(^/etc/|/\.ssh/|(^|/)\.env(\.|$)|id_rsa|authorized_keys|/\.claude/settings)")


def _read_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def _write_json(path, data):
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f, indent=2)
        os.replace(tmp, path)
    except OSError:
        pass


def load_config():
    """Defaults < settings from the Stick (cached) < local file (connection + overrides)."""
    cfg = {
        "host": "claudestick.local",
        "port": 7880,
        "token": "",
        "device_id": "",
        "permission_wait_s": 20,          # at the desk: short, then the terminal prompt appears
        "remote_permission_wait_s": 600,  # remote mode: wait for the Stick / phone
        "stop_wait_s": 600,               # Done menu wait (only shown when stop_menu allows)
        "stop_menu": "remote",            # "remote" | "always" | "never"
        "questions_on_device": True,      # show AskUserQuestion on the Stick in remote mode
        "remote_on_text": REMOTE_ON,
        "remote_off_text": REMOTE_OFF,
        "ntfy_url": "",
        "ntfy_token": "",
        "macros": DEFAULT_MACROS,
    }
    local = _read_json(CONFIG_PATH) or {}
    device = _read_json(DEVICE_CFG) or {}
    cfg.update({k: v for k, v in device.items() if k in cfg and v not in (None, "")})
    # Local keys that only make sense per machine always win; older installs may also
    # carry macros/waits locally, which the Stick's settings now replace.
    for k in ("host", "port", "token", "device_id", "remote_on_text", "remote_off_text"):
        if k in local:
            cfg[k] = local[k]
    for k, v in local.items():
        if k not in cfg:
            cfg[k] = v
    return cfg


# ------------------------------------------------------------------ transport

_lan = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # never use HTTP(S)_PROXY for the LAN


def _recent(path, seconds):
    try:
        return time.time() - os.path.getmtime(path) < seconds
    except OSError:
        return False


def _touch(path):
    try:
        with open(path, "w") as f:
            f.write(str(time.time()))
    except OSError:
        pass


def device_down():
    return _recent(DOWN_FILE, DOWN_BACKOFF_S)


def _http_json(url, token="", method="GET", body=None, timeout=1.5):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("X-Token", token)
    with _lan.open(req, timeout=timeout) as r:
        return json.loads(r.read() or b"{}")


# --- finding the Stick again when its IP changes

def mdns_lookup(name, timeout=1.0):
    """Minimal mDNS A-record query (no dependencies). Returns an IPv4 string or None."""
    qid = os.getpid() & 0xFFFF
    q = struct.pack(">HHHHHH", qid, 0, 1, 0, 0, 0)
    for label in name.rstrip(".").split("."):
        q += bytes([len(label)]) + label.encode()
    q += b"\x00" + struct.pack(">HH", 1, 0x8001)  # type A, class IN + unicast-response bit
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        s.sendto(q, ("224.0.0.251", 5353))
        end = time.time() + timeout
        while time.time() < end:
            data, _ = s.recvfrom(1500)
            ip = _parse_a_record(data)
            if ip:
                return ip
    except OSError:
        return None
    finally:
        s.close()
    return None


def _skip_name(data, i):
    while i < len(data):
        n = data[i]
        if n == 0:
            return i + 1
        if n & 0xC0 == 0xC0:
            return i + 2
        i += n + 1
    return i


def _parse_a_record(data):
    if len(data) < 12:
        return None
    _, flags, qd, an, ns, ar = struct.unpack(">HHHHHH", data[:12])
    if not flags & 0x8000:
        return None
    i = 12
    for _ in range(qd):
        i = _skip_name(data, i) + 4
    for _ in range(an + ns + ar):
        i = _skip_name(data, i)
        if i + 10 > len(data):
            return None
        rtype, _, _, rdlen = struct.unpack(">HHIH", data[i:i + 10])
        i += 10
        if rtype == 1 and rdlen == 4:
            return socket.inet_ntoa(data[i:i + 4])
        i += rdlen
    return None


def _local_prefixes(extra_host):
    prefixes = set()
    try:  # primary interface address (no packets are sent)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        prefixes.add(s.getsockname()[0].rsplit(".", 1)[0])
        s.close()
    except OSError:
        pass
    if re.match(r"^\d+\.\d+\.\d+\.\d+$", extra_host or ""):
        prefixes.add(extra_host.rsplit(".", 1)[0])
    return [p for p in prefixes if not p.startswith("127.")]


def discover(cfg):
    """Find the Stick by its device id: mDNS name first, then a quick /24 sweep. Returns host or None."""
    dev_id, port = cfg.get("device_id"), cfg.get("port", 7880)
    if not dev_id or _recent(DISCOVER_FILE, DISCOVER_BACKOFF_S):
        return None
    _touch(DISCOVER_FILE)

    def is_ours(host, timeout=0.5):
        try:
            return _http_json(f"http://{host}:{port}/health", timeout=timeout).get("id") == dev_id
        except Exception:
            return False

    mdns_name = cfg.get("mdns_host")
    if mdns_name:
        ip = mdns_lookup(mdns_name)
        if ip and is_ours(ip):
            return ip
    hosts = [f"{p}.{n}" for p in _local_prefixes(cfg.get("host")) for n in range(1, 255)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as pool:
        futs = {pool.submit(is_ours, h, 0.4): h for h in hosts}
        for f in concurrent.futures.as_completed(futs):
            if f.result():
                for other in futs:
                    other.cancel()
                return futs[f]
    return None


class Device:
    def __init__(self, cfg, may_discover=False):
        self.cfg = cfg
        self.may_discover = may_discover
        self.base = f"http://{cfg['host']}:{cfg['port']}"
        self.token = cfg.get("token", "")

    def call(self, method, path, body=None, timeout=1.5):
        try:
            return _http_json(self.base + path, self.token, method, body, timeout)
        except urllib.error.HTTPError as e:
            if e.code == 401:
                sys.stderr.write("claudestick: the Stick rejected the token; re-run its install command\n")
            return None
        except (OSError, ValueError):
            if self.may_discover:
                self.may_discover = False
                host = discover(self.cfg)
                if host and host != self.cfg["host"]:
                    local = _read_json(CONFIG_PATH) or {}
                    local["host"] = host
                    _write_json(CONFIG_PATH, local)
                    self.cfg["host"] = host
                    self.base = f"http://{host}:{self.cfg['port']}"
                    sys.stderr.write(f"claudestick: found the Stick at its new address {host}\n")
                    return self.call(method, path, body, timeout)
            _touch(DOWN_FILE)
            return None

    def refresh_settings(self):
        """Pull macros/waits/alerts from the Stick at most once a minute, and keep this hook
        in step with the Stick's firmware (it serves the matching hook)."""
        if _recent(DEVICE_CFG, DEVICE_CFG_TTL_S):
            return
        r = self.call("GET", "/config", timeout=1.0)
        if r is not None:
            _write_json(DEVICE_CFG, r)
            self.cfg.update(load_config())
            self.self_update()

    def self_update(self):
        if os.environ.get("CLAUDESTICK_NO_SELF_UPDATE"):
            return
        try:
            want = (self.call("GET", "/health", timeout=1.0) or {}).get("hook")
            me = os.path.abspath(__file__)
            with open(me, "rb") as f:
                if not want or hashlib.sha256(f.read()).hexdigest() == want:
                    return
            req = urllib.request.Request(self.base + "/claudestick_hook.py")
            with _lan.open(req, timeout=5) as resp:
                body = resp.read()
            if hashlib.sha256(body).hexdigest() != want:
                return
            compile(body, me, "exec")  # never install something that doesn't parse
            tmp = me + ".new"
            with open(tmp, "wb") as f:
                f.write(body)
            os.replace(tmp, me)
            sys.stderr.write("claudestick: hook updated to match the Stick's firmware\n")
        except (OSError, SyntaxError, ValueError):
            pass


# --------------------------------------------------------------- formatting

def clip(s, n=300):
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[: n - 1] + "~"


def clip_lines(s, n=600, max_lines=14):
    lines = [l.rstrip() for l in str(s).splitlines()][:max_lines]
    out = "\n".join(lines)
    return out if len(out) <= n else out[: n - 1] + "~"


def _first_lines(text, n):
    lines = str(text or "").splitlines() or [""]
    more = len(lines) - n
    return lines[:n], (f" (+{more} more lines)" if more > 0 else "")


def describe_tool(name, inp, rich=False):
    """One-line summary for status; with rich=True a multi-line detail (diffs) for approval cards."""
    inp = inp or {}
    if name == "Bash":
        cmd = inp.get("command", "")
        return clip_lines(cmd) if rich else clip(cmd)
    if rich and name in ("Edit", "MultiEdit"):
        edits = inp.get("edits") or [{"old_string": inp.get("old_string", ""), "new_string": inp.get("new_string", "")}]
        out = [inp.get("file_path", "")]
        if len(edits) > 1:
            out.append(f"{len(edits)} edits, first:")
        old, om = _first_lines(edits[0].get("old_string"), 3)
        new, nm = _first_lines(edits[0].get("new_string"), 4)
        out += [f"- {l}" for l in old] + ([f"- ...{om}"] if om else [])
        out += [f"+ {l}" for l in new] + ([f"+ ...{nm}"] if nm else [])
        return clip_lines("\n".join(out), 700, 16)
    if rich and name == "Write":
        new, nm = _first_lines(inp.get("content"), 5)
        return clip_lines("\n".join([inp.get("file_path", "") + " (new/overwrite)"] + [f"+ {l}" for l in new] +
                                    ([f"+ ...{nm}"] if nm else [])), 700, 16)
    for key in ("file_path", "notebook_path", "path", "url", "pattern", "query", "description", "prompt"):
        if inp.get(key):
            return clip(inp[key])
    return clip(json.dumps(inp)) if inp else ""


def assess_risk(tool, inp):
    """('high', why) for things that deserve a deliberate hold, else ('', '')."""
    inp = inp or {}
    if tool == "Bash":
        cmd = inp.get("command", "")
        for pattern, why in RISKY:
            if re.search(pattern, cmd, re.I):
                return "high", why
    elif tool in ("Edit", "MultiEdit", "Write", "NotebookEdit"):
        path = inp.get("file_path") or inp.get("notebook_path") or ""
        if SENSITIVE_PATHS.search(path):
            return "high", "edits a sensitive file"
    return "", ""


def always_rule(tool, inp):
    """What 'always allow' would remember: (label for the Stick, updatedPermissions list) or ('', None)."""
    inp = inp or {}
    if tool == "Bash":
        cmd = (inp.get("command") or "").strip().splitlines()[0] if inp.get("command") else ""
        if not cmd or re.search(r"[;&|`$<>]", cmd):  # compound commands: no blanket rule
            return "", None
        try:
            parts = shlex.split(cmd)
        except ValueError:
            return "", None
        if not parts:
            return "", None
        prefix = parts[0]
        if len(parts) > 1 and re.match(r"^[a-z][a-z0-9_-]*$", parts[1]):  # subcommand: git status, npm run
            prefix += " " + parts[1]
        rules = [{"toolName": "Bash", "ruleContent": prefix}, {"toolName": "Bash", "ruleContent": prefix + " *"}]
        return f"Bash({prefix} *) here", [{"type": "addRules", "rules": rules, "behavior": "allow",
                                           "destination": "localSettings"}]
    if tool in ("Edit", "MultiEdit", "Write", "NotebookEdit"):
        return "auto-accept edits (session)", [{"type": "setMode", "mode": "acceptEdits", "destination": "session"}]
    if tool == "WebFetch" and inp.get("url"):
        host = urllib.parse.urlparse(inp["url"]).hostname or ""
        if host:
            return f"WebFetch({host}) here", [{"type": "addRules", "behavior": "allow", "destination": "localSettings",
                                               "rules": [{"toolName": "WebFetch", "ruleContent": f"domain:{host}"}]}]
    if tool.startswith("mcp__") or tool in ("WebSearch", "Read", "Glob", "Grep"):
        return f"{tool} here", [{"type": "addRules", "behavior": "allow", "destination": "localSettings",
                                 "rules": [{"toolName": tool}]}]
    return "", None


def last_assistant_text(transcript_path, max_bytes=200_000):
    try:
        with open(transcript_path, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - max_bytes))
            lines = f.read().decode("utf-8", "ignore").splitlines()
    except (OSError, TypeError):
        return ""
    for line in reversed(lines):
        try:
            entry = json.loads(line)
        except ValueError:
            continue
        if entry.get("type") != "assistant":
            continue
        content = (entry.get("message") or {}).get("content") or []
        texts = [b.get("text", "") for b in content if isinstance(b, dict) and b.get("type") == "text"]
        if any(t.strip() for t in texts):
            return "\n".join(texts).strip()[-2000:]
    return ""


def emit(obj):
    sys.stdout.write(json.dumps(obj))
    sys.stdout.flush()


OPTIONS_RE = re.compile(r"^\s*\**options\**\s*:\s*(.+)$", re.I)


def split_options(text):
    """Pull the `Options: a | b | c` line Claude adds in remote mode."""
    if not text:
        return text, []
    for line in reversed([l for l in text.splitlines() if l.strip()][-3:]):
        m = OPTIONS_RE.match(line)
        if m:
            opts = [o.strip(" `*-") for o in m.group(1).split("|")]
            opts = [o for o in opts if o][:4]
            return text.replace(line, "").strip(), opts
    return text, []


def git_branch(cwd):
    cache = os.path.join(TMP, f"claudestick-branch-{UID}-{hashlib.md5(cwd.encode()).hexdigest()[:12]}")
    if _recent(cache, 60):
        try:
            with open(cache) as f:
                return f.read().strip()
        except OSError:
            pass
    branch = ""
    try:
        branch = subprocess.run(["git", "-C", cwd, "rev-parse", "--abbrev-ref", "HEAD"], capture_output=True,
                                text=True, timeout=0.5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    if branch == "HEAD":
        branch = ""
    try:
        with open(cache, "w") as f:
            f.write(branch)
    except OSError:
        pass
    return branch


# ------------------------------------------------------------------ session tracking
# Claude's to-do list (TodoWrite, or TaskCreate/TaskUpdate on newer Claude Code) and token
# totals read from the session transcript, kept per session so each hook run only
# reads what's new.

def _sess_path(session):
    return os.path.join(SESS_DIR, re.sub(r"[^A-Za-z0-9_-]", "_", session)[:80] + ".json")


def load_sess(session):
    st = _read_json(_sess_path(session)) or {}
    st.setdefault("tasks", {})        # id -> {"t": subject, "a": activeForm, "s": status}
    st.setdefault("order", [])
    st.setdefault("usage", {})
    return st


def save_sess(session, st):
    st["seen"] = int(time.time())
    _write_json(_sess_path(session), st)


def prune_sessions(max_age_days=14):
    """Forget session files nobody has touched in two weeks."""
    try:
        cutoff = time.time() - max_age_days * 86400
        for name in os.listdir(SESS_DIR):
            path = os.path.join(SESS_DIR, name)
            if os.path.getmtime(path) < cutoff:
                os.remove(path)
    except OSError:
        pass


def _task_id(inp):
    v = inp.get("taskId") or inp.get("id") or inp.get("task_id")
    return str(v) if v not in (None, "") else ""


def track_tasks(st, tool, inp, resp):
    """Apply one to-do tool call to the stored list. Returns True if it changed."""
    inp = inp if isinstance(inp, dict) else {}
    if tool == "TodoWrite":
        todos = inp.get("todos") or []
        st["tasks"], st["order"] = {}, []
        for i, t in enumerate(todos):
            if not isinstance(t, dict):
                continue
            tid = str(i + 1)
            st["tasks"][tid] = {"t": t.get("content", ""), "a": t.get("activeForm") or t.get("active_form") or "",
                                "s": t.get("status", "pending")}
            st["order"].append(tid)
        return True
    if tool == "TaskCreate":
        task = (resp or {}).get("task") if isinstance(resp, dict) else None
        tid = str((task or {}).get("id") or "")
        if not tid:  # older builds: ids are sequential
            tid = str(max([int(x) for x in st["order"] if x.isdigit()] + [0]) + 1)
        st["tasks"][tid] = {"t": inp.get("subject", ""), "a": inp.get("activeForm") or inp.get("active_form") or "",
                            "s": "pending"}
        if tid not in st["order"]:
            st["order"].append(tid)
        return True
    if tool == "TaskUpdate":
        tid = _task_id(inp)
        if not tid:
            return False
        if inp.get("status") == "deleted":
            st["tasks"].pop(tid, None)
            st["order"] = [x for x in st["order"] if x != tid]
            return True
        t = st["tasks"].get(tid)
        if t is None:  # created before we were watching
            t = st["tasks"][tid] = {"t": inp.get("subject") or f"task {tid}", "a": "", "s": "pending"}
            st["order"].append(tid)
        if inp.get("status"):
            t["s"] = inp["status"]
        if inp.get("subject"):
            t["t"] = inp["subject"]
        if inp.get("activeForm") or inp.get("active_form"):
            t["a"] = inp.get("activeForm") or inp.get("active_form")
        return True
    return False


def todo_payload(st):
    """Compact list for the Stick: done/total, what's happening now, and the items."""
    items = [st["tasks"][i] for i in st["order"] if i in st["tasks"]]
    if not items:
        return {"total": 0, "done": 0, "now": "", "items": []}
    done = sum(1 for t in items if t["s"] == "completed")
    cur = next((t for t in items if t["s"] == "in_progress"), None)
    # Show the unfinished end of long lists: the last few done items, then the rest.
    first_open = next((i for i, t in enumerate(items) if t["s"] != "completed"), len(items))
    start = max(0, min(first_open - 2, len(items) - TODO_MAX))
    code = {"completed": "c", "in_progress": "i"}
    return {"total": len(items), "done": done, "now": clip((cur["a"] or cur["t"]) if cur else "", 60),
            "skip": start,
            "items": [[code.get(t["s"], "p"), clip(t["t"], 48)] for t in items[start:start + TODO_MAX]]}


def _local_day(ts):
    """Transcript timestamps are UTC ISO strings; bucket tokens by the local calendar day."""
    try:
        import datetime
        t = datetime.datetime.fromisoformat(ts.replace("Z", "+00:00"))
        return t.astimezone().strftime("%Y-%m-%d")
    except (ValueError, AttributeError, TypeError):
        return time.strftime("%Y-%m-%d")


def _short_model(m):
    m = re.sub(r"^claude-", "", m or "")
    return re.sub(r"-\d{8}$", "", m)


def read_usage(st, transcript_path, max_bytes=8_000_000):
    """Add token usage from transcript lines we haven't read yet. Returns True if anything changed."""
    u = st["usage"]
    if not transcript_path:
        return False
    try:
        size = os.path.getsize(transcript_path)
    except OSError:
        return False
    if u.get("path") != transcript_path or size < u.get("off", 0):   # new or rewritten transcript
        st["usage"] = u = {"path": transcript_path, "off": 0, "in": 0, "out": 0, "cr": 0, "cw": 0,
                           "ctx": 0, "model": "", "days": {}, "last_id": "", "last": [0, 0, 0, 0], "last_day": ""}
    if size == u["off"]:
        return False
    try:
        with open(transcript_path, "rb") as f:
            f.seek(u["off"])
            chunk = f.read(max_bytes)
    except OSError:
        return False
    end = chunk.rfind(b"\n")
    if end < 0:
        return False   # a line still being written
    chunk = chunk[:end + 1]
    u["off"] += len(chunk)
    changed = False
    for raw in chunk.splitlines():
        if b'"usage"' not in raw:
            continue
        try:
            e = json.loads(raw)
        except ValueError:
            continue
        if e.get("type") != "assistant" or e.get("isSidechain"):
            continue
        msg = e.get("message") or {}
        us = msg.get("usage") or {}
        vals = [int(us.get("input_tokens") or 0), int(us.get("output_tokens") or 0),
                int(us.get("cache_read_input_tokens") or 0), int(us.get("cache_creation_input_tokens") or 0)]
        mid = msg.get("id") or ""
        day = _local_day(e.get("timestamp", ""))
        if mid and mid == u["last_id"]:   # one API message is written as several lines: replace, don't add
            prev = u["last"]
            for k, i in (("in", 0), ("out", 1), ("cr", 2), ("cw", 3)):
                u[k] -= prev[i]
            pday = u.get("last_day") or day
            u["days"][pday] = u["days"].get(pday, 0) - sum(prev)
        for k, i in (("in", 0), ("out", 1), ("cr", 2), ("cw", 3)):
            u[k] += vals[i]
        u["days"][day] = u["days"].get(day, 0) + sum(vals)
        u["last_id"], u["last"], u["last_day"] = mid, vals, day
        u["ctx"] = vals[0] + vals[2] + vals[3]
        if msg.get("model") and not msg["model"].startswith("<"):
            u["model"] = _short_model(msg["model"])
        changed = True
    if len(u["days"]) > 3:
        for d in sorted(u["days"])[:-3]:
            del u["days"][d]
    return changed


def usage_payload(st):
    u = st["usage"]
    today = time.strftime("%Y-%m-%d")
    return {"in": u.get("in", 0), "out": u.get("out", 0), "cr": u.get("cr", 0), "cw": u.get("cw", 0),
            "ctx": u.get("ctx", 0), "model": u.get("model", ""), "day": today,
            "today": max(0, u.get("days", {}).get(today, 0))}


def session_extras(ev, force_usage=False):
    """Fields to piggyback on an /event call: 'todo' when the list changed, 'usage' every so often."""
    session = ev.get("session_id", "")
    if not session:
        return {}
    name = ev.get("hook_event_name", "")
    st = load_sess(session)
    extra, dirty = {}, False
    if name == "SessionStart" and ev.get("source") in ("startup", "clear"):
        st["tasks"], st["order"], dirty = {}, [], True
        extra["todo"] = todo_payload(st)
    if name == "PostToolUse":
        before = todo_payload(st)
        if track_tasks(st, ev.get("tool_name", ""), ev.get("tool_input"), ev.get("tool_response")):
            extra["todo"], dirty = todo_payload(st), True
            t = extra["todo"]
            if t["total"] and t["done"] == t["total"] and before["done"] != before["total"]:
                extra["plan_done"] = True   # last item just ticked off
    if name in ("Stop", "SessionStart") and st["order"]:
        extra["todo"] = todo_payload(st)   # cheap resync after a Stick reboot
    if force_usage or time.time() - st.get("usage_sent", 0) >= USAGE_EVERY_S:
        if read_usage(st, ev.get("transcript_path")) or force_usage:
            extra["usage"] = usage_payload(st)
            st["usage_sent"] = time.time()
        dirty = True
    if dirty:
        save_sess(session, st)
    return extra


# --- remote-mode bookkeeping: remember which mode Claude was last told about, per session

def _mode_path(session):
    return os.path.join(MODE_DIR, re.sub(r"[^A-Za-z0-9_-]", "_", session)[:80])


def told_mode(session):
    try:
        with open(_mode_path(session)) as f:
            return f.read().strip() == "1"
    except OSError:
        return None


def set_told(session, remote):
    try:
        os.makedirs(MODE_DIR, exist_ok=True)
        with open(_mode_path(session), "w") as f:
            f.write("1" if remote else "0")
    except OSError:
        pass


def mode_note(session, remote, cfg):
    """Instruction text to hand Claude if the mode changed since we last told it, else ''."""
    if remote is None:
        return ""
    prev = told_mode(session)
    if prev == remote or (prev is None and not remote):
        return ""
    set_told(session, remote)
    return cfg["remote_on_text"] if remote else cfg["remote_off_text"]


def notify(cfg, title, body, priority="default", actions=None):
    """Optional phone push via ntfy. Action buttons call the Stick directly (phone must reach it)."""
    if not cfg.get("ntfy_url"):
        return
    base = f"http://{cfg['host']}:{cfg['port']}"
    req = urllib.request.Request(cfg["ntfy_url"], data=clip(body, 400).encode(), method="POST")
    req.add_header("Title", clip(title, 80))
    req.add_header("Priority", priority)
    req.add_header("Tags", "robot")
    req.add_header("Click", f"{base}/?t={cfg.get('token', '')}")
    if actions:
        req.add_header("Actions", "; ".join(
            f"http, {label}, {base}/act?action={act}&id={aid}&t={cfg.get('token', '')}, method=POST, clear=true"
            for label, act, aid in actions[:3]))
    if cfg.get("ntfy_token"):
        req.add_header("Authorization", f"Bearer {cfg['ntfy_token']}")
    try:
        urllib.request.urlopen(req, timeout=3).close()   # normal proxy rules apply (ntfy may be remote)
    except OSError:
        pass


# ------------------------------------------------------------------ handlers

def base_body(ev):
    cwd = ev.get("cwd") or os.getcwd()
    now = time.time()
    lt = time.localtime(now)
    name = os.environ.get("CLAUDESTICK_NAME")
    if not name:
        name = os.path.basename(cwd.rstrip("/\\")) or cwd
        branch = git_branch(cwd)
        if branch and branch not in ("main", "master"):
            name += "@" + branch
    return {"session": ev.get("session_id", "unknown"), "project": name, "now": int(now), "tz": lt.tm_gmtoff or 0}


def wait_for(dev, path, rid, wait_s):
    """Poll the device until it answers, the wait expires, or it goes away."""
    deadline = time.time() + wait_s
    while time.time() < deadline:
        r = dev.call("GET", f"{path}?id={rid}")
        if r is None:
            return None
        if r.get("state") != "pending":
            return r
        time.sleep(POLL_S)
    dev.call("DELETE", f"{path}?id={rid}")
    return None


def on_permission(dev, ev, cfg):
    tool = ev.get("tool_name", "")
    inp = ev.get("tool_input")
    body = base_body(ev)
    if tool == "AskUserQuestion":  # needs the terminal UI; just flag it on the device
        qs = (inp or {}).get("questions") or [{}]
        dev.call("POST", "/event", {**body, "event": "waiting", "detail": clip(qs[0].get("question", "Question"))})
        return
    risk, why = assess_risk(tool, inp)
    label, perms = ("", None) if risk else always_rule(tool, inp)
    detail = describe_tool(tool, inp, rich=True)
    r = dev.call("POST", "/permission", {**body, "tool": tool, "detail": detail, "always": label,
                                         "risk": risk, "risk_why": why})
    if not r or "id" not in r:
        return
    remote = bool(r.get("remote"))
    if remote:
        acts = [("Approve", "approve", r["id"])] + ([("Always", "always", r["id"])] if label else []) + \
               [("Deny", "deny", r["id"])]
        notify(cfg, f"{'DANGER: ' if risk else ''}Approve {tool}? ({body['project']})",
               (why + "\n" if why else "") + detail, "urgent" if risk else "high", acts)
    res = wait_for(dev, "/permission", r["id"], cfg["remote_permission_wait_s" if remote else "permission_wait_s"])
    if not res:
        return  # timeout / gone: fall through to the normal terminal prompt
    state = res.get("state")
    if state == "allow":
        emit({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": {"behavior": "allow"}}})
    elif state == "always":
        decision = {"behavior": "allow"}
        if perms:
            decision["updatedPermissions"] = perms
        emit({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": decision}})
    elif state == "deny":
        msg = "Denied by the user from their ClaudeStick."
        if res.get("reason"):
            msg = f"The user denied this from their ClaudeStick and said: {res['reason']}"
        emit({"hookSpecificOutput": {"hookEventName": "PermissionRequest",
                                     "decision": {"behavior": "deny", "message": msg}}})
    # "skip": user chose to answer at the keyboard -> no output


def on_stop(dev, ev, cfg):
    body = base_body(ev)
    full = last_assistant_text(ev.get("transcript_path"))
    text, opts = split_options(full)
    last = clip(text, 240)
    r = dev.call("POST", "/event", {**body, **session_extras(ev, force_usage=True), "event": "done", "detail": last,
                                    "can_inject": True})
    if r is None:
        return
    remote = bool(r.get("remote"))
    note = mode_note(body["session"], remote, cfg)

    def cont(instruction):
        emit({"decision": "block", "reason": (note + "\n\n" if note else "") + instruction})

    if r.get("inject"):
        return cont(r["inject"])
    menu = cfg.get("stop_menu", "remote")
    if menu is True or menu is False:  # pre-0.2 boolean config
        menu = "remote" if menu else "never"
    if menu == "never" or (menu == "remote" and not remote):
        if note:  # mode changed while idle; make sure Claude hears about it
            set_told(body["session"], not remote)  # re-deliver on the next prompt instead of forcing a turn
        return
    dyn = [{"label": clip(o, 28), "text": f"Go with: {o}"} for o in opts]
    r = dev.call("POST", "/stop", {**body, "kind": "done", "last": last, "options": dyn})
    if not r or "id" not in r:
        return
    notify(cfg, f"Claude finished ({body['project']})", last or "Done. Pick what's next.")
    res = wait_for(dev, "/stop", r["id"], cfg["stop_wait_s"])
    if res and res.get("state") in ("macro", "custom") and res.get("text"):
        cont(res["text"])
    elif note:
        set_told(body["session"], not remote)


def on_question(dev, ev, cfg, body):
    """AskUserQuestion in remote mode: show each question on the Stick, return answers as the tool result."""
    questions = (ev.get("tool_input") or {}).get("questions") or []
    answers = []
    deadline = time.time() + cfg["remote_permission_wait_s"]
    for q in questions:
        opts = q.get("options") or []
        macros = [{"label": clip(o.get("label", "?"), 28), "text": o.get("label", "")} for o in opts]
        r = dev.call("POST", "/stop", {**body, "kind": "question", "title": clip(q.get("question", ""), 200),
                                       "last": "", "macros": macros})
        if not r or "id" not in r:
            return False
        notify(cfg, f"Claude asks ({body['project']})", q.get("question", ""), "high")
        res = wait_for(dev, "/stop", r["id"], max(1, deadline - time.time()))
        if not res or res.get("state") not in ("macro", "custom"):
            return False  # dismissed / timed out: let the normal terminal question UI run
        answers.append(f"Q: {q.get('question', '')}\nA: {res.get('text', '')}")
    if not answers:
        return False
    emit({"hookSpecificOutput": {
        "hookEventName": "PreToolUse",
        "permissionDecision": "deny",
        "permissionDecisionReason": "The user already answered these questions on their ClaudeStick, so the question "
                                    "dialog was skipped. Treat these as their answers and continue:\n" + "\n".join(answers),
    }})
    return True


def on_event(dev, ev, cfg):
    name = ev.get("hook_event_name", "")
    body = base_body(ev)
    if name == "SessionStart":
        r = dev.call("POST", "/event", {**body, **session_extras(ev, force_usage=True), "event": "ready",
                                        "detail": ev.get("source", "")})
        if r is not None:
            set_told(body["session"], False)  # fresh context: Claude knows nothing yet
            note = mode_note(body["session"], bool(r.get("remote")), cfg)
            if note:
                emit({"hookSpecificOutput": {"hookEventName": "SessionStart", "additionalContext": note}})
    elif name == "UserPromptSubmit":
        # typed=True tells the Stick you're at the keyboard (auto remote mode switches off)
        r = dev.call("POST", "/event", {**body, "event": "working", "detail": clip(ev.get("prompt", ""), 120),
                                        "typed": True})
        note = mode_note(body["session"], bool(r.get("remote")), cfg) if r is not None else ""
        if note:
            emit({"hookSpecificOutput": {"hookEventName": "UserPromptSubmit", "additionalContext": note}})
    elif name == "PreToolUse":
        tool = ev.get("tool_name", "")
        r = dev.call("POST", "/event", {**body, "event": "tool",
                                        "detail": f"{tool}: {describe_tool(tool, ev.get('tool_input'))}",
                                        "can_stop": True}, timeout=0.8)
        if r and r.get("stop"):
            emit({"continue": False, "stopReason": "Stopped from ClaudeStick."})
        elif r and r.get("remote") and tool == "AskUserQuestion" and cfg.get("questions_on_device", True):
            on_question(dev, ev, cfg, body)
    elif name == "PostToolUse":
        tool = ev.get("tool_name", "")
        resp = ev.get("tool_response")
        failed = isinstance(resp, dict) and (resp.get("is_error") or resp.get("error"))
        extras = session_extras(ev)
        r = dev.call("POST", "/event", {**body, **extras, "event": "error" if failed else "working",
                                        "detail": f"{tool} {'failed' if failed else 'done'}",
                                        "can_inject": True}, timeout=0.8)
        parts = []
        if extras.get("plan_done") and r and r.get("remote"):
            notify(cfg, f"Plan done ({body['project']})", f"All {extras['todo']['total']} to-dos are complete.")
        if r is not None:
            note = mode_note(body["session"], bool(r.get("remote")), cfg)
            if note:
                parts.append(note)
            if r.get("inject"):
                parts.append("New instruction from the user (sent from their ClaudeStick): " + r["inject"])
        if parts:
            emit({"hookSpecificOutput": {"hookEventName": "PostToolUse", "additionalContext": "\n\n".join(parts)}})
    elif name == "Notification":
        r = dev.call("POST", "/event", {**body, "event": "waiting", "detail": clip(ev.get("message", ""), 160)})
        if r and r.get("remote"):
            notify(cfg, f"Claude needs you ({body['project']})", ev.get("message", ""))
    elif name == "SessionEnd":
        dev.call("POST", "/event", {**body, "event": "end"})
        prune_sessions()


# Events where a short extra delay is acceptable: re-read settings, and look for the
# Stick at a new IP if it stopped answering. Tool events stay fast.
SLOW_EVENTS = {"SessionStart", "UserPromptSubmit", "Stop", "PermissionRequest"}


def main():
    if os.environ.get("CLAUDESTICK", "1") == "0":
        return
    try:
        ev = json.load(sys.stdin)
    except ValueError:
        return
    name = ev.get("hook_event_name", "")
    slow = name in SLOW_EVENTS
    if device_down() and not (slow and not _recent(DISCOVER_FILE, DISCOVER_BACKOFF_S)):
        return
    cfg = load_config()
    dev = Device(cfg, may_discover=slow)
    if slow:
        dev.refresh_settings()
    if name == "PermissionRequest":
        on_permission(dev, ev, cfg)
    elif name == "Stop":
        on_stop(dev, ev, cfg)
    else:
        on_event(dev, ev, cfg)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # never break Claude Code because of the gadget
        sys.stderr.write(f"claudestick: {e}\n")
    sys.exit(0)
