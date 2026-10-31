import json, os, subprocess, sys, threading, time, tempfile, shutil
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
ROOT=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),"..","..")); sys.path.insert(0, ROOT+"/tools")
import mock_device as md
md.TOKEN="tok7"; md.SETUP["done"]=True
PORT=17992
srv=ThreadingHTTPServer(("127.0.0.1",PORT),md.H); threading.Thread(target=srv.serve_forever,daemon=True).start()
NT=[]
class N(BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def do_POST(self):
        NT.append({"title":self.headers.get("Title"),"actions":self.headers.get("Actions"),"click":self.headers.get("Click")})
        self.rfile.read(int(self.headers["Content-Length"])); self.send_response(200); self.end_headers()
ns=ThreadingHTTPServer(("127.0.0.1",17993),N); threading.Thread(target=ns.serve_forever,daemon=True).start()
home=tempfile.mkdtemp(); env=dict(os.environ,HOME=home); work=tempfile.mkdtemp()
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ")+name+(("  | "+str(extra)[:160]) if extra and not cond else ""))
    return cond
# --- install from the device, no checkout
r=subprocess.run(["bash","-c",f"curl -s http://127.0.0.1:{PORT}/install.py | python3 - --host 127.0.0.1 --port {PORT} --token tok7"],cwd=work,env=env,capture_output=True,text=True)
local=json.load(open(home+"/.claudestick/config.json"))
check("install: local config holds only connection + identity", set(local)<={"host","port","token","device_id","mdns_host","added_todo_env"}, local)
check("install: device id recorded", local.get("device_id")==md.DEVICE_ID, local)
st=json.load(open(home+"/.claude/settings.json")); check("install: 8 hooks", len(st["hooks"])==8)
HOOK=home+"/.claudestick/claudestick_hook.py"
repo=os.path.join(work,"api-server"); os.makedirs(repo)
subprocess.run(["git","init","-q","-b","fix-auth",repo]); subprocess.run(["git","-C",repo,"commit","-q","--allow-empty","-m","x"],env={**env,"GIT_AUTHOR_NAME":"a","GIT_AUTHOR_EMAIL":"a@b","GIT_COMMITTER_NAME":"a","GIT_COMMITTER_EMAIL":"a@b"})
tp=home+"/t.jsonl"
import time as _t
def line(mid,i,o,cr,cw):
    return json.dumps({"type":"assistant","timestamp":_t.strftime("%Y-%m-%dT%H:%M:%SZ",_t.gmtime()),
      "message":{"id":mid,"model":"claude-opus-5-5","content":[{"type":"text","text":"Phase 1 done."}],
      "usage":{"input_tokens":i,"output_tokens":o,"cache_read_input_tokens":cr,"cache_creation_input_tokens":cw}}})+"\n"
open(tp,"w").write(line("m1",100,400,5000,2000))
def run(ev, actions=(), delay=0.8, sid="s7"):
    ev={"session_id":sid,"cwd":repo,"transcript_path":tp,**ev}
    p=subprocess.Popen([sys.executable,HOOK],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    p.stdin.write(json.dumps(ev)); p.stdin.close()
    for a in actions:
        time.sleep(delay); a() if callable(a) else md.press(a)
    out=p.stdout.read(); err=p.stderr.read(); p.wait()
    return (json.loads(out) if out else None), err
def state():
    import urllib.request
    return json.load(urllib.request.urlopen(f"http://127.0.0.1:{PORT}/state?t=tok7"))
run({"hook_event_name":"SessionStart","source":"startup"})
s=[x for x in state()["sessions"] if x["id"]=="s7"][0]
check("session start: usage sent", s.get("usage",{}).get("out")==400 and s["usage"]["ctx"]==7100, s)
for subj,tid in (("Audit jobs","1"),("Rewrite sync.py","2"),("Dry run","3")):
    run({"hook_event_name":"PostToolUse","tool_name":"TaskCreate","tool_input":{"subject":subj},"tool_response":{"task":{"id":tid,"subject":subj}}})
run({"hook_event_name":"PostToolUse","tool_name":"TaskUpdate","tool_input":{"taskId":"1","status":"completed"},"tool_response":{}})
run({"hook_event_name":"PostToolUse","tool_name":"TaskUpdate","tool_input":{"taskId":"2","status":"in_progress","activeForm":"Rewriting sync.py"},"tool_response":{}})
t=[x for x in state()["sessions"] if x["id"]=="s7"][0].get("todo",{})
check("to-do: 1/3 with current step", t.get("done")==1 and t.get("total")==3 and t.get("now")=="Rewriting sync.py", t)
check("to-do: item states", [i[0] for i in t.get("items",[])]==["c","i","p"], t)
open(tp,"a").write(line("m2",50,600,7000,0))
run({"hook_event_name":"Stop"})
st=state(); s=[x for x in st["sessions"] if x["id"]=="s7"][0]
check("stop: usage updated", s["usage"]["out"]==1000 and s["usage"]["in"]==150, s["usage"])
check("tokens today = everything incl. cache", st.get("tokens_today")==150+1000+12000+2000, st.get("tokens_today"))
o,_=run({"hook_event_name":"PermissionRequest","tool_name":"Bash","tool_input":{"command":"rm -rf build/"}},["n use make clean instead"])
msg=(o or {}).get("hookSpecificOutput",{}).get("decision",{})
check("voice deny: reason reaches Claude", msg.get("behavior")=="deny" and "use make clean instead" in msg.get("message",""), o)
o,_=run({"hook_event_name":"PermissionRequest","tool_name":"Bash","tool_input":{"command":"ls"}},["b"])
msg=(o or {}).get("hookSpecificOutput",{}).get("decision",{})
check("plain deny unchanged", msg.get("message")=="Denied by the user from their ClaudeStick.", o)
run({"hook_event_name":"SessionStart","source":"clear"})
t=[x for x in state()["sessions"] if x["id"]=="s7"][0].get("todo")
check("/clear resets the to-do list", not t, t)
md.CONF["remote"]="on"; md.CONF["ntfy_url"]="http://127.0.0.1:17993/claude"
try: os.remove(home+"/.claudestick/device_config.json")
except OSError: pass
run({"hook_event_name":"SessionStart","source":"startup"}, sid="s8")
for tid in ("1","2"):
    run({"hook_event_name":"PostToolUse","tool_name":"TaskCreate","tool_input":{"subject":"t"+tid},"tool_response":{"task":{"id":tid}}}, sid="s8")
run({"hook_event_name":"PostToolUse","tool_name":"TaskUpdate","tool_input":{"taskId":"1","status":"completed"},"tool_response":{}}, sid="s8")
check("no plan-done push while items remain", not any("Plan done" in (n["title"] or "") for n in NT), NT)
run({"hook_event_name":"PostToolUse","tool_name":"TaskUpdate","tool_input":{"taskId":"2","status":"completed"},"tool_response":{}}, sid="s8")
check("plan done: phone push in remote mode", any("Plan done" in (n["title"] or "") for n in NT), NT)
plan_events=[b for (p_,b) in md.S["log"] if p_=="/event" and b.get("plan_done")]
check("plan done: Stick told once", len(plan_events)==1, len(plan_events))
md.CONF["remote"]="auto"; md.CONF["ntfy_url"]=""
# --- text folding: the screen gets ASCII, Claude still gets the exact option text
open(tp,"w").write(json.dumps({"type":"assistant","message":{"id":"m9","content":[{"type":"text","text":"Voil\u00e0 \u2014 it\u2019s done.\n\nOptions: Caf\u00e9 plan | Next"}],"usage":{"input_tokens":1,"output_tokens":1}}})+"\n")
md.CONF["stop_menu"]="always"
try: os.remove(home+"/.claudestick/device_config.json")
except OSError: pass
p=subprocess.Popen([sys.executable,HOOK],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
p.stdin.write(json.dumps({"hook_event_name":"Stop","session_id":"s7","cwd":repo,"transcript_path":tp})); p.stdin.close(); time.sleep(1.2)
card=[r for r in md.S["stops"].values() if r["result"] is None][0]
check("screen text is ASCII", card["macros"][0]["label"]=="Cafe plan" and "\u2019" not in card["last"] and "Voila - it's done." in card["last"], (card["macros"][0], card["last"]))
md.act({"action":"choose","id":max(md.S["stops"]),"index":0}); out=json.loads(p.stdout.read()); p.wait()
check("Claude still gets the exact option text", "Go with: Caf\u00e9 plan" in out.get("reason",""), out)
md.CONF["stop_menu"]="remote"
st=json.load(open(home+"/.claude/settings.json"))
check("install: to-do tools enabled", st.get("env",{}).get("CLAUDE_CODE_ENABLE_TODO_TOOLS")=="1", st.get("env"))
r=subprocess.run([sys.executable,home+"/.claudestick/claudestick_hook.py"],input="{}",env=env,capture_output=True,text=True)
import glob
inst=glob.glob(work+"/*.py")
r=subprocess.run(["bash","-c",f"curl -s http://127.0.0.1:{PORT}/install.py | python3 - --uninstall"],cwd=work,env=env,capture_output=True,text=True)
st=json.load(open(home+"/.claude/settings.json"))
check("uninstall: removes hooks and our env var", "hooks" not in st and "env" not in st, st)
