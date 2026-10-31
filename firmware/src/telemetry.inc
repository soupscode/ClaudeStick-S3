// ------------------------------------------------------------- telemetry ---
// Opt-in usage reporting. Off by default; the checkbox is in first-time setup and in
// settings, and only shown there when a report destination is actually configured
// (telemetry_available in /setup/info). A report never contains anything typed, any
// tool, command or approval text, macro text, voice audio, or a token/password/URL you
// configured — see docs/TELEMETRY.md for the exact field list.
// Included from main.cpp, right after bridge.inc (needs conf, tel, bridgeConfigured()).

static uint32_t telLastAttempt = 0, telLastTry = 0, telRemoteLast = 0;

static String telemetryBase() {
  String u = conf.telemetryUrl;
  if (u.length() && !u.endsWith("/")) u += "/";
  return u;
}

// A random id for this device, separate from devId (which is derived from the Wi-Fi MAC
// and used for mDNS/discovery) so a telemetry report doesn't carry anything hardware-identifying.
static String telemetryId() {
  String id = prefs.getString("telid", "");
  if (!id.length()) {
    char t[17];
    snprintf(t, sizeof t, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    id = t;
    prefs.putString("telid", id);
  }
  return id;
}

static bool postJson(const String& url, const String& body, String& err) {
  HTTPClient http;
  WiFiClientSecure tls;
  WiFiClient plain;
  bool https = url.startsWith("https://");
  if (https) {
    if (!clockOk()) { err = "waiting for the clock"; return false; }   // certificates need the date
    tls.setCACertBundle(rootca_crt_bundle_start);
  }
  if (!(https ? http.begin(tls, url) : http.begin(plain, url))) { err = "bad address"; return false; }
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest("POST", (uint8_t*)body.c_str(), body.length());
  http.end();
  if (code >= 200 && code < 300) return true;
  err = code > 0 ? "http " + String(code) : http.errorToString(code);
  return false;
}

// Accumulates how long remote mode has been on vs off, for the next report. Keeps running
// even if a report is late or fails; only bothers when telemetry is actually on.
static void telemetryRemoteTick() {
  uint32_t now = millis();
  if (!telRemoteLast) { telRemoteLast = now; return; }
  uint32_t dt = now - telRemoteLast;
  telRemoteLast = now;
  if (!conf.telemetryOn) return;
  if (remoteMode) tel.remoteOnMs += dt; else tel.remoteOffMs += dt;
}

static void sendTelemetry() {
  telLastAttempt = millis() | 1;   // record the attempt whether or not it succeeds, like update checks
  JsonDocument d;
  d["device_id"] = telemetryId();
  d["fw"] = FW_VERSION;
  d["uptime_s"] = millis() / 1000;
  if (clockOk()) d["boot_epoch"] = (long)(time(nullptr) - millis() / 1000);
  d["reset_reason"] = resetWhy;
  d["safe_mode"] = safeMode;
  d["approvals"] = tel.appr; d["denies"] = tel.deny; d["always_allow"] = tel.always; d["danger_cards"] = tel.danger;
  d["macro_taps"] = tel.macroTaps; d["done_choose"] = tel.doneChoose; d["done_dismiss"] = tel.doneDismiss;
  d["voice_ok"] = tel.voiceOk; d["voice_fail"] = tel.voiceFail; d["sessions_seen"] = tel.sessionsSeen;
  d["remote_on_s"] = tel.remoteOnMs / 1000; d["remote_off_s"] = tel.remoteOffMs / 1000;
  d["battery_pct"] = M5.Power.getBatteryLevel();
  d["battery_charging"] = (int)M5.Power.isCharging() == 1;
  float r = drainPerHour();
  if (r > 0) d["battery_drain_pct_h"] = r;
  d["bridge_configured"] = bridgeConfigured();
  d["bt_on"] = btPref;
  d["remote_mode"] = remoteName(conf.remote);
  d["text_style"] = textStyle ? "code" : "classic";
  d["buddy"] = buddySpeciesName();
  d["tilt_on"] = tiltMode != 0;
  d["flip_on"] = flipped;
  d["wifi_networks"] = nNets;
  String body; serializeJson(d, body);
  String err;
  if (postJson(telemetryBase() + "report", body, err)) tel = TelCounters();   // only the acknowledged window clears
}

// Background checks wait for a quiet moment, same as update checks: a request can block the
// screen and the hooks for a few seconds.
static void telemetryTick() {
  if (!setupDone || !conf.telemetryOn || !conf.telemetryUrl.length() || WiFi.status() != WL_CONNECTED) return;
  uint32_t now = millis();
  if (!(!telLastAttempt ? now > 90000 : now - telLastAttempt > 6UL * 3600 * 1000)) return;
  if (now - telLastTry < 15000) return;
  if (topApproval() || topStop() || now - lastActivity < 15000) return;
  telLastTry = now;
  sendTelemetry();
}
