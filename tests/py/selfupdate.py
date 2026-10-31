import os, sys, json, threading, subprocess, tempfile, hashlib, time
sys.path.insert(0, os.path.join(os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),"..","..")),"tools")); import mock_device as md
from http.server import ThreadingHTTPServer
md.TOKEN="t6"; srv=ThreadingHTTPServer(("127.0.0.1",17998),md.H); threading.Thread(target=srv.serve_forever,daemon=True).start()
home=tempfile.mkdtemp(); env=dict(os.environ,HOME=home)
subprocess.run(["bash","-c","curl -s http://127.0.0.1:17998/install.py | python3 - --host 127.0.0.1 --port 17998 --token t6"],cwd=tempfile.mkdtemp(),env=env,capture_output=True)
hook=home+"/.claudestick/claudestick_hook.py"; repo=open(os.path.join(os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),"..","..")),"hooks","claudestick_hook.py"),"rb").read()
open(hook,"ab").write(b"\n# old version\n")                      # simulate an outdated install
os.remove(home+"/.claudestick/device_config.json") if os.path.exists(home+"/.claudestick/device_config.json") else None
r=subprocess.run([sys.executable,hook],input=json.dumps({"hook_event_name":"SessionStart","session_id":"x","cwd":"/tmp"}),env=env,capture_output=True,text=True)
print("PASS" if open(hook,"rb").read()==repo else "FAIL", "outdated hook replaced with the Stick's copy |", r.stderr.strip())
r=subprocess.run([sys.executable,hook],input=json.dumps({"hook_event_name":"PreToolUse","session_id":"x","cwd":"/tmp","tool_name":"Bash","tool_input":{}}),env=env,capture_output=True,text=True)
print("PASS" if r.returncode==0 and not r.stderr else "FAIL", "updated hook runs cleanly")
# a corrupted download must never be installed
open(hook,"ab").write(b"\n# old again\n"); os.remove(home+"/.claudestick/device_config.json")
orig=md.H.do_GET
def bad(self):
    if self.path.startswith("/claudestick_hook.py"): return self.reply("def broken(:\n", ctype="text/x-python")
    return orig(self)
md.H.do_GET=bad
before=open(hook,"rb").read()
subprocess.run([sys.executable,hook],input=json.dumps({"hook_event_name":"SessionStart","session_id":"y","cwd":"/tmp"}),env=env,capture_output=True,text=True)
print("PASS" if open(hook,"rb").read()==before else "FAIL", "bad download (hash mismatch) is ignored")
