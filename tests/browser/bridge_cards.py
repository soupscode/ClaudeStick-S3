"""Setup page, remote-access parts: Wi-Fi networks and bridge cards on the Stick; settings-only view on the bridge."""
import os
import re
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, ROOT + "/tools")
sys.path.insert(0, ROOT + "/bridge")
import claudestick_bridge as br  # noqa: E402
import mock_device as md  # noqa: E402
from playwright.sync_api import sync_playwright  # noqa: E402

SHOTS = os.path.join(tempfile.gettempdir(), "claudestick-tests")
os.makedirs(SHOTS, exist_ok=True)
FAILED = []


def check(n, c, x=""):
    print(("PASS " if c else "FAIL ") + n + ("" if c else "  | " + str(x)[:200]))
    if not c:
        FAILED.append(n)


md.TOKEN = "tokm"
md.SETUP.update(done=True, connected=True, ssid="home-wifi")
stick = ThreadingHTTPServer(("127.0.0.1", 0), md.H)
threading.Thread(target=stick.serve_forever, daemon=True).start()

store = br.Store(tempfile.mkdtemp())
store.ident["token"] = "b" * 32
store.ensure_identity()
bridge = br.make_server(store, "127.0.0.1", 0)
threading.Thread(target=bridge.serve_forever, daemon=True).start()

with sync_playwright() as p:
    b = p.chromium.launch()
    errs = []

    # ------------------------------------------------------------- on the Stick
    pg = b.new_page(viewport={"width": 420, "height": 900}, device_scale_factor=2)
    pg.on("pageerror", lambda e: errs.append(str(e)))
    pg.goto(f"http://127.0.0.1:{stick.server_port}/setup?t=tokm")
    pg.wait_for_selector("#nets:not(.hide)")
    check("both cards show once setup is done", pg.is_visible("#brcard") and pg.is_visible("#nets"))
    check("the current network is listed as the first choice", "home-wifi" in pg.inner_text("#netlist") and "first choice" in pg.inner_text("#netlist"))
    pg.fill("#nn", "Phone hotspot")
    pg.fill("#np", "hotspotpw")
    pg.click("#nets button:text-is('add')")
    pg.wait_for_selector("#netlist >> text=Phone hotspot")
    check("a hotspot can be added", [n["s"] for n in md.SETUP["nets"]] == ["home-wifi", "Phone hotspot"] and md.SETUP["nets"][1]["p"] == "hotspotpw", md.SETUP["nets"])
    check("...and the form is cleared", pg.input_value("#nn") == "" and pg.input_value("#np") == "")
    pg.click("#nets button:text-is('add')")
    check("adding nothing says what's missing", "network name" in pg.inner_text("#wnmsg"), pg.inner_text("#wnmsg"))
    for n in ("Office", "Cafe"):
        pg.fill("#nn", n)
        pg.click("#nets button:text-is('add')")
        pg.wait_for_selector(f"#netlist >> text={n}")
    pg.fill("#nn", "Fifth")
    pg.click("#nets button:text-is('add')")
    pg.wait_for_selector("#wnmsg >> text=limit")
    check("a fifth network is refused with the reason", "four networks is the limit" in pg.inner_text("#wnmsg"), pg.inner_text("#wnmsg"))
    pg.click("#netlist button >> nth=1")   # forget "Phone hotspot"
    pg.wait_for_function("!document.querySelector('#netlist').innerText.includes('Phone hotspot')")
    check("a network can be forgotten", [n["s"] for n in md.SETUP["nets"]] == ["home-wifi", "Office", "Cafe"], md.SETUP["nets"])
    pg.fill("#nn", "<b>x</b>")
    pg.click("#nets button:text-is('add')")
    pg.wait_for_selector("#netlist >> text=<b>x</b>")
    check("network names are shown as text, never as markup", pg.locator("#netlist b").count() == 0)

    pg.fill("#bu", "stick.example.com")
    pg.fill("#btk", "x")
    pg.click("#brcard button:text-is('connect')")
    pg.wait_for_selector("#bmsg >> text=address")
    check("a bad address is explained", "should look like https://" in pg.inner_text("#bmsg"), pg.inner_text("#bmsg"))
    pg.fill("#bu", "https://stick.example.com/")
    pg.fill("#btk", "wrongtoken")
    pg.click("#brcard button:text-is('connect')")
    pg.wait_for_selector("#bmsg >> text=rejected")
    check("a wrong token is explained", "rejected that token" in pg.inner_text("#bmsg"))
    check("the token field is cleared after sending", pg.input_value("#btk") == "")
    pg.fill("#btk", "goodtoken")
    pg.click("#brcard button:text-is('connect')")
    pg.wait_for_selector("#bmsg >> text=connected")
    check("the right token connects", md.SETUP["bridge"]["up"] and md.SETUP["bridge"]["url"] == "https://stick.example.com", md.SETUP["bridge"])
    check("the address is shown without a trailing slash and the token is not shown", pg.input_value("#bu") == "https://stick.example.com" and
          "goodtoken" not in pg.content() and pg.get_attribute("#btk", "placeholder").startswith("(set)"))
    pg.screenshot(path=os.path.join(SHOTS, "bridge_cards_stick.png"), full_page=True)
    pg.reload()
    pg.wait_for_selector("#bmsg >> text=connected")
    check("status is still shown after a reload", pg.input_value("#bu") == "https://stick.example.com")
    pg.click("#brcard button:text-is('turn off')")
    pg.wait_for_selector("#bmsg >> text=turned off")
    check("turning it off clears it", md.SETUP["bridge"]["url"] == "" and pg.input_value("#bu") == "")
    check("the Stick page still has its own cards", pg.is_visible("#fwcard") and not pg.is_visible("#pubcard"))
    pg.fill("#bu", "https://stick.example.com")
    pg.fill("#bcode", "12345")
    pg.click("#brcard button:text-is('connect')")
    pg.wait_for_selector("#bmsg >> text=6 digits")
    check("a short pairing code is explained", "6 digits" in pg.inner_text("#bmsg"), pg.inner_text("#bmsg"))
    pg.fill("#bcode", "000 000")
    pg.click("#brcard button:text-is('connect')")
    pg.wait_for_selector("#bmsg >> text=expired")
    check("a wrong pairing code is explained and nothing is set", "wrong or has expired" in pg.inner_text("#bmsg") and not md.SETUP["bridge"]["up"], md.SETUP["bridge"])
    pg.fill("#bcode", "123 456")
    pg.click("#brcard button:text-is('connect')")
    pg.wait_for_selector("#bmsg >> text=connected")
    check("a pairing code connects without typing the token", md.SETUP["bridge"]["up"] and md.SETUP["bridge"]["tok"] == "goodtoken", md.SETUP["bridge"])
    check("the code field is cleared and the token never reaches the page", pg.input_value("#bcode") == "" and "goodtoken" not in pg.content())
    pg.click("#brcard button:text-is('turn off')")
    pg.wait_for_selector("#bmsg >> text=turned off")

    # ------------------------------------------------------------- first-time setup: away-from-home pieces come up right away
    md.SETUP.update(done=False, connected=True, ssid="home-wifi", nets=[], bt_on=True)
    md.SETUP["bridge"] = {"url": "", "tok": "", "up": False, "error": ""}
    pf = b.new_page(viewport={"width": 420, "height": 900}, device_scale_factor=2)
    pf.on("pageerror", lambda e: errs.append(str(e)))
    pf.goto(f"http://127.0.0.1:{stick.server_port}/setup")   # no token yet: this is the first-time wizard
    pf.wait_for_selector("#me:not(.hide)")
    check("first-time setup: Wi-Fi networks and bridge cards are there once the Stick is online",
          pf.is_visible("#nets") and pf.is_visible("#brcard") and "first-time" in pf.inner_text("#hello"))
    pf.fill("#nn", "Phone hotspot")
    pf.fill("#np", "hotspotpw")
    pf.click("#nets button:text-is('add')")
    pf.wait_for_selector("#netlist >> text=Phone hotspot")
    pf.fill("#bu", "https://stick.example.com")
    pf.fill("#btk", "goodtoken")
    pf.click("#brcard button:text-is('connect')")
    pf.wait_for_selector("#bmsg >> text=connected")
    check("first-time setup: hotspot and bridge can be set before finishing", md.SETUP["bridge"]["up"] and
          [n["s"] for n in md.SETUP["nets"]] == ["home-wifi", "Phone hotspot"], (md.SETUP["bridge"], md.SETUP["nets"]))
    check("remote mode explains 'always on' for being away", "away from your desk" in pf.inner_text("#me"))
    check("Bluetooth checkbox starts on", pf.is_checked("#bton"))
    pf.uncheck("#bton")
    pf.click("#me button:text-is('save')")
    pf.wait_for_selector("#smsg >> text=saved")
    check("turning Bluetooth off is saved and the page says the Stick restarts", md.SETUP["bt_on"] is False and "restarting" in pf.inner_text("#smsg"),
          (md.SETUP["bt_on"], pf.inner_text("#smsg")))
    check("first-time setup: with a bridge set, the page sends you to the bridge's install command, not the Stick's",
          "bridge" in pf.inner_text("#cmd") and "curl" not in pf.inner_text("#cmd") and not pf.is_visible("#cmdw"), pf.inner_text("#cmd"))
    pf.screenshot(path=os.path.join(SHOTS, "bridge_cards_ftue.png"), full_page=True)
    md.SETUP.update(done=True, bt_on=True)

    # ------------------------------------------------------------- on the bridge
    pb = b.new_page(viewport={"width": 420, "height": 900}, device_scale_factor=2)
    pb.on("pageerror", lambda e: errs.append(str(e)))
    pb.goto(f"http://127.0.0.1:{bridge.server_port}/setup?t={'b' * 32}")
    pb.wait_for_selector("#settings:not(.hide)")
    check("bridge: says what it is", "bridge" in pb.inner_text("#hello") and br.VERSION in pb.inner_text("#hello"), pb.inner_text("#hello"))
    check("bridge: no Wi-Fi, time zone, buddy, firmware or Stick cards",
          not any(pb.is_visible(i) for i in ("#wifi", "#time", "#me", "#nets", "#brcard", "#fwcard")))
    check("bridge: install command for the machines running Claude Code", f"--port {bridge.server_port}" in pb.inner_text("#cmd") and
          "--token " + "b" * 32 in pb.inner_text("#cmd") and "/install.py" in pb.inner_text("#cmd"), pb.inner_text("#cmd"))
    check("bridge: no Bluetooth line", not pb.is_visible("#btline"))
    check("bridge: shows what to type into the Stick", pb.is_visible("#sc") and "b" * 32 in pb.inner_text("#stickcfg"))
    check("bridge: remote mode card, 'always on' by default", pb.is_visible("#rmcard") and pb.input_value("#rmode") == "on")
    pb.select_option("#rmode", "auto")
    pb.fill("#pubu", "https://stick.example.com")
    pb.fill("#macros", "Go | Go ahead.\nStop | Stop here.")
    pb.fill("#stt", "http://whisper.lan:8000/v1/audio/transcriptions")
    pb.fill("#sttk", "sk-secret")
    pb.click("button:text-is('save settings')")
    pb.wait_for_selector("#cmsg >> text=saved")
    c = store.config
    check("bridge: settings saved (public address, macros, voice, key)", c["public_url"] == "https://stick.example.com" and
          [m["label"] for m in c["macros"]] == ["Go", "Stop"] and c["stt_key"] == "sk-secret", c)
    check("bridge: no firmware update source sent", "update_url" not in c)
    check("bridge: remote mode saved", c["remote"] == "auto", c["remote"])
    pb.reload()
    pb.wait_for_selector("#settings:not(.hide)")
    check("bridge: secrets are never sent back to the page", "sk-secret" not in pb.content() and pb.get_attribute("#sttk", "placeholder").startswith("(set)"),
          pb.get_attribute("#sttk", "placeholder"))
    check("bridge: the public address comes back and fills the phone link", pb.input_value("#pubu") == "https://stick.example.com" and
          pb.inner_text("#phone").startswith("https://stick.example.com/?t="), pb.inner_text("#phone"))
    pb.screenshot(path=os.path.join(SHOTS, "bridge_cards_bridge.png"), full_page=True)
    # pairing: a one-time code from the bridge's own page
    check("bridge: the pair-a-device card is there", pb.is_visible("#paircard"))
    pb.click("#paircard button")
    pb.wait_for_selector("#paircode")
    shown = pb.inner_text("#paircode").replace(" ", "")
    check("bridge: shows a 6-digit code, the countdown and the install command",
          shown.isdigit() and len(shown) == 6 and re.fullmatch(r"expires in [45]:\d\d", pb.inner_text("#pairleft")) is not None, (shown, pb.inner_text("#pairleft")))
    check("bridge: the command carries the code, the port and the public address",
          f"--pair {shown}" in pb.inner_text("#paircmd") and f"--port {bridge.server_port}" in pb.inner_text("#paircmd") and
          "https://stick.example.com/install.py" in pb.inner_text("#paircmd"), pb.inner_text("#paircmd"))
    check("bridge: the shown code is the live one and the long token is not on the card", bridge.core.pair and bridge.core.pair["code"] == shown and
          "b" * 32 not in pb.inner_text("#pairout"), bridge.core.pair)
    pb.screenshot(path=os.path.join(SHOTS, "bridge_cards_pair.png"), full_page=True)

    # the phone: no token yet, so it asks for a code
    ctx = b.new_context(viewport={"width": 390, "height": 844}, device_scale_factor=2)
    ph = ctx.new_page()
    ph.on("pageerror", lambda e: errs.append(str(e)))
    ph.goto(f"http://127.0.0.1:{bridge.server_port}/")
    ph.wait_for_selector("#pc")
    check("phone: without a token it offers pairing", "Pair this phone" in ph.inner_text("#ub"), ph.inner_text("#ub"))
    ph.fill("#pc", "000 000" if shown != "000000" else "111 111")
    ph.click("button:text-is('Pair')")
    ph.wait_for_selector("#pm >> text=wrong")
    check("phone: a wrong code is explained and nothing is stored", "wrong or has expired" in ph.inner_text("#pm") and ph.evaluate("localStorage.cst") is None)
    ph.fill("#pc", shown[:3] + " " + shown[3:])
    ph.click("button:text-is('Pair')")
    ph.wait_for_function("localStorage.cst")
    check("phone: the right code stores the token and spends the code", ph.evaluate("localStorage.cst") == "b" * 32 and bridge.core.pair is None, bridge.core.pair)
    ph.wait_for_selector("#sel")
    check("phone: the prompt is gone and the page loads", ph.locator("#pc").count() == 0 and "Send to Claude" in ph.inner_text("main"))
    ph.screenshot(path=os.path.join(SHOTS, "bridge_cards_phone_paired.png"), full_page=True)
    # a stale token is dropped and the prompt comes back
    ph.evaluate("localStorage.cst='stale'")
    ph.reload()
    ph.wait_for_selector("#pc")
    check("phone: a rejected token is dropped and pairing is offered again", ph.evaluate("localStorage.cst") is None)
    pb2 = b.new_page()
    pb2.goto(f"http://127.0.0.1:{bridge.server_port}/setup")
    check("bridge: without the token the page can't read anything", "setup is finished" in pb2.inner_text("#hello") or "token" in pb2.inner_text("#hello"), pb2.inner_text("#hello"))

    check("no JS errors", not errs, errs)
    b.close()

print("\nbridge cards: %d failed" % len(FAILED) if FAILED else "\nbridge cards: all passed")
sys.exit(1 if FAILED else 0)
