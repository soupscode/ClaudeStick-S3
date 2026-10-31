import json, os, subprocess, sys, threading, time, tempfile, shutil
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
ROOT=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),"..","..")); sys.path.insert(0, ROOT+"/tools")
import mock_device as md
md.TOKEN="tok8"; md.SETUP["done"]=True
PORT=17998
srv=ThreadingHTTPServer(("127.0.0.1",PORT),md.H); threading.Thread(target=srv.serve_forever,daemon=True).start()
NT=[]
class N(BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def do_POST(self):
        NT.append({"title":self.headers.get("Title"),"actions":self.headers.get("Actions"),"click":self.headers.get("Click")})
        self.rfile.read(int(self.headers["Content-Length"])); self.send_response(200); self.end_headers()
ns=ThreadingHTTPServer(("127.0.0.1",17999),N); threading.Thread(target=ns.serve_forever,daemon=True).start()
home=tempfile.mkdtemp(); env=dict(os.environ,HOME=home); work=tempfile.mkdtemp()
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ")+name+(("  | "+str(extra)[:160]) if extra and not cond else ""))
    return cond
# --- install from the device, no checkout
r=subprocess.run(["bash","-c",f"curl -s http://127.0.0.1:{PORT}/install.py | python3 - --host 127.0.0.1 --port {PORT} --token tok8"],cwd=work,env=env,capture_output=True,text=True)
local=json.load(open(home+"/.claudestick/config.json"))
check("install: local config holds only connection + identity", set(local)<={"host","port","token","device_id","mdns_host","added_todo_env"}, local)
check("install: device id recorded", local.get("device_id")==md.DEVICE_ID, local)
st=json.load(open(home+"/.claude/settings.json")); check("install: 8 hooks", len(st["hooks"])==8)
HOOK=home+"/.claudestick/claudestick_hook.py"
repo=os.path.join(work,"api-server"); os.makedirs(repo)
subprocess.run(["git","init","-q","-b","fix-auth",repo]); subprocess.run(["git","-C",repo,"commit","-q","--allow-empty","-m","x"],env={**env,"GIT_AUTHOR_NAME":"a","GIT_AUTHOR_EMAIL":"a@b","GIT_COMMITTER_NAME":"a","GIT_COMMITTER_EMAIL":"a@b"})
tp=home+"/t.jsonl"
import glob, time as _t
st_path=home+"/.claude/settings.json"
def settings(): return json.load(open(st_path))
def local(): return json.load(open(home+"/.claudestick/config.json"))
def install(*extra): return subprocess.run(["bash","-c",f"curl -s http://127.0.0.1:{PORT}/install.py | python3 - --host 127.0.0.1 --port {PORT} --token tok8 {' '.join(extra)}"],cwd=work,env=env,capture_output=True,text=True)
def uninstall(): return subprocess.run(["bash","-c",f"curl -s http://127.0.0.1:{PORT}/install.py | python3 - --uninstall"],cwd=work,env=env,capture_output=True,text=True)
STATUS=lambda stdin,timeout=10: subprocess.run([sys.executable,HOOK,"--statusline"],input=stdin,env=env,capture_output=True,text=True,timeout=timeout)
DATA={"session_id":"s8","workspace":{"current_dir":repo},"context_window":{"used_percentage":42.5,"context_window_size":200000},
      "cost":{"total_cost_usd":1.84},"model":{"display_name":"Opus 5.5"},
      "rate_limits":{"five_hour":{"used_percentage":63.2,"resets_at":1793398040},"seven_day":{"used_percentage":28,"resets_at":1793663600}}}
def usage_calls(): return [b for (p,b) in md.S["log"] if p=="/usage"]

os.system("rm -f /tmp/claudestick-status-* /tmp/claudestick-down-*")
# --- 1. fresh install adds a status line
sl=settings().get("statusLine",{})
check("install adds a status line", sl.get("type")=="command" and "--statusline" in sl.get("command",""), sl)
check("...with nothing to chain", "statusline_chain" not in local(), local())
md.S["sessions"]["s8"]={"project":"x","state":"tool","detail":"","stop":False,"inject":"","seen":_t.time()}
md.S["log"].clear()
r=STATUS(json.dumps(DATA))
check("default status line prints ctx / 5h / 7d", r.stdout.strip()=="ctx 42% | 5h 63% | 7d 28%", (r.stdout,r.stderr))
_t.sleep(0.3)
check("limits reach the Stick", md.S.get("limits",{}).get("five",{}).get("pct")==63.2 and md.S["limits"]["seven"]["resets"]==1793663600, md.S.get("limits"))
check("context % and model reach the Stick", md.S["sessions"]["s8"].get("u_ctx_pct")==42.5 and md.S["sessions"]["s8"].get("u_model")=="Opus 5.5", md.S["sessions"]["s8"])
n=len(usage_calls()); STATUS(json.dumps(DATA))
check("forwarding is rate-limited", len(usage_calls())==n, (n,len(usage_calls())))
# bad / empty input never breaks the status line
r=STATUS("not json"); check("garbage input -> empty line, no crash", r.stdout=="" and r.returncode==0, (r.stdout,r.stderr))
r=STATUS(json.dumps({"session_id":"s8","model":{"display_name":"Opus"}})); check("no limits (API user) -> still fine", r.returncode==0 and "5h" not in r.stdout, r.stdout)

# --- 2. an existing status line is kept and wrapped
json.dump({**settings(),"statusLine":{"type":"command","command":"echo MYLINE","padding":2}},open(st_path,"w"))
r=install(); out=r.stdout
sl=settings()["statusLine"]
check("existing status line is wrapped", "--statusline" in sl["command"] and sl.get("padding")==2, sl)
check("...and remembered for chaining", local().get("statusline_chain")=="echo MYLINE", local())
md.S["log"].clear(); os.system("rm -f /tmp/claudestick-status-*")
r=STATUS(json.dumps(DATA))
check("chained output passes through unchanged", r.stdout.strip()=="MYLINE", (r.stdout,r.stderr))
_t.sleep(0.3); check("...while limits still reach the Stick", len(usage_calls())==1)
install()
check("re-running install doesn't wrap twice", local().get("statusline_chain")=="echo MYLINE" and settings()["statusLine"]["command"].count("--statusline")==1, (local(),settings()["statusLine"]))
uninstall()
st=settings()
check("uninstall restores your status line", st.get("statusLine")=={"type":"command","command":"echo MYLINE","padding":2} and "hooks" not in st, st.get("statusLine"))

# --- 3. nothing there before -> nothing left after; and --no-statusline
json.dump({k:v for k,v in settings().items() if k!="statusLine"},open(st_path,"w"))   # you remove your own status line
check("uninstall forgets the chain", "statusline_chain" not in local(), local())
install(); check("reinstall adds ours again", "--statusline" in settings().get("statusLine",{}).get("command",""))
uninstall(); check("uninstall removes the status line we added", "statusLine" not in settings(), settings())
install("--no-statusline"); check("--no-statusline leaves it alone", "statusLine" not in settings(), settings())

# --- 4. a non-command status line is left alone
json.dump({**settings(),"statusLine":{"type":"static","text":"hi"}},open(st_path,"w")); install()
check("non-command status line untouched", settings()["statusLine"]=={"type":"static","text":"hi"}, settings()["statusLine"])
json.dump({k:v for k,v in settings().items() if k!="statusLine"},open(st_path,"w"))

# --- 5. Stick unreachable: the status line stays fast and your chain still prints
install(); json.dump({**settings(),"statusLine":{"type":"command","command":"echo MYLINE"}},open(st_path,"w")); install()
cfg=json.load(open(home+"/.claudestick/config.json")); cfg["host"]="127.0.0.1"; cfg["port"]=1; json.dump(cfg,open(home+"/.claudestick/config.json","w"))
os.system("rm -f /tmp/claudestick-status-* /tmp/claudestick-down-*")
t0=_t.time(); r=STATUS(json.dumps(DATA)); dt=_t.time()-t0
check("unreachable Stick: chain still prints, fast", r.stdout.strip()=="MYLINE" and dt<3, (r.stdout,dt))
