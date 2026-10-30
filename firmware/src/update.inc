// --------------------------------------------------------------- updates ---
// The Stick checks <update_url>/ota.json (your web flasher site) 90 s after boot
// and every 6 h. A newer version shows a prompt with the changelog:
//   update now · remind me later (24 h) · skip this version
// Downloads are HTTPS-verified against the embedded Mozilla root bundle.
//
// Boot guard: every boot counts as a failure until it has run for 20 s. Three
// failures in a row switch back to the previous firmware slot and skip the bad
// version, so a broken update never needs a USB reflash.
// Included from main.cpp; shares its state.

#include <HTTPUpdate.h>
#include <esp_ota_ops.h>

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");

static const uint16_t C_OK_U = 0x4E89, C_BAD_U = 0xE945, C_YEL_U = 0xF6A8;   // match the UI palette

static struct {
  bool     available = false;
  String   version, notes, app, error;
  uint32_t lastCheck = 0, remindAt = 0;
  int      progress = -1;            // -1 idle, 0..100 installing
} upd;
static String   skipVersion;
static String   noticeTitle, noticeText;   // one-off message card ("updated to x", "rolled back")
static uint16_t noticeColor = 0;
static uint32_t noticeUntil = 0;

static void notice(const String& title, const String& text, uint16_t color, uint32_t ms = 6000) {
  noticeTitle = title; noticeText = text; noticeColor = color; noticeUntil = millis() + ms; touch();
}

static bool versionNewer(const String& a, const String& b) {   // a > b, "x.y.z"
  int pa[3] = {0, 0, 0}, pb[3] = {0, 0, 0};
  sscanf(a.c_str(), "%d.%d.%d", &pa[0], &pa[1], &pa[2]);
  sscanf(b.c_str(), "%d.%d.%d", &pb[0], &pb[1], &pb[2]);
  for (int i = 0; i < 3; i++) if (pa[i] != pb[i]) return pa[i] > pb[i];
  return false;
}

static String updateBase() {
  String u = conf.updateUrl;
  if (u.length() && !u.endsWith("/")) u += "/";
  return u;
}

static bool fetchText(const String& url, String& out) {
  HTTPClient http;
  WiFiClientSecure tls;
  WiFiClient plain;
  bool https = url.startsWith("https://");
  if (https) tls.setCACertBundle(rootca_crt_bundle_start);
  if (!(https ? http.begin(tls, url) : http.begin(plain, url))) return false;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  int code = http.GET();
  if (code == 200) out = http.getString();
  else upd.error = code > 0 ? "http " + String(code) : http.errorToString(code);
  http.end();
  return code == 200;
}

static void checkForUpdate(bool manual) {
  upd.lastCheck = millis() | 1;
  if (!conf.updateUrl.length()) { if (manual) notice("updates", "no update source set: open settings", C_YEL_U); return; }
  if (WiFi.status() != WL_CONNECTED) { if (manual) notice("updates", "no wi-fi", C_YEL_U); return; }
  String body;
  if (!fetchText(updateBase() + "ota.json", body)) {
    if (manual) notice("updates", "couldn't reach " + conf.updateUrl + " (" + upd.error + ")", C_BAD_U);
    return;
  }
  JsonDocument d;
  if (deserializeJson(d, body)) { if (manual) notice("updates", "bad ota.json", C_BAD_U); return; }
  String v = d["version"] | "";
  upd.available = v.length() && versionNewer(v, FW_VERSION) && v != skipVersion;
  if (upd.available) {
    bool fresh = v != upd.version;
    upd.version = v;
    upd.notes = (const char*)(d["notes"] | "");
    upd.app = (const char*)(d["app"] | "firmware/claudestick-app.bin");
    if (fresh || manual) upd.remindAt = 0;
    if (fresh) beep(0);
  } else if (manual) {
    notice("updates", String("up to date (") + FW_VERSION + ")", C_OK_U);
  }
  dirty = true;
}

static bool updatePromptDue() {
  return upd.available && setupDone && upd.progress < 0 && (!upd.remindAt || (int32_t)(millis() - upd.remindAt) >= 0);
}

static void updateTick() {
  if (!setupDone || WiFi.status() != WL_CONNECTED || upd.progress >= 0) return;
  uint32_t now = millis();
  if (!upd.lastCheck ? now > 90000 : now - upd.lastCheck > 6UL * 3600 * 1000) checkForUpdate(false);
}

static void drawUpdateProgress();   // UI section

static void installUpdate() {
  String url = updateBase() + upd.app;
  upd.progress = 0; upd.error = "";
  drawUpdateProgress();
  WiFiClientSecure tls;
  WiFiClient plain;
  bool https = url.startsWith("https://");
  if (https) tls.setCACertBundle(rootca_crt_bundle_start);
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.onProgress([](int cur, int total) {
    int p = total > 0 ? (int)((int64_t)cur * 100 / total) : 0;
    if (p != upd.progress) { upd.progress = p; drawUpdateProgress(); }
  });
  t_httpUpdate_return r = https ? httpUpdate.update(tls, url) : httpUpdate.update(plain, url);
  if (r == HTTP_UPDATE_OK) {
    prefs.putString("upd_to", upd.version);
    upd.progress = 100; drawUpdateProgress();
    restartAt = millis() + 800;
  } else {
    upd.progress = -1;
    notice("update failed", httpUpdate.getLastErrorString(), C_BAD_U, 10000);
  }
}

// ---- boot guard / rollback

static bool bootHealthy = false;

static void bootGuard() {
  int fails = prefs.getInt("bootfail", 0) + 1;
  prefs.putInt("bootfail", fails);
  if (fails < 3) return;
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(running);
  esp_app_desc_t desc;
  if (other && esp_ota_get_partition_description(other, &desc) == ESP_OK &&
      esp_ota_set_boot_partition(other) == ESP_OK) {
    prefs.putInt("bootfail", 0);
    prefs.putString("rolledback", FW_VERSION);
    prefs.putString("skipver", FW_VERSION);     // don't offer the broken build again
    ESP.restart();
  }
  prefs.putInt("bootfail", 0);                  // nothing to roll back to; keep going
}

static void bootHealthTick() {
  if (bootHealthy || millis() < 20000) return;
  bootHealthy = true;
  prefs.putInt("bootfail", 0);
}

static void bootNotices() {
  String rb = prefs.getString("rolledback", "");
  if (rb.length()) {
    notice("rolled back", "v" + rb + " kept crashing on boot, so the Stick went back to v" + String(FW_VERSION) +
           ". That version is skipped.", C_YEL_U, 15000);
    prefs.remove("rolledback");
  }
  String to = prefs.getString("upd_to", "");
  if (to.length()) {
    if (to == FW_VERSION) notice("updated!", String("now on v") + FW_VERSION, C_OK_U, 8000);
    prefs.remove("upd_to");
  }
}
