"""Writes what a real bridge sends the Stick (GET /state?n=1) for a known scenario, so the firmware's client
is tested against the bridge's actual output rather than a hand-written copy of it.
Usage: gen_bridge_fixture.py OUT.json   (used by tests/host/run.sh; the expectations are in tests/host/bridge_test.cpp)"""
import json
import os
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, ROOT + "/bridge")
import claudestick_bridge as br  # noqa: E402

store = br.Store(tempfile.mkdtemp())
store.ident["token"] = "t" * 32
store.ensure_identity()
core = br.Core(store)
H = {"session": "real-1", "project": "api-server@main"}
core.event({**H, "event": "working", "detail": "Edit main.py",
            "todo": {"done": 1, "total": 3, "skip": 0, "now": "Write tests", "items": [["c", "Plan"], ["i", "Write tests"], ["p", "Ship"]]},
            "usage": {"in": 10, "out": 20, "cr": 300, "cw": 40, "ctx": 5000, "model": "sonnet", "day": "2026-10-30", "today": 4242}})
core.event({"session": "real-2", "project": "docs", "event": "working"})
core.usage({"session": "real-1", "ctx_pct": 42.5, "cost": 1.25, "model": "Opus 5.5",
            "five": {"pct": 61, "resets": 1793393600}, "seven": {"pct": 12, "resets": 1793890000}})
core.event({"session": "real-2", "event": "working", "plan_done": True})
core.poll("stick@x")                                    # a Stick is connected, so remote can be on
core.act({"action": "remote", "setting": "on"})
code, ap = core.permission_new({**H, "tool": "Bash", "detail": "npm run build", "always": "npm run *"})
core.stop_new({"session": "real-2", "project": "docs", "kind": "done", "last": "All tests pass", "title": "Done"})
core.act({"action": "pin", "session": "real-1"})
json.dump(core.poll("stick@x", None, lite=True), open(sys.argv[1], "w"))
print("approval id", ap["id"])
