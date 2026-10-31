"""The telemetry collector: schema enforcement, storage, rate limiting, auth on /stats, --summary."""
import http.client
import json
import os
import sys
import tempfile
import threading
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, ROOT + "/telemetry")
import claudestick_telemetry as tl  # noqa: E402

FAILED = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (("  | " + str(extra)[:200]) if extra and not cond else ""))
    if not cond:
        FAILED.append(name)
    return cond


REPORT = {
    "device_id": "abc123", "fw": "0.9.1", "uptime_s": 3600, "boot_epoch": 1793390000,
    "reset_reason": "power on", "safe_mode": False,
    "approvals": 5, "denies": 1, "always_allow": 2, "danger_cards": 0,
    "macro_taps": 3, "done_choose": 2, "done_dismiss": 1,
    "voice_ok": 1, "voice_fail": 0, "sessions_seen": 2,
    "remote_on_s": 120, "remote_off_s": 600,
    "battery_pct": 80, "battery_charging": False, "battery_drain_pct_h": 4.5,
    "bridge_configured": True, "bt_on": False, "remote_mode": "on",
    "text_style": "code", "buddy": "capybara", "tilt_on": True, "flip_on": False,
    "wifi_networks": 2,
}

# ----------------------------------------------------------------- clean(): schema enforcement
check("a full valid report passes through unchanged", tl.clean(dict(REPORT)) == REPORT)
check("missing device_id is rejected", tl.clean({**REPORT, "device_id": None}) is None)
d = dict(REPORT); del d["device_id"]
check("absent device_id is rejected", tl.clean(d) is None)
check("empty fw string is rejected", tl.clean({**REPORT, "fw": ""}) is None)
f = dict(REPORT); del f["fw"]
check("absent fw is rejected", tl.clean(f) is None)
check("not a dict at all is rejected", tl.clean(["nope"]) is None and tl.clean("nope") is None and tl.clean(None) is None)
check("an unknown field is dropped, not stored", "evil" not in tl.clean({**REPORT, "evil": "haha"}))
check("free text crammed into a known field is still type-checked away",
      tl.clean({**REPORT, "approvals": "rm -rf /"}).get("approvals") is None)
check("a bool sent where a number is wanted is rejected (bool is an int subclass in JSON/Python)",
      "approvals" not in tl.clean({**REPORT, "approvals": True}))
check("a number sent where a bool is wanted is rejected", "safe_mode" not in tl.clean({**REPORT, "safe_mode": 1}))
check("oversized strings are clipped, not rejected outright",
      len(tl.clean({**REPORT, "device_id": "x" * 500})["device_id"]) == tl.FIELDS["device_id"][1])
check("a huge integer is clamped rather than corrupting storage",
      tl.clean({**REPORT, "approvals": 10**12})["approvals"] == (1 << 31))
check("battery_drain_pct_h accepts a float", tl.clean({**REPORT, "battery_drain_pct_h": 2.5})["battery_drain_pct_h"] == 2.5)
check("a negative count is kept, just clamped at the floor (none of ours ever go negative, but don't crash on one)",
      tl.clean({**REPORT, "approvals": -(10**12)})["approvals"] == -(1 << 31))
check("extra junk keys never reach storage: only FIELDS + required are possible keys",
      set(tl.clean({**REPORT, "tool": "Bash", "detail": "rm -rf /", "token": "secret", "ssid": "home-wifi"})) <= set(tl.FIELDS))

# ----------------------------------------------------------------- live server
data = tempfile.mkdtemp()
store = tl.Store(data)
TOKEN = store.ident_token()
srv = tl.Server(("127.0.0.1", 0), store, TOKEN)
PORT = srv.server_port
threading.Thread(target=srv.serve_forever, daemon=True).start()


def call(method, path, body=None, headers=None, raw=None):
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
    h = dict(headers or {})
    data_ = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    if data_ is not None:
        h.setdefault("Content-Type", "application/json")
    try:
        c.request(method, path, body=data_, headers=h)
    except (BrokenPipeError, ConnectionResetError):
        pass
    r = c.getresponse()
    out = r.read()
    try:
        j = json.loads(out)
    except ValueError:
        j = None
    c.close()
    return r.status, j


st, j = call("GET", "/health")
check("/health needs no token", st == 200 and j["service"] == "claudestick-telemetry" and j["version"] == tl.VERSION, j)

st, j = call("POST", "/report", REPORT)
check("a report is accepted", st == 200 and j == {"ok": True}, j)
st, j = call("POST", "/report", {"fw": "0.9.1"})
check("device_id alone missing -> 400", st == 400, j)
st, j = call("POST", "/report", raw=b"not json")
check("malformed JSON -> 400", st == 400, j)
st, j = call("POST", "/report", raw=b"\0" * (tl.MAX_BODY + 1))
check("oversized body -> 413", st == 413, j)
c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=5)
c.putrequest("POST", "/report")
c.putheader("Content-Length", "-1")
c.endheaders()
r = c.getresponse()
check("bad Content-Length -> 400, not a crash", r.status == 400)
c.close()
st, j = call("GET", "/report")
check("GET on /report -> 404 (it's a POST-only endpoint)", st == 404)
st, j = call("POST", "/nope", {})
check("unknown POST path -> 404", st == 404)

# ----------------------------------------------------------------- /stats auth and content
st, j = call("GET", "/stats")
check("/stats with no token -> 401", st == 401)
st, j = call("GET", "/stats", headers={"X-Token": "wrong"})
check("/stats with the wrong token -> 401", st == 401)
st, j = call("GET", "/stats", headers={"X-Token": TOKEN})
check("/stats with the right token", st == 200 and j["reports_total"] == 1 and j["devices_total"] == 1, j)
check("/stats totals sum the numeric fields across reports", j["totals"]["approvals"] == 5 and j["totals"]["macro_taps"] == 3, j["totals"])
check("/stats never leaks the token itself or anything the schema doesn't allow",
      "token" not in json.dumps(j) and "detail" not in json.dumps(j))
st, j = call("GET", "/stats?t=" + TOKEN)
check("/stats token also accepted as ?t=", st == 200)

report2 = {**REPORT, "device_id": "device-two", "approvals": 10}
call("POST", "/report", report2)
call("POST", "/report", {**REPORT, "approvals": 1})   # a second report from device one
st, j = call("GET", "/stats", headers={"X-Token": TOKEN})
check("two devices now", j["devices_total"] == 2, j)
check("three reports now", j["reports_total"] == 3, j)
check("totals accumulate across every report, not just the latest per device", j["totals"]["approvals"] == 5 + 1 + 10, j["totals"])
check("fw histogram", j["fw_versions"] == {"0.9.1": 3}, j["fw_versions"])

# ----------------------------------------------------------------- rate limiting
# its own store, so this burst of requests doesn't pollute the report counts checked above/below
limit_store = tl.Store(tempfile.mkdtemp())
limited = tl.Server(("127.0.0.1", 0), limit_store, TOKEN)
threading.Thread(target=limited.serve_forever, daemon=True).start()


def call2(method, path, body=None, **kw):
    c = http.client.HTTPConnection("127.0.0.1", limited.server_port, timeout=10)
    data_ = json.dumps(body).encode() if body is not None else None
    c.request(method, path, body=data_, headers={"Content-Type": "application/json"} if data_ else {})
    r = c.getresponse()
    r.read()
    c.close()
    return r.status


codes = [call2("POST", "/report", REPORT) for _ in range(tl.RATE_MAX + 5)]
check("an address is rate-limited after the burst", codes[: tl.RATE_MAX] == [200] * tl.RATE_MAX and all(c == 429 for c in codes[tl.RATE_MAX:]),
      codes)
limited.shutdown()

# ----------------------------------------------------------------- trust-proxy
proxy_store = tl.Store(tempfile.mkdtemp())
proxied = tl.Server(("127.0.0.1", 0), proxy_store, TOKEN, trust_proxy=True)
threading.Thread(target=proxied.serve_forever, daemon=True).start()


def call_fwd(ip):
    c = http.client.HTTPConnection("127.0.0.1", proxied.server_port, timeout=5)
    c.request("POST", "/report", body=json.dumps(REPORT).encode(),
              headers={"Content-Type": "application/json", "X-Forwarded-For": ip})
    r = c.getresponse()
    r.read()
    c.close()
    return r.status


codes_a = [call_fwd("203.0.113.9") for _ in range(tl.RATE_MAX + 2)]
check("behind a proxy, the real client is rate-limited (not the proxy)", codes_a[-1] == 429, codes_a[-3:])
check("a different client behind the same proxy is unaffected", call_fwd("198.51.100.4") == 200)
proxied.shutdown()

# ----------------------------------------------------------------- --summary / data persistence
out = tl.summary_text(store.stats())
check("--summary output names the counts a human would check", "reports:" in out and "devices:" in out and "0.9.1" in out, out)
store2 = tl.Store(data)   # reopening the same data dir: the sqlite file persists
check("reopening the data dir keeps the reports (sqlite persists)", store2.stats()["reports_total"] == 3)
check("the stats token is stable across restarts, not regenerated", store2.ident_token() == TOKEN)
check("the data dir is private (0700)", oct(os.stat(data).st_mode & 0o777) == "0o700")
check("the stats-token file is private (0600)", oct(os.stat(os.path.join(data, "stats_token.json")).st_mode & 0o777) == "0o600")

# ----------------------------------------------------------------- the version matches the firmware (like the bridge)
import re  # noqa: E402
fw = re.search(r'FW_VERSION\s*=\s*"([^"]+)"', open(ROOT + "/firmware/src/main.cpp").read()).group(1)
check(f"collector version == firmware version ({fw})", tl.VERSION == fw, (tl.VERSION, fw))

srv.shutdown()
print("\ntelemetry collector: %d failed" % len(FAILED) if FAILED else "\ntelemetry collector: all passed")
sys.exit(1 if FAILED else 0)
