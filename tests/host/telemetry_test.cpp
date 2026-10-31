// Opt-in telemetry: off by default, gated on a destination being set, scheduled like update checks,
// counts the right things in the right places, and never sends anything but the documented fields.
#include "../../firmware/src/main.cpp"
#include <string>
#include <map>
#include <vector>
extern std::map<std::string, std::string> PREFS;
extern std::string REQ_BODY, LAST_SENT, HTTP_BODY;
extern int LAST_CODE, HTTP_CODE;
extern unsigned long NOW;
struct HttpScripted { int code; std::string body; };
extern std::vector<HttpScripted> HTTP_SCRIPT;
extern std::vector<std::string> HTTP_LOG;
static int fails = 0;
static void check(const char* n, bool c, const std::string& x = "") { printf("%s %s%s\n", c ? "PASS" : "FAIL", n, c ? "" : (" | " + x).c_str()); if (!c) fails++; }
void M5Canvas::pushSprite(int, int) {}
static void at(long epoch) { struct timeval tv = { (time_t)epoch, 0 }; settimeofday(&tv, nullptr); }
static void script(int code, const std::string& body) { HTTP_SCRIPT.push_back({ code, body }); }
static std::string lastReq() { return HTTP_LOG.empty() ? "" : HTTP_LOG.back(); }
static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
static const long NOWEPOCH = 1793390000;

// Drives telemetryTick() past its own scheduling gates without waiting in real time.
static void forceTick() { NOW += 91000; telemetryTick(); }

int main() {
  PREFS.clear(); PREFS["setup"] = "1"; PREFS["ssid"] = "home-wifi";
  NOW = 100000; setup(); WiFi.st = WL_CONNECTED; at(NOWEPOCH); token = "";

  // ---- off by default, and gated on having somewhere to send it
  check("telemetry starts off", !conf.telemetryOn && conf.telemetryUrl.length() == 0);
  forceTick();
  check("off: never sends, even once past the 90 s mark", HTTP_LOG.empty());
  conf.telemetryOn = true;
  forceTick();
  check("on with no destination: still nothing to send", HTTP_LOG.empty());
  conf.telemetryUrl = "https://stats.example.com";
  conf.telemetryOn = false;
  forceTick();
  check("a destination alone isn't enough: still opt-in", HTTP_LOG.empty());

  // ---- scheduling: like update checks, waits for a quiet moment, then every 6 h
  conf.telemetryOn = true;
  tel.appr = 3;
  Approval* pending = newApproval(); pending->risk = "";
  telemetryTick();
  check("not yet 90 s since boot: doesn't fire early", HTTP_LOG.empty());
  NOW += 91000;
  telemetryTick();
  check("an open approval holds it off", HTTP_LOG.empty());
  pending->used = false;
  NOW += 16000;   // past the 15 s idle/retry gate
  script(200, "{}");
  telemetryTick();
  check("quiet + past 90 s: sends", HTTP_LOG.size() == 1, std::to_string(HTTP_LOG.size()));
  std::string r = lastReq();
  check("POST to <url>/report, JSON content type, the token header absent (telemetry carries none)",
        has(r, "POST https://stats.example.com/report") && has(r, "Content-Type: application/json") && !has(r, "X-Token"), r);
  NOW += 16000;
  telemetryTick();
  check("right after a send: too soon for another", HTTP_LOG.size() == 1);
  NOW += 6UL * 3600 * 1000;
  script(200, "{}");
  telemetryTick();
  check("6 h later: sends again", HTTP_LOG.size() == 2);

  // ---- the payload: what's in it, and what never is
  tel = TelCounters();
  tel.appr = 5; tel.deny = 2; tel.always = 1; tel.danger = 1; tel.macroTaps = 4; tel.doneChoose = 3; tel.doneDismiss = 1;
  tel.voiceOk = 2; tel.voiceFail = 1; tel.sessionsSeen = 3; tel.remoteOnMs = 1800000; tel.remoteOffMs = 3600000;
  resetWhy = "power on"; safeMode = false;
  NOW += 7UL * 3600 * 1000;
  script(200, "{}");
  telemetryTick();
  r = lastReq();
  check("device id, firmware version, boot/uptime, reset reason",
        has(r, "\"device_id\":\"") && has(r, (String("\"fw\":\"") + FW_VERSION + "\"").c_str()) && has(r, "\"reset_reason\":\"power on\"") &&
        has(r, "\"uptime_s\":") && has(r, "\"boot_epoch\":"), r);
  check("approval counts", has(r, "\"approvals\":5") && has(r, "\"denies\":2") && has(r, "\"always_allow\":1") && has(r, "\"danger_cards\":1"), r);
  check("macro/done/voice/session counts",
        has(r, "\"macro_taps\":4") && has(r, "\"done_choose\":3") && has(r, "\"done_dismiss\":1") &&
        has(r, "\"voice_ok\":2") && has(r, "\"voice_fail\":1") && has(r, "\"sessions_seen\":3"), r);
  check("remote on/off time, in seconds", has(r, "\"remote_on_s\":1800") && has(r, "\"remote_off_s\":3600"), r);
  check("battery and device flags", has(r, "\"battery_pct\":") && has(r, "\"battery_charging\":") && has(r, "\"bridge_configured\":false") &&
        has(r, "\"bt_on\":true") && has(r, "\"remote_mode\":") && has(r, "\"text_style\":") && has(r, "\"buddy\":") &&
        has(r, "\"tilt_on\":") && has(r, "\"flip_on\":") && has(r, "\"wifi_networks\":1"), r);
  check("never carries a tool/approval name, 'detail', a token, your SSID, or a macro's label/text",
        !has(r, "\"tool\"") && !has(r, "\"detail\"") && !has(r, "\"token\"") && !has(r, "\"ssid\"") &&
        !has(r, ssid.c_str()) && !has(r, "Continue.") && !has(r, "Keep going"), r);

  // ---- reset on success, kept on failure
  tel.appr = 5; tel.sessionsSeen = 3;
  NOW += 7UL * 3600 * 1000;
  script(500, "{}");
  size_t before = HTTP_LOG.size();
  telemetryTick();
  check("a failed send is still attempted", HTTP_LOG.size() == before + 1);
  check("...but counts are kept, not thrown away", tel.appr == 5 && tel.sessionsSeen == 3);
  NOW += 7UL * 3600 * 1000;
  script(200, "{}");
  telemetryTick();
  check("a successful send clears the window", tel.appr == 0 && tel.deny == 0 && tel.macroTaps == 0 && tel.remoteOnMs == 0);

  // ---- https needs the clock, same as updates and the bridge
  at(1);   // clock unknown
  conf.telemetryUrl = "https://stats.example.com"; NOW += 7UL * 3600 * 1000;
  HTTP_LOG.clear();
  telemetryTick();
  check("https with no clock yet: doesn't try", HTTP_LOG.empty());
  at(NOWEPOCH);
  conf.telemetryUrl = "http://192.168.1.50:9000"; NOW += 7UL * 3600 * 1000;
  script(200, "{}");
  telemetryTick();
  check("plain http needs no clock", has(lastReq(), "POST http://192.168.1.50:9000/report"), lastReq());
  conf.telemetryUrl = "https://stats.example.com";

  // ---- device id: stable, random, not the Wi-Fi-derived device id used for mDNS
  String id1 = telemetryId();
  PREFS.erase("telid"); PREFS["telid_keep"] = std::string(id1.c_str());   // simulate re-reading settings, not a reboot
  check("the id is cached, not regenerated on every call", telemetryId() == id1);
  check("it isn't the hardware-derived device id used for discovery", id1 != devId, (std::string(id1.c_str()) + " vs " + std::string(devId.c_str())).c_str());

  // ---- counters increment where they should
  tel = TelCounters();
  Approval* a1 = newApproval(); a1->risk = ""; a1->always = "";
  decide(a1, D_ALLOW);
  check("plain allow", tel.appr == 1 && tel.deny == 0 && tel.always == 0 && tel.danger == 0);
  Approval* a2 = newApproval(); a2->risk = ""; a2->always = "npm run *";
  decide(a2, D_ALWAYS);
  check("always-allow counts both appr and always", tel.appr == 2 && tel.always == 1);
  Approval* a3 = newApproval(); a3->risk = "high"; a3->always = "";
  decide(a3, D_ALLOW);
  check("a danger card counts even when simply allowed", tel.danger == 1);
  Approval* a4 = newApproval(); a4->risk = "";
  decide(a4, D_DENY);
  check("deny", tel.deny == 1);

  tel = TelCounters();
  StopReq* sr = nullptr;
  for (auto& x : stops) if (!x.used) { x = StopReq(); x.used = true; x.id = 1; sr = &x; break; }
  stopResult(sr, 0);
  check("choosing an option counts as a choose", tel.doneChoose == 1 && tel.doneDismiss == 0);
  for (auto& x : stops) if (!x.used) { x = StopReq(); x.used = true; x.id = 2; sr = &x; break; }
  stopResult(sr, -2);
  check("dismissing counts separately", tel.doneChoose == 1 && tel.doneDismiss == 1);

  tel = TelCounters();
  int before_sessions = 0; for (auto& x : sessions) if (x.used) before_sessions++;
  findSession("new-session-xyz", true);
  check("a brand new session counts once", tel.sessionsSeen == 1);
  findSession("new-session-xyz", true);   // same id again: not a new session
  check("...and not again for the same id", tel.sessionsSeen == 1);

  tel = TelCounters();
  for (auto& s : sessions) s.used = false;
  Session* ms = findSession("macro-sess", true); ms->bridge = false;
  REQ_BODY = R"({"action":"macro","session":"macro-sess","index":0})";
  hAct();
  check("a macro sent from the phone/ntfy counts a tap", tel.macroTaps == 1, LAST_SENT);
  REQ_BODY = R"({"action":"say","session":"macro-sess","text":"hello"})";
  hAct();
  check("free text ('say') is not counted as a macro tap", tel.macroTaps == 1);

  tel = TelCounters();
  voiceFail("out of memory");
  check("a voice failure is counted", tel.voiceFail == 1 && tel.voiceOk == 0);

  // ---- remote on/off time only accrues while telemetry is actually on
  tel.remoteOnMs = tel.remoteOffMs = 0;
  conf.telemetryOn = false;
  conf.remote = 1; updateRemote();   // remote mode on
  telemetryRemoteTick();
  NOW += 5000; telemetryRemoteTick();
  check("not counted while telemetry is off", tel.remoteOnMs == 0 && tel.remoteOffMs == 0);
  conf.telemetryOn = true;
  NOW += 3000; telemetryRemoteTick();
  NOW += 2000; telemetryRemoteTick();
  check("counts time in remote mode once telemetry is on", tel.remoteOnMs >= 2000 && tel.remoteOnMs <= 6000, std::to_string(tel.remoteOnMs));
  conf.remote = 0; updateRemote();
  NOW += 4000; telemetryRemoteTick();
  check("switches to the off bucket", tel.remoteOffMs >= 3000 && tel.remoteOffMs <= 5000, std::to_string(tel.remoteOffMs));

  // ---- /setup/info and /setup/save: the FTUE checkbox
  conf.telemetryOn = false; conf.telemetryUrl = "";
  hSetupInfo();
  check("unavailable (no destination configured) and off", has(LAST_SENT, "\"telemetry_available\":false") && has(LAST_SENT, "\"telemetry\":false"), LAST_SENT);
  conf.telemetryUrl = "https://stats.example.com";
  hSetupInfo();
  check("available once a destination exists", has(LAST_SENT, "\"telemetry_available\":true"), LAST_SENT);
  REQ_BODY = R"({"telemetry":true})";
  hSetupSave();
  check("the FTUE checkbox turns it on without a restart", conf.telemetryOn && !has(LAST_SENT, "\"restart\":true"), LAST_SENT);
  REQ_BODY = R"({"telemetry":false})";
  hSetupSave();
  check("...and off again", !conf.telemetryOn);

  // ---- /config: the destination is editable like the update source, and round-trips
  // (hConfig() itself branches on server.method(), which the host WebServer stub can't fake as
  // POST; configFromJson/configToJson are the actual logic underneath it, and the one under test.)
  JsonDocument cin;
  deserializeJson(cin, R"({"telemetry":true,"telemetry_url":"https://new.example.com"})");
  configFromJson(cin);
  check("saved via /config", conf.telemetryOn && conf.telemetryUrl == "https://new.example.com");
  JsonDocument cout; configToJson(cout, true);
  String cbodyS; serializeJson(cout, cbodyS); std::string cbody(cbodyS.c_str());
  check("GET /config reports it back, same shape as update_url", has(cbody, "\"telemetry\":true") && has(cbody, "\"telemetry_url\":\"https://new.example.com\""), cbody);
  check("settings persist across a reboot (saveConfig/loadConfig), like every other setting", saveConfig());
  conf.telemetryOn = false; conf.telemetryUrl = "";
  loadConfig();
  check("reloaded from flash", conf.telemetryOn && conf.telemetryUrl == "https://new.example.com");

  printf("\ntelemetry: %s\n", fails ? "FAILED" : "all passed");
  return fails;
}
