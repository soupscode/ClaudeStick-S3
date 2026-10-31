import json, os, sys, importlib.util, tempfile
os.environ["CLAUDESTICK_CONFIG"] = tempfile.mkdtemp() + "/config.json"
spec = importlib.util.spec_from_file_location("h", os.path.join(os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),"..","..")),"hooks","claudestick_hook.py"))
h = importlib.util.module_from_spec(spec); spec.loader.exec_module(h)
ok = 0
def check(c, m):
    global ok
    assert c, m; ok += 1
# Task tools
st = h.load_sess("s1")
h.track_tasks(st, "TaskCreate", {"subject": "A", "activeForm": "Doing A"}, {"task": {"id": "1", "subject": "A"}})
h.track_tasks(st, "TaskCreate", {"subject": "B"}, {"task": {"id": "2"}})
h.track_tasks(st, "TaskCreate", {"subject": "C"}, {})            # no id in result -> sequential
h.track_tasks(st, "TaskUpdate", {"taskId": "1", "status": "completed"}, {})
h.track_tasks(st, "TaskUpdate", {"id": "2", "status": "in_progress", "activeForm": "Doing B"}, {})
p = h.todo_payload(st)
check(p["total"] == 3 and p["done"] == 1 and p["now"] == "Doing B", p)
check([i[0] for i in p["items"]] == ["c", "i", "p"], p)
h.track_tasks(st, "TaskUpdate", {"taskId": "3", "status": "deleted"}, {})
check(h.todo_payload(st)["total"] == 2, "delete")
# TodoWrite replaces list
h.track_tasks(st, "TodoWrite", {"todos": [{"content": "x", "status": "completed", "activeForm": "X"},
                                         {"content": "y", "status": "in_progress", "activeForm": "Y-ing"}]}, None)
p = h.todo_payload(st); check(p["total"] == 2 and p["now"] == "Y-ing", p)
# long list windowing
st2 = {"tasks": {}, "order": [], "usage": {}}
for i in range(20):
    h.track_tasks(st2, "TaskCreate", {"subject": f"t{i}"}, {"task": {"id": str(i + 1)}})
for i in range(15):
    h.track_tasks(st2, "TaskUpdate", {"taskId": str(i + 1), "status": "completed"}, {})
p = h.todo_payload(st2); check(len(p["items"]) == 12 and p["skip"] == 8 and p["items"][7][1] == "t15", p)
# transcript usage, incl. duplicated message lines and a partial last line
tp = os.path.join(tempfile.mkdtemp(), "t.jsonl")
def line(mid, i, o, cr, cw, ts="2026-10-30T15:00:00Z", side=False):
    return json.dumps({"type": "assistant", "isSidechain": side, "timestamp": ts,
                       "message": {"id": mid, "model": "claude-opus-5-5", "usage": {"input_tokens": i, "output_tokens": o,
                       "cache_read_input_tokens": cr, "cache_creation_input_tokens": cw}}}) + "\n"
with open(tp, "w") as f:
    f.write(json.dumps({"type": "user", "message": {"content": "hi"}}) + "\n")
    f.write(line("m1", 10, 5, 1000, 200))
    f.write(line("m1", 10, 50, 1000, 200))      # same message, later line: replaces
    f.write(line("sub", 999, 999, 0, 0, side=True))
st3 = {"tasks": {}, "order": [], "usage": {}}
check(h.read_usage(st3, tp), "read")
u = h.usage_payload(st3)
check(u["in"] == 10 and u["out"] == 50 and u["cr"] == 1000 and u["cw"] == 200 and u["ctx"] == 1210 and u["model"] == "opus-5-5", u)
with open(tp, "a") as f:
    f.write(line("m2", 3, 7, 1210, 0))
    f.write('{"type":"assistant","message":{"id":"m3","usage":{"input_tokens":1')   # partial
check(h.read_usage(st3, tp), "incremental")
u = h.usage_payload(st3); check(u["in"] == 13 and u["out"] == 57 and u["ctx"] == 1213, u)
check(not h.read_usage(st3, tp), "nothing new")
check(u["today"] == 13 + 57 + 2210 + 200 or u["day"] != "2026-10-30", u)
# session_extras end-to-end
ev = {"session_id": "s9", "hook_event_name": "PostToolUse", "tool_name": "TaskCreate",
      "tool_input": {"subject": "Plan"}, "tool_response": {"task": {"id": "1"}}, "transcript_path": tp}
x = h.session_extras(ev); check(x["todo"]["total"] == 1 and "usage" in x, x)
x = h.session_extras({**ev, "tool_name": "Bash", "tool_input": {}}); check("todo" not in x and "usage" not in x, x)
x = h.session_extras({**ev, "hook_event_name": "Stop"}, force_usage=True); check("usage" in x and x["todo"]["total"] == 1, x)
x = h.session_extras({"session_id": "s9", "hook_event_name": "SessionStart", "source": "clear", "transcript_path": tp}, True)
check(x["todo"]["total"] == 0, x)
print("PASS hook unit tests:", ok, "assertions")
