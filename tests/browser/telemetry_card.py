"""Setup page: the opt-in telemetry checkbox (FTUE) and the collector URL field (settings)."""
import os
import sys
import tempfile
import threading
from http.server import ThreadingHTTPServer

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, ROOT + "/tools")
import mock_device as md  # noqa: E402
from playwright.sync_api import sync_playwright  # noqa: E402

SHOTS = os.path.join(tempfile.gettempdir(), "claudestick-tests")
os.makedirs(SHOTS, exist_ok=True)
FAILED = []


def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n + ("" if c else "  | " + str(x)[:200]))
    if not c:
        FAILED.append(n)


md.TOKEN = "tokt"
md.SETUP.update(done=False, connected=True, ssid="home-wifi")
srv = ThreadingHTTPServer(("127.0.0.1", 0), md.H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
URL = f"http://127.0.0.1:{srv.server_port}/setup"

with sync_playwright() as p:
    b = p.chromium.launch()
    errs = []
    pg = b.new_page(viewport={"width": 420, "height": 900}, device_scale_factor=2)
    pg.on("pageerror", lambda e: errs.append(str(e)))

    # ----------------------------------------------------------- first-time setup, no destination baked in
    pg.goto(URL)
    pg.wait_for_selector("#me:not(.hide)")
    check("FTUE: no telemetry destination configured -> the checkbox is hidden entirely",
          not pg.is_visible("#telrow"), md.CONF.get("telemetry_url"))

    # a destination appears (as if a fork had baked CS_TELEMETRY_URL in, or it was set another way)
    md.CONF["telemetry_url"] = "https://stats.example.com"
    pg.reload()
    pg.wait_for_selector("#me:not(.hide)")
    check("FTUE: once a destination exists, the checkbox appears, unchecked by default",
          pg.is_visible("#telrow") and not pg.is_checked("#tel"))
    check("FTUE: the row says what it sends and what it doesn't", "docs/TELEMETRY.md" in pg.inner_text("#telrow") and
          "never your commands" in pg.inner_text("#telrow"), pg.inner_text("#telrow"))

    pg.check("#tel")
    pg.click("#me button:text-is('save')")
    pg.wait_for_selector("#smsg >> text=saved")
    check("checking it during FTUE turns it on, with no restart needed (unlike Bluetooth)",
          md.CONF["telemetry"] is True and "restarting" not in pg.inner_text("#smsg"), (md.CONF["telemetry"], pg.inner_text("#smsg")))

    # ----------------------------------------------------------- settings: the collector URL, editable like update_url
    pg.wait_for_selector("#settings:not(.hide)")
    check("settings: the destination field is pre-filled from what's already configured",
          pg.input_value("#telu") == "https://stats.example.com")
    pg.fill("#telu", "https://my-own-collector.example.com")
    pg.click("button:text-is('save settings')")
    pg.wait_for_selector("#cmsg >> text=saved")
    check("settings: a self-hoster can point it at their own collector", md.CONF["telemetry_url"] == "https://my-own-collector.example.com",
          md.CONF["telemetry_url"])
    check("settings: turning it on there didn't change (telemetry stays on from the FTUE save)", md.CONF["telemetry"] is True)

    pg.screenshot(path=os.path.join(SHOTS, "telemetry_card.png"), full_page=True)

    # turning it off again, from settings this time, via the same FTUE-card checkbox (still present post-setup).
    # #smsg already reads "saved" from the first save above, so clear it first or the wait below could
    # match that stale text instead of waiting for this save's own round trip (seen once on a slow CI runner).
    pg.evaluate("document.getElementById('smsg').textContent = ''")
    pg.uncheck("#tel")
    pg.click("#me button:text-is('save')")
    pg.wait_for_selector("#smsg >> text=saved")
    check("it can be turned back off at any time, not just during FTUE", md.CONF["telemetry"] is False)

    # The bridge's own settings page has none of this: #me and #fwcard (which hold the telemetry
    # checkbox and URL field) are already hidden there, as tests/browser/bridge_cards.py checks.

    check("no JS errors", not errs, errs)
    b.close()

print("\ntelemetry card: %d failed" % len(FAILED) if FAILED else "\ntelemetry card: all passed")
sys.exit(1 if FAILED else 0)
