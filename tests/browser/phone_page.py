import tempfile, os
SHOTS=os.path.join(tempfile.gettempdir(),"claudestick-tests"); os.makedirs(SHOTS,exist_ok=True)
import json, os, sys, threading, time
ROOT=os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),"..","..")); sys.path.insert(0, ROOT+"/tools")
import mock_device as md
from http.server import ThreadingHTTPServer
md.TOKEN="tok"; srv=ThreadingHTTPServer(("127.0.0.1",17894),md.H); threading.Thread(target=srv.serve_forever,daemon=True).start()
with md.LOCK:
    md.S["sessions"]={"s1":{"project":"api-server@fix-auth","state":"tool","detail":"Bash: npm test","stop":False,"inject":"","seen":time.time(),
        "todo":{"done":2,"total":5,"now":"Rewriting auth.py","skip":0,"items":[["c","Audit the session code"],["c","Pick a session store"],["i","Rewrite auth.py for Redis"],["p","Add token refresh"],["p","Run the unit tests"]]},
        "usage":{"in":18234,"out":61200,"cr":4200000,"cw":340000,"ctx":84213,"model":"opus-5-5"}}}
    md.S["day"]="2026-10-30"; md.S["ledger"]={("s1","2026-10-30"):1834000}
    md.S["approvals"]={7:{"tool":"Bash","detail":"rm -rf build/ dist/","decision":"pending","project":"api-server","always":"","risk":"","risk_why":""}}
from playwright.sync_api import sync_playwright
with sync_playwright() as p:
    b=p.chromium.launch(); pg=b.new_page(viewport={"width":390,"height":844}, device_scale_factor=2)
    errs=[]; pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto("http://127.0.0.1:17894/?t=tok"); pg.wait_for_selector("text=Approve?")
    pg.screenshot(path=os.path.join(SHOTS,"phone7.png"), full_page=True)
    print("header:", pg.inner_text("#tk"))
    pg.once("dialog", lambda d: d.accept("use make clean instead"))
    pg.click("text=Deny + note"); time.sleep(0.6)
    a=md.S["approvals"][7]; print("deny+note ->", a["decision"], a.get("reason"))
    print("js errors:", errs)
    b.close()
