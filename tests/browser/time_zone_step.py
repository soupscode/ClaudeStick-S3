import tempfile, os
SHOTS=os.path.join(tempfile.gettempdir(),"claudestick-tests"); os.makedirs(SHOTS,exist_ok=True)
import os, sys, threading, time, json, re
sys.path.insert(0, os.path.join(os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),"..","..")),"tools")); import mock_device as md
from http.server import ThreadingHTTPServer
md.TOKEN = "tok"
srv = ThreadingHTTPServer(("127.0.0.1", 17951), md.H); threading.Thread(target=srv.serve_forever, daemon=True).start()
from playwright.sync_api import sync_playwright
fails = 0
def check(n, c, x=""):
    global fails
    print(("PASS " if c else "FAIL ") + n + ("" if c else " | " + str(x)))
    if not c: fails += 1
def fresh():
    md.SETUP.update(done=False, ap=False, connected=True, ip="127.0.0.1", tz="", tz_name="", clock24=False, epoch=None)
with sync_playwright() as p:
    b = p.chromium.launch()
    def page(tzid):
        ctx = b.new_context(timezone_id=tzid, viewport={"width": 390, "height": 900}, device_scale_factor=2)
        pg = ctx.new_page(); errs = []; pg.on("pageerror", lambda e: errs.append(str(e))); pg._errs = errs
        return pg
    for tzid, posix, name in [("America/Chicago", "CST6CDT,M3.2.0,M11.1.0", "America/Chicago"),
                              ("America/Detroit", "EST5EDT,M3.2.0,M11.1.0", "America/New_York"),
                              ("Asia/Kolkata", "IST-5:30", "Asia/Kolkata"),
                              ("Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3", "Europe/Paris")]:
        fresh(); pg = page(tzid)
        pg.goto("http://127.0.0.1:17951/setup"); pg.wait_for_selector("#time:not(.hide)"); time.sleep(0.6)
        check(f"{tzid}: detected + auto-applied", md.SETUP["tz"] == posix and md.SETUP["tz_name"] == name, (md.SETUP["tz"], md.SETUP["tz_name"]))
        check(f"{tzid}: browser clock sent", abs((md.SETUP["epoch"] or 0) - time.time()) < 30, md.SETUP["epoch"])
        check(f"{tzid}: 12-hour is the default", md.SETUP["clock24"] is False and pg.is_checked("#c12"))
        check(f"{tzid}: no JS errors", not pg._errs, pg._errs)
        pg.context.close()
    # a zone that isn't in the table falls back to a fixed offset from the browser
    fresh(); pg = page("Pacific/Chatham")
    pg.goto("http://127.0.0.1:17951/setup"); pg.wait_for_selector("#time:not(.hide)"); time.sleep(0.6)
    check("unlisted zone -> fixed-offset rule", re.fullmatch(r"<[+-]\d{4}>-?\d+:\d{2}", md.SETUP["tz"] or "") is not None, md.SETUP["tz"])
    check("...and the select says so", pg.inner_text("#tz option:checked").startswith("fixed UTC"), pg.inner_text("#tz option:checked"))
    pg.context.close()
    # change it by hand
    fresh(); pg = page("America/Chicago")
    pg.goto("http://127.0.0.1:17951/setup"); pg.wait_for_selector("#time:not(.hide)"); time.sleep(0.6)
    pg.select_option("#tz", label="Tokyo"); pg.uncheck("#c12"); pg.click("text=save time"); time.sleep(0.6)
    check("manual choice saved", md.SETUP["tz"] == "JST-9" and md.SETUP["clock24"] is True and md.SETUP["tz_name"] == "Asia/Tokyo", md.SETUP)
    check("confirmation shows the Stick's time", "the Stick shows" in pg.inner_text("#tmsg"), pg.inner_text("#tmsg"))
    pg.screenshot(path=os.path.join(SHOTS,"time_card.png"), full_page=True)
    # the final save keeps the choice and finishes setup
    pg.click("button:text-is(\"save\")"); pg.wait_for_selector("#done:not(.hide)")
    check("final save keeps the zone", md.SETUP["tz"] == "JST-9" and md.SETUP["done"], md.SETUP)
    # reopening settings (with the token) shows the saved zone, not the browser's
    pg.goto("http://127.0.0.1:17951/setup?t=tok"); time.sleep(0.8)
    check("settings page shows the saved zone", pg.inner_text("#tz option:checked").startswith("Tokyo") and not pg.is_checked("#c12"), pg.inner_text("#tz option:checked"))
    check("no JS errors (manual)", not pg._errs, pg._errs)
    b.close()
sys.exit(fails)
