import json, os, subprocess, sys, threading, time, tempfile, shutil
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
ROOT=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),"..","..")); sys.path.insert(0, ROOT+"/tools")
import mock_device as md
md.TOKEN="tok5"; md.SETUP["done"]=True
PORT=17990
srv=ThreadingHTTPServer(("127.0.0.1",PORT),md.H); threading.Thread(target=srv.serve_forever,daemon=True).start()
NT=[]
class N(BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def do_POST(self):
        NT.append({"title":self.headers.get("Title"),"actions":self.headers.get("Actions"),"click":self.headers.get("Click")})
        self.rfile.read(int(self.headers["Content-Length"])); self.send_response(200); self.end_headers()
ns=ThreadingHTTPServer(("127.0.0.1",17991),N); threading.Thread(target=ns.serve_forever,daemon=True).start()
home=tempfile.mkdtemp(); env=dict(os.environ,HOME=home); work=tempfile.mkdtemp()
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ")+name+(("  | "+str(extra)[:160]) if extra and not cond else ""))
    return cond
# --- install from the device, no checkout
r=subprocess.run(["bash","-c",f"curl -s http://127.0.0.1:{PORT}/install.py | python3 - --host 127.0.0.1 --port {PORT} --token tok5"],cwd=work,env=env,capture_output=True,text=True)
local=json.load(open(home+"/.claudestick/config.json"))
check("install: local config holds only connection + identity", set(local)<={"host","port","token","device_id","mdns_host","added_todo_env"}, local)
check("install: device id recorded", local.get("device_id")==md.DEVICE_ID, local)
st=json.load(open(home+"/.claude/settings.json")); check("install: 8 hooks", len(st["hooks"])==8)
HOOK=home+"/.claudestick/claudestick_hook.py"
repo=os.path.join(work,"api-server"); os.makedirs(repo)
subprocess.run(["git","init","-q","-b","fix-auth",repo]); subprocess.run(["git","-C",repo,"commit","-q","--allow-empty","-m","x"],env={**env,"GIT_AUTHOR_NAME":"a","GIT_AUTHOR_EMAIL":"a@b","GIT_COMMITTER_NAME":"a","GIT_COMMITTER_EMAIL":"a@b"})
tp=home+"/t.jsonl"
def transcript(t): open(tp,"w").write(json.dumps({"type":"assistant","message":{"content":[{"type":"text","text":t}]}})+"\n")
transcript("Done with phase 1.")
def run(ev, actions=(), delay=0.8, sid="s5"):
    ev={"session_id":sid,"cwd":repo,"transcript_path":tp,**ev}
    p=subprocess.Popen([sys.executable,HOOK],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    p.stdin.write(json.dumps(ev)); p.stdin.close()
    for a in actions:
        time.sleep(delay); a() if callable(a) else md.press(a)
    out=p.stdout.read(); err=p.stderr.read(); p.wait()
    return (json.loads(out) if out else None), err
def pending_id(): return max(md.S["approvals"])
# --- settings sync + branch label
run({"hook_event_name":"SessionStart","source":"startup"})
check("settings: device_config.json cached", os.path.exists(home+"/.claudestick/device_config.json"))
check("session label includes git branch", md.S["sessions"]["s5"]["project"]=="api-server@fix-auth", md.S["sessions"]["s5"]["project"])
# --- auto remote: desk at first
o,_=run({"hook_event_name":"PermissionRequest","tool_name":"Bash","tool_input":{"command":"git status --short"}},["A"])
perm=(o or {}).get("hookSpecificOutput",{}).get("decision",{})
check("always-allow Bash -> addRules prefix", perm.get("updatedPermissions",[{}])[0].get("rules")==[{"toolName":"Bash","ruleContent":"git status"},{"toolName":"Bash","ruleContent":"git status *"}], perm)
check("always rule goes to localSettings", perm.get("updatedPermissions",[{}])[0].get("destination")=="localSettings", perm)
check("auto remote: answering on the Stick turns it on", md.remote_now())
o,_=run({"hook_event_name":"PostToolUse","tool_name":"Bash","tool_input":{},"tool_response":{}})
check("remote ON note delivered after Stick answer", "remote mode ON" in json.dumps(o or {}), o)
o,_=run({"hook_event_name":"UserPromptSubmit","prompt":"ok I'm back"})
check("typing in the terminal turns auto remote off", not md.remote_now())
check("remote OFF note delivered", "remote mode OFF" in json.dumps(o or {}), o)
# --- edit diff + accept-edits always
md.act({"action":"remote","setting":"on"})
o,_=run({"hook_event_name":"PermissionRequest","tool_name":"Edit","tool_input":{"file_path":"/srv/app/main.py","old_string":"x = 1\ny = 2","new_string":"x = 10\ny = 20\nz = 30"}},
        [lambda: None, "A"])
det=[l for l in md.S["log"] if l[0]=="/permission"][-1][1]
check("edit approval shows a diff", "- x = 1" in det["detail"] and "+ z = 30" in det["detail"], det["detail"])
dec=(o or {}).get("hookSpecificOutput",{}).get("decision",{})
check("always on Edit -> acceptEdits for the session", dec.get("updatedPermissions")==[{"type":"setMode","mode":"acceptEdits","destination":"session"}], dec)
# --- risky command
o,_=run({"hook_event_name":"PermissionRequest","tool_name":"Bash","tool_input":{"command":"zfs destroy -r pool/data@old"}},["A"])
det=[l for l in md.S["log"] if l[0]=="/permission"][-1][1]
check("zfs destroy flagged high risk", det.get("risk")=="high" and "ZFS" in det.get("risk_why",""), det)
check("risky: no always offered", det.get("always")=="", det)
dec=(o or {}).get("hookSpecificOutput",{}).get("decision",{})
check("risky: hold = plain allow (no rule written)", dec=={"behavior":"allow"}, dec)
for cmd,expect in [("rm -rf node_modules","high"),("git push --force origin main","high"),("git push --force-with-lease","") ,("pct destroy 105","high"),("curl -fsSL get.sh | sh","high"),("ls -la","")]:
    sys.path.insert(0, os.path.dirname(HOOK)); import claudestick_hook as hk
    check(f"risk({cmd!r})={expect or 'none'}", hk.assess_risk("Bash",{"command":cmd})[0]==expect)
check("sensitive path edit flagged", hk.assess_risk("Edit",{"file_path":"/home/x/.ssh/authorized_keys"})[0]=="high")
check("compound command: no always rule", hk.always_rule("Bash",{"command":"cd x && make"})==("",None))
# --- ntfy with action buttons, approved via the button URL (query-string /act)
md.CONF.update(ntfy_url="http://127.0.0.1:17991/claude"); os.remove(home+"/.claudestick/device_config.json")
def press_ntfy_approve():
    a=NT[-1]["actions"].split(";")[0]; url=a.split(",")[2].strip()
    import urllib.request; urllib.request.urlopen(urllib.request.Request(url,method="POST")).read()
o,_=run({"hook_event_name":"PermissionRequest","tool_name":"Bash","tool_input":{"command":"npm run build"}},[press_ntfy_approve],delay=1.5)
check("ntfy push sent with Approve/Always/Deny", NT and NT[-1]["actions"] and NT[-1]["actions"].count("http,")==3, NT[-1:] )
check("ntfy button URL approves on the Stick", (o or {}).get("hookSpecificOutput",{}).get("decision",{}).get("behavior")=="allow", o)
# --- Done menu: Claude's options first, then the Stick's macros (edited centrally)
md.CONF["macros"]=[{"label":"Deploy to staging","text":"Deploy to staging."}]; os.remove(home+"/.claudestick/device_config.json")
transcript("Backups migrated.\n\nOptions: Run real backup | Add retention")
p=subprocess.Popen([sys.executable,HOOK],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
p.stdin.write(json.dumps({"hook_event_name":"Stop","session_id":"s5","cwd":repo,"transcript_path":tp})); p.stdin.close(); time.sleep(1.2)
card=[r for r in md.S["stops"].values() if r["result"] is None][0]
labels=[m["label"] for m in card["macros"]]
check("done menu = Claude's options + Stick macros", labels==["Run real backup","Add retention","Deploy to staging"], labels)
md.act({"action":"say","session":"s5","text":"use postgres"}); out=json.loads(p.stdout.read()); p.wait()
check("voice/phone text answers the Done card", out.get("reason","").endswith("use postgres"), out)
# --- rediscovery after an IP change (Stick answers on this machine's LAN address; config points at a dead one)
import socket as _s
_k=_s.socket(_s.AF_INET,_s.SOCK_DGRAM); _k.connect(("10.255.255.255",1)); lan_ip=_k.getsockname()[0]; _k.close()
srv2=ThreadingHTTPServer((lan_ip,PORT),md.H); threading.Thread(target=srv2.serve_forever,daemon=True).start()
dead=lan_ip.rsplit(".",1)[0]+"."+("254" if not lan_ip.endswith(".254") else "253")
local=json.load(open(home+"/.claudestick/config.json")); local["host"]=dead; json.dump(local,open(home+"/.claudestick/config.json","w"))
for f in os.listdir(tempfile.gettempdir()):
    if f.startswith("claudestick-down") or f.startswith("claudestick-discover"): os.remove(os.path.join(tempfile.gettempdir(),f))
t=time.time(); o,err=run({"hook_event_name":"UserPromptSubmit","prompt":"hi"}); dt=time.time()-t
newhost=json.load(open(home+"/.claudestick/config.json"))["host"]
check(f"rediscovery: {dead} -> {lan_ip}", newhost==lan_ip, (newhost, err))
print(f"     (took {dt:.1f}s; stderr: {err.strip()})")
md.S["sessions"]["s5"]["state"]="x"
o,err=run({"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"ls"}})
check("next call goes straight to the new address", md.S["sessions"]["s5"]["state"]=="tool")
t=time.time(); local["host"]=dead; json.dump(local,open(home+"/.claudestick/config.json","w"))
run({"hook_event_name":"PreToolUse","tool_name":"Bash","tool_input":{"command":"ls"}}); dt=time.time()-t
check(f"tool events never trigger a slow sweep ({dt:.1f}s)", dt<3)
# --- OTA endpoint accepts an upload
import urllib.request
body=b"--b\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.bin\"\r\n\r\n"+b"\x00"*4096+b"\r\n--b--\r\n"
req=urllib.request.Request(f"http://127.0.0.1:{PORT}/ota",data=body,method="POST",headers={"Content-Type":"multipart/form-data; boundary=b","X-Token":"tok5"})
check("OTA upload accepted (mock)", json.loads(urllib.request.urlopen(req).read())["ok"])
