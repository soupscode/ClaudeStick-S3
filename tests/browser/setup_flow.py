import tempfile, os
SHOTS=os.path.join(tempfile.gettempdir(),"claudestick-tests"); os.makedirs(SHOTS,exist_ok=True)
import os, sys, threading, time, json, subprocess, tempfile
sys.path.insert(0, os.path.join(os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),"..","..")),"tools")); import mock_device as md
from http.server import ThreadingHTTPServer
md.TOKEN = "a1b2c3d4e5f60789"
srv = ThreadingHTTPServer(("127.0.0.1", 17950), md.H); threading.Thread(target=srv.serve_forever, daemon=True).start()
md.SETUP.update(ap=True, connected=False, ip="127.0.0.1")
from playwright.sync_api import sync_playwright
shots = []
with sync_playwright() as p:
    b = p.chromium.launch(); pg = b.new_page(viewport={"width": 390, "height": 900}, device_scale_factor=2)
    errs = []; pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto("http://127.0.0.1:17950/setup"); pg.wait_for_selector("#ssid option >> nth=1", state="attached")
    pg.screenshot(path=os.path.join(SHOTS,"setup1.png"), full_page=True)
    pg.fill("#pw", "goodpass"); pg.click("text=connect"); pg.wait_for_selector("#me:not(.hide)", timeout=15000)
    pg.fill("#name", "Tester"); pg.select_option("#buddy", "5"); time.sleep(0.3)
    pg.select_option("#remote","on"); pg.screenshot(path=os.path.join(SHOTS,"setup2.png"), full_page=True)
    pg.click("button:text-is(\"save\")"); pg.wait_for_selector("#done:not(.hide)")
    pg.screenshot(path=os.path.join(SHOTS,"setup3.png"), full_page=True)
    cmd = pg.inner_text("#cmd"); phone = pg.inner_text("#phone")
    print("SETUP state:", {k: md.SETUP[k] for k in ("done", "name", "buddy", "ssid", "tz_name", "font")})
    assert md.SETUP["done"] and md.SETUP["name"] == "Claude-Tester" and md.SETUP["buddy"] == 5 and md.SETUP["tz_name"], md.SETUP
    assert not errs, errs
    print("cmd:", cmd); print("phone:", phone); print("js errors:", errs)
    # after setup: page without token is locked
    pg.goto("http://127.0.0.1:17950/setup"); time.sleep(0.8); print("no-token page says:", pg.inner_text("#hello"))
    b.close()
# run the exact install command the page printed, in a fake HOME, from an empty dir (no checkout)
home = tempfile.mkdtemp(); work = tempfile.mkdtemp()
r = subprocess.run(["bash", "-c", cmd], cwd=work, env=dict(os.environ, HOME=home), capture_output=True, text=True, timeout=60)
print("\ninstaller output:\n" + r.stdout + r.stderr)
st = json.load(open(home + "/.claude/settings.json"))
print("hooks installed:", sorted(st["hooks"]), "| hook file:", os.path.exists(home + "/.claudestick/claudestick_hook.py"))
assert len(st["hooks"]) == 8 and os.path.exists(home + "/.claudestick/claudestick_hook.py"), st["hooks"]
print("PASS setup wizard -> install command -> hooks installed")
