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

static const char*    FW_VERSION   = "0.5.0";
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

struct Session {
  bool     used = false;
  String   id, project, state, detail;
  uint32_t lastSeen = 0, stateSince = 0;
  bool     stopFlag = false;   // "Stop now" queued from the device
  String   inject;             // macro queued mid-run
};

enum Src : uint8_t { SRC_WIFI = 0, SRC_BLE = 1 };
enum Decision : int8_t { D_PENDING = -1, D_DENY = 0, D_ALLOW = 1, D_SKIP = 2, D_ALWAYS = 3 };

struct Approval {
  bool     used = false;
  uint32_t id = 0;
  Src      src = SRC_WIFI;
  String   bleId;              // desktop prompt id
  String   tool, detail, project;
  String   always;             // what "always allow" would remember ("" = not offered)
  String   risk, riskWhy;      // risk == "high" needs a hold to allow
  uint32_t created = 0, lastPoll = 0;
  int8_t   decision = D_PENDING;
};

struct StopReq {
  bool     used = false;
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
} conf;
static uint32_t lastStickAt = 0, lastKbdAt = 0;   // auto remote mode inputs
static int32_t     tzOffset = 0;

enum Screen { SCR_HOME, SCR_INFO, SCR_ACTIONS, SCR_SESSIONS, SCR_HELP };
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
  return &sessions[i];
}

static int activeSessionCount() {
  int n = 0;
  for (auto& s : sessions) if (s.used) n++;
  return n;
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
  tzOffset = offset;
  clockSet = true;
}

static String clockStr() {
  if (!clockSet) return "";
  time_t t = time(nullptr) + tzOffset;
  struct tm tm;
  gmtime_r(&t, &tm);
  char b[8];
  snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
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

// Effective remote mode. In auto it follows where you last answered: the Stick or
// phone (remote) versus typing in the terminal (desk).
static void updateRemote() {
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
      bleClearBonds(); bleAck("unpair");
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
  desk.msg      = (const char*)(doc["msg"] | "");
  desk.tokensToday = doc["tokens_today"] | 0L;
  desk.entry    = doc["entries"].is<JsonArray>() ? String((const char*)(doc["entries"][0] | "")) : "";
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
        a->tool = (const char*)(doc["prompt"]["tool"] | "tool");
        a->detail = (const char*)(doc["prompt"]["hint"] | "");
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
  if (d == D_ALLOW || d == D_ALWAYS) { stats.appr++; heartUntil = millis() + 2500; }
  if (d == D_SKIP) noteKeyboard(); else if (fromStick) noteStick();
  if (d == D_DENY)  stats.deny++;
  if (a->src == SRC_BLE) {
    if (d != D_SKIP) {
      JsonDocument j;
      j["cmd"] = "permission"; j["id"] = a->bleId; j["decision"] = d == D_DENY ? "deny" : "once";
      bleSend(j);
    }
    answeredBleId = a->bleId;
    a->used = false;          // desktop's next heartbeat confirms
  } else {
    a->decision = d;          // hook picks it up on next poll
    a->lastPoll = millis();
  }
  beep(d == D_DENY ? 3 : 0);
  touch();
}

// ----------------------------------------------------------- HTTP side ---

static bool authed() {
  if (!token.length()) return true;
  if (server.header("X-Token") == token || server.arg("t") == token) return true;
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
  if (!clockSet && in["now"].is<long>()) setClock(in["now"].as<long>(), in["tz"] | 0L);
}

// POST /event  {session, project, event, tool?, detail?, can_stop?, can_inject?, macros?}
// -> {"stop":bool, "inject":string|null}
static void hEvent() {
  if (!authed()) return;
  JsonDocument in; if (!readBody(in)) return;
  Session* s; noteSession(in, s);
  String ev = in["event"] | "";
  if (in["typed"] | false) noteKeyboard();   // a prompt typed in the terminal: you're at the desk

  if (ev == "end") { s->used = false; }
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
  JsonDocument out; out["state"] = st; sendJson(out);
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

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>ClaudeStick</title>
<style>body{font:15px system-ui;margin:0;background:#111;color:#eee}main{padding:12px;max-width:640px;margin:auto}
.c{background:#1d1d1d;border-radius:10px;padding:10px;margin:8px 0}.t{color:#f90;font-weight:600}
.d{color:#999;font-size:13px;word-break:break-word}button{font:inherit;border:0;border-radius:8px;padding:9px 12px;margin:3px 3px 0 0;background:#333;color:#eee}
.ok{background:#1a5}.no{background:#b33}.ac{background:#f90;color:#000}textarea,select{width:100%;box-sizing:border-box;font:inherit;background:#222;color:#eee;border:1px solid #444;border-radius:8px;padding:8px;margin-top:6px}
.row{display:flex;justify-content:space-between;align-items:center}.risk{border:1px solid #b33}.rk{color:#f55;font-weight:600}
pre{white-space:pre-wrap;word-break:break-word;margin:6px 0;font-size:13px;color:#bbb}.add{color:#5c5}.del{color:#e66}</style></head><body><main>
<div class=row><h3>ClaudeStick</h3><span><button id=rm onclick="act({action:'remote'})"></button> <a href="#" id=st style="color:#888">settings</a></span></div>
<div id=ap></div><div id=ch></div><div id=ss></div>
<div class=c><div class=t>Send to Claude</div><select id=sel></select>
<textarea id=tx rows=3 placeholder="Type an instruction..."></textarea>
<button class=ac onclick="say()">Send</button><button class=no onclick="act({action:'stop',session:sel.value})">Stop</button>
<div id=mc></div><div class=d>Delivered when Claude finishes its turn, or after its current step.</div></div>
</main><script>
const T=new URLSearchParams(location.search).get('t')||localStorage.cst||'';if(T)localStorage.cst=T;let S={};
const e=s=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
async function api(p,b){const r=await fetch(p,{method:b?'POST':'GET',headers:{'X-Token':T,'Content-Type':'application/json'},body:b?JSON.stringify(b):undefined});return r.json()}
async function act(b){await api('/act',b);load()}
function say(){if(!tx.value.trim())return;act({action:'say',session:sel.value,text:tx.value});tx.value=''}
async function load(){try{S=await api('/state')}catch(x){return}
rm.textContent='Remote: '+S.remote_setting+(S.remote_setting=='auto'?(S.remote?' (on)':' (off)'):'');rm.className=S.remote?'ac':'';st.href='/setup?t='+T;
const diff=t=>e(t).split('\n').map(l=>l.startsWith('+ ')?`<span class=add>${l}</span>`:l.startsWith('- ')?`<span class=del>${l}</span>`:l).join('\n');
ap.innerHTML=(S.approvals||[]).map(a=>`<div class="c ${a.risk=='high'?'risk':''}"><div class=t>${a.risk=='high'?'<span class=rk>DANGER</span> ':''}Approve? ${e(a.tool)} <span class=d>${e(a.project)}</span></div>${a.risk_why?`<div class=rk>${e(a.risk_why)}</div>`:''}<pre>${diff(a.detail)}</pre>
<button class=ok onclick="act({action:'approve',id:${a.id}})">Approve</button>${a.always?`<button class=ok onclick="act({action:'always',id:${a.id}})" title="${e(a.always)}">Always</button>`:''}<button class=no onclick="act({action:'deny',id:${a.id}})">Deny</button>${a.src=='wifi'?`<button onclick="act({action:'keyboard',id:${a.id}})">Keyboard</button>`:''}${a.always?`<div class=d>always = ${e(a.always)}</div>`:''}</div>`).join('');
ch.innerHTML=(S.choices||[]).map(c=>`<div class=c><div class=t>${c.kind=='question'?'Question':'Done'} <span class=d>${e(c.project)}</span></div><div class=d>${e(c.title||c.last)}</div>
${c.options.map((o,i)=>`<button class=ac onclick="act({action:'choose',id:${c.id},index:${i}})">${e(o)}</button>`).join('')}<button onclick="act({action:'dismiss',id:${c.id}})">Dismiss</button></div>`).join('');
const v=sel.value;sel.innerHTML=(S.sessions||[]).map(s=>`<option value="${e(s.id)}">${e(s.project)} (${e(s.state)})</option>`).join('');if(v)sel.value=v;
ss.innerHTML=(S.sessions||[]).map(s=>`<div class=c><div class=row><div class=t>${S.pinned==s.id?'* ':''}${e(s.project)} <span class=d>${e(s.state)}</span></div><button onclick="act({action:'pin',session:'${S.pinned==s.id?'':e(s.id)}'})">${S.pinned==s.id?'Unpin':'Pin'}</button></div><div class=d>${e(s.detail)}</div></div>`).join('')+(S.desktop?`<div class=c><div class=t>Desktop</div><div class=d>${e(S.desktop)}</div></div>`:'');
mc.innerHTML=(S.macros||[]).map((m,i)=>`<button onclick="act({action:'macro',session:sel.value,index:${i}})">${e(m)}</button>`).join('')}
load();setInterval(load,2000)</script></body></html>)HTML";

static void hRoot() {
  if (!setupDone) { server.sendHeader("Location", "/setup"); server.send(302, "text/plain", ""); return; }
  server.send_P(200, "text/html", INDEX_HTML);
}

static void hState() {
  if (!authed()) return;
  JsonDocument out;
  updateRemote();
  out["remote"] = remoteMode;
  out["remote_setting"] = remoteName(conf.remote);
  out["pinned"] = pinned;
  JsonArray ss = out["sessions"].to<JsonArray>();
  for (auto& s : sessions) if (s.used) {
    JsonObject o = ss.add<JsonObject>();
    o["id"] = s.id; o["project"] = s.project; o["state"] = s.state; o["detail"] = s.detail;
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
  updateRemote();
  beep(remoteMode ? 2 : 0);
  touch();
}

// Hand text to a session: answers its Done/Question card if one is up, otherwise
// it's delivered after Claude's current step or when the turn ends.
static bool deliverText(Session* s, const String& text) {
  if (!s || !text.length()) return false;
  for (auto& x : stops)
    if (x.used && x.result == -1 && x.session == s->id) { x.custom = text; x.result = -3; return true; }
  s->inject = text;
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
    if (a) decide(a, d); else ok = false;
  } else if (action == "choose" || action == "dismiss") {
    StopReq* r = nullptr;
    for (auto& x : stops) if (x.used && x.id == id && x.result == -1) r = &x;
    if (!r) ok = false;
    else if (action == "dismiss") r->result = -2;
    else if (index >= 0 && index < r->nMacros) r->result = index;
    else ok = false;
  } else if (action == "say" || action == "macro" || action == "stop") {
    Session* s = sid.length() ? findSession(sid, false) : targetSession();
    String text = action == "say" ? String((const char*)(in["text"] | "")) :
                  (action == "macro" && index >= 0 && index < nMacros) ? macros[index].text : String();
    if (!s) ok = false;
    else if (action == "stop") s->stopFlag = true;
    else ok = deliverText(s, text);
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
  out["auth"] = token.length() > 0;
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
    otaAllowed = server.header("X-Token") == token || server.arg("t") == token;
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
    if (a.used && a.src == SRC_WIFI && now - a.lastPoll > POLL_STALE_MS) { a.used = false; dirty = true; }
  for (auto& r : stops)
    if (r.used && now - r.lastPoll > POLL_STALE_MS) { r.used = false; dirty = true; }
  for (int i = 0; i < MAX_SESSIONS; i++)
    if (sessions[i].used && now - sessions[i].lastSeen > 30UL * 60 * 1000) { sessions[i].used = false; dirty = true; }
}

// ----------------------------------------------------------- Wi-Fi/mDNS ---

static void wifiTick() {
  static uint32_t lastTry = 0;
  if (!ssid.length()) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (!mdnsUp && MDNS.begin(mdnsHost.c_str())) {
      MDNS.addService("http", "tcp", HTTP_PORT);
      MDNS.addService("claudestick", "tcp", HTTP_PORT);   // hooks find the Stick again if its IP changes
      MDNS.addServiceTxt("claudestick", "tcp", "id", devId.c_str());
      mdnsUp = true; dirty = true;
    }
    return;
  }
  if (mdnsUp) { MDNS.end(); mdnsUp = false; dirty = true; }
  if (millis() - lastTry > 15000) {
    lastTry = millis();
    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), pass.c_str());
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
  prefs.putString("ssid", ssid); prefs.putString("pass", pass);
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
      bleConnected() ? "connected" : "idle", setupDone ? "done" : "pending");
  } else if (line.startsWith("tilt ")) {
    String m = line.substring(5);
    if (m == "off") tiltMode = 0;
    else if (m == "flip") tiltMode = tiltMode < 0 ? 1 : -1;
    else tiltMode = 1;
    prefs.putChar("tilt", tiltMode);
    Serial.printf("ok tilt=%d\n", tiltMode);
  } else if (line == "setup") {
    setupDone = false; prefs.putBool("setup", false);
    Serial.println("ok, first-time setup re-opened");
  } else if (line == "reboot") {
    ESP.restart();
  } else if (line == "forget") {
    prefs.remove("ssid"); prefs.remove("pass"); prefs.putBool("setup", false); bleClearBonds();
    Serial.println("ok forgotten, rebooting"); delay(200); ESP.restart();
  } else {
    Serial.println("commands: wifi <ssid> <pass> | token <t> | name <n> | tilt on|off|flip | info | setup | reboot | forget");
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
  if (server.header("X-Token") == token || server.arg("t") == token) return true;
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
  out["name"] = devName; out["bt"] = devName; out["buddy"] = buddySpeciesIdx();
  out["volume"] = volume; out["tilt"] = tiltMode != 0; out["remote"] = remoteName(conf.remote);
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
  prefs.putString("ssid", ssid); prefs.putString("pass", pass);
  server.send(200, "application/json", "{\"ok\":true}");
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pass.c_str());   // non-blocking; the page polls /setup/info
  dirty = true;
}

static void hSetupPreview() {
  if (!setupAuthed()) return;
  JsonDocument in; if (!readBody(in)) return;
  buddySetSpeciesIdx(in["buddy"] | 0);
  heartUntil = millis() + 1500;
  touch();
  server.send(200, "application/json", "{\"ok\":true}");
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
  if (in["remote"].is<const char*>()) { String rm = in["remote"].as<const char*>(); conf.remote = rm == "off" ? 0 : rm == "on" ? 1 : 2; }
  else if (in["remote"].is<bool>()) conf.remote = in["remote"].as<bool>() ? 1 : 2;
  saveConfig(); updateRemote();
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
  server.on("/setup/preview", HTTP_POST, hSetupPreview);
  server.on("/setup/save", HTTP_POST, hSetupSave);
  server.on("/install.py", HTTP_GET, hInstallPy);
  server.on("/claudestick_hook.py", HTTP_GET, hHookPy);
  server.onNotFound([] { server.send(404, "application/json", "{\"error\":\"not found\"}"); });
}

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

static const int W = 240, H = 135;
static const int LEFT_W = 86;                 // buddy pane width
static const int RX = LEFT_W + 4;             // right pane text x
static const int RW = W - RX - 4;             // right pane width (24 chars)
static const int RCHARS = RW / 6;
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
        if (cut < width / 2) cut = width;
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

// Frame with a title tab on the top border and key hints on the bottom border.
static void frame(const String& title, uint16_t accent, const String& hints) {
  spr.drawRect(0, 4, W, H - 4, C_FRAME);
  spr.drawFastVLine(LEFT_W, 5, H - 6, C_FRAME);
  txt(" " + title + " ", 6, 0, accent);
  String r = " ";
  r += WiFi.status() == WL_CONNECTED ? "wf" : "--";
  r += bleConnected() ? " bt " : " ";
  r += String(M5.Power.getBatteryLevel()) + "%";
  String clk = clockStr();
  if (clk.length()) r += " " + clk;
  r += " ";
  txt(r, W - 6 - spr.textWidth(r), 0, C_DIM);
  if (hints.length()) txt(" " + hints + " ", 6, H - 8, C_DIM);
}

// Left pane: buddy, its name, and a one-word mood.
static void buddyPane(uint8_t state, const char* mood, uint16_t moodColor) {
  buddyDraw(state);
  String name = buddySpeciesName();
  txt(name, LEFT_W / 2 - spr.textWidth(name) / 2, 96, C_DIM);
  String m = mood;
  txt(m, LEFT_W / 2 - spr.textWidth(m) / 2, 107, moodColor);
}

// Scrollable text block in the right pane. Returns lines drawn.
static int textBlock(const String& text, int y, int maxLines, uint16_t color, bool scrollable) {
  static String lines[MAX_WRAP];
  static uint16_t colors[MAX_WRAP];
  int n = wrapLines(text, RCHARS, lines, MAX_WRAP, colors, color);
  int first = scrollable ? max(0, min(scrollLine, n - maxLines)) : 0;
  if (scrollable) scrollLine = first;
  int shown = 0;
  for (int i = first; i < n && shown < maxLines; i++, shown++)
    txt(lines[i], RX, y + shown * LINE_H, colors[i]);
  if (scrollable && n > maxLines) {   // scrollbar
    int trackY = y, trackH = maxLines * LINE_H - 2;
    int barH = max(4, trackH * maxLines / n);
    int barY = trackY + (trackH - barH) * first / max(1, n - maxLines);
    spr.drawFastVLine(W - 3, trackY, trackH, C_FRAME);
    spr.fillRect(W - 4, barY, 3, barH, C_CLAUDE);
  }
  return shown;
}

// Selectable list in the right pane starting at y.
static void listBlock(const String* labels, int n, int y, uint16_t accent, const String* marks = nullptr) {
  int rows = max(1, (BOTTOM - y) / 11);
  int start = max(0, min(menuIdx - rows + 1, n - rows));
  for (int r = 0; r < rows && start + r < n; r++) {
    int i = start + r;
    bool sel = i == menuIdx;
    int yy = y + r * 11;
    String mark = marks ? marks[i] : String(" ");
    String label = fit(labels[i], RCHARS - 2);
    if (sel) {
      spr.fillRect(RX - 2, yy - 1, RW + 2, 10, accent);
      txt(">" + mark + label, RX, yy, C_BG, accent);
    } else {
      txt(" " + mark + label, RX, yy, C_FG);
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

static String titleBase() { return remoteMode ? "claudestick:remote" : "claudestick"; }

// ---- screens

static uint32_t holdHintUntil = 0;   // "hold A to allow" nudge on risky cards

static void drawApproval(Approval* a) {
  bool risky = a->risk == "high";
  String hint = risky ? "hold A allow  B deny" : a->always.length() ? "A ok  hold A always  B no" : "A allow  B deny";
  frame(risky ? "!! danger !!" : a->src == SRC_BLE ? "approve? desktop" : "approve?", risky ? C_BAD : C_CLAUDE, hint);
  int y = TOP + 2;
  txt("$ " + fit(a->tool, RCHARS - 2), RX, y, risky ? C_BAD : C_CLAUDE); y += LINE_H;
  if (risky && a->riskWhy.length()) { txt(fit(a->riskWhy, RCHARS), RX, y, C_BAD); y += LINE_H; }
  txt("@" + fit(a->project, RCHARS - 1), RX, y, C_DIM); y += LINE_H;
  spr.drawFastHLine(RX, y, RW - 6, risky ? C_BAD : C_FRAME);
  int rows = (BOTTOM - (y + 4)) / LINE_H - (a->src == SRC_WIFI ? 1 : 0);
  textBlock(a->detail, y + 4, rows, C_FG, true);
  String foot = millis() < holdHintUntil ? "hold A to allow this one" :
                a->always.length() ? "always: " + a->always : a->src == SRC_WIFI ? "pwr: answer at keyboard" : "";
  if (foot.length()) txt(fit(foot, RCHARS), RX, BOTTOM - LINE_H + 2, millis() < holdHintUntil ? C_YEL : C_DIM);
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
  frame(q ? "question" : "done > next?", q ? C_CLAUDE : C_OK, "B next  A pick  hold A talk");
  txt("@" + fit(r->project, RCHARS - 1), RX, TOP + 2, C_DIM);
  int used = textBlock(q ? r->title : r->last, TOP + 2 + LINE_H, q ? 4 : 3, q ? C_FG : C_DIM, true);
  int y = TOP + 6 + (used + 1) * LINE_H;
  spr.drawFastHLine(RX, y - 4, RW - 6, C_FRAME);
  String labels[MAX_ITEMS];
  int n = choiceLabels(r, labels);
  listBlock(labels, n, y, q ? C_CLAUDE : C_OK);
}

enum MenuItem : uint8_t { MI_STOP, MI_MACRO, MI_REMOTE, MI_TIE, MI_BUDDY, MI_HELP, MI_INFO, MI_TOKEN, MI_SETUP, MI_BACK };
struct MenuEntry { MenuItem kind; int arg; };

static int menuEntries(MenuEntry* e, String* labels) {
  int n = 0;
  auto add = [&](MenuItem k, int arg, const String& l) { e[n] = { k, arg }; labels[n] = l; n++; };
  add(MI_STOP, 0, "stop now");
  for (int i = 0; i < nMacros; i++) add(MI_MACRO, i, "> " + macros[i].label);
  add(MI_REMOTE, 0, String("remote: ") + remoteName(conf.remote) + (conf.remote == 2 ? (remoteMode ? " (on)" : " (off)") : ""));
  add(MI_TIE, 0, pinned.length() ? "tie to session (tied)" : "tie to session");
  add(MI_BUDDY, 0, String("buddy: ") + buddySpeciesName());
  add(MI_HELP, 0, "button help");
  add(MI_INFO, 0, "info");
  add(MI_TOKEN, 0, "new token (hold A)");
  add(MI_SETUP, 0, "re-run setup");
  add(MI_BACK, 0, "back");
  return n;
}

static void drawActions() {
  Session* s = targetSession();
  frame("menu", C_CLAUDE, "B next  PWR up  A ok");
  txt(s ? ("-> " + fit(sessionLabel(*s), RCHARS - 3)) : String("no cli session"), RX, TOP + 2, C_DIM);
  String labels[MAX_ITEMS]; MenuEntry ents[MAX_ITEMS];
  int n = menuEntries(ents, labels);
  listBlock(labels, n, TOP + 4 + LINE_H, C_CLAUDE);
}

static void drawHelp() {
  frame("buttons", C_BLUE, "any key: back");
  int y = TOP + 2;
  auto line = [&](const String& s, uint16_t c) { txt(fit(s, RCHARS), RX, y, c); y += LINE_H; };
  line("A front  B side  P power", C_DIM);
  line("approve: A ok  B no", C_FG);
  line("  hold A: always allow", C_FG);
  line("  danger: hold A to ok", C_BAD);
  line("  P: answer at keyboard", C_FG);
  line("lists: B next  P back up", C_FG);
  line("  A pick  hold B: close", C_FG);
  line("hold A: talk to claude", C_CLAUDE);
  line("hold B: menu", C_FG);
  line("tilt: scroll long text", C_DIM);
  line("A+B at power-on: reset", C_DIM);
}

// Push-to-talk screens
static void drawVoiceCard() {
  if (voiceState == V_ERROR) {
    frame("voice", C_BAD, "any key: ok");
    textBlock(voiceErr, TOP + 10, 6, C_BAD, false);
    return;
  }
  Session* s = findSession(voiceTarget, false);
  frame(voiceStopReq ? "reply?" : "send to claude?", C_CLAUDE, "A send  B discard  hold A redo");
  txt("@" + fit(s ? sessionLabel(*s) : String("?"), RCHARS - 1), RX, TOP + 2, C_DIM);
  spr.drawFastHLine(RX, TOP + 2 + LINE_H, RW - 6, C_FRAME);
  textBlock("\"" + voiceText + "\"", TOP + 6 + LINE_H, 10, C_FG, true);
}

static void drawVoiceRec(uint32_t ms) {
  spr.fillScreen(C_BG);
  spr.setFont(&fonts::Font0); spr.setTextSize(1);
  bool rec = voiceState == V_REC;
  frame(rec ? "listening" : "transcribing", C_CLAUDE, rec ? "release A to send" : "");
  if (rec) {
    txt(String(ms / 1000) + "." + String((ms / 100) % 10) + "s / 20s", RX, TOP + 4, C_FG);
    int w = min(RW - 8, voiceLevel * (RW - 8) / 12000 + 2);
    spr.drawRect(RX, TOP + 20, RW - 6, 10, C_FRAME);
    spr.fillRect(RX + 1, TOP + 21, w, 8, C_CLAUDE);
    txt("speak now...", RX, TOP + 40, C_DIM);
  } else {
    txt("sending audio to", RX, TOP + 4, C_DIM);
    txt(fit(conf.sttUrl, RCHARS), RX, TOP + 4 + LINE_H, C_DIM);
  }
  buddyPane(rec ? B_ATTENTION : B_BUSY, rec ? "listening" : "thinking", C_CLAUDE);
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

static void drawSessions() {
  frame("tie to session", C_BLUE, "B next  PWR up  A pin");
  txt("* = stick answers these", RX, TOP + 2, C_DIM);
  String labels[MAX_ITEMS], marks[MAX_ITEMS], ids[MAX_ITEMS];
  int n = sessionItems(labels, marks, ids);
  listBlock(labels, n, TOP + 4 + LINE_H, C_BLUE, marks);
}

static void drawInfo() {
  frame("info", C_BLUE, "any key: back");
  int y = TOP + 2;
  auto line = [&](const String& s, uint16_t c) { txt(fit(s, RCHARS), RX, y, c); y += LINE_H; };
  line("fw " + String(FW_VERSION), C_FG);
  line("bt " + devName, C_FG);
  line("wifi " + (ssid.length() ? ssid : String("(none)")), WiFi.status() == WL_CONNECTED ? C_OK : C_BAD);
  line(WiFi.localIP().toString() + ":" + String(HTTP_PORT), C_FG);
  line(mdnsHost + ".local", C_DIM);
  line("token", C_DIM);
  line(token, C_CLAUDE);
  line("ok " + String(stats.appr) + "  no " + String(stats.deny), C_FG);
  line(String("tilt ") + (tiltMode == 0 ? "off" : tiltMode > 0 ? "on" : "flipped") + "  voice " + (conf.sttUrl.length() ? "on" : "off"), C_DIM);
  if (desk.lastBeat) line("desk tok " + String(desk.tokensToday), C_DIM);
}

static void drawPasskey(uint32_t pk) {
  frame("bluetooth pairing", C_BLUE, "");
  txt("enter this code", RX, TOP + 10, C_FG);
  txt("on your computer:", RX, TOP + 10 + LINE_H, C_FG);
  char b[8]; snprintf(b, sizeof b, "%06lu", (unsigned long)pk);
  spr.setTextSize(3);
  txt(b, RX, TOP + 40, C_CLAUDE);
  spr.setTextSize(1);
}

static void drawSetup() {
  bool up = WiFi.status() == WL_CONNECTED;
  frame(up ? "almost there" : "hello!", C_CLAUDE, "hold B: skip setup");
  int y = TOP + 3;
  auto line = [&](const String& s, uint16_t c) { txt(fit(s, RCHARS), RX, y, c); y += LINE_H; };
  if (up) {
    line("wi-fi ok! now open this", C_FG);
    line("on a phone or pc:", C_FG);
    y += 4;
    line("http://" + WiFi.localIP().toString(), C_CLAUDE);
    line("  :" + String(HTTP_PORT) + "/setup", C_CLAUDE);
    y += 4;
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

static void drawHome() {
  String title = titleBase();
  if (pinned.length()) title += " *";
  frame(title, remoteMode ? C_CLAUDE : C_FG, "A next  hold A talk  hold B menu");
  int y = TOP + 2;
  if (desk.lastBeat) {
    txt("desktop " + String(desk.running) + "/" + String(desk.total), RX, y, C_BLUE); y += LINE_H;
    txt("  " + fit(desk.msg.length() ? desk.msg : desk.entry, RCHARS - 2), RX, y, C_DIM); y += LINE_H + 3;
  }
  Session* tgt = targetSession();
  int shown = 0;
  for (auto& s : sessions) {
    if (!s.used || y > BOTTOM - LINE_H) continue;
    bool isT = &s == tgt;
    spr.fillRect(RX, y + 1, 4, 6, stateColor(s.state));
    String head = sessionLabel(s);
    if (pinned == s.id) head = "*" + head;
    txt(fit(head, RCHARS - 10), RX + 7, y, isT ? C_FG : C_DIM);
    String st = s.stopFlag ? "stop.." : s.inject.length() ? "msg.." : s.state;
    txt(st, W - 6 - spr.textWidth(st), y, stateColor(s.state));
    if (isT) spr.drawFastHLine(RX + 7, y + 8, spr.textWidth(fit(head, RCHARS - 10)), C_CLAUDE);
    y += LINE_H;
    if (s.detail.length() && y <= BOTTOM - LINE_H) { txt("  " + fit(s.detail, RCHARS - 2), RX, y, C_DIM); y += LINE_H; }
    y += 2; shown++;
  }
  if (!shown && !desk.lastBeat && ssid.length()) {
    txt(WiFi.status() == WL_CONNECTED ? "waiting for claude..." : "joining wifi...", RX, TOP + 30, C_DIM);
    if (WiFi.status() == WL_CONNECTED) txt(WiFi.localIP().toString(), RX, TOP + 30 + LINE_H, C_FRAME);
  }
}

static void draw() {
  spr.fillScreen(C_BG);
  spr.setFont(&fonts::Font0);
  spr.setTextSize(1);
  const char* mood; uint16_t moodColor;
  uint8_t bs = buddyState(&mood, &moodColor);

  uint32_t pk = blePasskey();
  Approval* a = topApproval();
  StopReq* r = topStop();
  uint32_t key = pk ? 1 : a ? a->id : r ? r->id : 0;
  if (voiceState == V_CONFIRM && !pk && !a) key = 0x7FFFFFFF;
  if (key != scrollKey) { scrollKey = key; scrollLine = 0; }

  if (pk) drawPasskey(pk);
  else if (a) drawApproval(a);
  else if (voiceState == V_CONFIRM || voiceState == V_ERROR) drawVoiceCard();
  else if (r) drawChoice(r);
  else if (!setupDone) drawSetup();
  else if (screen == SCR_ACTIONS) drawActions();
  else if (screen == SCR_SESSIONS) drawSessions();
  else if (screen == SCR_INFO) drawInfo();
  else if (screen == SCR_HELP) drawHelp();
  else drawHome();
  buddyPane(bs, mood, moodColor);
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
  float d = (ax - base) * tiltMode;
  if (d > 0.3f) { scrollLine++; dirty = true; touch(); }
  else if (d < -0.3f && scrollLine > 0) { scrollLine--; dirty = true; touch(); }
}

static void listNav(int total, bool next, bool prev) {
  if (next) { menuIdx = (menuIdx + 1) % total; beep(0); }
  if (prev) { menuIdx = (menuIdx + total - 1) % total; beep(0); }
}

static void buttons() {
  bool aClick = M5.BtnA.wasClicked(), aHold = M5.BtnA.wasHold();
  bool bClick = M5.BtnB.wasClicked(), bHold = M5.BtnB.wasHold();
  bool pClick = M5.BtnPWR.wasClicked();
  if (!(aClick || aHold || bClick || bHold || pClick)) return;

  if (dimmed) { touch(); return; }      // first press only wakes the screen
  touch();
  if (setupDone) noteStick();           // answering on the Stick = you're remote (auto mode)

  if (Approval* a = topApproval()) {
    bool risky = a->risk == "high";
    if (aClick && risky) { holdHintUntil = millis() + 2500; beep(3); }
    else if (aClick) decide(a, D_ALLOW);
    else if (aHold) decide(a, risky || !a->always.length() ? D_ALLOW : D_ALWAYS);
    else if (bClick) decide(a, D_DENY);
    else if (pClick && a->src == SRC_WIFI) decide(a, D_SKIP);
    return;
  }

  if (voiceState == V_ERROR) { voiceState = V_IDLE; return; }
  if (voiceState == V_CONFIRM) {
    if (aClick) voiceSend();
    else if (bClick || bHold) { voiceState = V_IDLE; beep(0); }
    else if (aHold) {   // redo, same target
      StopReq* card = nullptr;
      for (auto& x : stops) if (voiceStopReq && x.used && x.id == voiceStopReq && x.result == -1) card = &x;
      voiceCapture(card);
    }
    return;
  }

  if (StopReq* r = topStop()) {
    int total = r->nMacros + 1;
    listNav(total, bClick, pClick);
    if (aHold) { voiceCapture(r); return; }
    if (bHold || (aClick && menuIdx == total - 1)) { r->result = -2; menuIdx = 0; beep(0); }
    else if (aClick) { r->result = menuIdx; menuIdx = 0; beep(2); }
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
      case MI_STOP:   if (s) { s->stopFlag = true; beep(3); } screen = SCR_HOME; menuIdx = 0; break;
      case MI_MACRO:  if (s) { deliverText(s, macros[e.arg].text); beep(2); } screen = SCR_HOME; menuIdx = 0; break;
      case MI_REMOTE: setRemoteSetting(conf.remote + 1); break;                       // stay in menu
      case MI_TIE:    screen = SCR_SESSIONS; menuIdx = 0; break;
      case MI_BUDDY:  prefs.putUChar("buddy", buddyNextSpecies()); break;            // stay in menu
      case MI_HELP:   screen = SCR_HELP; menuIdx = 0; break;
      case MI_INFO:   screen = SCR_INFO; menuIdx = 0; break;
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
      if (pinned.length())
        for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].used && sessions[i].id == pinned) focusSession = i;
      beep(2); screen = SCR_HOME; menuIdx = 0;
    }
    return;
  }

  if (screen == SCR_INFO || screen == SCR_HELP) { screen = SCR_HOME; return; }

  if (!setupDone) {   // first-time setup screen: only "skip" is offered
    if (bHold) { setupDone = true; prefs.putBool("setup", true); beep(0); }
    return;
  }

  // home
  if (aHold) voiceCapture(nullptr);
  else if (bHold) { screen = SCR_ACTIONS; menuIdx = 0; beep(0); }
  else if (aClick) {   // cycle focused session
    for (int k = 1; k <= MAX_SESSIONS; k++) {
      int i = (focusSession + k + MAX_SESSIONS) % MAX_SESSIONS;
      if (sessions[i].used) { focusSession = i; break; }
    }
  }
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
  ssid  = prefs.getString("ssid", CS_WIFI_SSID);
  pass  = prefs.getString("pass", CS_WIFI_PASS);
  token = prefs.getString("token", CS_TOKEN);
  tiltMode = prefs.getChar("tilt", 1);
  loadConfig();
  setupDone = prefs.getBool("setup", false);
  volume = prefs.getUChar("vol", 120);
  M5.Speaker.setVolume(volume);
  M5.update();
  if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) {   // hold A+B while powering on: forget Wi-Fi, redo setup
    prefs.remove("ssid"); prefs.remove("pass"); prefs.putBool("setup", false); setupDone = false;
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
  WiFi.setSleep(true);     // required for Wi-Fi + BLE coexistence
  if (ssid.length()) WiFi.begin(ssid.c_str(), pass.c_str());
  else if (!setupDone) startAP();

  bleInit(devName.c_str());
  startHttp();
  lastActivity = millis();
  Serial.printf("ClaudeStick %s ready. Type 'info' for details.\n", FW_VERSION);
}

void loop() {
  M5.update();
  serialTick();
  wifiTick();
  server.handleClient();
  pollBle();
  expireStale();
  apTick();
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
  if (!dimmed && getCpuFrequencyMhz() != 240) setCpuFrequencyMhz(240);

  static uint32_t lastPk = 0;
  if (blePasskey() != lastPk) { lastPk = blePasskey(); touch(); }

  // ~5 fps keeps the buddy animated; dimmed screen refreshes slowly to save power
  uint32_t period = screenOff ? 5000 : dimmed ? 1000 : 200;
  if (dirty || millis() - lastDraw > period) {
    draw(); dirty = false; lastDraw = millis();
  }
  delay(5);
}
