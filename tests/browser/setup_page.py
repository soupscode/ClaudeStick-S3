import tempfile, os
SHOTS=os.path.join(tempfile.gettempdir(),"claudestick-tests"); os.makedirs(SHOTS,exist_ok=True)
import os, sys, threading, time, json
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler, BaseHTTPRequestHandler
sys.path.insert(0, os.path.join(os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),"..","..")),"tools")); import mock_device as md
md.TOKEN="tok5"; md.SETUP.update(done=True, connected=True, ssid="home-wifi")
srv=ThreadingHTTPServer(("127.0.0.1",17995),md.H); threading.Thread(target=srv.serve_forever,daemon=True).start()
FAKE=tempfile.mkdtemp()
open(os.path.join(FAKE,"ota.json"),"w").write('{"version":"99.0.0","app":"firmware/claudestick-app.bin"}')
os.makedirs(os.path.join(FAKE,"firmware"),exist_ok=True)
open(os.path.join(FAKE,"firmware","claudestick-app.bin"),"wb").write(os.urandom(400000))   # stands in for the real app image
class Pages(SimpleHTTPRequestHandler):   # like GitHub Pages: static + CORS *
    def __init__(s,*a,**k): super().__init__(*a,directory=FAKE,**k)
    def end_headers(s): s.send_header("Access-Control-Allow-Origin","*"); super().end_headers()
    def log_message(s,*a): pass
ps=ThreadingHTTPServer(("127.0.0.1",17996),Pages); threading.Thread(target=ps.serve_forever,daemon=True).start()
NT=[]
class N(BaseHTTPRequestHandler):
    def log_message(s,*a): pass
    def do_OPTIONS(s): s.send_response(204); s.send_header("Access-Control-Allow-Origin","*"); s.send_header("Access-Control-Allow-Headers","*"); s.send_header("Access-Control-Allow-Methods","POST"); s.end_headers()
    def do_POST(s):
        NT.append(s.headers.get("Title")); s.rfile.read(int(s.headers["Content-Length"])); s.send_response(200); s.send_header("Access-Control-Allow-Origin","*"); s.end_headers()
ns=ThreadingHTTPServer(("127.0.0.1",17997),N); threading.Thread(target=ns.serve_forever,daemon=True).start()
# a pending risky approval + an always-able one, for the phone page
md.session({"session":"s1","project":"api-server@fix-auth"})
md.S["approvals"]={7:{"tool":"Bash","detail":"zfs destroy -r pool/data@old","decision":"pending","project":"api-server@fix-auth","always":"","risk":"high","risk_why":"destroys ZFS data"},
                   8:{"tool":"Edit","detail":"/srv/app/main.py\n- x = 1\n+ x = 10\n+ z = 30","decision":"pending","project":"api-server@fix-auth","always":"auto-accept edits (session)","risk":"","risk_why":""}}
from playwright.sync_api import sync_playwright
def check(n,c,x=""): print(("PASS " if c else "FAIL ")+n+("" if c else "  | "+str(x)[:200]))
with sync_playwright() as p:
    b=p.chromium.launch(); pg=b.new_page(viewport={"width":420,"height":900},device_scale_factor=2)
    errs=[]; pg.on("pageerror",lambda e: errs.append(str(e)))
    pg.goto("http://127.0.0.1:17995/setup?t=tok5"); pg.wait_for_selector("#settings:not(.hide)")
    pg.fill("#macros","Continue | Continue.\nShip it | Run tests, then commit and push.\nbad line without pipe")
    pg.fill("#pw1","30"); pg.select_option("#sm","always"); pg.fill("#stt","http://whisper.lan:8000/v1/audio/transcriptions"); pg.fill("#sttk","sk-secret")
    pg.fill("#nu","http://127.0.0.1:17997/claude"); pg.click("text=send test"); time.sleep(0.6)
    check("ntfy test sent from the browser", NT==["ClaudeStick test"], NT)
    check("wake-on-motion is on by default", pg.is_checked("#wake"))
    pg.uncheck("#wake")
    pg.fill("#uu","http://127.0.0.1:17996/"); pg.click("text=save settings"); time.sleep(0.6)
    pg.click("#smsg >> xpath=preceding-sibling::button[1]"); time.sleep(0.6)   # the "you & your stick" card's save button
    check("wake-on-motion off is saved to the Stick", md.SETUP["wake"] is False, md.SETUP.get("wake"))
    pg.reload(); pg.wait_for_selector("#settings:not(.hide)")
    check("...and the checkbox stays off after a reload", not pg.is_checked("#wake"))
    pg.check("#wake")
    check("settings saved to the Stick", md.CONF["permission_wait_s"]==30 and md.CONF["stop_menu"]=="always" and md.CONF["stt_key"]=="sk-secret", md.CONF)
    check("macros parsed (bad line dropped)", md.CONF["macros"]==[{"label":"Continue","text":"Continue."},{"label":"Ship it","text":"Run tests, then commit and push."}], md.CONF["macros"])
    pg.click("text=check for update"); pg.wait_for_selector("#umsg button")
    check("update found", "99.0.0 available" in pg.inner_text("#umsg"), pg.inner_text("#umsg"))
    pg.click("#umsg button"); time.sleep(2)
    check("firmware downloaded from Pages and uploaded to the Stick", md.S["ota_bytes"]>300000, md.S["ota_bytes"])
    pg.screenshot(path=os.path.join(SHOTS,"settings5.png"), full_page=True)
    # reinstalling on purpose (the Stick keeps crashing): offered even when it is up to date
    open(os.path.join(FAKE,"ota.json"),"w").write('{"version":"0.8.1","app":"firmware/claudestick-app.bin"}')
    md.S["ota_bytes"]=0
    pg.click("text=check for update"); pg.wait_for_selector("#umsg button")
    check("up to date still offers a reinstall", "up to date" in pg.inner_text("#umsg") and "reinstall 0.8.1" in pg.inner_text("#umsg"), pg.inner_text("#umsg"))
    pg.click("#umsg button"); time.sleep(2)
    check("reinstall downloads the firmware again and uploads it", md.S["ota_bytes"]>300000, md.S["ota_bytes"])
    pg2=b.new_page(viewport={"width":420,"height":900},device_scale_factor=2); pg2.on("pageerror",lambda e: errs.append(str(e)))
    pg2.goto("http://127.0.0.1:17995/?t=tok5"); pg2.wait_for_selector("text=DANGER")
    pg2.screenshot(path=os.path.join(SHOTS,"phone5.png"), full_page=True)
    pg2.click("button:text-is('Always')"); time.sleep(0.5)
    check("phone Always button", md.S["approvals"][8]["decision"]=="always")
    check("risky card has no Always button", pg2.locator("button:text-is('Always')").count()==0)
    pg2.click("#rm"); time.sleep(0.4); check("remote button cycles setting", md.CONF["remote"]=="off", md.CONF["remote"])
    check("no JS errors", not errs, errs)
    b.close()
