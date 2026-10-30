#!/usr/bin/env python3
"""Install / uninstall the ClaudeStick hooks for Claude Code.

  python3 install.py --host 192.168.1.50 --token abcd1234...   # install / update
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
import shutil
import sys
import time

import importlib.util
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


def strip_ours(settings):
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
    url = f"http://{host}:{port}/claudestick_hook.py"
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
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


def install(args):
    fetch_hook(args)
    load_hook()

    # Local config only holds how to reach the Stick; macros, waits and alerts live on
    # the Stick (settings page) and are fetched by the hook.
    cfg = load_json(CONFIG, None) or {}
    cfg = {k: v for k, v in cfg.items() if k in ("host", "port", "token", "device_id", "mdns_host", "added_todo_env")}
    cfg.setdefault("port", 7880)
    for k in ("host", "token", "port"):
        v = getattr(args, k)
        if v is not None:
            cfg[k] = v
    save_json(CONFIG, cfg)
    print(f"config  -> {CONFIG}")

    cmd = f'"{sys.executable}" "{HOOK_DST}"'
    settings = strip_ours(load_json(SETTINGS, {}))
    hooks = settings.setdefault("hooks", {})

    def add(event, timeout):
        hooks.setdefault(event, []).append({"hooks": [{"type": "command", "command": cmd, "timeout": timeout}]})

    # Hook timeouts are a ceiling: the real waits come from the Stick's settings (max 3600 s).
    long_wait = 3615
    for ev in QUICK:
        add(ev, long_wait if ev == "PreToolUse" else 5)  # PreToolUse may show AskUserQuestion on the Stick
    add("PermissionRequest", long_wait)
    add("Stop", long_wait)

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
    save_json(CONFIG, cfg)

    backup_settings()
    save_json(SETTINGS, settings)
    print(f"hooks   -> {SETTINGS}")
    test()


def uninstall(_args):
    if os.path.exists(SETTINGS):
        backup_settings()
        settings = strip_ours(load_json(SETTINGS, {}))
        if (load_json(CONFIG, {}) or {}).get("added_todo_env"):
            settings.get("env", {}).pop(TODO_ENV, None)
            if not settings.get("env"):
                settings.pop("env", None)
        save_json(SETTINGS, settings)
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
    p.add_argument("--host", help="device IP or hostname (default claudestick.local)")
    p.add_argument("--token", help="token shown on the device (menu > info)")
    p.add_argument("--no-todo-tools", action="store_true",
                   help=f"don't set {TODO_ENV}=1 (the Stick's to-do view stays empty on newer models)")
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
