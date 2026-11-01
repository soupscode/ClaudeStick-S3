#!/usr/bin/env python3
"""Install / uninstall the ClaudeStick hooks for Claude Code.

  python3 install.py --host 192.168.1.50 --token abcd1234...   # install / update
  python3 install.py --host https://stick.example.com --pair 123456   # a bridge on the internet, with its one-time code
  python3 install.py ... --wake       # also install the Stop hook that lets the Stick wake an idle session (default: if Claude Code is new enough)
  python3 install.py --test                                     # ping the device
  python3 install.py --uninstall

No checkout needed: the Stick serves this file and the hook, so on a new machine
  curl -s http://<stick-ip>:7880/install.py | python3 - --host <stick-ip> --token <token>

Copies the hook to ~/.claudestick/, writes ~/.claudestick/config.json (kept on
re-install, only --host/--token/--port are updated), and merges entries into
~/.claude/settings.json (backed up first). Re-running is safe.
"""
import argparse
import json
import os
import re
import subprocess
import shutil
import sys
import time

import importlib.util
import urllib.error
import urllib.request

# __file__ is missing when piped in (curl ... | python3 -)
HERE = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else None
LOCAL_HOOK = os.path.join(HERE, "hooks", "claudestick_hook.py") if HERE else None
hook = None  # loaded in main()

HOME_DIR = os.path.expanduser("~/.claudestick")
HOOK_DST = os.path.join(HOME_DIR, "claudestick_hook.py")
CONFIG = os.path.join(HOME_DIR, "config.json")
SETTINGS = os.path.expanduser("~/.claude/settings.json")
MARK = "claudestick_hook.py"

QUICK = ["SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "Notification", "SessionEnd"]


TODO_ENV = "CLAUDE_CODE_ENABLE_TODO_TOOLS"


def base_url(host, port):
    """A full address (https://stick.example.com) as given; a bare host or IP is http on its port (the LAN case)."""
    h = str(host).strip().rstrip("/")
    return h if h.startswith(("http://", "https://")) else f"http://{h}:{port}"


def opener_for(url):
    """The LAN never goes through a proxy; an https address (the internet) uses the system's proxies and CAs."""
    return urllib.request.build_opener() if url.startswith("https://") else urllib.request.build_opener(urllib.request.ProxyHandler({}))


def load_json(path, default):
    try:
        with open(path) as f:
            return json.load(f)
    except FileNotFoundError:
        return default


def save_json(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def strip_ours(settings, cfg=None):
    sl = settings.get("statusLine")
    if isinstance(sl, dict) and MARK in sl.get("command", ""):   # put back the status line we replaced
        chain = (cfg or {}).get("statusline_chain")
        if chain:
            settings["statusLine"] = {**sl, "command": chain}
        else:
            settings.pop("statusLine", None)
    hooks = settings.get("hooks", {})
    for event in list(hooks):
        groups = []
        for g in hooks[event]:
            g = dict(g)
            g["hooks"] = [h for h in g.get("hooks", []) if MARK not in h.get("command", "")]
            if g["hooks"]:
                groups.append(g)
        if groups:
            hooks[event] = groups
        else:
            del hooks[event]
    if not hooks:
        settings.pop("hooks", None)
    return settings


def backup_settings():
    if os.path.exists(SETTINGS):
        dst = f"{SETTINGS}.bak-{time.strftime('%Y%m%d-%H%M%S')}"
        shutil.copy2(SETTINGS, dst)
        print(f"backed up settings -> {dst}")


def fetch_hook(args):
    """Put the hook in ~/.claudestick: from this checkout, or downloaded from the Stick."""
    os.makedirs(HOME_DIR, exist_ok=True)
    if LOCAL_HOOK and os.path.exists(LOCAL_HOOK):
        shutil.copy2(LOCAL_HOOK, HOOK_DST)
        return
    host = args.host or (load_json(CONFIG, {}) or {}).get("host")
    port = args.port or (load_json(CONFIG, {}) or {}).get("port") or 7880
    if not host:
        sys.exit("need --host <stick-ip> to download the hook")
    url = f"{base_url(host, port)}/claudestick_hook.py"
    opener = opener_for(url)
    try:
        with opener.open(url, timeout=10) as r:
            body = r.read()
    except OSError as e:
        sys.exit(f"could not download the hook from {url}: {e}")
    with open(HOOK_DST, "wb") as f:
        f.write(body)
    print(f"hook    <- {url}")


def load_hook():
    global hook
    path = LOCAL_HOOK if LOCAL_HOOK and os.path.exists(LOCAL_HOOK) else HOOK_DST
    if not os.path.exists(path):
        sys.exit("hook not installed yet; run install with --host first")
    spec = importlib.util.spec_from_file_location("claudestick_hook", path)
    hook = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hook)


def exchange_pair_code(host, port, code):
    """Trade a bridge's one-time pairing code for its token (so the token is never typed or pasted)."""
    url = f"{base_url(host, port)}/pair"
    req = urllib.request.Request(url, data=json.dumps({"code": code}).encode(), method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with opener_for(url).open(req, timeout=10) as r:
            return json.loads(r.read())["token"]
    except urllib.error.HTTPError as e:
        why = {403: "that pairing code is wrong or has expired (a code lasts 5 minutes and dies after 3 wrong tries): "
                    "make a new one with `claudestick_bridge.py --pair`",
               404: "this device doesn't take pairing codes (only a bridge does): use --token",
               429: "too many wrong codes from this address: wait a few minutes"}.get(e.code, f"it answered {e.code}")
        sys.exit(f"pairing failed: {why}")
    except (OSError, ValueError, KeyError) as e:
        sys.exit(f"could not reach {url}: {e}")


WAKE_MIN_VERSION = (2, 1, 154)   # first Claude Code version the async-rewake Stop hook was checked against


def claude_version():
    """(major, minor, patch) of the `claude` on PATH, or None if it can't be told."""
    try:
        out = subprocess.run(["claude", "--version"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r"(\d+)\.(\d+)\.(\d+)", out or "")
    return tuple(int(x) for x in m.groups()) if m else None


def want_wake(args):
    """Should the idle-session watcher hook be installed? Yes if asked; by default only on a Claude Code
    new enough to run it in the background (on an older one it would hold up every Stop)."""
    if args.no_wake:
        return False, "skipped (--no-wake)"
    if args.wake:
        return True, ""
    v = claude_version()
    if v is None:
        return False, "not installed: couldn't tell which Claude Code version this is (add --wake to install it anyway)"
    if v < WAKE_MIN_VERSION:
        return False, f"not installed: Claude Code {'.'.join(map(str, v))} is older than {'.'.join(map(str, WAKE_MIN_VERSION))} (add --wake to force it)"
    return True, ""


def install(args):
    if args.pair:
        if args.token:
            sys.exit("use --pair or --token, not both")
        host = args.host or (load_json(CONFIG, {}) or {}).get("host")
        if not host:
            sys.exit("--pair needs --host <bridge address>")
        port = args.port or (load_json(CONFIG, {}) or {}).get("port") or 7890
        args.token = exchange_pair_code(host, port, args.pair)
        args.port = args.port or (None if "://" in str(host) else port)
        print("paired: got the bridge's token")
    fetch_hook(args)
    load_hook()

    # Local config only holds how to reach the Stick; macros, waits and alerts live on
    # the Stick (settings page) and are fetched by the hook.
    cfg = load_json(CONFIG, None) or {}
    cfg = {k: v for k, v in cfg.items() if k in ("host", "port", "token", "device_id", "mdns_host", "added_todo_env", "statusline_chain")}
    cfg.setdefault("port", 7880)
    for k in ("host", "token", "port"):
        v = getattr(args, k)
        if v is not None:
            cfg[k] = v
    save_json(CONFIG, cfg)
    print(f"config  -> {CONFIG}")

    cmd = f'"{sys.executable}" "{HOOK_DST}"'
    settings = strip_ours(load_json(SETTINGS, {}), cfg)   # a re-run starts from your original status line
    cfg.pop("statusline_chain", None)
    hooks = settings.setdefault("hooks", {})

    def add(event, timeout):
        hooks.setdefault(event, []).append({"hooks": [{"type": "command", "command": cmd, "timeout": timeout}]})

    # Hook timeouts are a ceiling: the real waits come from the Stick's settings (max 3600 s).
    long_wait = 3615
    for ev in QUICK:
        add(ev, long_wait if ev == "PreToolUse" else 5)  # PreToolUse may show AskUserQuestion on the Stick
    add("PermissionRequest", long_wait)
    add("Stop", long_wait)
    wake, why = want_wake(args)
    if wake:   # keeps running after the turn ends and wakes an idle session when text arrives from the Stick
        hooks.setdefault("Stop", []).append({"hooks": [{"type": "command", "command": cmd + " --watch", "async": True, "asyncRewake": True}]})
        print("hook    -> Stop watcher: a message sent from the Stick can wake an idle session")
    else:
        print(f"hook    -> Stop watcher {why}")

    # Newer Claude models don't load the to-do tools unless asked; the Stick's
    # to-do view needs them. Only set it if you haven't chosen either way.
    env = settings.setdefault("env", {})
    if args.no_todo_tools:
        if cfg.get("added_todo_env"):
            env.pop(TODO_ENV, None)
            cfg.pop("added_todo_env", None)
    elif TODO_ENV not in env:
        env[TODO_ENV] = "1"
        cfg["added_todo_env"] = True
        print(f"env     -> {TODO_ENV}=1 (to-do list on the Stick; --no-todo-tools to skip)")
    if not env:
        settings.pop("env", None)

    # Plan limits and context % only reach a status-line command, so the Stick's usage strip needs one.
    # An existing status line is kept: ours runs it and prints its output unchanged.
    status_cmd = cmd + " --statusline"
    cur = settings.get("statusLine")
    if args.no_statusline:
        pass
    elif isinstance(cur, dict) and cur.get("command"):
        cfg["statusline_chain"] = cur["command"]
        settings["statusLine"] = {**cur, "command": status_cmd}
        print("status  -> wraps your existing status line (usage limits go to the Stick)")
    elif cur:
        print("status  -> your statusLine isn't a command; left alone (no usage limits on the Stick)")
    else:
        settings["statusLine"] = {"type": "command", "command": status_cmd}
        print("status  -> added a small status line (ctx / 5h / 7d); --no-status-line to skip")
    save_json(CONFIG, cfg)

    backup_settings()
    save_json(SETTINGS, settings)
    print(f"hooks   -> {SETTINGS}")
    test()


def uninstall(_args):
    if os.path.exists(SETTINGS):
        backup_settings()
        local = load_json(CONFIG, {}) or {}
        settings = strip_ours(load_json(SETTINGS, {}), local)
        if local.get("added_todo_env"):
            settings.get("env", {}).pop(TODO_ENV, None)
            if not settings.get("env"):
                settings.pop("env", None)
        save_json(SETTINGS, settings)
        if local.pop("statusline_chain", None) is not None:   # restored above; don't keep a stale copy
            save_json(CONFIG, local)
        print("removed ClaudeStick hooks from Claude Code settings")
    print(f"left {HOME_DIR} in place (delete it to remove your config/macros)")


def test(_args=None):
    if hook is None:
        load_hook()
    cfg = hook.load_config() if os.path.exists(CONFIG) else None
    if not cfg:
        print("no config yet; run install first")
        return
    try:
        os.remove(hook.DOWN_FILE)
    except OSError:
        pass
    dev = hook.Device(cfg)
    r = dev.call("GET", "/health")
    if not r:
        print(f"device NOT reachable at {dev.base}. Check host/IP, Wi-Fi, and that this machine can reach it.")
        return
    print(f"device OK at {dev.base}: {r.get('name')} fw {r.get('fw')}")
    if r.get("id"):  # remember who it is, so the hook can find it again if its IP changes
        local = load_json(CONFIG, {}) or {}
        if local.get("device_id") != r["id"] or local.get("mdns_host") != r.get("host"):
            local["device_id"], local["mdns_host"] = r["id"], r.get("host", "")
            save_json(CONFIG, local)
    ok = dev.call("POST", "/event", {"session": "install-test", "project": "install-test",
                                     "event": "ready", "detail": "hello from install.py"})
    print("token OK" if ok is not None else "token REJECTED - copy it from the device (menu > info)")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", help="device IP or hostname (default claudestick.local); a bridge on the internet: https://stick.example.com")
    p.add_argument("--token", help="token shown on the device (menu > info)")
    p.add_argument("--pair", metavar="CODE", help="a bridge's one-time pairing code instead of its token (see `claudestick_bridge.py --pair`)")
    p.add_argument("--no-statusline", "--no-status-line", dest="no_statusline", action="store_true",
                   help="don't touch the status line (the Stick then can't show plan limits)")
    p.add_argument("--no-todo-tools", action="store_true",
                   help=f"don't set {TODO_ENV}=1 (the Stick's to-do view stays empty on newer models)")
    p.add_argument("--wake", action="store_true", help="install the Stop hook that lets the Stick wake an idle session, whatever Claude Code version this is")
    p.add_argument("--no-wake", action="store_true", help="don't install that Stop hook")
    p.add_argument("--port", type=int)
    p.add_argument("--uninstall", action="store_true")
    p.add_argument("--test", action="store_true")
    a = p.parse_args()
    if a.uninstall:
        uninstall(a)
    elif a.test:
        test(a)
    else:
        install(a)


if __name__ == "__main__":
    main()
