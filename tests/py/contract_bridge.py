"""The bridge must answer hooks exactly like the Stick does (the mock mirrors the firmware's API).

The same scripted conversation goes to both; every answer the Stick gives must be matched by the bridge.
The bridge may add fields (state version, session ids on cards...) but never change or drop one.
"""
import http.client
import json
import os
import sys
import tempfile
import threading
from http.server import ThreadingHTTPServer

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, ROOT + "/bridge")
sys.path.insert(0, ROOT + "/tools")
import claudestick_bridge as br  # noqa: E402
import mock_device as md  # noqa: E402

TOKEN = "c" * 32
md.TOKEN = TOKEN
md.SETUP["done"] = True
mock = ThreadingHTTPServer(("127.0.0.1", 0), md.H)
threading.Thread(target=mock.serve_forever, daemon=True).start()
store = br.Store(tempfile.mkdtemp())
store.ident["token"] = TOKEN
store.ensure_identity()
bridge = br.make_server(store, "127.0.0.1", 0)
threading.Thread(target=bridge.serve_forever, daemon=True).start()
FAILED = []


def send(port, method, path, body=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request(method, path, body=json.dumps(body).encode() if body is not None else None,
              headers={"X-Token": TOKEN, "Content-Type": "application/json", "X-Client": "stick"})
    r = c.getresponse()
    out = json.loads(r.read() or b"{}")
    c.close()
    return r.status, out


# make both sides "remote on" so the flag in answers is comparable (the bridge also needs a client polling)
send(mock.server_port, "POST", "/act", {"action": "remote", "setting": "on"})
send(bridge.server_port, "POST", "/act", {"action": "remote", "setting": "on"})
send(bridge.server_port, "GET", "/state")

ids = {}   # normalise ids: the Nth id seen on each side is "id#N"


def norm(side, v, key=None):
    if isinstance(v, dict):
        return {k: norm(side, x, k) for k, x in v.items()}
    if isinstance(v, list):
        return [norm(side, x, key) for x in v]
    if key == "id" and isinstance(v, int):
        return "id#%d" % ids.setdefault(side, {}).setdefault(v, len(ids.get(side, {})) + 1)
    return v


def subset(want, got, path=""):
    """Everything in `want` is in `got` with the same value; returns the first difference."""
    if isinstance(want, dict):
        if not isinstance(got, dict):
            return f"{path}: not an object"
        for k, v in want.items():
            if k not in got:
                return f"{path}.{k}: missing"
            d = subset(v, got[k], f"{path}.{k}")
            if d:
                return d
        return None
    if isinstance(want, list):
        if not isinstance(got, list) or len(want) != len(got):
            return f"{path}: list differs ({want!r} vs {got!r})"
        for i, (a, b) in enumerate(zip(want, got)):
            d = subset(a, b, f"{path}[{i}]")
            if d:
                return d
        return None
    return None if want == got else f"{path}: {want!r} != {got!r}"


STEPS = [
    ("POST", "/event", {"session": "s1", "project": "api", "event": "working", "detail": "Edit x", "can_inject": True, "can_stop": True}),
    ("POST", "/permission", {"session": "s1", "project": "api", "tool": "Bash", "detail": "npm test", "always": "npm test *"}),
    ("GET", "/permission?id=@", None),
    ("ACT", {"action": "always", "id": "@"}),
    ("GET", "/permission?id=@", None),
    ("GET", "/permission?id=@", None),
    ("POST", "/permission", {"session": "s1", "tool": "Bash", "detail": "rm -rf x", "risk": "high", "risk_why": "deletes", "always": "rm *"}),
    ("ACT", {"action": "deny", "id": "@", "text": "no thanks"}),
    ("GET", "/permission?id=@", None),
    ("POST", "/permission", {"session": "s1", "tool": "Edit", "detail": "a.py"}),
    ("ACT", {"action": "keyboard", "id": "@"}),
    ("GET", "/permission?id=@", None),
    ("POST", "/stop", {"session": "s1", "kind": "done", "last": "All done", "title": "Done"}),
    ("GET", "/stop?id=@", None),
    ("ACT", {"action": "choose", "id": "@", "index": 1}),
    ("GET", "/stop?id=@", None),
    ("POST", "/stop", {"session": "s1", "kind": "question", "title": "Which?", "options": [{"label": "A", "text": "Use A"}, {"label": "B", "text": "Use B"}]}),
    ("ACT", {"action": "say", "session": "s1", "text": "use C instead"}),
    ("GET", "/stop?id=@", None),
    ("POST", "/stop", {"session": "s1", "kind": "done", "last": "x"}),
    ("ACT", {"action": "dismiss", "id": "@"}),
    ("GET", "/stop?id=@", None),
    ("ACT", {"action": "macro", "session": "s1", "index": 0}),
    ("GET", "/state", None),
    ("ACT", {"action": "unsend", "session": "s1"}),
    ("GET", "/state", None),
    ("ACT", {"action": "macro", "session": "s1", "index": 0}),
    ("POST", "/event", {"session": "s1", "event": "working", "can_inject": True}),
    ("ACT", {"action": "macro", "session": "s1", "index": 0}),
    ("ACT", {"action": "sendnow", "session": "s1"}),
    ("GET", "/state", None),
    ("POST", "/event", {"session": "s1", "event": "tool", "can_stop": True, "can_preempt": True}),
    ("ACT", {"action": "sendnow", "session": "s1"}),
    ("ACT", {"action": "macro", "session": "s1", "index": 1}),
    ("POST", "/event", {"session": "s1", "event": "tool", "can_stop": True, "can_preempt": True}),
    ("POST", "/watch", {"session": "s1"}),
    ("POST", "/watch", {"session": "s1"}),
    ("ACT", {"action": "stop", "session": "s1"}),
    ("POST", "/event", {"session": "s1", "event": "working", "can_stop": True}),
    ("POST", "/event", {"session": "s1", "event": "working", "todo": {"done": 1, "total": 2, "skip": 0, "now": "B", "items": [["c", "A"], ["i", "B"]]},
                        "usage": {"in": 1, "out": 2, "cr": 3, "cw": 4, "ctx": 5, "model": "m", "day": "2026-10-30", "today": 77}}),
    ("POST", "/usage", {"session": "s1", "ctx_pct": 12.5, "cost": 0.5, "model": "Opus", "five": {"pct": 40, "resets": 111}, "seven": {"pct": 9, "resets": 222}}),
    ("POST", "/permission", {"session": "s1", "tool": "Bash", "detail": "pending one"}),
    ("GET", "/state", None),
    ("ACT", {"action": "approve", "id": 99999}),
    ("ACT", {"action": "pin", "session": "s1"}),
    ("GET", "/state", None),
    ("POST", "/event", {"session": "s1", "event": "end"}),
    ("GET", "/state", None),
]


def check_all():
    last = {}
    for n, step in enumerate(STEPS):
        res = {}
        for side, port in (("mock", mock.server_port), ("bridge", bridge.server_port)):
            if step[0] == "ACT":
                body = dict(step[1])
                if body.get("id") == "@":
                    body["id"] = last[side]
                res[side] = send(port, "POST", "/act", body)
            else:
                m, path, body = step
                path = path.replace("@", str(last.get(side, 0)))
                res[side] = send(port, m, path, body)
                if m == "POST" and "id" in res[side][1]:
                    last[side] = res[side][1]["id"]
        want = (res["mock"][0], norm("mock", res["mock"][1]))
        got = (res["bridge"][0], norm("bridge", res["bridge"][1]))
        diff = None if want[0] == got[0] else f"status {want[0]} vs {got[0]}"
        if not diff:
            if step[0] == "GET" and step[1] == "/state":
                # the bridge adds ids to cards and a version; compare what the Stick reports
                w, g = want[1], got[1]
                diff = subset({k: v for k, v in w.items() if k != "remote"}, g)
            else:
                diff = subset(want[1], got[1])
        label = f"{step[0]} {step[1] if step[0] != 'ACT' else step[1]['action']}"
        print(("PASS " if not diff else "FAIL ") + f"step {n:02d} {label}" + (f"  | {diff}" if diff else ""))
        if diff:
            FAILED.append(label)
            print("      mock:  ", json.dumps(want)[:300])
            print("      bridge:", json.dumps(got)[:300])


check_all()
print("\ncontract: %d differences" % len(FAILED) if FAILED else "\ncontract: bridge == Stick")
sys.exit(1 if FAILED else 0)
