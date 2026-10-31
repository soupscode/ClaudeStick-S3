// ClaudeStick — approvals, status and macros for Claude on an M5Stack StickS3.
//
//  * Bluetooth: speaks Anthropic's Hardware Buddy protocol (REFERENCE.md in
//    anthropics/claude-desktop-buddy) for the Claude desktop app.
//  * Wi-Fi: small token-protected HTTP API driven by Claude Code hooks
//    (hooks/claudestick_hook.py) for CLI sessions anywhere on your LAN.
//
// Buttons (A = front, B = side):
//   Approval card : A click approve · B click deny · A hold = answer at keyboard
//   Macro menu    : B click next · A click send · B hold dismiss
//   Question card : B click next option · A click answer · B hold answer at keyboard
//   Lists         : B next · PWR (power button) previous · A select · B hold back
//   Home          : A click next session · A hold session picker (pin) · B hold menu
//   Long text     : tilt the Stick to scroll
//   Phone         : http://<stick-ip>:7880/?t=<token>
//
// MIT License. ble_bridge.* is from anthropics/claude-desktop-buddy (MIT).

#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <sys/time.h>
#include "ble_bridge.h"
#include "buddy.h"
#include "embedded_files.h"

#include "build_info.h"      // generated: CS_UPDATE_URL from CI
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef CS_WIFI_SSID
#define CS_WIFI_SSID ""
#endif
#ifndef CS_WIFI_PASS
#define CS_WIFI_PASS ""
#endif
#ifndef CS_TOKEN
#define CS_TOKEN ""
#endif
#ifndef CS_UPDATE_URL          // set by CI to the GitHub Pages URL of the web flasher
#define CS_UPDATE_URL ""
#endif
#ifndef CS_TELEMETRY_URL       // optional: where opt-in telemetry reports go (see docs/TELEMETRY.md)
#define CS_TELEMETRY_URL ""
#endif

static const char*    FW_VERSION   = "0.9.3";
static const uint16_t HTTP_PORT    = 7880;
static const uint32_t DIM_AFTER_MS = 60000;
static const uint32_t POLL_STALE_MS = 15000;   // hook stopped polling -> drop request
static const int      MAX_SESSIONS = 6;
static const int      MAX_APPROVALS = 8;
static const int      MAX_STOPS    = 4;
static const int      MAX_MACROS   = 10;
static const int      MAX_CHOICES  = MAX_MACROS + 4;   // Claude's own options + your macros

// ---------------------------------------------------------------- state ---

struct Macro { String label; String text; };

static const int      MAX_TODO = 12;

struct TodoItem { char st = 'p'; String text; };   // st: p pending, i in progress, c completed

struct Session {
  bool     used = false;
  String   id, project, state, detail;
  uint32_t lastSeen = 0, stateSince = 0;
  bool     stopFlag = false;   // "Stop now" queued from the device
  String   inject;             // macro queued mid-run
  // Claude's to-do list (from the hook)
  TodoItem todo[MAX_TODO];
  uint8_t  nTodo = 0, todoSkip = 0, todoDone = 0, todoTotal = 0;
  String   todoNow;
  // token usage for this session (from the transcript)
  uint32_t tokIn = 0, tokOut = 0, tokCR = 0, tokCW = 0, ctx = 0;
  String   model;
  bool     hasUsage = false;
  // from Claude Code's status line (see the hook's --statusline)
  float    ctxPct = -1, cost = -1;
  String   modelName;
  // mirrored from a bridge (bridge.inc) rather than reported by a hook talking to the Stick
  bool     bridge = false;
  uint8_t  gen = 0;
  uint16_t planDone = 0;
};

// Plan limits are account-wide: the 5-hour and 7-day windows (Pro/Max), with reset times.
struct Limit { float pct = -1; uint32_t resets = 0; };
static struct { Limit five, seven; uint32_t at = 0; bool set = false; } lim;

// Tokens per session per day, kept after a session ends so "today" stays right.
struct DayTok { String sid, day; uint32_t tok = 0; };
static const int MAX_LEDGER = 24;
static DayTok   ledger[MAX_LEDGER];
static String   curDay;          // local date from the newest hook report

enum Src : uint8_t { SRC_WIFI = 0, SRC_BLE = 1 };
enum Decision : int8_t { D_PENDING = -1, D_DENY = 0, D_ALLOW = 1, D_SKIP = 2, D_ALWAYS = 3 };

struct Approval {
  bool     used = false;
  bool     bridge = false;     // lives on a bridge: answer it there
  uint32_t id = 0;
  Src      src = SRC_WIFI;
  String   bleId;              // desktop prompt id
  String   tool, detail, project;
  String   always;             // what "always allow" would remember ("" = not offered)
  String   risk, riskWhy;      // risk == "high" needs a double-press to allow
  String   session, reason;    // reason: what you said when denying by voice
  uint32_t created = 0, lastPoll = 0;
  int8_t   decision = D_PENDING;
};

struct StopReq {
  bool     used = false;
  bool     bridge = false;
  uint32_t id = 0;
  String   kind = "done";      // "done" (turn finished) or "question" (AskUserQuestion)
  String   session, project, last, title, custom;
  Macro    macros[MAX_CHOICES];
  int      nMacros = 0;
  uint32_t created = 0, lastPoll = 0;
  int      result = -1;        // -1 pending, -2 dismissed, -3 custom text, >=0 macro index
};

static Session  sessions[MAX_SESSIONS];
static Approval approvals[MAX_APPROVALS];
static StopReq  stops[MAX_STOPS];
static Macro    macros[MAX_MACROS];
static int      nMacros = 0;
static uint32_t nextId = 1;

// desktop (BLE) snapshot
static struct {
  int total = 0, running = 0, waiting = 0;
  String msg, entry;
  long tokensToday = 0;
  uint32_t lastBeat = 0;
} desk;

static struct { uint32_t appr = 0, deny = 0; } stats;

// Opt-in telemetry: a window of counts since the last successful report (telemetry.inc resets
// it to zero on success). Never holds anything typed, any tool/approval text, or audio.
struct TelCounters {
  uint32_t appr = 0, deny = 0, always = 0, danger = 0;
  uint32_t macroTaps = 0, doneChoose = 0, doneDismiss = 0;
  uint32_t voiceOk = 0, voiceFail = 0, sessionsSeen = 0;
  uint32_t remoteOnMs = 0, remoteOffMs = 0;
};
static TelCounters tel;

static Preferences prefs;
static WebServer   server(HTTP_PORT);
static WebServer   portal(80);          // hotspot captive-portal redirector
static DNSServer   dns;
M5Canvas           spr(&M5.Display);   // shared with buddy.cpp (extern)
static String      ssid, pass, token, mdnsHost, devName, ownerName;
static bool        mdnsUp = false, clockSet = false, remoteMode = false;
static bool        setupDone = false, apOn = false;
static uint8_t     volume = 120;
static uint32_t    restartAt = 0, doneAt = 0;
static String      apName, devId;
static bool        flipped = false;    // screen rotated 180 degrees
static bool        safeMode = false;   // Bluetooth off (hold B at power-on, or automatic after a crash loop)
static bool        btPref = true;      // Bluetooth for the Claude desktop app; off saves battery when you're away from your computer
static bool        btOff() { return safeMode || !btPref; }
static String      resetWhy;

// Settings shared with every machine's hook (GET/POST /config). Stored on the Stick
// so macros and waits are edited once, from the settings page.
struct Settings {
  int     permWait = 20, remotePermWait = 600, stopWait = 600;
  String  stopMenu = "remote";          // remote | always | never
  bool    questions = true;             // AskUserQuestion on the Stick in remote mode
  uint8_t remote = 2;                   // 0 off, 1 on, 2 auto (follows where you last answered)
  String  ntfyUrl, ntfyToken;
  String  sttUrl, sttModel = "whisper-1", sttKey, sttLang;
  String  updateUrl = CS_UPDATE_URL;
  bool    telemetryOn = false;          // opt-in; see docs/TELEMETRY.md for exactly what a report contains
  String  telemetryUrl = CS_TELEMETRY_URL;
} conf;
static uint32_t lastStickAt = 0, lastKbdAt = 0;   // auto remote mode inputs
static int32_t     tzOffset = 0;       // fixed offset from the hook / desktop, used only until a time zone is chosen
static String      tzPosix, tzName;    // chosen zone: POSIX rule (knows daylight saving) and its friendly id
static bool        clock24 = false;    // false = 12-hour "8:59p"
static bool        ntpStarted = false;
static uint8_t     textStyle = 1;       // 0 = classic 6x8, 1 = "code" (same cell, cleaner shapes; see tools/gen_font.py)

enum Screen { SCR_HOME, SCR_INFO, SCR_ACTIONS, SCR_SESSIONS, SCR_HELP, SCR_TEST, SCR_DETAIL, SCR_PET };
static Screen   screen = SCR_HOME;
static int      menuIdx = 0, focusSession = -1;
static uint32_t lastActivity = 0, lastBeep = 0, lastDraw = 0;
static bool     dirty = true, dimmed = false;
static uint32_t shownApproval = 0, shownStop = 0;
static String   pinned;          // session id the Stick is tied to ("" = all sessions)
static uint32_t heartUntil = 0;  // buddy shows a heart briefly after an approval
static int      scrollLine = 0;  // first visible line of long card text
static uint32_t scrollKey = 0;   // which card the scroll belongs to
static int8_t   tiltMode = 1;    // 1 normal, -1 flipped, 0 off
static String   answeredBleId;   // ignore the prompt we just answered until the desktop clears it

// --------------------------------------------------------------- helpers ---

static void touch() {
  lastActivity = millis();
  dirty = true;
  if (dimmed) { M5.Display.setBrightness(160); dimmed = false; }
}

static void beep(int pattern) {
  switch (pattern) {
    case 0: M5.Speaker.tone(2200, 60); break;                         // click
    case 1: M5.Speaker.tone(1800, 120); delay(150); M5.Speaker.tone(2400, 160); break;  // approval
    case 2: M5.Speaker.tone(1400, 90); delay(110); M5.Speaker.tone(1900, 90);
            delay(110); M5.Speaker.tone(2600, 140); break;            // done
    case 3: M5.Speaker.tone(600, 200); break;                         // deny / stop
  }
}

static String shortId(const String& s) { return s.length() > 8 ? s.substring(0, 8) : s; }

static Session* findSession(const String& id, bool create) {
  int freeIdx = -1, oldest = 0;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].used && sessions[i].id == id) return &sessions[i];
    if (!sessions[i].used && freeIdx < 0) freeIdx = i;
    if (sessions[i].lastSeen < sessions[oldest].lastSeen) oldest = i;
  }
  if (!create) return nullptr;
  int i = freeIdx >= 0 ? freeIdx : oldest;
  sessions[i] = Session();
  sessions[i].used = true;
  sessions[i].id = id;
  tel.sessionsSeen++;
  return &sessions[i];
}

// Session the actions menu targets: focused one, else most recently active.
static Session* targetSession() {
  if (focusSession >= 0 && sessions[focusSession].used) return &sessions[focusSession];
  Session* best = nullptr;
  for (auto& s : sessions) if (s.used && (!best || s.lastSeen > best->lastSeen)) best = &s;
  return best;
}

static Approval* topApproval() {
  Approval* best = nullptr;
  for (auto& a : approvals)
    if (a.used && a.decision == D_PENDING && (!best || a.created < best->created)) best = &a;
  return best;
}

static StopReq* topStop() {
  StopReq* best = nullptr;
  for (auto& s : stops)
    if (s.used && s.result == -1 && (!best || s.created < best->created)) best = &s;
  return best;
}

static Approval* newApproval() {
  for (auto& a : approvals) if (!a.used) { a = Approval(); a.used = true; a.id = nextId++;
                                           a.created = a.lastPoll = millis(); return &a; }
  return nullptr;
}

static void setClock(long epoch, long offset) {
  struct timeval tv = { (time_t)epoch, 0 };
  settimeofday(&tv, nullptr);
  if (!tzPosix.length()) tzOffset = offset;   // a chosen time zone wins over the sender's offset
  clockSet = true;
}

static bool clockOk() { return time(nullptr) > 1700000000; }   // set by NTP, the hook, the desktop or the setup page

static bool validTz(const String& t) {
  if (!t.length() || t.length() > 63) return false;
  for (size_t i = 0; i < t.length(); i++)
    if (!(isalnum((unsigned char)t[i]) || strchr(",.:/+-<>", t[i]))) return false;
  return true;
}

static void applyTimezone() {
  setenv("TZ", tzPosix.length() ? tzPosix.c_str() : "UTC0", 1);
  tzset();
  ntpStarted = false;   // restart SNTP so it uses the rule
}

// Start SNTP once Wi-Fi is up. No internet is fine: the hook, the desktop and the setup page can still set the time.
static void ntpTick() {
  if (ntpStarted || WiFi.status() != WL_CONNECTED) return;
  ntpStarted = true;
  configTzTime(tzPosix.length() ? tzPosix.c_str() : "UTC0", "pool.ntp.org", "time.cloudflare.com", "time.google.com");
}

static String clockStr() {
  if (!clockOk()) return "";
  time_t t = time(nullptr);
  struct tm tm;
  if (tzPosix.length()) localtime_r(&t, &tm);
  else { t += tzOffset; gmtime_r(&t, &tm); }
  char b[10];
  if (clock24) snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
  else {
    int h = tm.tm_hour % 12;
    snprintf(b, sizeof b, "%d:%02d%c", h ? h : 12, tm.tm_min, tm.tm_hour < 12 ? 'a' : 'p');
  }
  return b;
}
static void loadMacros(JsonArrayConst arr, Macro* out, int& n) {
  n = 0;
  for (JsonVariantConst m : arr) {
    if (n >= MAX_MACROS) break;
    out[n].label = (const char*)(m["label"] | "");
    out[n].text  = (const char*)(m["text"] | "");
    if (out[n].label.length() && out[n].text.length()) n++;
  }
}

static const char* DEFAULT_MACROS = R"JSON([
 {"label":"Continue","text":"Continue."},
 {"label":"Keep going","text":"Keep going until the task is fully done. Only stop if you are truly blocked."},
 {"label":"Next phase","text":"That phase looks good. Proceed to the next phase of the plan."},
 {"label":"Run tests + fix","text":"Run the test suite and fix any failures before continuing."},
 {"label":"Summarize","text":"Summarize what you did, what's left, and any risks, in under 10 bullets."},
 {"label":"Commit","text":"Commit the current work with a clear, conventional commit message."}])JSON";

static const char* remoteName(uint8_t r) { return r == 0 ? "off" : r == 1 ? "on" : "auto"; }
static bool bridgeUp = false, bridgeRemote = false;   // set by bridge.inc
static uint8_t radioSleep = 0;   // 0 = standard modem sleep, 1 = maximum (experimental: see the "sleep" serial command)
static void applyRadioSleep() { WiFi.setSleep(radioSleep ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM); }

// Battery: how fast it drains while unplugged, measured from the moment it was unplugged (or boot).
static struct { uint32_t t0 = 0; int pct0 = -1; } drain;

static void drainTick() {
  static uint32_t last = 0;
  if (millis() - last < 30000) return;
  last = millis();
  int pct = M5.Power.getBatteryLevel();
  if ((int)M5.Power.isCharging() == 1 || pct < 0) { drain.pct0 = -1; return; }   // plugged in: start over when unplugged
  if (drain.pct0 < 0 || pct > drain.pct0) { drain.pct0 = pct; drain.t0 = millis(); }
}

// Percent per hour once there are 10 minutes and at least 2 points to go on, else -1.
static float drainPerHour() {
  int pct = M5.Power.getBatteryLevel();
  uint32_t ms = millis() - drain.t0;
  if (drain.pct0 < 0 || pct < 0 || ms < 10UL * 60 * 1000 || drain.pct0 - pct < 2) return -1;
  return (drain.pct0 - pct) * 3600000.0f / ms;
}

static String batteryLine() {
  int pct = M5.Power.getBatteryLevel();
  String s = String(pct) + "% " + String(M5.Power.getBatteryVoltage() / 1000.0f, 2) + "V";
  if ((int)M5.Power.isCharging() == 1) return s + " charging";
  float r = drainPerHour();
  if (r > 0) { float h = pct / r; s += "  " + String(r, 1) + "%/h ~" + String(h, h < 10 ? 1 : 0) + "h"; }
  else s += "  measuring";
  return s;
}

// Effective remote mode. In auto it follows where you last answered: the Stick or
// phone (remote) versus typing in the terminal (desk).
static void updateRemote() {
  if (bridgeUp) { if (bridgeRemote != remoteMode) { remoteMode = bridgeRemote; dirty = true; } return; }
  bool r = conf.remote == 1 || (conf.remote == 2 && lastStickAt > lastKbdAt);
  if (r != remoteMode) { remoteMode = r; dirty = true; }
}
static void noteStick() { lastStickAt = millis() | 1; updateRemote(); }
static void noteKeyboard() { lastKbdAt = millis() | 1; updateRemote(); }

static void configToJson(JsonDocument& o, bool secrets) {
  o["permission_wait_s"] = conf.permWait;
  o["remote_permission_wait_s"] = conf.remotePermWait;
  o["stop_wait_s"] = conf.stopWait;
  o["stop_menu"] = conf.stopMenu;
  o["questions_on_device"] = conf.questions;
  o["remote"] = remoteName(conf.remote);
  o["ntfy_url"] = conf.ntfyUrl;
  o["stt_url"] = conf.sttUrl; o["stt_model"] = conf.sttModel; o["stt_language"] = conf.sttLang;
  o["update_url"] = conf.updateUrl;
  o["telemetry"] = conf.telemetryOn; o["telemetry_url"] = conf.telemetryUrl;
  if (secrets) { o["ntfy_token"] = conf.ntfyToken; o["stt_key"] = conf.sttKey; }
  else { o["ntfy_token_set"] = conf.ntfyToken.length() > 0; o["stt_key_set"] = conf.sttKey.length() > 0; }
  JsonArray m = o["macros"].to<JsonArray>();
  for (int i = 0; i < nMacros; i++) { JsonObject x = m.add<JsonObject>(); x["label"] = macros[i].label; x["text"] = macros[i].text; }
}

static void configFromJson(JsonDocument& in) {
  auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
  if (in["permission_wait_s"].is<int>()) conf.permWait = clampi(in["permission_wait_s"], 1, 3600);
  if (in["remote_permission_wait_s"].is<int>()) conf.remotePermWait = clampi(in["remote_permission_wait_s"], 5, 3600);
  if (in["stop_wait_s"].is<int>()) conf.stopWait = clampi(in["stop_wait_s"], 5, 3600);
  if (in["stop_menu"].is<const char*>()) conf.stopMenu = in["stop_menu"].as<const char*>();
  if (in["questions_on_device"].is<bool>()) conf.questions = in["questions_on_device"];
  if (in["remote"].is<const char*>()) { String r = in["remote"].as<const char*>(); conf.remote = r == "off" ? 0 : r == "on" ? 1 : 2; }
  if (in["ntfy_url"].is<const char*>()) conf.ntfyUrl = in["ntfy_url"].as<const char*>();
  if (in["ntfy_token"].is<const char*>()) conf.ntfyToken = in["ntfy_token"].as<const char*>();
  if (in["stt_url"].is<const char*>()) conf.sttUrl = in["stt_url"].as<const char*>();
  if (in["stt_model"].is<const char*>()) conf.sttModel = in["stt_model"].as<const char*>();
  if (in["stt_key"].is<const char*>()) conf.sttKey = in["stt_key"].as<const char*>();
  if (in["stt_language"].is<const char*>()) conf.sttLang = in["stt_language"].as<const char*>();
  if (in["update_url"].is<const char*>()) conf.updateUrl = in["update_url"].as<const char*>();
  if (in["telemetry"].is<bool>()) conf.telemetryOn = in["telemetry"];
  if (in["telemetry_url"].is<const char*>()) conf.telemetryUrl = in["telemetry_url"].as<const char*>();
  if (in["macros"].is<JsonArray>()) loadMacros(in["macros"].as<JsonArrayConst>(), macros, nMacros);
  updateRemote();
}

static bool saveConfig() {
  JsonDocument o; configToJson(o, true);
  String out; serializeJson(o, out);
  return prefs.putBytes("conf", out.c_str(), out.length()) == out.length();
}

static void loadConfig() {
  size_t n = prefs.getBytesLength("conf");
  JsonDocument in;
  if (n > 0 && n < 16384) {
    char* buf = (char*)malloc(n + 1);
    prefs.getBytes("conf", buf, n); buf[n] = 0;
    if (!deserializeJson(in, buf)) configFromJson(in);
    free(buf);
  }
  if (!nMacros) {
    JsonDocument d; deserializeJson(d, DEFAULT_MACROS);
    loadMacros(d.as<JsonArrayConst>(), macros, nMacros);
  }
  if (!conf.updateUrl.length()) conf.updateUrl = CS_UPDATE_URL;
}

#include "termfont.h"
static const lgfx::GLCDfont termFont = { termfont_data, termfont_info, 6, 8, 7 };
static const lgfx::IFont* uiFont() { return textStyle ? (const lgfx::IFont*)&termFont : (const lgfx::IFont*)&fonts::Font0; }
#include "update.inc"   // update checks, rollback, notices (needs settings + helpers above)
#include "bridge.inc"   // remote access through a bridge at home (needs notices, TLS helpers from update.inc)

// ------------------------------------------------------------ BLE side ---

static void bleSend(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  out += "\n";
  bleWrite((const uint8_t*)out.c_str(), out.length());
}

static void bleAck(const char* cmd, bool ok = true, int n = 0) {
  JsonDocument d;
  d["ack"] = cmd; d["ok"] = ok; d["n"] = n;
  bleSend(d);
}

// The 6x8 font is ASCII only: fold UTF-8 punctuation and Latin letters to plain equivalents
// (the Wi-Fi hook does the same on its side).
static String asciiFold(const char* in) {
  String out;
  const uint8_t* p = (const uint8_t*)in;
  bool prevQ = false;
  while (*p) {
    uint32_t c = *p++;
    if (c >= 0xC0 && *p) {   // decode one UTF-8 sequence
      int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
      c &= (0x3F >> extra);
      while (extra-- > 0 && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    }
    if (c < 0x80) { if (c == '\t') out += "  "; else if (c != '\r') out += (char)c; prevQ = false; continue; }
    const char* r = nullptr;
    switch (c) {
      case 0x2018: case 0x2019: r = "'"; break;
      case 0x201C: case 0x201D: r = "\""; break;
      case 0x2013: case 0x2014: case 0x2212: r = "-"; break;
      case 0x2026: r = "..."; break;
      case 0x2022: case 0x00B7: r = "*"; break;
      case 0x2192: r = "->"; break;
      case 0x2190: r = "<-"; break;
      case 0x00D7: r = "x"; break;
      case 0x00A0: r = " "; break;
      default: break;
    }
    if (!r && c >= 0xC0 && c <= 0xFF) {   // Latin-1 letters -> base letter
      static const char* L = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
      char b = L[c - 0xC0];
      if (b != '*' && b != '/') { out += b; prevQ = false; continue; }
    }
    if (r) { out += r; prevQ = false; }
    else if (!prevQ) { out += '?'; prevQ = true; }
  }
  return out;
}

static void handleBleLine(const String& line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;

  if (doc["time"].is<JsonArray>()) {
    setClock(doc["time"][0] | 0L, doc["time"][1] | 0L);
    return;
  }

  if (doc["cmd"].is<const char*>()) {
    String cmd = doc["cmd"].as<String>();
    if (cmd == "status") {
      JsonDocument d;
      d["ack"] = "status"; d["ok"] = true;
      JsonObject data = d["data"].to<JsonObject>();
      data["name"] = devName;
      data["sec"]  = bleSecure();
      JsonObject bat = data["bat"].to<JsonObject>();
      bat["pct"] = M5.Power.getBatteryLevel();
      bat["mV"]  = M5.Power.getBatteryVoltage();
      bat["usb"] = (int)M5.Power.isCharging() == 1;
      JsonObject sys = data["sys"].to<JsonObject>();
      sys["up"] = millis() / 1000; sys["heap"] = ESP.getFreeHeap();
      JsonObject st = data["stats"].to<JsonObject>();
      st["appr"] = stats.appr; st["deny"] = stats.deny;
      bleSend(d);
    } else if (cmd == "name") {
      devName = doc["name"] | devName.c_str(); prefs.putString("name", devName); bleAck("name");
    } else if (cmd == "owner") {
      ownerName = doc["name"] | ""; bleAck("owner");
    } else if (cmd == "unpair") {
      if (!btOff()) bleClearBonds();
      bleAck("unpair");
    }
    // char_begin and other folder-push commands are deliberately not acked:
    // the desktop times out and reports that this device doesn't take files.
    dirty = true;
    return;
  }

  if (doc["evt"].is<const char*>()) return;   // turn events: not used

  // Heartbeat snapshot
  desk.total    = doc["total"] | 0;
  desk.running  = doc["running"] | 0;
  desk.waiting  = doc["waiting"] | 0;
  desk.msg      = asciiFold(doc["msg"] | "");
  desk.tokensToday = doc["tokens_today"] | 0L;
  desk.entry    = doc["entries"].is<JsonArray>() ? asciiFold(doc["entries"][0] | "") : String();
  desk.lastBeat = millis();

  String pid = doc["prompt"]["id"] | "";
  // Drop BLE approvals the desktop no longer shows (answered elsewhere).
  for (auto& a : approvals)
    if (a.used && a.src == SRC_BLE && a.bleId != pid) a.used = false;
  if (pid != answeredBleId && !pid.length()) answeredBleId = "";
  if (pid.length()) {
    bool have = pid == answeredBleId;
    for (auto& a : approvals) if (a.used && a.src == SRC_BLE && a.bleId == pid) have = true;
    if (!have) {
      if (Approval* a = newApproval()) {
        a->src = SRC_BLE; a->bleId = pid;
        a->tool = asciiFold(doc["prompt"]["tool"] | "tool");
        a->detail = asciiFold(doc["prompt"]["hint"] | "");
        a->project = "Desktop";
      }
    }
  }
  dirty = true;
}

static void pollBle() {
  static String buf;
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    if (c == '\n') { if (buf.length()) handleBleLine(buf); buf = ""; }
    else if (buf.length() < 4096) buf += (char)c;
  }
  if (desk.lastBeat && millis() - desk.lastBeat > 30000) {   // treat link as dead
    desk.lastBeat = 0; desk.total = desk.running = desk.waiting = 0;
    for (auto& a : approvals) if (a.used && a.src == SRC_BLE) a.used = false;
    dirty = true;
  }
}

static void decide(Approval* a, int8_t d, bool fromStick = true) {
  if (!a) return;
  if (d == D_ALWAYS && (a->src == SRC_BLE || !a->always.length())) d = D_ALLOW;   // not offered: plain allow
  if (a->bridge && !bridgeDecide(a, d)) return;   // couldn't reach the bridge: the card stays up
  if (d == D_ALLOW || d == D_ALWAYS) { stats.appr++; tel.appr++; heartUntil = millis() + 2500; }
  if (d == D_ALWAYS) tel.always++;
  if (a->risk == "high") tel.danger++;
  if (d == D_SKIP) noteKeyboard(); else if (fromStick) noteStick();
  if (d == D_DENY)  { stats.deny++; tel.deny++; }
  if (a->src == SRC_BLE) {
    if (d != D_SKIP) {
      JsonDocument j;
      j["cmd"] = "permission"; j["id"] = a->bleId; j["decision"] = d == D_DENY ? "deny" : "once";
      bleSend(j);
    }
    answeredBleId = a->bleId;
    a->used = false;          // desktop's next heartbeat confirms
  } else if (a->bridge) {
    a->used = false;          // the bridge has it; the next poll confirms
  } else {
    a->decision = d;          // hook picks it up on next poll
    a->lastPoll = millis();
  }
  beep(d == D_DENY ? 3 : 0);
  touch();
}

// ----------------------------------------------------------- HTTP side ---

// Compares in constant time so the token can't be guessed byte by byte from response timing.
static bool tokenEq(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

static bool authed() {
  if (!token.length()) return true;
  if (tokenEq(server.header("X-Token"), token) || tokenEq(server.arg("t"), token)) return true;
  server.send(401, "application/json", "{\"error\":\"bad token\"}");
  return false;
}

static bool readBody(JsonDocument& doc) {
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"bad json\"}");
    return false;
  }
  return true;
}

static void sendJson(JsonDocument& doc, int code = 200) {
  String out; serializeJson(doc, out);
  server.send(code, "application/json", out);
}

static void noteSession(JsonDocument& in, Session*& s) {
  String sid = in["session"] | "unknown";
  s = findSession(sid, true);
  String proj = in["project"] | "";
  if (proj.length()) s->project = proj;
  if (!s->project.length()) s->project = shortId(sid);
  s->lastSeen = millis();
  if (!clockOk() && in["now"].is<long>()) setClock(in["now"].as<long>(), in["tz"] | 0L);
  else if (!tzPosix.length() && in["tz"].is<long>()) tzOffset = in["tz"];   // follows the machine's daylight saving
}

// POST /event  {session, project, event, tool?, detail?, can_stop?, can_inject?, macros?}
// -> {"stop":bool, "inject":string|null}
static void noteLedger(const String& sid, const String& day, uint32_t tok) {
  if (!day.length()) return;
  if (day > curDay) curDay = day;
  int slot = -1;
  for (int i = 0; i < MAX_LEDGER; i++) if (ledger[i].sid == sid && ledger[i].day == day) slot = i;
  if (slot < 0)   // reuse a free slot, else one from an older day, else the smallest
    for (int i = 0; i < MAX_LEDGER; i++) if (!ledger[i].sid.length()) { slot = i; break; }
  if (slot < 0)
    for (int i = 0; i < MAX_LEDGER; i++) if (ledger[i].day != curDay) { slot = i; break; }
  if (slot < 0) {
    slot = 0;
    for (int i = 1; i < MAX_LEDGER; i++) if (ledger[i].tok < ledger[slot].tok) slot = i;
  }
  ledger[slot].sid = sid; ledger[slot].day = day; ledger[slot].tok = tok;
}

static uint32_t tokensToday() {
  uint32_t t = bridgeUp ? bridgeTokens : 0;
  for (auto& l : ledger) if (l.sid.length() && l.day == curDay) t += l.tok;
  return t;
}

// Optional to-do list / token usage riding along on any /event.
static void noteExtras(JsonDocument& in, Session* s) {
  if (in["plan_done"] | false) {   // Claude ticked off its last to-do
    beep(2); heartUntil = millis() + 3000; touch();
  }
  if (in["todo"].is<JsonObject>()) {
    JsonObject t = in["todo"];
    s->todoTotal = t["total"] | 0; s->todoDone = t["done"] | 0; s->todoSkip = t["skip"] | 0;
    s->todoNow = (const char*)(t["now"] | "");
    s->nTodo = 0;
    for (JsonArray it : t["items"].as<JsonArray>()) {
      if (s->nTodo >= MAX_TODO) break;
      String st = it[0] | "p";
      s->todo[s->nTodo].st = st.length() ? st[0] : 'p';
      s->todo[s->nTodo].text = (const char*)(it[1] | "");
      s->nTodo++;
    }
  }
  if (in["usage"].is<JsonObject>()) {
    JsonObject u = in["usage"];
    s->tokIn = u["in"] | 0UL; s->tokOut = u["out"] | 0UL; s->tokCR = u["cr"] | 0UL; s->tokCW = u["cw"] | 0UL;
    s->ctx = u["ctx"] | 0UL;
    String m = u["model"] | "";
    if (m.length()) s->model = m;
    s->hasUsage = true;
    noteLedger(s->id, u["day"] | "", u["today"] | 0UL);
  }
}

static void hEvent() {
  if (!authed()) return;
  JsonDocument in; if (!readBody(in)) return;
  Session* s; noteSession(in, s);
  noteExtras(in, s);
  String ev = in["event"] | "";
  if (in["typed"] | false) noteKeyboard();   // a prompt typed in the terminal: you're at the desk

  if (ev == "end") { s->used = false; if (pinned == s->id) pinned = ""; }
  else {
    String prev = s->state;
    s->state = ev;
    if (prev != ev) s->stateSince = millis();
    s->detail = (const char*)(in["detail"] | "");
    if (ev == "done" && prev != "done") beep(2);
    if (ev == "waiting") beep(1);
  }

  JsonDocument out;
  out["stop"] = false; out["inject"] = nullptr;
  out["remote"] = remoteMode;
  if (s->used && (in["can_stop"] | false) && s->stopFlag) { out["stop"] = true; s->stopFlag = false; }
  else if (s->used && (in["can_inject"] | false) && s->inject.length()) { out["inject"] = s->inject; s->inject = ""; }
  sendJson(out);
  dirty = true;
  if (ev == "done" || ev == "waiting") touch();
}

// POST /usage {session, ctx_pct?, ctx_size?, cost?, model?, five?:{pct,resets}, seven?:{pct,resets}}
// Sent by the hook's status-line mode. Limits are account-wide; the rest belongs to one session.
static void hUsage() {
  if (!authed()) return;
  JsonDocument in; if (!readBody(in)) return;
  String sid = in["session"] | "";
  Session* s = sid.length() ? findSession(sid, false) : nullptr;   // never creates a session
  if (s) {
    if (in["ctx_pct"].is<float>()) s->ctxPct = in["ctx_pct"];
    if (in["cost"].is<float>()) s->cost = in["cost"];
    if (in["model"].is<const char*>()) s->modelName = in["model"].as<const char*>();
  }
  bool any = false;
  auto take = [&](const char* k, Limit& l) {
    if (!in[k].is<JsonObject>()) return;
    l.pct = in[k]["pct"] | -1.0f; l.resets = in[k]["resets"] | 0UL; any = true;
  };
  take("five", lim.five); take("seven", lim.seven);
  if (any) { lim.at = millis(); lim.set = true; }
  if (!clockOk() && in["now"].is<long>()) setClock(in["now"].as<long>(), in["tz"] | 0L);
  JsonDocument out; out["ok"] = true; sendJson(out);
  dirty = true;
}

// POST /permission {session, project, tool, detail} -> {"id":N}
static void hPermissionNew() {
  if (!authed()) return;
  JsonDocument in; if (!readBody(in)) return;
  Session* s; noteSession(in, s);
  s->state = "approve";
  Approval* a = newApproval();
  if (!a) { server.send(503, "application/json", "{\"error\":\"queue full\"}"); return; }
  a->src = SRC_WIFI;
  a->tool = (const char*)(in["tool"] | "tool");
  a->detail = (const char*)(in["detail"] | "");
  a->project = s->project;
  a->session = s->id;
  a->always = (const char*)(in["always"] | "");
  a->risk = (const char*)(in["risk"] | "");
  a->riskWhy = (const char*)(in["risk_why"] | "");
  if (a->risk == "high") a->always = "";
  if (pinned.length() && pinned != s->id) a->decision = D_SKIP;   // not our session: terminal handles it
  JsonDocument out; out["id"] = a->id; out["remote"] = remoteMode; sendJson(out);
  touch();
}

static Approval* approvalById(uint32_t id) {
  for (auto& a : approvals) if (a.used && a.src == SRC_WIFI && a.id == id) return &a;
  return nullptr;
}

// GET /permission?id=N -> {"state":"pending|allow|deny|skip|gone"}
// DELETE /permission?id=N  (hook gave up / answered at keyboard)
static void hPermissionPoll() {
  if (!authed()) return;
  Approval* a = approvalById(server.arg("id").toInt());
  const char* st = "gone";
  if (a) {
    a->lastPoll = millis();
    if (server.method() == HTTP_DELETE) { a->used = false; st = "cancelled"; dirty = true; }
    else switch (a->decision) {
      case D_PENDING: st = "pending"; break;
      case D_ALLOW: st = "allow"; a->used = false; break;
      case D_DENY:  st = "deny";  a->used = false; break;
      case D_ALWAYS: st = "always"; a->used = false; break;
      default:      st = "skip";  a->used = false; break;
    }
  }
  JsonDocument out; out["state"] = st;
  if (a && a->decision == D_DENY && a->reason.length()) out["reason"] = a->reason;
  sendJson(out);
}

// POST /stop {session, project, kind?, title?, last, macros:[{label,text}]} -> {"id":N}
static void hStopNew() {
  if (!authed()) return;
  JsonDocument in; if (!readBody(in)) return;
  Session* s; noteSession(in, s);
  String kind = in["kind"] | "done";
  s->state = kind == "question" ? "question" : "done";
  StopReq* r = nullptr;
  for (auto& x : stops) if (!x.used) { r = &x; break; }
  if (!r) { server.send(503, "application/json", "{\"error\":\"queue full\"}"); return; }
  *r = StopReq();
  r->used = true; r->id = nextId++; r->created = r->lastPoll = millis();
  r->session = s->id; r->project = s->project;
  r->kind = kind;
  r->last = (const char*)(in["last"] | "");
  r->title = (const char*)(in["title"] | "");
  if (in["macros"].is<JsonArray>()) loadMacros(in["macros"].as<JsonArrayConst>(), r->macros, r->nMacros);
  else {
    // Claude's own suggested options first (remote mode), then the Stick's macros.
    if (in["options"].is<JsonArray>()) loadMacros(in["options"].as<JsonArrayConst>(), r->macros, r->nMacros);
    if (kind != "question")
      for (int i = 0; i < nMacros && r->nMacros < MAX_CHOICES; i++) r->macros[r->nMacros++] = macros[i];
  }
  if (pinned.length() && pinned != s->id) r->result = -2;           // not our session: dismiss
  JsonDocument out; out["id"] = r->id; out["remote"] = remoteMode; sendJson(out);
  touch();
}

// GET /stop?id=N -> {"state":"pending|dismiss|macro|custom|gone","text":...}
static void hStopPoll() {
  if (!authed()) return;
  uint32_t id = server.arg("id").toInt();
  StopReq* r = nullptr;
  for (auto& x : stops) if (x.used && x.id == id) r = &x;
  JsonDocument out;
  if (!r) out["state"] = "gone";
  else {
    r->lastPoll = millis();
    if (server.method() == HTTP_DELETE) { r->used = false; out["state"] = "cancelled"; dirty = true; }
    else if (r->result == -1) out["state"] = "pending";
    else if (r->result == -2) { out["state"] = "dismiss"; r->used = false; }
    else if (r->result == -3) { out["state"] = "custom"; out["text"] = r->custom; r->used = false; }
    else { out["state"] = "macro"; out["text"] = r->macros[r->result].text; r->used = false; }
  }
  sendJson(out);
}


// ------------------------------------------------------------- phone UI ---
// GET /?t=<token> serves a single page; it calls /state and /act with X-Token.

// The phone page lives in firmware/phone_page.html (embedded at build time).

static void hRoot() {
  if (!setupDone) { server.sendHeader("Location", "/setup"); server.send(302, "text/plain", ""); return; }
  server.send_P(200, "text/html", EMBED_PHONE_HTML);
}

static void hState() {
  if (!authed()) return;
  JsonDocument out;
  updateRemote();
  out["remote"] = remoteMode;
  out["remote_setting"] = remoteName(conf.remote);
  if (upd.available) out["update"] = upd.version;
  out["pinned"] = pinned;
  JsonArray ss = out["sessions"].to<JsonArray>();
  for (auto& s : sessions) if (s.used) {
    JsonObject o = ss.add<JsonObject>();
    o["id"] = s.id; o["project"] = s.project; o["state"] = s.state; o["detail"] = s.detail;
    if (s.todoTotal) {
      JsonObject t = o["todo"].to<JsonObject>();
      t["done"] = s.todoDone; t["total"] = s.todoTotal; t["now"] = s.todoNow; t["skip"] = s.todoSkip;
      JsonArray it = t["items"].to<JsonArray>();
      for (int i = 0; i < s.nTodo; i++) { JsonArray e = it.add<JsonArray>(); e.add(String(s.todo[i].st)); e.add(s.todo[i].text); }
    }
    if (s.ctxPct >= 0) o["ctx_pct"] = s.ctxPct;
    if (s.modelName.length()) o["model_name"] = s.modelName;
    if (s.hasUsage) {
      JsonObject u = o["usage"].to<JsonObject>();
      u["in"] = s.tokIn; u["out"] = s.tokOut; u["cr"] = s.tokCR; u["cw"] = s.tokCW; u["ctx"] = s.ctx; u["model"] = s.model;
    }
  }
  out["tokens_today"] = tokensToday();
  if (lim.five.pct >= 0 || lim.seven.pct >= 0) {
    JsonObject l = out["limits"].to<JsonObject>();
    if (lim.five.pct >= 0) { l["five"]["pct"] = lim.five.pct; l["five"]["resets"] = lim.five.resets; }
    if (lim.seven.pct >= 0) { l["seven"]["pct"] = lim.seven.pct; l["seven"]["resets"] = lim.seven.resets; }
    l["age_s"] = (millis() - lim.at) / 1000;
  }
  JsonArray as = out["approvals"].to<JsonArray>();
  for (auto& a : approvals) if (a.used && a.decision == D_PENDING) {
    JsonObject o = as.add<JsonObject>();
    o["id"] = a.id; o["tool"] = a.tool; o["detail"] = a.detail; o["project"] = a.project;
    o["src"] = a.src == SRC_BLE ? "ble" : "wifi";
    o["always"] = a.always; o["risk"] = a.risk; o["risk_why"] = a.riskWhy;
  }
  JsonArray cs = out["choices"].to<JsonArray>();
  for (auto& r : stops) if (r.used && r.result == -1) {
    JsonObject o = cs.add<JsonObject>();
    o["id"] = r.id; o["kind"] = r.kind; o["project"] = r.project; o["title"] = r.title; o["last"] = r.last;
    JsonArray opts = o["options"].to<JsonArray>();
    for (int i = 0; i < r.nMacros; i++) opts.add(r.macros[i].label);
  }
  JsonArray ms = out["macros"].to<JsonArray>();
  for (int i = 0; i < nMacros; i++) ms.add(macros[i].label);
  if (desk.lastBeat) out["desktop"] = String(desk.running) + " running · " + (desk.msg.length() ? desk.msg : desk.entry);
  sendJson(out);
}

// Remote setting: off -> on -> auto -> off. "on"/"off" pin it; auto follows you.
static void setRemoteSetting(uint8_t r) {
  conf.remote = r % 3;
  saveConfig();
  if (bridgeUp) bridgeSetRemote(conf.remote);   // the bridge decides what "auto" means; its answer comes with the next poll
  updateRemote();
  beep(remoteMode ? 2 : 0);
  touch();
}

// Hand text to a session: answers its Done/Question card if one is up, otherwise
// it's delivered after Claude's current step or when the turn ends.
static bool deliverText(Session* s, const String& text) {
  if (!s || !text.length()) return false;
  if (s->bridge) return bridgeSessionAct(s, "say", -1, text);
  for (auto& x : stops)
    if (x.used && x.result == -1 && x.session == s->id) { x.custom = text; x.result = -3; return true; }
  s->inject = text;
  return true;
}

// Answer a Done/Question card: here (the hook polls for it) or on the bridge it came from.
static bool stopResult(StopReq* r, int result) {
  if (r->bridge) { if (!bridgeStopResult(r, result)) return false; r->used = false; }
  else r->result = result;
  if (result == -2) tel.doneDismiss++; else tel.doneChoose++;
  return true;
}

static bool sessionStop(Session* s) {
  if (s->bridge) return bridgeSessionAct(s, "stop");
  s->stopFlag = true;
  return true;
}

// POST /act {action, id?, index?, session?, text?, on?}
static void hAct() {
  if (!authed()) return;
  JsonDocument in;
  if (server.arg("plain").length()) { if (!readBody(in)) return; }
  else {   // query-string form, used by ntfy notification buttons
    in["action"] = server.arg("action");
    if (server.arg("id").length()) in["id"] = server.arg("id").toInt();
    if (server.arg("session").length()) in["session"] = server.arg("session");
  }
  String action = in["action"] | "";
  uint32_t id = in["id"] | 0;
  int index = in["index"] | -1;
  String sid = in["session"] | "";
  bool ok = true;

  if (action != "remote" && action != "pin") noteStick();
  if (action == "approve" || action == "deny" || action == "keyboard" || action == "always") {
    Approval* a = nullptr;
    for (auto& x : approvals) if (x.used && x.id == id && x.decision == D_PENDING) a = &x;
    int8_t d = action == "approve" ? D_ALLOW : action == "always" ? D_ALWAYS : action == "deny" ? D_DENY : D_SKIP;
    if (a && d == D_DENY && in["text"].is<const char*>()) a->reason = in["text"].as<const char*>();
    if (a) decide(a, d); else ok = false;
  } else if (action == "choose" || action == "dismiss") {
    StopReq* r = nullptr;
    for (auto& x : stops) if (x.used && x.id == id && x.result == -1) r = &x;
    if (!r) ok = false;
    else if (action == "dismiss") ok = stopResult(r, -2);
    else if (index >= 0 && index < r->nMacros) ok = stopResult(r, index);
    else ok = false;
  } else if (action == "say" || action == "macro" || action == "stop") {
    Session* s = sid.length() ? findSession(sid, false) : targetSession();
    String text = action == "say" ? String((const char*)(in["text"] | "")) :
                  (action == "macro" && index >= 0 && index < nMacros) ? macros[index].text : String();
    if (!s) ok = false;
    else if (action == "stop") ok = sessionStop(s);
    else { if (action == "macro") tel.macroTaps++; ok = deliverText(s, text); }
  } else if (action == "pin") {
    pinned = sid.length() && findSession(sid, false) ? sid : String();
  } else if (action == "remote") {
    if (in["setting"].is<const char*>()) { String r = in["setting"].as<const char*>(); setRemoteSetting(r == "off" ? 0 : r == "on" ? 1 : 2); }
    else setRemoteSetting(conf.remote + 1);
  } else ok = false;

  JsonDocument out; out["ok"] = ok; sendJson(out, ok ? 200 : 404);
  dirty = true;
}

static void hHealth() {
  JsonDocument out;
  out["ok"] = true; out["fw"] = FW_VERSION; out["name"] = devName;
  out["id"] = devId; out["host"] = mdnsHost + ".local";
  out["hook"] = EMBED_HOOK_SHA;
  out["reset"] = resetWhy; out["safe_mode"] = safeMode;
  out["heap"] = ESP.getFreeHeap(); out["psram"] = ESP.getFreePsram();
  out["heap_min"] = ESP.getMinFreeHeap(); out["heap_block"] = (uint32_t)internalBlock();
  out["uptime_s"] = millis() / 1000;
  out["auth"] = token.length() > 0;
  out["bridge"] = bridgeConfigured() ? (bridgeUp ? "up" : "down") : "off";
  sendJson(out);
}

// GET /config -> settings for hooks (secrets included: the caller has the token)
// POST /config {partial settings} -> saved settings
static void hConfig() {
  if (!authed()) return;
  if (server.method() == HTTP_POST) {
    JsonDocument in; if (!readBody(in)) return;
    configFromJson(in);
    if (!saveConfig()) { server.send(507, "application/json", "{\"error\":\"settings too large\"}"); return; }
    dirty = true;
  }
  JsonDocument out; configToJson(out, true); sendJson(out);
}

// POST /ota (multipart file upload of the app image, X-Token header) -> flash + reboot
static bool otaAllowed = false;
static size_t otaBytes = 0;
static void hOtaDone() {
  if (!authed()) return;
  bool ok = otaAllowed && !Update.hasError() && Update.isFinished();
  server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"update failed\"}");
  if (ok) restartAt = millis() + 1500;
}
static void hOtaUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaAllowed = tokenEq(server.header("X-Token"), token) || tokenEq(server.arg("t"), token);
    otaBytes = 0;
    if (otaAllowed && !Update.begin(UPDATE_SIZE_UNKNOWN)) otaAllowed = false;
  } else if (up.status == UPLOAD_FILE_WRITE && otaAllowed) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) otaAllowed = false;
    otaBytes += up.currentSize;
    static uint32_t lastPaint = 0;
    if (millis() - lastPaint > 300) {   // progress on the Stick's screen
      lastPaint = millis();
      M5.Display.fillRect(0, 60, 240, 16, 0x0000);
      M5.Display.setTextColor(0xDBAA, 0x0000);
      M5.Display.drawString((String("updating... ") + String(otaBytes / 1024) + " KB").c_str(), 8, 64);
    }
  } else if (up.status == UPLOAD_FILE_END && otaAllowed) {
    if (!Update.end(true)) otaAllowed = false;
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); otaAllowed = false;
  }
}

static void registerSetupRoutes();

static void startHttp() {
  server.on("/config", HTTP_GET, hConfig);
  server.on("/config", HTTP_POST, hConfig);
  server.on("/ota", HTTP_POST, hOtaDone, hOtaUpload);
  const char* hdrs[] = { "X-Token" };
  server.collectHeaders(hdrs, 1);
  server.on("/", HTTP_GET, hRoot);
  server.on("/state", HTTP_GET, hState);
  server.on("/act", HTTP_POST, hAct);
  server.on("/health", HTTP_GET, hHealth);
  server.on("/event", HTTP_POST, hEvent);
  server.on("/usage", HTTP_POST, hUsage);
  server.on("/permission", HTTP_POST, hPermissionNew);
  server.on("/permission", HTTP_GET, hPermissionPoll);
  server.on("/permission", HTTP_DELETE, hPermissionPoll);
  server.on("/stop", HTTP_POST, hStopNew);
  server.on("/stop", HTTP_GET, hStopPoll);
  server.on("/stop", HTTP_DELETE, hStopPoll);
  registerSetupRoutes();
  server.begin();
}

static void expireStale() {
  uint32_t now = millis();
  for (auto& a : approvals)
    if (a.used && a.src == SRC_WIFI && !a.bridge && now - a.lastPoll > POLL_STALE_MS) { a.used = false; dirty = true; }
  for (auto& r : stops)   // (cards from a bridge come and go with its state, see bridgeApply)
    if (r.used && !r.bridge && now - r.lastPoll > POLL_STALE_MS) { r.used = false; dirty = true; }
  for (int i = 0; i < MAX_SESSIONS; i++)
    if (sessions[i].used && !sessions[i].bridge && now - sessions[i].lastSeen > 30UL * 60 * 1000) { sessions[i].used = false; dirty = true; }
  // A tie to a session that's gone (ended, expired, replaced) would make every other session's prompts skip.
  if (pinned.length() && !findSession(pinned, false)) { pinned = ""; dirty = true; }
}

// ----------------------------------------------------------- Wi-Fi/mDNS ---

// Known networks: home, a phone hotspot, ... The Stick joins the strongest one it can see.
// The first is the primary (also kept under "ssid"/"pass", where older firmware and the serial commands look).
static const int MAX_NETS = 4;
struct WifiNet { String ssid, pass; };
static WifiNet nets[MAX_NETS];
static int     nNets = 0, lastNet = 0;

static void saveNets() {
  JsonDocument d; JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < nNets; i++) { JsonObject o = a.add<JsonObject>(); o["s"] = nets[i].ssid; o["p"] = nets[i].pass; }
  String out; serializeJson(d, out);
  prefs.putString("nets", out);
  if (nNets) { prefs.putString("ssid", nets[0].ssid); prefs.putString("pass", nets[0].pass); }
  else { prefs.remove("ssid"); prefs.remove("pass"); }
}

static void loadNets() {
  nNets = 0;
  JsonDocument d;
  if (!deserializeJson(d, prefs.getString("nets", "")) && d.is<JsonArray>())
    for (JsonObject o : d.as<JsonArray>()) {
      String s = o["s"] | "";
      if (nNets < MAX_NETS && s.length()) { nets[nNets].ssid = s; nets[nNets].pass = (const char*)(o["p"] | ""); nNets++; }
    }
  if (!nNets && ssid.length()) { nets[0].ssid = ssid; nets[0].pass = pass; nNets = 1; }   // a single network saved by older firmware
  if (nNets) { ssid = nets[0].ssid; pass = nets[0].pass; }
}

// Remember a network (or update its password). `primary` also makes it the first choice.
static bool addNet(const String& s, const String& p, bool primary) {
  int at = -1;
  for (int i = 0; i < nNets; i++) if (nets[i].ssid == s) at = i;
  if (at < 0) {
    if (nNets >= MAX_NETS) { if (!primary) return false; at = nNets - 1; }   // full: a network you just set up replaces the last
    else at = nNets++;
  }
  nets[at].ssid = s; nets[at].pass = p;
  if (primary) { WifiNet n = nets[at]; for (int i = at; i > 0; i--) nets[i] = nets[i - 1]; nets[0] = n; }
  saveNets();
  return true;
}

static void forgetNet(const String& s) {
  for (int i = 0; i < nNets; i++)
    if (nets[i].ssid == s) { for (int j = i; j < nNets - 1; j++) nets[j] = nets[j + 1]; nNets--; break; }
  if (ssid == s) {   // forgetting the one we're aiming at: don't keep retrying it after a drop
    ssid = nNets ? nets[0].ssid : String(); pass = nNets ? nets[0].pass : String();
  }
  saveNets();
}

static void forgetAllNets() {
  nNets = 0; ssid = ""; pass = "";
  prefs.remove("nets"); prefs.remove("ssid"); prefs.remove("pass");
}

// The strongest known network among `n` scan results, or -1.
static int bestKnownNet(int n) {
  int best = -1, bestRssi = -1000;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    for (int j = 0; j < nNets; j++)
      if (s == nets[j].ssid && WiFi.RSSI(i) > bestRssi) { best = j; bestRssi = WiFi.RSSI(i); }
  }
  return best;
}

// At power-on with several networks known: look around once and start with the strongest.
static void pickNetAtBoot() {
  if (nNets < 2) return;
  int best = bestKnownNet(WiFi.scanNetworks());
  WiFi.scanDelete();
  if (best >= 0) { lastNet = best; ssid = nets[best].ssid; pass = nets[best].pass; }
}

static void wifiTick() {
  static uint32_t lastTry = 0, scanAt = 0;
  static bool scanning = false;
  if (!nNets) return;
  if (WiFi.status() == WL_CONNECTED) {
    scanning = false;
    if (!mdnsUp && MDNS.begin(mdnsHost.c_str())) {
      MDNS.addService("http", "tcp", HTTP_PORT);
      MDNS.addService("claudestick", "tcp", HTTP_PORT);   // hooks find the Stick again if its IP changes
      MDNS.addServiceTxt("claudestick", "tcp", "id", devId.c_str());
      mdnsUp = true; dirty = true;
    }
    return;
  }
  if (mdnsUp) { MDNS.end(); mdnsUp = false; dirty = true; }
  if (scanning) {   // choose among the known networks that are in range
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING && millis() - scanAt < 12000) return;
    scanning = false;
    int best = bestKnownNet(n);
    WiFi.scanDelete();
    if (best < 0) best = (lastNet + 1) % nNets;   // none in sight (or a hidden one): take turns
    lastNet = best; ssid = nets[best].ssid; pass = nets[best].pass;
    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), pass.c_str());
    lastTry = millis(); dirty = true;
    return;
  }
  if (millis() - lastTry > 15000) {
    lastTry = millis();
    if (nNets == 1) { WiFi.disconnect(); WiFi.begin(ssid.c_str(), pass.c_str()); }
    else { WiFi.scanNetworks(true); scanning = true; scanAt = millis(); }   // async: the screen keeps running
  }
}

// ---------------------------------------------------------- USB serial ---
// Two protocols share the USB serial port:
//  * Improv Wi-Fi Serial (binary packets starting "IMPROV"): used by the web
//    flasher to send Wi-Fi credentials right after flashing.
//  * Plain text commands, one per line:
//      wifi <ssid> <password> | token <value> | name <value> | tilt on|off|flip
//      info | setup (re-run first-time setup) | reboot | forget

static const uint8_t IMPROV_STATE_READY = 0x02, IMPROV_STATE_PROVISIONING = 0x03, IMPROV_STATE_PROVISIONED = 0x04;
static const uint8_t IMPROV_ERR_INVALID = 0x01, IMPROV_ERR_UNKNOWN_CMD = 0x02, IMPROV_ERR_CONNECT = 0x03;
static const uint8_t IMPROV_TYPE_STATE = 0x01, IMPROV_TYPE_ERROR = 0x02, IMPROV_TYPE_RPC = 0x03, IMPROV_TYPE_RESULT = 0x04;

static String setupUrl() {
  String u = "http://" + WiFi.localIP().toString() + ":" + String(HTTP_PORT) + "/setup";
  if (setupDone) u += "?t=" + token;
  return u;
}

static void improvSend(uint8_t type, const uint8_t* data, uint8_t len) {
  uint8_t hdr[9] = { 'I', 'M', 'P', 'R', 'O', 'V', 1, type, len };
  uint8_t sum = 0;
  for (uint8_t b : hdr) sum += b;
  for (int i = 0; i < len; i++) sum += data[i];
  Serial.write(hdr, 9);
  if (len) Serial.write(data, len);
  Serial.write(sum);
  Serial.write('\n');
}

static void improvState(uint8_t st) { improvSend(IMPROV_TYPE_STATE, &st, 1); }
static void improvError(uint8_t e) { improvSend(IMPROV_TYPE_ERROR, &e, 1); }

// RPC result: [command, total_len, (len, bytes)...]
static void improvResult(uint8_t cmd, const String* strs, int n) {
  uint8_t buf[256];
  int pos = 2;
  for (int i = 0; i < n; i++) {
    int l = min((int)strs[i].length(), 250 - pos);
    if (l < 0) break;
    buf[pos++] = (uint8_t)l;
    memcpy(buf + pos, strs[i].c_str(), l);
    pos += l;
  }
  buf[0] = cmd;
  buf[1] = (uint8_t)(pos - 2);
  improvSend(IMPROV_TYPE_RESULT, buf, (uint8_t)pos);
}

static bool connectWifi(const String& s, const String& p, uint32_t timeoutMs) {
  ssid = s; pass = p;
  addNet(s, p, true);
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  dirty = true;
  return WiFi.status() == WL_CONNECTED;
}

static void improvHandle(const uint8_t* d, int len) {
  if (len < 2 || d[1] != len - 2) { improvError(IMPROV_ERR_INVALID); return; }
  uint8_t cmd = d[0];
  if (cmd == 0x01) {                       // send Wi-Fi settings
    int sl = d[2];
    if (3 + sl >= len) { improvError(IMPROV_ERR_INVALID); return; }
    int pl = d[3 + sl];
    if (4 + sl + pl > len) { improvError(IMPROV_ERR_INVALID); return; }
    String s, p;
    for (int i = 0; i < sl; i++) s += (char)d[3 + i];
    for (int i = 0; i < pl; i++) p += (char)d[4 + sl + i];
    improvState(IMPROV_STATE_PROVISIONING);
    if (connectWifi(s, p, 20000)) {
      improvState(IMPROV_STATE_PROVISIONED);
      String u = setupUrl();
      improvResult(0x01, &u, 1);
    } else {
      improvError(IMPROV_ERR_CONNECT);
      improvState(IMPROV_STATE_READY);
    }
  } else if (cmd == 0x02) {                // current state
    bool up = WiFi.status() == WL_CONNECTED;
    improvState(up ? IMPROV_STATE_PROVISIONED : IMPROV_STATE_READY);
    if (up) { String u = setupUrl(); improvResult(0x02, &u, 1); }
  } else if (cmd == 0x03) {                // device info
    String info[4] = { "ClaudeStick", FW_VERSION, "ESP32-S3", devName };
    improvResult(0x03, info, 4);
  } else if (cmd == 0x04) {                // scan networks
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n && i < 20; i++) {
      String row[3] = { WiFi.SSID(i), String(WiFi.RSSI(i)), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES" };
      if (row[0].length()) improvResult(0x04, row, 3);
    }
    WiFi.scanDelete();
    improvResult(0x04, nullptr, 0);        // empty result = end of list
  } else {
    improvError(IMPROV_ERR_UNKNOWN_CMD);
  }
}

static void textCommand(String line) {
  line.trim();
  if (!line.length()) return;
  if (line.startsWith("wifi ")) {
    String rest = line.substring(5);
    int sp = rest.indexOf(' ');
    String s = sp < 0 ? rest : rest.substring(0, sp);
    String p = sp < 0 ? "" : rest.substring(sp + 1);
    Serial.printf(connectWifi(s, p, 15000) ? "ok wifi=%s ip=%s\n" : "saved wifi=%s (not connected yet)%s\n",
                  ssid.c_str(), WiFi.localIP().toString().c_str());
  } else if (line.startsWith("token ")) {
    token = line.substring(6); prefs.putString("token", token);
    Serial.println("ok token set");
  } else if (line.startsWith("name ")) {
    devName = line.substring(5); prefs.putString("name", devName);
    Serial.println("ok name set (reboot to re-advertise)");
  } else if (line == "info") {
    Serial.printf("ClaudeStick %s\nname=%s host=%s.local ip=%s port=%u\ntoken=%s\nwifi=%s (%s)\nble=%s setup=%s\n",
      FW_VERSION, devName.c_str(), mdnsHost.c_str(), WiFi.localIP().toString().c_str(), HTTP_PORT,
      token.c_str(), ssid.c_str(), WiFi.status() == WL_CONNECTED ? "connected" : "down",
      safeMode ? "off (safe mode)" : !btPref ? "off (setting)" : bleConnected() ? "connected" : "idle", setupDone ? "done" : "pending");
    Serial.printf("last reset: %s  heap %u  psram %u\n", resetWhy.c_str(), ESP.getFreeHeap(), ESP.getFreePsram());
  } else if (line.startsWith("tilt ")) {
    String m = line.substring(5);
    if (m == "off") tiltMode = 0;
    else if (m == "flip") tiltMode = tiltMode < 0 ? 1 : -1;
    else tiltMode = 1;
    prefs.putChar("tilt", tiltMode);
    Serial.printf("ok tilt=%d\n", tiltMode);
  } else if (line.startsWith("tz ")) {
    String t = line.substring(3); t.trim();
    if (!validTz(t)) { Serial.println("usage: tz <POSIX rule>, e.g. tz CST6CDT,M3.2.0,M11.1.0"); }
    else { tzPosix = t; tzName = ""; prefs.putString("tz", tzPosix); prefs.putString("tzname", tzName); applyTimezone(); Serial.printf("ok tz=%s now=%s\n", tzPosix.c_str(), clockStr().c_str()); }
  } else if (line.startsWith("font ")) {
    textStyle = line.substring(5).toInt() ? 1 : 0; prefs.putUChar("font", textStyle);
    Serial.printf("ok font=%s\n", textStyle ? "code" : "classic");
  } else if (line.startsWith("clock ")) {
    clock24 = line.substring(6).toInt() == 24; prefs.putBool("clk24", clock24);
    Serial.printf("ok clock=%s now=%s\n", clock24 ? "24h" : "12h", clockStr().c_str());
  } else if (line == "flip") {
    flipped = !flipped; prefs.putBool("flip", flipped); M5.Display.setRotation(flipped ? 3 : 1);
    Serial.printf("ok flip=%d\n", flipped);
  } else if (line == "setup") {
    setupDone = false; prefs.putBool("setup", false);
    Serial.println("ok, first-time setup re-opened");
  } else if (line == "bat") {
    float r = drainPerHour();
    Serial.printf("batt %d%% %.0f mV charging=%d drain=%s\n", M5.Power.getBatteryLevel(), (float)M5.Power.getBatteryVoltage(),
                  (int)M5.Power.isCharging() == 1, r > 0 ? (String(r, 1) + "%/h").c_str() : "measuring");
  } else if (line.startsWith("sleep ")) {
    String m = line.substring(6); m.trim();
    if (m != "max" && m != "min") Serial.println("sleep max|min (experimental: max lets the radio sleep longer between beacons)");
    else { radioSleep = m == "max"; prefs.putUChar("wps", radioSleep); applyRadioSleep(); Serial.printf("ok radio sleep %s\n", m.c_str()); }
  } else if (line == "reboot") {
    ESP.restart();
  } else if (line == "forget") {
    forgetAllNets(); prefs.putBool("setup", false); if (!btOff()) bleClearBonds();
    Serial.println("ok forgotten, rebooting"); delay(200); ESP.restart();
  } else {
    Serial.println("commands: wifi <ssid> <pass> | token <t> | name <n> | tilt on|off|flip | flip | tz <rule> | clock 12|24 | font 0|1 | bat | sleep max|min | info | setup | reboot | forget");
  }
  dirty = true;
}

static void serialTick() {
  static String line;
  static uint8_t pkt[300];
  static int pos = 0;            // bytes of a possible Improv packet collected so far
  static const char* MAGIC = "IMPROV";

  auto textByte = [&](uint8_t c) {
    if (c == '\r') return;
    if (c == '\n') { textCommand(line); line = ""; return; }
    if (line.length() < 200) line += (char)c;
  };

  while (Serial.available()) {
    uint8_t c = Serial.read();
    if (pos == 0 && !(c == 'I' && line.length() == 0)) { textByte(c); continue; }
    pkt[pos++] = c;
    bool bad = (pos <= 6 && c != (uint8_t)MAGIC[pos - 1]) || (pos == 7 && c != 1);
    if (bad) {                     // not Improv after all: replay as text
      for (int i = 0; i < pos; i++) textByte(pkt[i]);
      pos = 0;
      continue;
    }
    if (pos >= 9 && pos == 9 + pkt[8] + 1) {
      uint8_t sum = 0;
      for (int i = 0; i < pos - 1; i++) sum += pkt[i];
      if (sum != pkt[pos - 1]) improvError(IMPROV_ERR_INVALID);
      else if (pkt[7] == IMPROV_TYPE_RPC) improvHandle(pkt + 9, pkt[8]);
      pos = 0;
    }
    if (pos >= (int)sizeof(pkt)) pos = 0;
  }
}

// ------------------------------------------------- first-time setup ---
// Until setup is done, the Stick shows a "set me up" screen. Wi-Fi comes from
// the web flasher (Improv), the hotspot below, or the serial `wifi` command.
// The rest happens on /setup, served by the Stick.

// Port 80 on the hotspot only redirects to the real setup page on :7880, so the
// phone's "sign in to network" sheet opens it automatically.
static void portalRedirect() {
  portal.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + ":" + String(HTTP_PORT) + "/setup");
  portal.send(302, "text/plain", "");
}

static void startAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str());
  dns.start(53, "*", WiFi.softAPIP());
  portal.onNotFound(portalRedirect);
  portal.begin();
  apOn = true;
  dirty = true;
}

static void stopAP() {
  dns.stop();
  portal.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apOn = false;
  dirty = true;
}

static void apTick() {
  static uint32_t wifiUpSince = 0, wifiDownSince = 0;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up) { if (!wifiUpSince) wifiUpSince = millis(); wifiDownSince = 0; }
  else    { wifiUpSince = 0; if (!wifiDownSince) wifiDownSince = millis(); }
  if (apOn) { dns.processNextRequest(); portal.handleClient(); }
  // The hotspot closes once home Wi-Fi works and setup is finished (after a grace
  // period so the phone can load the last page), or after 5 min of working Wi-Fi.
  if (apOn && up && ((setupDone && millis() - doneAt > 20000) || millis() - wifiUpSince > 300000)) stopAP();
  // It opens when there's no Wi-Fi to join, or saved Wi-Fi keeps failing during setup.
  if (!apOn && !setupDone && (!ssid.length() || (wifiDownSince && millis() - wifiDownSince > 60000))) startAP();
  if (restartAt && millis() > restartAt) ESP.restart();
}

static bool setupAuthed() {
  if (!setupDone) return true;
  if (tokenEq(server.header("X-Token"), token) || tokenEq(server.arg("t"), token)) return true;
  server.send(401, "application/json", "{\"error\":\"setup is finished; open the link with ?t=<token>\"}");
  return false;
}

// The setup/settings page lives in firmware/setup_page.html (embedded at build time).

static void hSetupPage() {
  if (setupDone && server.arg("t") != token) {
    server.send(401, "text/html", "<p style='font-family:monospace'>Setup is finished. Open this page with <b>?t=&lt;token&gt;</b> (hold A on the Stick, then Info, to see it).</p>");
    return;
  }
  server.send_P(200, "text/html", EMBED_SETUP_HTML);
}

static void hSetupInfo() {
  if (!setupAuthed()) return;
  JsonDocument out;
  out["done"] = setupDone; out["ap"] = apOn; out["fw"] = FW_VERSION; out["port"] = HTTP_PORT;
  JsonObject w = out["wifi"].to<JsonObject>();
  w["ssid"] = ssid; w["connected"] = WiFi.status() == WL_CONNECTED; w["ip"] = WiFi.localIP().toString();
  JsonArray known = w["saved"].to<JsonArray>();
  for (int i = 0; i < nNets; i++) known.add(nets[i].ssid);
  JsonObject br = out["link"].to<JsonObject>();   // the bridge the Stick follows; its token is never sent back
  br["url"] = bridgeUrl; br["set"] = bridgeConfigured(); br["up"] = bridgeUp; br["error"] = bridgeUp ? "" : bridgeErr;
  out["name"] = devName; out["bt"] = devName; out["buddy"] = buddySpeciesIdx();
  out["bt_on"] = btPref;
  out["volume"] = volume; out["tilt"] = tiltMode != 0; out["remote"] = remoteName(conf.remote); out["flip"] = flipped;
  out["tz"] = tzPosix; out["tz_name"] = tzName; out["clock24"] = clock24; out["clock"] = clockStr(); out["font"] = textStyle;
  out["telemetry_available"] = conf.telemetryUrl.length() > 0; out["telemetry"] = conf.telemetryOn;
  JsonArray b = out["buddies"].to<JsonArray>();
  uint8_t cur = buddySpeciesIdx();
  for (uint8_t i = 0; i < buddySpeciesCount(); i++) { buddySetSpeciesIdx(i); b.add(buddySpeciesName()); }
  buddySetSpeciesIdx(cur);
  out["token"] = token;   // only reachable before setup is done, or with the token
  out["ip"] = WiFi.localIP().toString();
  sendJson(out);
}

static void hSetupScan() {
  if (!setupAuthed()) return;
  int n = WiFi.scanNetworks();
  JsonDocument out;
  JsonArray arr = out.to<JsonArray>();
  for (int i = 0; i < n && i < 25; i++) {
    if (!WiFi.SSID(i).length()) continue;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i); o["rssi"] = WiFi.RSSI(i); o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(out);
}

static void hSetupWifi() {
  if (!setupAuthed()) return;
  JsonDocument in; if (!readBody(in)) return;
  ssid = (const char*)(in["ssid"] | ""); pass = (const char*)(in["pass"] | "");
  if (ssid.length()) addNet(ssid, pass, true);
  server.send(200, "application/json", "{\"ok\":true}");
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pass.c_str());   // non-blocking; the page polls /setup/info
  dirty = true;
}

// POST /setup/wifi/add {ssid, pass}: remember another network (a phone hotspot, the office...) without leaving this one.
static void hSetupWifiAdd() {
  if (!setupAuthed()) return;
  JsonDocument in; if (!readBody(in)) return;
  String s = in["ssid"] | "", p = in["pass"] | "";
  if (!s.length() || s.length() > 32 || p.length() > 63) { server.send(400, "application/json", "{\"error\":\"need a network name (32 characters at most) and a password (63 at most)\"}"); return; }
  if (!addNet(s, p, false)) { server.send(409, "application/json", "{\"error\":\"four networks is the limit: forget one first\"}"); return; }
  server.send(200, "application/json", "{\"ok\":true}");
  dirty = true;
}

// POST /setup/wifi/forget {ssid}
static void hSetupWifiForget() {
  if (!setupAuthed()) return;
  JsonDocument in; if (!readBody(in)) return;
  forgetNet(in["ssid"] | "");
  server.send(200, "application/json", "{\"ok\":true}");
  dirty = true;
}

// POST /setup/bridge {url, token}: where your bridge is. It is tried before answering, so the page can say what's wrong.
// An empty url clears it; a blank token keeps the saved one.
static void hSetupBridge() {
  if (!setupAuthed()) return;
  JsonDocument in; if (!readBody(in)) return;
  String url = in["url"] | "", tok = in["token"] | "", codeIn = in["code"] | "";
  JsonDocument out;
  url.trim(); tok.trim();
  if (!url.length()) { bridgeSave("", ""); bridgeDrop(); out["ok"] = true; out["set"] = false; sendJson(out); return; }
  url = bridgeCleanUrl(url);
  if (!url.length()) { server.send(400, "application/json", "{\"error\":\"the address should look like https://stick.example.com\"}"); return; }
  String digits;
  for (size_t i = 0; i < codeIn.length(); i++) if (codeIn[i] >= '0' && codeIn[i] <= '9') digits += codeIn[i];
  if (codeIn.length() && digits.length() != 6) { server.send(400, "application/json", "{\"error\":\"a pairing code is 6 digits\"}"); return; }
  if (digits.length()) {   // a one-time pairing code stands in for the token: the bridge hands the real one back
    String oldUrl = bridgeUrl, oldTok = bridgeTok;
    bridgeUrl = url; bridgeTok = "";   // only for this one request: nothing is kept until the code is accepted
    bridgeTls.stop(); bridgePlain.stop();   // a fresh connection: the kept-alive one (if any) is to the old address
    JsonDocument pj; pj["code"] = digits;
    String pbody, presp; serializeJson(pj, pbody);
    int pc = WiFi.status() == WL_CONNECTED ? bridgeRequest("POST", "/pair", pbody, presp, 6000, "application/json", nullptr, 0, false) : -3;
    String pairErr = bridgeErr;
    bridgeTls.stop(); bridgePlain.stop();
    bridgeUrl = oldUrl; bridgeTok = oldTok;
    JsonDocument pr;
    if (pc == 200 && !deserializeJson(pr, presp)) tok = (const char*)(pr["token"] | "");
    if (!tok.length()) {   // the earlier bridge settings are untouched
      out["ok"] = false; out["set"] = bridgeConfigured();
      out["error"] = pc == 403 ? "that pairing code is wrong or has expired (they last 5 minutes and stop working after 3 wrong tries)" :
                     pc == 429 ? "too many wrong codes from this network: wait a few minutes" :
                     pc == 404 ? "that address answered, but it isn't a ClaudeStick bridge" :
                     pc > 0 ? "the bridge answered " + String(pc) : WiFi.status() != WL_CONNECTED ? "the Stick isn't on Wi-Fi yet" :
                     pairErr.length() ? pairErr : "could not reach it";
      sendJson(out);
      return;
    }
  }
  if (!tok.length() && url == bridgeUrl) tok = bridgeTok;
  if (!tok.length() || tok.length() > 64) { server.send(400, "application/json", "{\"error\":\"paste the bridge's token, or enter a pairing code\"}"); return; }
  bridgeDrop();
  bridgeSave(url, tok);
  String body;
  int code = WiFi.status() == WL_CONNECTED ? bridgeRequest("GET", "/state?n=1&v=0", "", body) : -3;
  JsonDocument d;
  bool good = code == 200 && !deserializeJson(d, body) && d["v"].is<uint32_t>();
  if (good) { bridgeUp = true; bridgeEverUp = true; bridgeFails = 0; bridgeApply(d); bridgeV = d["v"] | 0UL; bridgeNextPoll = millis() + BRIDGE_POLL_ACTIVE_MS; }
  out["ok"] = good; out["set"] = true;
  out["error"] = good ? "" : code == 401 ? "the bridge rejected that token" : code == 404 ? "that address answered, but it isn't a ClaudeStick bridge" :
                 code > 0 ? "the bridge answered " + String(code) : WiFi.status() != WL_CONNECTED ? "the Stick isn't on Wi-Fi yet" :
                 bridgeErr.length() ? bridgeErr : "could not reach it";
  sendJson(out);
}

static void hSetupPreview() {
  if (!setupAuthed()) return;
  JsonDocument in; if (!readBody(in)) return;
  buddySetSpeciesIdx(in["buddy"] | 0);
  if (in["font"].is<int>()) { textStyle = in["font"].as<int>() ? 1 : 0; spr.setFont(uiFont()); }   // live preview; /setup/save keeps it
  heartUntil = millis() + 1500;
  touch();
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /setup/time {tz?, tz_name?, clock24?, epoch?}: time zone and clock style.
// "epoch" (the browser's clock) is only used when the Stick can't get the time elsewhere.
static void hSetupTime() {
  if (!setupAuthed()) return;
  JsonDocument in; if (!readBody(in)) return;
  if (in["tz"].is<const char*>()) {
    String t = in["tz"].as<const char*>();
    if (t.length() && !validTz(t)) { server.send(400, "application/json", "{\"error\":\"bad time zone\"}"); return; }
    tzPosix = t;
    tzName = String((const char*)(in["tz_name"] | "")).substring(0, 40);
    prefs.putString("tz", tzPosix); prefs.putString("tzname", tzName);
    applyTimezone();
  }
  if (in["clock24"].is<bool>()) { clock24 = in["clock24"]; prefs.putBool("clk24", clock24); }
  long epoch = in["epoch"] | 0L;
  if (epoch > 1700000000L && !clockOk()) setClock(epoch, 0);
  JsonDocument out;
  out["ok"] = true; out["clock"] = clockStr(); out["tz"] = tzPosix; out["tz_name"] = tzName; out["clock24"] = clock24;
  sendJson(out);
  dirty = true;
}

static void hSetupSave() {
  if (!setupAuthed()) return;
  JsonDocument in; if (!readBody(in)) return;
  String name = in["name"] | devName.c_str();
  name.trim();
  if (!name.startsWith("Claude")) name = "Claude-" + name;   // desktop picker filters on this prefix
  if (name.length() > 24) name = name.substring(0, 24);
  bool restart = name != devName;
  devName = name; prefs.putString("name", devName);
  buddySetSpeciesIdx(in["buddy"] | (int)buddySpeciesIdx()); prefs.putUChar("buddy", buddySpeciesIdx());
  volume = in["volume"] | (int)volume; prefs.putUChar("vol", volume); M5.Speaker.setVolume(volume);
  tiltMode = (in["tilt"] | true) ? 1 : 0; prefs.putChar("tilt", tiltMode);
  if (in["font"].is<int>()) { textStyle = in["font"].as<int>() ? 1 : 0; prefs.putUChar("font", textStyle); }
  if (in["flip"].is<bool>()) { flipped = in["flip"]; prefs.putBool("flip", flipped); M5.Display.setRotation(flipped ? 3 : 1); }
  if (in["bt"].is<bool>() && in["bt"].as<bool>() != btPref) { btPref = in["bt"]; prefs.putBool("bt", btPref); restart = true; }   // Bluetooth starts at boot
  if (in["telemetry"].is<bool>()) conf.telemetryOn = in["telemetry"];
  if (in["remote"].is<const char*>()) { String rm = in["remote"].as<const char*>(); conf.remote = rm == "off" ? 0 : rm == "on" ? 1 : 2; }
  else if (in["remote"].is<bool>()) conf.remote = in["remote"].as<bool>() ? 1 : 2;
  saveConfig(); updateRemote();
  if (bridgeUp && in["remote"].is<const char*>()) bridgeSetRemote(conf.remote);   // the bridge owns remote mode while connected
  if (!setupDone) { setupDone = true; doneAt = millis(); prefs.putBool("setup", true); }
  JsonDocument out;
  out["ok"] = true; out["restart"] = restart; out["token"] = token; out["bt"] = devName;
  out["ip"] = WiFi.localIP().toString(); out["port"] = HTTP_PORT;
  sendJson(out);
  if (restart) restartAt = millis() + 2000;
  beep(2); heartUntil = millis() + 3000; screen = SCR_HOME; touch();
}

// The installer and hook are embedded at build time (embed_files.py) so a new
// machine needs only:  curl -s http://<stick>:7880/install.py | python3 - ...
static void hInstallPy() { server.send_P(200, "text/x-python", EMBED_INSTALL_PY); }
static void hHookPy()    { server.send_P(200, "text/x-python", EMBED_HOOK_PY); }

static void registerSetupRoutes() {
  server.on("/setup", HTTP_GET, hSetupPage);
  server.on("/setup/info", HTTP_GET, hSetupInfo);
  server.on("/setup/scan", HTTP_GET, hSetupScan);
  server.on("/setup/wifi", HTTP_POST, hSetupWifi);
  server.on("/setup/wifi/add", HTTP_POST, hSetupWifiAdd);
  server.on("/setup/wifi/forget", HTTP_POST, hSetupWifiForget);
  server.on("/setup/bridge", HTTP_POST, hSetupBridge);
  server.on("/setup/preview", HTTP_POST, hSetupPreview);
  server.on("/setup/save", HTTP_POST, hSetupSave);
  server.on("/setup/time", HTTP_POST, hSetupTime);
  server.on("/install.py", HTTP_GET, hInstallPy);
  server.on("/claudestick_hook.py", HTTP_GET, hHookPy);
  server.onNotFound([] { server.send(404, "application/json", "{\"error\":\"not found\"}"); });
}

#include "telemetry.inc"   // opt-in usage reporting (needs conf, tel, bridgeConfigured(), nNets above)
#include "voice.inc"

// ------------------------------------------------------------------ UI ----
// TUI look: one 6x8 monospace font, a thin frame with inset titles like a
// terminal pane, a left pane with the animated ASCII buddy and a right pane
// with the content. Bottom border carries the key hints.

static const uint16_t C_BG     = 0x0000;
static const uint16_t C_FG     = 0xD69A;   // light grey
static const uint16_t C_DIM    = 0x7BCF;   // mid grey
static const uint16_t C_FRAME  = 0x39E7;   // dark grey
static const uint16_t C_CLAUDE = 0xDBAA;   // Claude orange (#D97757)
static const uint16_t C_OK     = 0x4E89;   // green
static const uint16_t C_BAD    = 0xE945;   // red
static const uint16_t C_BLUE   = 0x5D3F;   // blue
static const uint16_t C_YEL    = 0xF6A8;   // amber
static const uint16_t C_BADDIM = 0xA0E3;   // dark red (danger border)

static const int W = 240, H = 135;
// Two layouts. Normal: full-width content with a small buddy face in the top-right corner.
// Pet: the big species buddy in a left pane (idle home, buddy picker, first-time setup).
static const int LEFT_W = 86;                 // pet layout: buddy pane width
static const int CORNER_X = 203, CORNER_Y = 12;   // normal layout: mini face
static const int CORNER_ROWS_Y = 28;          // rows above this y share their line with the face
static const int NARROW_CHARS = 31;           // characters that fit left of the face
static bool petLayout = false;
static int  PANE_X = 7, PANE_W = 229, PANE_CHARS = 38;   // content box, set by layout()
static void layout(bool pet) {
  petLayout = pet;
  PANE_X = pet ? LEFT_W + 4 : 7;
  PANE_W = W - PANE_X - 4;
  PANE_CHARS = PANE_W / 6;
}
// Right edge for text on a row at y (stays clear of the corner face).
static int rightX(int y) { return (petLayout || y >= CORNER_ROWS_Y) ? W - 6 : PANE_X + NARROW_CHARS * 6; }
static int charsAt(int y) { return (rightX(y) - PANE_X) / 6; }
static const int TOP = 11, BOTTOM = 123, LINE_H = 9;
static const int MAX_ITEMS = MAX_CHOICES + 12;
static const int MAX_WRAP = 64;

static void txt(const String& s, int x, int y, uint16_t fg, uint16_t bg = C_BG) {
  spr.setTextColor(fg, bg);
  spr.drawString(s, x, y);
}

static String fit(const String& s, int chars) {
  if ((int)s.length() <= chars) return s;
  return chars > 1 ? s.substring(0, chars - 1) + "~" : s.substring(0, chars);
}

// Word-wrap into fixed-width lines.
// Word-wrap, keeping explicit line breaks. Lines starting "+ " / "- " (diffs from
// the hook) are coloured green / red, including their wrapped continuations.
static int wrapLines(const String& text, int width, String* out, int maxOut,
                     uint16_t* colors = nullptr, uint16_t base = 0) {
  int n = 0, start = 0;
  while (start <= (int)text.length() && n < maxOut) {
    int nl = text.indexOf('\n', start);
    String para = text.substring(start, nl < 0 ? text.length() : nl);
    start = nl < 0 ? text.length() + 1 : nl + 1;
    uint16_t c = para.startsWith("+ ") ? 0x4E89 : para.startsWith("- ") ? 0xE945 : base;
    para.trim();
    if (!para.length()) { if (nl >= 0 && n > 0) { if (colors) colors[n] = c; out[n++] = ""; } continue; }
    while (para.length() && n < maxOut) {
      String seg;
      if ((int)para.length() <= width) { seg = para; para = ""; }
      else {
        int cut = para.lastIndexOf(' ', width);
        if (cut < width / 2) {   // long token: prefer to break after punctuation, else hard cut
          cut = width;
          for (int i = width; i > width / 2; i--)
            if (strchr("/_-=,:;.&|", para[i - 1])) { cut = i; break; }
        }
        seg = para.substring(0, cut);
        para = para.substring(cut); para.trim();
      }
      if (colors) colors[n] = c;
      out[n++] = seg;
    }
  }
  while (n > 0 && !out[n - 1].length()) n--;
  return n;
}

// ---- Wi-Fi signal: sampled once a second and smoothed, bars change with 2 dB of hysteresis

static float wifiRssiAvg() {
  static float avg = 0;
  static uint32_t at = 0;
  if (WiFi.status() != WL_CONNECTED) { avg = 0; return 0; }
  if (!avg || millis() - at >= 1000) {
    at = millis();
    int r = WiFi.RSSI();
    if (r < 0 && r > -127) avg = avg ? avg * 0.7f + r * 0.3f : (float)r;   // 0 / out-of-range = no reading, keep last
  }
  return avg;
}

// 0 = not connected, else 1..4: >= -55 dBm 4 bars, >= -65 3, >= -75 2, weaker 1.
static int wifiBars() {
  static int cur = 0;
  float r = wifiRssiAvg();
  if (r == 0) return cur = 0;
  static const int T[3] = { -75, -65, -55 };   // threshold for 2, 3 and 4 bars
  int want = 1;
  for (int i = 0; i < 3; i++) if (r >= T[i]) want = i + 2;
  if (!cur) cur = want;
  else if (want > cur && r >= T[cur - 1] + 2) cur++;
  else if (want < cur && r <= T[cur - 2] - 2) cur--;
  return cur;
}

// ---- status bar icons: drawn from primitives, so they don't depend on font glyphs

static void iconWifi(int x, int y, int lit, float rssi) {      // 11 x 8
  uint16_t on = lit == 1 ? (rssi < -82 ? C_BAD : C_YEL) : C_FG;
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    spr.fillRect(x + i * 3, y + 8 - h, 2, h, i < lit ? on : C_FRAME);
  }
}

static void iconBitmap(int x, int y, const uint8_t* rows, int n, int wBits, uint16_t c) {
  for (int r = 0; r < n; r++)
    for (int b = 0; b < wBits; b++)
      if (rows[r] & (1 << (wBits - 1 - b))) spr.drawPixel(x + b, y + r, c);
}

static void iconBluetooth(int x, int y, bool connected) {    // 5 x 9
  static const uint8_t rune[9] = { 0x04, 0x06, 0x15, 0x0E, 0x04, 0x0E, 0x15, 0x06, 0x04 };
  iconBitmap(x, y, rune, 9, 5, connected ? C_BLUE : C_FRAME);
}

static void iconBattery(int x, int y, int pct, bool charging) {   // 18 x 8
  uint16_t c = charging ? C_OK : pct > 30 ? C_FG : pct > 15 ? C_YEL : C_BAD;
  spr.drawRect(x, y, 16, 8, C_DIM);
  spr.fillRect(x + 16, y + 2, 2, 4, C_DIM);
  int w = max(0, min(14, 14 * pct / 100));
  if (pct > 0) spr.fillRect(x + 1, y + 1, max(1, w), 6, c);
}

// Right end of the top border: wifi bars, bluetooth, battery, clock. Returns its width.
static int statusBar(bool paint) {
  int pct = M5.Power.getBatteryLevel();
  bool chg = (int)M5.Power.isCharging() == 1;
  String pctS = String(pct), clk = clockStr();
  int x = W - 5;
  auto put = [&](int w) { x -= w; return x; };
  if (clk.length()) { int w = spr.textWidth(clk); int px = put(w); if (paint) txt(clk, px, 0, C_DIM); put(5); }
  { int w = spr.textWidth(pctS); int px = put(w); if (paint) txt(pctS, px, 0, chg ? C_OK : pct > 15 ? C_DIM : C_BAD); put(3); }
  { int px = put(18); if (paint) iconBattery(px, 0, pct, chg); }
  if (chg) { put(3); int px = put(5); if (paint) { static const uint8_t bolt[5] = { 0x06, 0x0C, 0x0E, 0x06, 0x0C }; iconBitmap(px, 1, bolt, 5, 5, C_YEL); } }
  put(4);
  if (!btOff()) { int px = put(5); if (paint) iconBluetooth(px, 0, bleConnected()); put(4); }
  { int px = put(11); if (paint) iconWifi(px, 0, wifiBars(), wifiRssiAvg()); }
  if (bridgeConfigured()) { put(4); int px = put(7); if (paint) iconBridge(px, 2, bridgeUp ? C_OK : bridgeFails ? C_BAD : C_DIM); }
  return W - 5 - x;
}

// Text where [KEY] segments are drawn as inverse key caps. Returns the width in px.
static int richText(const String& s, int x, int y, uint16_t fg, bool paint = true) {
  int x0 = x, i = 0, n = s.length();
  while (i < n) {
    if (s[i] == '[') {
      int j = s.indexOf(']', i);
      if (j > i) {
        String k = s.substring(i + 1, j);
        int w = spr.textWidth(k) + 3;
        if (paint) { spr.fillRect(x, y - 1, w, 9, C_DIM); txt(k, x + 2, y, C_BG, C_DIM); }
        x += w + 3; i = j + 1;
        continue;
      }
    }
    int j = s.indexOf('[', i + 1);
    if (j < 0) j = n;
    String seg = s.substring(i, j);
    if (paint) txt(seg, x, y, fg);
    x += spr.textWidth(seg); i = j;
  }
  return x - x0;
}

// Frame with a title tab on the top border and key hints (as key caps) on the bottom border.
static void frame(const String& title, uint16_t accent, const String& hints, uint16_t border = C_FRAME) {
  spr.drawRoundRect(0, 4, W, H - 4, 3, border);
  if (petLayout) spr.drawFastVLine(LEFT_W, 5, H - 7, border);
  txt(" " + title + " ", 6, 0, accent);
  int sw = statusBar(false);
  spr.fillRect(W - 5 - sw - 3, 0, sw + 6, 9, C_BG);
  statusBar(true);
  if (hints.length()) {
    int w = richText(hints, 0, 0, C_DIM, false);
    spr.fillRect(5, H - 9, w + 8, 9, C_BG);
    richText(hints, 8, H - 8, C_DIM);
  }
}

// ---- tiny 3x5 text (mood word in the corner)
#include "tinyfont.h"
static int tinyWidth(const String& t) { return t.length() * 4; }
static int drawTiny(const String& t, int x, int y, uint16_t c) {
  int x0 = x;
  for (size_t i = 0; i < t.length(); i++) {
    int ch = (unsigned char)t[i];
    if (ch >= 0x20 && ch <= 0x7E) {
      const uint8_t* g = tinyfont_rows[ch - 0x20];
      for (int r = 0; r < 7; r++)
        for (int b = 0; b < 4; b++)
          if (g[r] & (8 >> b)) spr.drawPixel(x + b, y + r, c);
    }
    x += 4;
  }
  return x - x0;
}

// Normal layout: a small face in the top-right corner with a tiny mood word under it.
static void buddyCorner(uint8_t state, const char* mood, uint16_t moodColor) {
  uint32_t t = millis() / 200;
  const char* face = "(o.o)"; uint16_t c = C_CLAUDE; bool sparkle = false, heart = false;
  switch (state) {
    case B_SLEEP:     face = "(-.-)"; c = C_DIM; break;
    case B_BUSY:      face = (t % 4 < 2) ? "(v.v)" : "(v_v)"; c = C_BLUE; break;
    case B_ATTENTION: { uint32_t k = t % 6; face = k < 2 ? "(O.O)" : k < 4 ? "(o.O)" : "(O.o)"; } break;
    case B_CELEBRATE: face = (t % 4 < 2) ? "(^o^)" : "(^O^)"; c = C_OK; sparkle = true; break;
    case B_DIZZY:     face = (t % 4 < 2) ? "(@.@)" : "(@_@)"; c = C_BAD; break;
    case B_HEART:     face = "(^.^)"; c = C_BAD; heart = true; break;
    default:          face = (millis() % 4200 < 180) ? "(-.-)" : "(o.o)"; break;
  }
  txt(face, CORNER_X, CORNER_Y, c);
  if (heart) {
    static const uint8_t H5[4] = { 0x0A, 0x1F, 0x0E, 0x04 };
    for (int r = 0; r < 4; r++) for (int b = 0; b < 5; b++) if (H5[r] & (16 >> b)) spr.drawPixel(CORNER_X - 8 + b, CORNER_Y + 1 + r, C_BAD);
  }
  if (sparkle && (t % 2)) {
    spr.drawPixel(CORNER_X - 6, CORNER_Y + 2, C_YEL); spr.drawPixel(CORNER_X - 7, CORNER_Y + 3, C_YEL); spr.drawPixel(CORNER_X - 5, CORNER_Y + 3, C_YEL);
    spr.drawPixel(CORNER_X - 6, CORNER_Y + 4, C_YEL);
    spr.drawPixel(CORNER_X + 33, CORNER_Y, C_YEL); spr.drawPixel(CORNER_X + 32, CORNER_Y + 1, C_YEL); spr.drawPixel(CORNER_X + 34, CORNER_Y + 1, C_YEL);
  }
  String m = mood;
  drawTiny(m, W - 6 - tinyWidth(m), CORNER_Y + 9, moodColor);
}

// Pet layout: the species buddy in the left pane, its name, and a one-word mood.
static void buddyPane(uint8_t state, const char* mood, uint16_t moodColor) {
  spr.setFont(&fonts::Font0);     // the buddy art was drawn for the classic glyphs
  buddyDraw(state);
  spr.setFont(uiFont());
  String name = buddySpeciesName();
  txt(name, LEFT_W / 2 - spr.textWidth(name) / 2, 96, C_DIM);
  String m = mood;
  txt(m, LEFT_W / 2 - spr.textWidth(m) / 2, 107, moodColor);
}

// Scrollable text block in the right pane. Returns lines drawn.
static int textBlock(const String& text, int y, int maxLines, uint16_t color, bool scrollable, const char* lead = nullptr) {
  static String lines[MAX_WRAP];
  static uint16_t colors[MAX_WRAP];
  int indent = lead ? 2 : 0;   // shell-style hanging indent after a "$" prompt
  int n = wrapLines(text, charsAt(y) - indent, lines, MAX_WRAP, colors, color);
  int first = scrollable ? max(0, min(scrollLine, n - maxLines)) : 0;
  if (scrollable) scrollLine = first;
  int shown = 0;
  for (int i = first; i < n && shown < maxLines; i++, shown++) {
    txt(lines[i], PANE_X + indent * 6, y + shown * LINE_H, colors[i]);
    if (lead && i == 0) txt(lead, PANE_X, y + shown * LINE_H, C_OK);
  }
  if (scrollable && n > maxLines) {   // scrollbar
    int trackY = y, trackH = maxLines * LINE_H - 2;
    int barH = max(4, trackH * maxLines / n);
    int barY = trackY + (trackH - barH) * first / max(1, n - maxLines);
    spr.drawFastVLine(W - 3, trackY, trackH, C_FRAME);
    spr.fillRect(W - 4, barY, 3, barH, C_CLAUDE);
  }
  return shown;
}

// Horizontal rule that stops short of the corner face on the top rows.
static void hrule(int y, uint16_t c = C_FRAME) { spr.drawFastHLine(PANE_X, y, rightX(y) - PANE_X - 2, c); }

// Selectable list in the right pane starting at y.
static void listBlock(const String* labels, int n, int y, uint16_t accent, const String* marks = nullptr) {
  int rows = max(1, (BOTTOM - y) / 11);
  int start = max(0, min(menuIdx - rows + 1, n - rows));
  for (int r = 0; r < rows && start + r < n; r++) {
    int i = start + r;
    bool sel = i == menuIdx;
    int yy = y + r * 11;
    String mark = marks ? marks[i] : String(" ");
    String label = fit(labels[i], PANE_CHARS - 2);
    if (sel) {
      spr.fillRect(PANE_X - 2, yy - 1, PANE_W + 2, 10, accent);
      txt(">" + mark + label, PANE_X, yy, C_BG, accent);
    } else {
      txt(" " + mark + label, PANE_X, yy, C_FG);
    }
  }
  if (start > 0) txt("^", W - 10, y, C_DIM);
  if (start + rows < n) txt("v", W - 10, y + (rows - 1) * 11, C_DIM);
}

static String sessionLabel(const Session& s) {
  String label = s.project;
  for (auto& o : sessions)   // disambiguate two sessions in the same project
    if (o.used && &o != &s && o.project == s.project) { label += "#" + s.id.substring(0, 4); break; }
  return label;
}

static uint16_t stateColor(const String& st) {
  if (st == "done") return C_OK;
  if (st == "approve" || st == "waiting" || st == "question") return C_CLAUDE;
  if (st == "error") return C_BAD;
  if (st == "tool" || st == "working") return C_BLUE;
  return C_DIM;
}

// What the buddy should be doing right now.
static uint8_t buddyState(const char** mood, uint16_t* moodColor) {
  uint32_t now = millis();
  *moodColor = C_DIM;
  if (dimmed) { *mood = "zzz"; return B_SLEEP; }
  if (!setupDone && now >= heartUntil) { *mood = "hi there!"; *moodColor = C_CLAUDE; return B_IDLE; }
  if (now < heartUntil) { *mood = "thanks!"; *moodColor = C_BAD; return B_HEART; }
  if (topApproval()) { *mood = "needs you"; *moodColor = C_CLAUDE; return B_ATTENTION; }
  if (StopReq* r = topStop()) {
    *moodColor = C_CLAUDE;
    if (r->kind == "question") { *mood = "question"; return B_ATTENTION; }
    *mood = "done!"; *moodColor = C_OK; return B_CELEBRATE;
  }
  if (screen == SCR_HOME && updatePromptDue()) { *mood = "new stuff!"; *moodColor = C_OK; return B_CELEBRATE; }
  Session* t = targetSession();
  if (t) {
    if (t->state == "error" && now - t->stateSince < 6000) { *mood = "oops"; *moodColor = C_BAD; return B_DIZZY; }
    if (t->state == "waiting") { *mood = "waiting"; *moodColor = C_CLAUDE; return B_ATTENTION; }
    if (t->state == "done" && now - t->stateSince < 10000) { *mood = "done!"; *moodColor = C_OK; return B_CELEBRATE; }
    if (t->state == "tool" || t->state == "working" || t->state == "error") { *mood = "working"; *moodColor = C_BLUE; return B_BUSY; }
  }
  if (desk.lastBeat && desk.waiting) { *mood = "needs you"; *moodColor = C_CLAUDE; return B_ATTENTION; }
  if (desk.lastBeat && desk.running) { *mood = "working"; *moodColor = C_BLUE; return B_BUSY; }
  if (!t && !desk.lastBeat && now - lastActivity > 5UL * 60 * 1000) { *mood = "zzz"; return B_SLEEP; }
  *mood = "idle";
  return B_IDLE;
}

static String titleBase() {
  if (safeMode) return "claudestick:safe";
  return remoteMode ? "claudestick:remote" : "claudestick";
}

// ---- screens

static uint32_t holdHintUntil = 0;   // "double-press A" nudge on risky cards / "needs a hold" in the menu

// 1234 -> "1234", 12345 -> "12.3k", 1234567 -> "1.23M"
static String fmtTok(uint32_t n) {
  char b[12];
  if (n < 10000) snprintf(b, sizeof b, "%lu", (unsigned long)n);
  else if (n < 100000) snprintf(b, sizeof b, "%.1fk", n / 1000.0);
  else if (n < 1000000) snprintf(b, sizeof b, "%luk", (unsigned long)(n / 1000));
  else if (n < 100000000) snprintf(b, sizeof b, "%.2fM", n / 1000000.0);
  else snprintf(b, sizeof b, "%luM", (unsigned long)(n / 1000000));
  return b;
}

static String fmtReset(uint32_t resets) {
  if (!resets || !clockOk()) return "";
  long d = (long)resets - (long)time(nullptr);
  if (d <= 0) return "now";
  if (d < 3600) return String(d / 60) + "m";
  if (d < 86400) return String(d / 3600) + "h" + String((d % 3600) / 60) + "m";
  return String(d / 86400) + "d" + String((d % 86400) / 3600) + "h";
}
// -1 = unknown. A window that has already reset is back at 0.
static float limitPct(const Limit& l) {
  if (l.pct < 0) return -1;
  if (l.resets && clockOk() && (long)time(nullptr) >= (long)l.resets) return 0;
  return l.pct;
}
static bool limitsStale() { return !lim.set || millis() - lim.at > 15UL * 60 * 1000; }
static uint16_t pctColor(float p) { return p >= 90 ? C_BAD : p >= 70 ? C_YEL : C_OK; }
static bool haveUsage() {
  Session* t = targetSession();
  return limitPct(lim.five) >= 0 || limitPct(lim.seven) >= 0 || tokensToday() || (t && (t->ctxPct >= 0 || t->hasUsage));
}

// One line along the bottom: context of the focused session, then the 5-hour and 7-day plan limits.
static void usageStrip(int y) {
  Session* t = targetSession();
  bool stale = limitsStale();
  int x = PANE_X;
  auto seg = [&](const char* label, float pct, bool dimIt) {
    String p = String((int)(pct + 0.5f)) + "%";
    txt(label, x, y, C_DIM); x += spr.textWidth(label) + 3;
    spr.drawRect(x, y + 1, 22, 6, C_FRAME);
    int w = (int)(20 * max(0.0f, min(100.0f, pct)) / 100.0f + 0.5f);
    if (w > 0) spr.fillRect(x + 1, y + 2, w, 4, dimIt ? C_DIM : pctColor(pct));
    x += 25;
    txt(p, x, y, dimIt ? C_DIM : C_FG); x += spr.textWidth(p) + 8;
  };
  if (t && t->ctxPct >= 0) seg("ctx", t->ctxPct, false);
  else if (t && t->hasUsage) { String c = "ctx " + fmtTok(t->ctx); txt(c, x, y, C_DIM); x += spr.textWidth(c) + 8; }
  float f = limitPct(lim.five), sv = limitPct(lim.seven);
  if (f >= 0) seg("5h", f, stale);
  if (sv >= 0) seg("7d", sv, stale);
  if (f < 0 && sv < 0 && tokensToday()) txt(fmtTok(tokensToday()) + " today", x, y, C_DIM);
}

static void drawApproval(Approval* a) {
  bool risky = a->risk == "high";
  String hint = risky ? "[2xA] allow  [B] deny" : a->always.length() ? "[A] ok  [2xA] always  [B] no" : "[A] allow  [B] deny";
  frame(risky ? "!! danger !!" : a->src == SRC_BLE ? "approve? desktop" : "approve?", risky ? C_BAD : C_CLAUDE, hint,
        risky ? C_BADDIM : C_FRAME);
  int y = TOP + 2;
  String tool = fit(a->tool, 12);
  txt(tool, PANE_X, y, risky ? C_BAD : C_CLAUDE);
  String proj = fit(a->project, charsAt(y) - (int)tool.length() - 1);
  txt(proj, rightX(y) - spr.textWidth(proj), y, C_DIM); y += LINE_H;
  if (risky && a->riskWhy.length()) { txt(fit(a->riskWhy, charsAt(y)), PANE_X, y, C_BAD); y += LINE_H; }
  hrule(y, risky ? C_BADDIM : C_FRAME);
  y = max(y + 4, petLayout ? 0 : CORNER_ROWS_Y);
  int rows = (BOTTOM - y) / LINE_H - (a->src == SRC_WIFI ? 1 : 0);
  textBlock(a->detail, y, rows, C_FG, true, a->tool == "Bash" ? "$" : nullptr);
  String foot = millis() < holdHintUntil ? "double-press A to allow" :
                a->always.length() ? "2xA: " + a->always : a->src == SRC_WIFI ? "hold B: at keyboard" : "";
  if (foot.length()) txt(fit(foot, PANE_CHARS - 4), PANE_X, BOTTOM - LINE_H + 2, millis() < holdHintUntil ? C_YEL : C_DIM);
  int pending = 0;
  for (auto& x : approvals) if (x.used && x.decision == D_PENDING) pending++;
  if (pending > 1) { String q = " +" + String(pending - 1) + " "; txt(q, W - 6 - spr.textWidth(q), H - 8, C_YEL); }
}

static int choiceLabels(StopReq* r, String* out) {
  int n = 0;
  for (int i = 0; i < r->nMacros; i++) out[n++] = r->macros[i].label;
  out[n++] = r->kind == "question" ? "answer at keyboard" : "dismiss";
  return n;
}

static void drawChoice(StopReq* r) {
  bool q = r->kind == "question";
  frame(q ? "question" : "done > next?", q ? C_CLAUDE : C_OK, "[B] next  [A] pick  [hold A] talk");
  txt("@" + fit(r->project, charsAt(TOP + 2) - 1), PANE_X, TOP + 2, C_DIM);
  int used = textBlock(q ? r->title : r->last, TOP + 2 + LINE_H, q ? 4 : 3, q ? C_FG : C_DIM, true);
  int y = max(TOP + 6 + (used + 1) * LINE_H, petLayout ? 0 : CORNER_ROWS_Y + 4);
  hrule(y - 4);
  String labels[MAX_ITEMS];
  int n = choiceLabels(r, labels);
  listBlock(labels, n, y, q ? C_CLAUDE : C_OK);
}

enum MenuItem : uint8_t { MI_STOP, MI_MACRO, MI_REMOTE, MI_TIE, MI_BUDDY, MI_FONT, MI_CLOCK, MI_UPDATE, MI_FLIP, MI_TEST, MI_HELP, MI_INFO, MI_BT, MI_TOKEN, MI_SETUP, MI_BACK };
struct MenuEntry { MenuItem kind; int arg; };

static int menuEntries(MenuEntry* e, String* labels) {
  int n = 0;
  auto add = [&](MenuItem k, int arg, const String& l) { e[n] = { k, arg }; labels[n] = l; n++; };
  add(MI_STOP, 0, "stop now");
  { Session* ts = targetSession(); bool br = ts && ts->bridge;   // a bridge session uses the bridge's macros
    for (int i = 0, m = br ? nBridgeMacro : nMacros; i < m; i++) add(MI_MACRO, i, "> " + (br ? bridgeMacro[i] : macros[i].label)); }
  add(MI_REMOTE, 0, String("remote: ") + remoteName(conf.remote) + (conf.remote == 2 ? (remoteMode ? " (on)" : " (off)") : ""));
  add(MI_TIE, 0, pinned.length() ? "tie to session (tied)" : "tie to session");
  add(MI_BUDDY, 0, String("buddy: ") + buddySpeciesName());
  add(MI_FONT, 0, String("text: ") + (textStyle ? "code" : "classic"));
  add(MI_CLOCK, 0, String("clock: ") + (clock24 ? "24-hour" : "12-hour"));
  add(MI_UPDATE, 0, upd.available ? "update to v" + upd.version : String("check for updates"));
  add(MI_FLIP, 0, "flip screen");
  add(MI_TEST, 0, "hardware test");
  add(MI_HELP, 0, "button help");
  add(MI_INFO, 0, "info");
  add(MI_BT, 0, String("bluetooth: ") + (btPref ? "on" : "off") + " (restarts)");
  add(MI_TOKEN, 0, "new token (hold A)");
  add(MI_SETUP, 0, "re-run setup");
  add(MI_BACK, 0, "back");
  return n;
}

static void drawActions() {
  Session* s = targetSession();
  frame("menu", C_CLAUDE, "[B] next  [P] up  [A] ok");
  txt(s ? ("-> " + fit(sessionLabel(*s), charsAt(TOP + 2) - 3)) : String("no cli session"), PANE_X, TOP + 2, C_DIM);
  String labels[MAX_ITEMS]; MenuEntry ents[MAX_ITEMS];
  int n = menuEntries(ents, labels);
  listBlock(labels, n, max(TOP + 4 + LINE_H, petLayout ? 0 : CORNER_ROWS_Y), C_CLAUDE);
}

static void drawHelp() {
  frame("buttons", C_BLUE, "[any key] back");
  int y = TOP + 3;
  auto line = [&](const String& keys, uint16_t c = C_FG) { richText(keys, PANE_X, y + 1, c); y += LINE_H + 2; };
  line("[A] allow  [B] deny");
  line("[2xA] always allow");
  line("[hold A] deny + why");
  line("[hold B] at keyboard");
  y += 3;
  line("[B] next  [A] pick");
  line("[P] up  [hold B] back");
  y += 3;
  line("[A] details  [B] menu");
  line("[hold A] talk", C_CLAUDE);
}

// Hardware test: live readings to check a new Stick in a minute.
static void drawTest() {
  frame("hardware test", C_BLUE, "[A] beep  [hold B] back");
  int y = TOP + 2;
  auto line = [&](const String& s, uint16_t c) { txt(fit(s, PANE_CHARS), PANE_X, y, c); y += LINE_H; };
  bool a = M5.BtnA.isPressed(), b = M5.BtnB.isPressed(), p = M5.BtnPWR.isPressed();
  txt("A", PANE_X, y, a ? C_BG : C_FG, a ? C_OK : C_BG);
  txt("B", PANE_X + 18, y, b ? C_BG : C_FG, b ? C_OK : C_BG);
  txt("PWR", PANE_X + 36, y, p ? C_BG : C_FG, p ? C_OK : C_BG);
  txt("<- press", PANE_X + 64, y, C_DIM); y += LINE_H + 2;
  float ax = 0, ay = 0, az = 0;
  bool imu = M5.Imu.getAccel(&ax, &ay, &az);
  line(imu ? "accel " + String(ax, 2) + " " + String(ay, 2) + " " + String(az, 2) : String("imu: no data"), imu ? C_FG : C_BAD);
  line("batt " + String(M5.Power.getBatteryLevel()) + "% " + String(M5.Power.getBatteryVoltage()) + "mV" +
       ((int)M5.Power.isCharging() == 1 ? " chg" : ""), C_FG);
  line(WiFi.status() == WL_CONNECTED ? "wifi " + String(WiFi.RSSI()) + " dBm" : String("wifi down"),
       WiFi.status() == WL_CONNECTED ? C_OK : C_BAD);
  line(String("bt ") + (safeMode ? "off (safe mode)" : !btPref ? "off (setting)" : bleConnected() ? "connected" : "advertising"), safeMode ? C_YEL : C_FG);
  line("heap " + String(ESP.getFreeHeap() / 1024) + "K psram " + String(ESP.getFreePsram() / 1024) + "K", C_FG);
  line("last reset: " + resetWhy, C_DIM);
  line("fw " + String(FW_VERSION) + (flipped ? " (flipped)" : ""), C_DIM);
}

// Update prompt + progress + one-off notices
static int updateLabels(String* out) {
  out[0] = "update now"; out[1] = "remind me later"; out[2] = "skip this version";
  return 3;
}

static void drawUpdatePrompt() {
  frame("update available!", C_OK, "[B] next  [A] pick  [hold B] later");
  txt("v" + String(FW_VERSION) + " -> v" + upd.version, PANE_X, TOP + 2, C_OK);
  hrule(TOP + 2 + LINE_H);
  String notes = upd.notes.length() ? "\n" + upd.notes : String("\n(no changelog)");
  notes.replace("\n- ", "\n* ");           // changelog bullets, not diff lines
  textBlock(notes.substring(1), TOP + 5 + LINE_H, 6, C_FG, true);
  String labels[3]; int n = updateLabels(labels);
  listBlock(labels, n, TOP + 6 + 8 * LINE_H, C_OK);
}

static void drawUpdateProgress() {
  layout(false);
  spr.fillScreen(C_BG); spr.setFont(uiFont()); spr.setTextSize(1);
  frame("updating", C_OK, "don't unplug");
  txt("v" + String(FW_VERSION) + " -> v" + upd.version, PANE_X, TOP + 4, C_FG);
  int p = max(0, min(100, upd.progress));
  spr.drawRect(PANE_X, TOP + 22, PANE_W - 6, 10, C_FRAME);
  spr.fillRect(PANE_X + 1, TOP + 23, (PANE_W - 8) * p / 100, 8, C_OK);
  txt(String(p) + "%", PANE_X, TOP + 38, C_DIM);
  if (p >= 100) txt("restarting...", PANE_X, TOP + 50, C_OK);
  buddyCorner(p >= 100 ? B_CELEBRATE : B_BUSY, p >= 100 ? "done!" : "updating", C_OK);
  spr.pushSprite(0, 0);
}

static void drawNotice() {
  frame(noticeTitle, noticeColor, "[any key] ok");
  textBlock(noticeText, TOP + 8, 10, noticeColor, false);
}

// Push-to-talk screens
static void drawVoiceCard() {
  if (voiceState == V_ERROR) {
    frame("voice", C_BAD, "[any key] ok");
    textBlock(voiceErr, TOP + 10, 6, C_BAD, false);
    return;
  }
  Session* s = findSession(voiceTarget, false);
  frame(voiceApproval ? "deny + tell claude?" : voiceStopReq ? "reply?" : "send to claude?",
        voiceApproval ? C_BAD : C_CLAUDE, "[A] send  [B] discard  [hold A] redo");
  txt("@" + fit(s ? sessionLabel(*s) : String("?"), charsAt(TOP + 2) - 1), PANE_X, TOP + 2, C_DIM);
  hrule(TOP + 2 + LINE_H);
  textBlock("\"" + voiceText + "\"", TOP + 6 + LINE_H, 10, C_FG, true);
}

static void drawVoiceRec(uint32_t ms) {
  layout(false);
  spr.fillScreen(C_BG);
  spr.setFont(uiFont()); spr.setTextSize(1);
  bool rec = voiceState == V_REC;
  frame(rec ? "listening" : "transcribing", C_CLAUDE, rec ? "release [A] to send" : "");
  if (rec) {
    txt(String(ms / 1000) + "." + String((ms / 100) % 10) + "s / 20s", PANE_X, TOP + 4, C_FG);
    int w = min(PANE_W - 8, voiceLevel * (PANE_W - 8) / 12000 + 2);
    spr.drawRect(PANE_X, TOP + 20, PANE_W - 6, 10, C_FRAME);
    spr.fillRect(PANE_X + 1, TOP + 21, w, 8, C_CLAUDE);
    txt("speak now...", PANE_X, TOP + 40, C_DIM);
  } else {
    txt("sending audio to", PANE_X, TOP + 4, C_DIM);
    txt(fit(conf.sttUrl, PANE_CHARS), PANE_X, TOP + 4 + LINE_H, C_DIM);
  }
  buddyCorner(rec ? B_ATTENTION : B_BUSY, rec ? "listening" : "thinking", C_CLAUDE);
  spr.pushSprite(0, 0);
}

static int sessionItems(String* labels, String* marks, String* ids) {
  int n = 0;
  labels[n] = "all sessions"; marks[n] = pinned.length() ? " " : "*"; ids[n++] = "";
  for (auto& s : sessions) if (s.used && n < MAX_ITEMS) {
    labels[n] = sessionLabel(s) + " " + s.state;
    marks[n] = pinned == s.id ? "*" : " ";
    ids[n++] = s.id;
  }
  return n;
}

// Buddy picker: the full-size species. It also shows on the idle home screen.
static void drawPet() {
  frame("buddy", C_CLAUDE, "[B] next  [P] prev  [A] back");
  int y = TOP + 6;
  spr.setTextSize(2); txt(buddySpeciesName(), PANE_X, y, C_CLAUDE); spr.setTextSize(1);
  y += 22;
  txt(String(buddySpeciesIdx() + 1) + " of " + String(buddySpeciesCount()), PANE_X, y, C_DIM); y += LINE_H + 8;
  txt("shows big when idle;", PANE_X, y, C_DIM); y += LINE_H;
  txt("a small face in the", PANE_X, y, C_DIM); y += LINE_H;
  txt("corner while you work.", PANE_X, y, C_DIM);
}

static void drawSessions() {
  frame("tie to session", C_BLUE, "[B] next  [P] up  [A] pin");
  txt("* = stick answers these", PANE_X, TOP + 2, C_DIM);
  String labels[MAX_ITEMS], marks[MAX_ITEMS], ids[MAX_ITEMS];
  int n = sessionItems(labels, marks, ids);
  listBlock(labels, n, max(TOP + 4 + LINE_H, petLayout ? 0 : CORNER_ROWS_Y), C_BLUE, marks);
}

static void drawInfo() {
  frame("info", C_BLUE, "[any key] back");
  int y = TOP + 2;
  auto kv = [&](const char* k, const String& v, uint16_t c = C_FG) {
    txt(k, PANE_X, y, C_DIM);
    txt(fit(v, PANE_CHARS - 5), PANE_X + 30, y, c);
    y += LINE_H;
  };
  bool up = WiFi.status() == WL_CONNECTED;
  kv("fw", FW_VERSION);
  kv("bt", devName);
  kv("wifi", ssid.length() ? ssid : String("(none)"), up ? C_OK : C_BAD);
  kv("sig", up ? String((int)wifiRssiAvg()) + " dBm  " + String(wifiBars()) + "/4" : String("not connected"), up ? C_FG : C_BAD);
  kv("ip", WiFi.localIP().toString() + ":" + String(HTTP_PORT));
  if (bridgeConfigured()) kv("link", bridgeUp ? String("connected") : "offline: " + (bridgeErr.length() ? bridgeErr : String("connecting")), bridgeUp ? C_OK : C_BAD);
  kv("batt", batteryLine(), (int)M5.Power.isCharging() == 1 ? C_OK : C_FG);
  kv("key", token, C_CLAUDE);
  String tk = fmtTok(tokensToday()) + " today";
  if (desk.lastBeat) tk += "  desk " + fmtTok(desk.tokensToday);
  tk += "  ok " + String(stats.appr) + " no " + String(stats.deny);
  kv("tok", tk);
  float f5 = limitPct(lim.five), f7 = limitPct(lim.seven);
  if (f5 >= 0 || f7 >= 0) {
    String L;
    if (f5 >= 0) { L += "5h " + String((int)(f5 + 0.5f)) + "%"; String r = fmtReset(lim.five.resets); if (r.length()) L += " " + r; }
    if (f7 >= 0) { if (L.length()) L += "  "; L += "7d " + String((int)(f7 + 0.5f)) + "%"; String r = fmtReset(lim.seven.resets); if (r.length()) L += " " + r; }
    kv("lim", L, limitsStale() ? C_DIM : C_FG);
  }
  kv("set", String("tilt ") + (tiltMode == 0 ? "off" : tiltMode > 0 ? "on" : "flip") + "  voice " + (conf.sttUrl.length() ? "on" : "off"), C_DIM);
  kv("rst", resetWhy + (safeMode ? "  SAFE" : ""), safeMode ? C_YEL : C_DIM);
}

static void drawPasskey(uint32_t pk) {
  frame("bluetooth pairing", C_BLUE, "");
  txt("enter this code", PANE_X, TOP + 10, C_FG);
  txt("on your computer:", PANE_X, TOP + 10 + LINE_H, C_FG);
  char b[8]; snprintf(b, sizeof b, "%06lu", (unsigned long)pk);
  spr.setTextSize(3);
  txt(b, PANE_X, TOP + 40, C_CLAUDE);
  spr.setTextSize(1);
}

static void drawSetup() {
  bool up = WiFi.status() == WL_CONNECTED;
  frame(up ? "almost there" : "hello!", C_CLAUDE, "[hold B] skip setup");
  int y = TOP + 3;
  auto line = [&](const String& s, uint16_t c) { txt(fit(s, PANE_CHARS), PANE_X, y, c); y += LINE_H; };
  if (up) {
    line("wi-fi ok! now open this", C_FG);
    line("on a phone or pc:", C_FG);
    y += 4;
    line("http://" + WiFi.localIP().toString(), C_CLAUDE);
    line("  :" + String(HTTP_PORT) + "/setup", C_CLAUDE);
    y += 4;
    line("set your time zone,", C_DIM);
    line("name it, pick a buddy,", C_DIM);
    line("get the install cmd", C_DIM);
  } else {
    line("let's get you set up.", C_FG);
    y += 4;
    if (apOn) {
      line("phone: join wi-fi", C_DIM);
      line(apName, C_CLAUDE);
      line("(the page pops up, or", C_DIM);
      line(" open 192.168.4.1)", C_DIM);
      y += 4;
    } else if (ssid.length()) {
      line("joining " + ssid + "...", C_YEL);
      y += 4;
    }
    line("pc: use the web flasher", C_DIM);
    line("(it sets wi-fi too)", C_DIM);
  }
}

// Session details: to-do list and token usage. B / PWR switch session.
static void drawDetail() {
  Session* s = targetSession();
  frame("session", C_BLUE, "[B/P] next  [A] back  [hold A] talk");
  if (!s) { txt("no claude session yet", PANE_X, TOP + 20, C_DIM); return; }
  int y = TOP + 2;
  String st = s->state;
  txt(fit(sessionLabel(*s), charsAt(y) - (int)st.length() - 1), PANE_X, y, C_FG);
  txt(st, rightX(y) - spr.textWidth(st), y, stateColor(st));
  y += LINE_H;
  String u;
  if (s->hasUsage) u = "tok " + fmtTok(s->tokIn + s->tokOut + s->tokCR + s->tokCW);
  if (s->ctxPct >= 0) u += (u.length() ? "  " : "") + String("ctx ") + String((int)(s->ctxPct + 0.5f)) + "%";
  else if (s->hasUsage) u += "  ctx " + fmtTok(s->ctx);
  if (s->cost >= 0) u += "  $" + String(s->cost, 2);
  if (u.length()) { txt(fit(u, charsAt(y)), PANE_X, y, C_DIM); y += LINE_H; }
  float f5 = limitPct(lim.five), f7 = limitPct(lim.seven);
  if (f5 >= 0 || f7 >= 0) {
    String L;
    if (f5 >= 0) { L += "5h " + String((int)(f5 + 0.5f)) + "%"; String r = fmtReset(lim.five.resets); if (r.length()) L += " " + r; }
    if (f7 >= 0) { if (L.length()) L += "  "; L += "7d " + String((int)(f7 + 0.5f)) + "%"; String r = fmtReset(lim.seven.resets); if (r.length()) L += " " + r; }
    txt(fit(L, PANE_CHARS), PANE_X, y, limitsStale() ? C_DIM : C_FG); y += LINE_H;
  }
  if (!petLayout && y < CORNER_ROWS_Y) y = CORNER_ROWS_Y - 3;
  spr.drawFastHLine(PANE_X, y + 1, PANE_W - 6, C_FRAME);
  y += 5;
  int bottom = haveUsage() ? BOTTOM - LINE_H - 2 : BOTTOM;
  if (haveUsage()) usageStrip(BOTTOM - LINE_H + 3);
  if (!s->todoTotal) {
    txt("no to-do list", PANE_X, y, C_DIM); y += LINE_H;
    textBlock(s->detail, y + 2, (bottom - y) / LINE_H, C_DIM, false);
    return;
  }
  // progress bar
  String p = String(s->todoDone) + "/" + String(s->todoTotal);
  txt(p, PANE_X, y, s->todoDone == s->todoTotal ? C_OK : C_FG);
  int bx = PANE_X + spr.textWidth(p) + 4, bw = W - 8 - bx;
  spr.drawRect(bx, y, bw, 7, C_FRAME);
  spr.fillRect(bx + 1, y + 1, (bw - 2) * s->todoDone / max(1, (int)s->todoTotal), 5, C_OK);
  y += LINE_H + 1;
  // items, scrolled by tilt; starts so the current item is visible
  int rows = (bottom - y) / LINE_H;
  int cur = 0;
  for (int i = 0; i < s->nTodo; i++) if (s->todo[i].st == 'i') { cur = i; break; }
  int maxFirst = max(0, (int)s->nTodo - rows);
  static uint32_t lastKey = 0;
  if (scrollKey != lastKey) { lastKey = scrollKey; scrollLine = max(0, min(cur - 1, maxFirst)); }
  int first = max(0, min(scrollLine, maxFirst));
  scrollLine = first;
  int y0 = y;
  for (int i = first; i < s->nTodo && i < first + rows; i++) {
    const TodoItem& t = s->todo[i];
    uint16_t c = t.st == 'c' ? C_FRAME : t.st == 'i' ? C_CLAUDE : C_FG;
    String mark = t.st == 'c' ? "x " : t.st == 'i' ? "> " : "- ";
    txt(mark + fit(t.text, PANE_CHARS - 2), PANE_X, y, c);
    y += LINE_H;
  }
  int hidden = (int)s->todoTotal - (int)s->todoSkip - (int)s->nTodo;
  if (first + rows < s->nTodo || hidden > 0) txt("v", W - 10, bottom - LINE_H, C_DIM);
  if (first > 0 || s->todoSkip) txt("^", W - 10, y0, C_DIM);
}

static String fmtAge(uint32_t ms) {
  uint32_t t = ms / 1000;
  if (t < 60) return String(t) + "s";
  if (t < 3600) return String(t / 60) + "m";
  return String(t / 3600) + "h";
}

static bool blinkOn() { return (millis() / 500) % 2 == 0; }

static void drawHome() {
  bool idle = !desk.lastBeat;
  for (auto& s : sessions) if (s.used) idle = false;
  layout(idle);   // nothing going on: the big buddy keeps you company; otherwise the compact layout
  String title = titleBase();
  if (pinned.length()) title += " *";
  frame(title, remoteMode ? C_CLAUDE : C_FG, "[A] details  [hold A] talk  [B] menu");
  if (idle) {
    if (ssid.length()) {
      bool up = WiFi.status() == WL_CONNECTED;
      String msg = up ? "$ waiting for claude" : "$ joining wifi";
      txt(msg, PANE_X, TOP + 30, C_DIM);
      if (blinkOn()) spr.fillRect(PANE_X + spr.textWidth(msg) + 2, TOP + 30, 5, 7, up ? C_CLAUDE : C_YEL);
      if (up) txt(WiFi.localIP().toString(), PANE_X, TOP + 30 + LINE_H + 2, C_FRAME);
    }
    String d = fmtTok(tokensToday()) + " tok today";
    if (tokensToday()) txt(d, PANE_X, BOTTOM - LINE_H + 2, C_DIM);
    return;
  }
  int y = TOP + 2;
  bool strip = haveUsage();
  int limitY = strip ? BOTTOM - 2 * LINE_H : BOTTOM - LINE_H;
  if (desk.lastBeat) {
    txt("desktop " + String(desk.running) + "/" + String(desk.total), PANE_X, y, C_BLUE); y += LINE_H;
    txt("  " + fit(desk.msg.length() ? desk.msg : desk.entry, charsAt(y) - 2), PANE_X, y, C_DIM); y += LINE_H + 3;
  }
  Session* tgt = targetSession();
  uint32_t now = millis();
  for (auto& s : sessions) {
    if (!s.used || y > limitY) continue;
    bool isT = &s == tgt;
    bool active = s.state == "tool" || s.state == "working";
    uint16_t sc = stateColor(s.state);
    if (active) {   // spinner instead of the state square
      static const char SPIN[] = "|/-\\";
      txt(String(SPIN[(now / 200) % 4]), PANE_X, y, sc);
    } else spr.fillRect(PANE_X, y + 1, 4, 6, sc);
    String st = s.stopFlag ? "stop.." : s.inject.length() ? "msg.." : s.state;
    if (!s.stopFlag && !s.inject.length() && (active || s.state == "done") && now - s.stateSince >= 3000)
      st += " " + fmtAge(now - s.stateSince);
    String head = sessionLabel(s);
    if (pinned == s.id) head = "*" + head;
    head = fit(head, charsAt(y) - 2 - (int)st.length() - 1);
    txt(head, PANE_X + 7, y, isT ? C_FG : C_DIM);
    txt(st, rightX(y) - spr.textWidth(st), y, sc);
    if (isT) spr.drawFastHLine(PANE_X + 7, y + 8, spr.textWidth(head), C_CLAUDE);
    y += LINE_H;
    if (y <= limitY && (s.detail.length() || s.todoTotal)) {
      int x = PANE_X + 12, room2 = (rightX(y) - x) / 6;
      String d = s.detail;
      if (s.todoTotal) {
        String pr = String(s.todoDone) + "/" + String(s.todoTotal);
        txt(pr, x, y, s.todoDone == s.todoTotal ? C_OK : C_CLAUDE);
        x += spr.textWidth(pr) + 6; room2 = (rightX(y) - x) / 6;
        if (s.todoNow.length() && s.state != "done") d = s.todoNow;
      }
      txt(fit(d, room2), x, y, C_DIM);
      y += LINE_H;
    }
    y += 2;
  }
  if (strip) usageStrip(BOTTOM - LINE_H + 3);
}

static void draw() {
  spr.fillScreen(C_BG);
  spr.setFont(uiFont());
  spr.setTextSize(1);
  const char* mood; uint16_t moodColor;
  uint8_t bs = buddyState(&mood, &moodColor);
  layout(false);

  uint32_t pk = blePasskey();
  Approval* a = topApproval();
  StopReq* r = topStop();
  uint32_t key = pk ? 1 : a ? a->id : r ? r->id : 0;
  if (voiceState == V_CONFIRM && !pk) key = 0x7FFFFFFF;
  else if (!pk && !a && !r && screen == SCR_HOME && updatePromptDue()) key = 0x7FFFFFFE;
  else if (!pk && !a && !r && screen == SCR_DETAIL) {
    Session* t = targetSession();
    key = 0x7FFF0000 + (t ? (uint32_t)(t - sessions) + 1 : 0);
  }
  if (key != scrollKey) { scrollKey = key; scrollLine = 0; }

  if (pk) drawPasskey(pk);
  else if (voiceState == V_CONFIRM || voiceState == V_ERROR) drawVoiceCard();   // finish what you said first
  else if (a) drawApproval(a);
  else if (noticeUntil && (int32_t)(millis() - noticeUntil) < 0) drawNotice();
  else if (r) drawChoice(r);
  else if (screen == SCR_HOME && updatePromptDue()) drawUpdatePrompt();
  else if (!setupDone) { layout(true); drawSetup(); }
  else if (screen == SCR_PET) { layout(true); drawPet(); bs = B_IDLE; mood = "hi!"; moodColor = C_CLAUDE; }
  else if (screen == SCR_ACTIONS) drawActions();
  else if (screen == SCR_SESSIONS) drawSessions();
  else if (screen == SCR_INFO) drawInfo();
  else if (screen == SCR_HELP) drawHelp();
  else if (screen == SCR_TEST) drawTest();
  else if (screen == SCR_DETAIL) drawDetail();
  else drawHome();
  if (petLayout) buddyPane(bs, mood, moodColor); else buddyCorner(bs, mood, moodColor);
  spr.pushSprite(0, 0);
}

// ----------------------------------------------------------------- input ---

// Tilt to scroll long text: baseline is taken when a card appears, then each
// ~0.3 g of forward/back tilt scrolls one line every 250 ms.
static void tiltTick() {
  static uint32_t last = 0, lastKey = 0;
  static float base = 0;
  if (!tiltMode || !scrollKey || millis() - last < 250) return;
  last = millis();
  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return;
  if (scrollKey != lastKey) { lastKey = scrollKey; base = ax; return; }
  float d = (ax - base) * tiltMode * (flipped ? -1 : 1);
  if (d > 0.3f) { scrollLine++; dirty = true; touch(); }
  else if (d < -0.3f && scrollLine > 0) { scrollLine--; dirty = true; touch(); }
}

static void listNav(int total, bool next, bool prev) {
  if (next) { menuIdx = (menuIdx + 1) % total; beep(0); }
  if (prev) { menuIdx = (menuIdx + total - 1) % total; beep(0); }
}

// Double-press A on an approval card = always allow (or allow a dangerous one).
// A single press there waits DOUBLE_MS to see if a second one follows.
static const uint32_t DOUBLE_MS = 350;
static uint32_t aFirstAt = 0, aFirstId = 0;

static bool needsDouble(Approval* a) { return a->risk == "high" || a->always.length(); }

static void singleA(Approval* a) {
  if (a->risk == "high") { holdHintUntil = millis() + 2500; beep(3); dirty = true; }
  else decide(a, D_ALLOW);
}

static void doubleTimeout() {
  if (!aFirstAt || millis() - aFirstAt < DOUBLE_MS) return;
  aFirstAt = 0;
  Approval* a = topApproval();
  if (a && a->id == aFirstId) singleA(a);
}

static void cycleSession(int dir) {
  for (int k = 1; k <= MAX_SESSIONS; k++) {
    int i = (focusSession + dir * k + 2 * MAX_SESSIONS) % MAX_SESSIONS;
    if (sessions[i].used) { focusSession = i; beep(0); return; }
  }
}

static void buttons() {
  bool aClick = M5.BtnA.wasClicked(), aHold = M5.BtnA.wasHold();
  bool bClick = M5.BtnB.wasClicked(), bHold = M5.BtnB.wasHold();
  bool pClick = M5.BtnPWR.wasClicked();
  doubleTimeout();
  if (!(aClick || aHold || bClick || bHold || pClick)) return;

  if (dimmed) { touch(); return; }      // first press only wakes the screen
  touch();
  if (screen == SCR_TEST) {             // buttons light up on screen; only these do anything
    if (bHold) screen = SCR_HOME;
    else if (aClick) beep(1);
    else if (pClick) beep(2);
    return;
  }
  if (setupDone) noteStick();           // answering on the Stick = you're remote (auto mode)

  // Voice result first: an approval that arrives while you're reading it waits.
  if (voiceState == V_ERROR) { voiceState = V_IDLE; return; }
  if (voiceState == V_CONFIRM) {
    if (aClick) voiceSend();
    else if (bClick || bHold) { voiceState = V_IDLE; voiceApproval = 0; beep(0); }
    else if (aHold) {   // redo, same target
      if (voiceApproval) { voiceDenyCapture(approvalById(voiceApproval)); return; }
      StopReq* card = nullptr;
      for (auto& x : stops) if (voiceStopReq && x.used && x.id == voiceStopReq && x.result == -1) card = &x;
      voiceCapture(card);
    }
    return;
  }

  if (Approval* a = topApproval()) {
    if (aFirstAt && a->id != aFirstId) aFirstAt = 0;           // card changed under the first press
    if (aClick && needsDouble(a)) {
      if (aFirstAt && millis() - aFirstAt < DOUBLE_MS) {       // second press
        aFirstAt = 0;
        decide(a, a->risk == "high" || !a->always.length() ? D_ALLOW : D_ALWAYS);
      } else { aFirstAt = millis(); aFirstId = a->id; }
    }
    else if (aClick) decide(a, D_ALLOW);
    else if (aHold) {
      aFirstAt = 0;
      if (a->src == SRC_WIFI) voiceDenyCapture(a);
      else notice("voice", "voice replies work with claude code (cli) sessions", C_DIM, 2500);
    }
    else if (bClick) { aFirstAt = 0; decide(a, D_DENY); }
    else if ((bHold || pClick) && a->src == SRC_WIFI) { aFirstAt = 0; decide(a, D_SKIP); }   // answer at the keyboard
    return;
  }

  if (noticeUntil && (int32_t)(millis() - noticeUntil) < 0) { noticeUntil = 0; return; }

  if (StopReq* r = topStop()) {
    int total = r->nMacros + 1;
    listNav(total, bClick, pClick);
    if (aHold) { voiceCapture(r); return; }
    if (bHold || (aClick && menuIdx == total - 1)) { if (stopResult(r, -2)) { menuIdx = 0; beep(0); } }
    else if (aClick) { if (stopResult(r, menuIdx)) { menuIdx = 0; beep(2); } }
    return;
  }

  if (screen == SCR_HOME && setupDone && updatePromptDue()) {
    listNav(3, bClick, pClick);
    if (bHold) { upd.remindAt = millis() + 24UL * 3600 * 1000; menuIdx = 0; beep(0); return; }
    if (!aClick) return;
    if (menuIdx == 0) { menuIdx = 0; installUpdate(); }
    else if (menuIdx == 1) { upd.remindAt = millis() + 24UL * 3600 * 1000; beep(0); }
    else { skipVersion = upd.version; prefs.putString("skipver", skipVersion); upd.available = false; beep(0); }
    menuIdx = 0;
    return;
  }

  if (screen == SCR_ACTIONS) {
    String labels[MAX_ITEMS]; MenuEntry ents[MAX_ITEMS];
    int total = menuEntries(ents, labels);
    Session* s = targetSession();
    listNav(total, bClick, pClick);
    if (bHold) { screen = SCR_HOME; menuIdx = 0; return; }
    MenuEntry e = ents[menuIdx];
    if (aHold && e.kind == MI_TOKEN) {
      char t[17];
      snprintf(t, sizeof t, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
      token = t; prefs.putString("token", token);
      beep(2); screen = SCR_INFO; menuIdx = 0;   // show it; re-run the install command on your machines
      return;
    }
    if (!aClick) return;
    switch (e.kind) {
      case MI_STOP:   if (s && sessionStop(s)) beep(3); screen = SCR_HOME; menuIdx = 0; break;
      case MI_MACRO:  if (s) { tel.macroTaps++; if (s->bridge ? bridgeSessionAct(s, "macro", e.arg) : deliverText(s, macros[e.arg].text)) beep(2); }
                      screen = SCR_HOME; menuIdx = 0; break;
      case MI_REMOTE: setRemoteSetting(conf.remote + 1); break;                       // stay in menu
      case MI_TIE:    screen = SCR_SESSIONS; menuIdx = 0; break;
      case MI_BUDDY:  screen = SCR_PET; break;
      case MI_FONT:   textStyle = textStyle ? 0 : 1; prefs.putUChar("font", textStyle); spr.setFont(uiFont()); break;   // stay in menu
      case MI_CLOCK:  clock24 = !clock24; prefs.putBool("clk24", clock24); break;     // stay in menu
      case MI_HELP:   screen = SCR_HELP; menuIdx = 0; break;
      case MI_FLIP:   flipped = !flipped; prefs.putBool("flip", flipped); M5.Display.setRotation(flipped ? 3 : 1); break;
      case MI_TEST:   screen = SCR_TEST; menuIdx = 0; break;
      case MI_UPDATE: screen = SCR_HOME; menuIdx = 0;
                      if (upd.available) upd.remindAt = 0; else { notice("updates", "checking...", C_DIM, 15000); draw(); checkForUpdate(true); }
                      break;
      case MI_INFO:   screen = SCR_INFO; menuIdx = 0; break;
      case MI_BT:     btPref = !btPref; prefs.putBool("bt", btPref); restartAt = millis() + 1500;
                      notice("bluetooth", btPref ? "turning on: restarting" : "turning off: restarting", C_YEL_U, 3000); break;
      case MI_TOKEN:  holdHintUntil = millis() + 2000; beep(3); break;              // needs a hold
      case MI_SETUP:  setupDone = false; prefs.putBool("setup", false); screen = SCR_HOME; menuIdx = 0; break;
      default:        screen = SCR_HOME; menuIdx = 0; break;
    }
    return;
  }

  if (screen == SCR_SESSIONS) {
    String labels[MAX_ITEMS], marks[MAX_ITEMS], ids[MAX_ITEMS];
    int total = sessionItems(labels, marks, ids);
    listNav(total, bClick, pClick);
    if (bHold) { screen = SCR_HOME; menuIdx = 0; }
    else if (aClick) {
      pinned = ids[menuIdx];
      if (bridgeUp) { Session* ps = pinned.length() ? findSession(pinned, false) : nullptr; if (!pinned.length() || (ps && ps->bridge)) bridgePin(pinned); }
      if (pinned.length())
        for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && sessions[i].id == pinned) focusSession = i;
      beep(2); screen = SCR_HOME; menuIdx = 0;
    }
    return;
  }

  if (screen == SCR_INFO || screen == SCR_HELP) { screen = SCR_HOME; return; }

  if (screen == SCR_PET) {
    if (bClick) prefs.putUChar("buddy", buddyNextSpecies());
    else if (pClick) { uint8_t i = (buddySpeciesIdx() + buddySpeciesCount() - 1) % buddySpeciesCount(); buddySetSpeciesIdx(i); prefs.putUChar("buddy", i); }
    else { screen = SCR_ACTIONS; beep(0); }           // A or hold B: back to the menu
    return;
  }

  if (screen == SCR_DETAIL) {
    if (aHold) voiceCapture(nullptr);                 // talk to the session you're looking at
    else if (bClick) cycleSession(1);
    else if (pClick) cycleSession(-1);
    else { screen = SCR_HOME; beep(0); }            // A or hold B: back
    return;
  }

  if (!setupDone) {   // first-time setup screen: only "skip" is offered
    if (bHold) { setupDone = true; prefs.putBool("setup", true); beep(0); }
    return;
  }

  // home
  if (aHold) voiceCapture(nullptr);
  else if (bHold) { screen = SCR_ACTIONS; menuIdx = 0; beep(0); }
  else if (aClick) {   // details of the focused session
    Session* t = targetSession();
    if (t) focusSession = t - sessions;
    screen = SCR_DETAIL; beep(0);
  }
  else if (bClick) { screen = SCR_ACTIONS; menuIdx = 0; beep(0); }
}

// ------------------------------------------------------------ setup/loop ---

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(160);
  M5.Speaker.begin();
  M5.BtnA.setHoldThresh(700);
  M5.BtnB.setHoldThresh(700);

  spr.setColorDepth(16);
  spr.setPsram(true);
  spr.createSprite(W, H);

  prefs.begin("cstick", false);
  bootGuard();                       // may switch back to the previous firmware and restart
  flipped = prefs.getBool("flip", false);
  textStyle = prefs.getUChar("font", 1);
  tzPosix = prefs.getString("tz", ""); tzName = prefs.getString("tzname", ""); clock24 = prefs.getBool("clk24", false);
  applyTimezone();
  M5.Display.setRotation(flipped ? 3 : 1);
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  resetWhy = "power on"; break;
    case ESP_RST_SW:       resetWhy = "restart"; break;
    case ESP_RST_PANIC:    resetWhy = "CRASH (panic)"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      resetWhy = "CRASH (watchdog)"; break;
    case ESP_RST_BROWNOUT: resetWhy = "brownout (low power)"; break;
    case ESP_RST_DEEPSLEEP: resetWhy = "wake"; break;
    default:               resetWhy = "other"; break;
  }
  { String why = prefs.getString("why", ""); if (why.length() && esp_reset_reason() == ESP_RST_SW) resetWhy = why; prefs.remove("why"); }
  M5.update();
  if (M5.BtnB.isPressed() && !M5.BtnA.isPressed()) safeMode = true;   // hold B at power-on
  ssid  = prefs.getString("ssid", CS_WIFI_SSID);
  pass  = prefs.getString("pass", CS_WIFI_PASS);
  token = prefs.getString("token", CS_TOKEN);
  bridgeUrl = prefs.getString("brurl", ""); bridgeTok = prefs.getString("brtok", "");
  loadNets();
  tiltMode = prefs.getChar("tilt", 1);
  loadConfig();
  skipVersion = prefs.getString("skipver", "");
  setupDone = prefs.getBool("setup", false);
  volume = prefs.getUChar("vol", 120);
  btPref = prefs.getBool("bt", true);
  radioSleep = prefs.getUChar("wps", 0);
  M5.Speaker.setVolume(volume);
  M5.update();
  if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) {   // hold A+B while powering on: forget Wi-Fi, redo setup
    forgetAllNets(); prefs.putBool("setup", false); setupDone = false;
  }
  buddySetSpeciesIdx(prefs.getUChar("buddy", 0));
  if (!token.length()) {   // first boot: generate a random token
    char t[17];
    snprintf(t, sizeof t, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    token = t;
    prefs.putString("token", token);
  }

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[5]; snprintf(suffix, sizeof suffix, "%02X%02X", mac[4], mac[5]);
  devName  = prefs.getString("name", String("Claude-") + suffix);   // must start with "Claude"
  char idbuf[13]; snprintf(idbuf, sizeof idbuf, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  devId    = idbuf;
  mdnsHost = String("claudestick-") + String(suffix).substring(0, 4);
  mdnsHost.toLowerCase();
  apName   = String("ClaudeStick-") + suffix;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(mdnsHost.c_str());
  applyRadioSleep();       // modem sleep is required for Wi-Fi + BLE coexistence
  if (nNets) { pickNetAtBoot(); WiFi.begin(ssid.c_str(), pass.c_str()); }
  else if (!setupDone) startAP();

  if (!btOff()) bleInit(devName.c_str());
  startHttp();
  bootNotices();
  lastActivity = millis();
  Serial.printf("ClaudeStick %s ready. Type 'info' for details.\n", FW_VERSION);
}

// If the heap fragments badly after days of uptime, restart while idle instead of failing in odd ways.
static void memoryWatch() {
  static uint32_t last = 0;
  static uint8_t low = 0;
  if (millis() - last < 30000) return;
  last = millis();
  if (internalBlock() >= 10000) { low = 0; return; }
  if (++low >= 3 && !topApproval() && !topStop() && upd.progress < 0 && millis() - lastActivity > 60000) {
    prefs.putString("why", "low memory"); delay(100); ESP.restart();
  }
}

void loop() {
  M5.update();
  serialTick();
  wifiTick();
  bridgeTick();
  server.handleClient();
  pollBle();
  expireStale();
  apTick();
  updateTick();
  ntpTick();
  drainTick();
  telemetryTick();
  telemetryRemoteTick();
  memoryWatch();
  bootHealthTick();
  buttons();
  tiltTick();

  // alert sounds for newly surfaced prompts, plus a gentle reminder
  Approval* a = topApproval();
  StopReq* r = topStop();
  if (a && a->id != shownApproval) { shownApproval = a->id; beep(1); touch(); lastBeep = millis(); }
  if (!a) shownApproval = 0;
  if (r && r->id != shownStop) { shownStop = r->id; menuIdx = 0; touch(); }
  if (!r) shownStop = 0;
  if (a && millis() - lastBeep > 30000) { beep(1); lastBeep = millis(); }

  updateRemote();
  // Power: dim after 1 min, screen off after 3 min, slower CPU while dimmed.
  // Wi-Fi and Bluetooth stay up so hooks and the desktop can still reach the Stick.
  uint32_t idle = millis() - lastActivity;
  if (!dimmed && !a && !r && idle > DIM_AFTER_MS) {
    M5.Display.setBrightness(8); dimmed = true; setCpuFrequencyMhz(80);
  }
  static bool screenOff = false;
  if (dimmed && !screenOff && idle > 3 * DIM_AFTER_MS) { M5.Display.setBrightness(0); screenOff = true; }
  if (!dimmed && screenOff) screenOff = false;
  if (!dimmed && getCpuFrequencyMhz() != 160) setCpuFrequencyMhz(160);   // plenty for this UI; Wi-Fi/BLE/TLS all run fine and it saves battery

  static uint32_t lastPk = 0;
  if (blePasskey() != lastPk) { lastPk = blePasskey(); touch(); }

  // ~5 fps keeps the buddy animated; dimmed screen refreshes slowly to save power
  uint32_t period = screenOff ? 5000 : dimmed ? 1000 : 200;
  if (dirty || millis() - lastDraw > period) {
    draw(); dirty = false; lastDraw = millis();
  }
  delay(5);
}
