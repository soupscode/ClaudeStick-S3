// The Stick as a bridge client: mirrors a bridge's state, sends answers back, copes with the network going away.
// The real firmware source runs here against stand-ins; the bridge's replies are scripted (see stub/common_impl.cpp).
#include "../../firmware/src/main.cpp"
#include <string>
#include <map>
#include <vector>
extern std::map<std::string, std::string> PREFS;
extern std::string REQ_BODY, LAST_SENT, HTTP_BODY;
extern int LAST_CODE, HTTP_CODE, TONES, TLS_STOPS, SCANS_STARTED, HEAP_FREE;
extern unsigned long NOW;
extern bool TLS_UP, SCAN_PENDING;
struct HttpScripted { int code; std::string body; };
extern std::vector<HttpScripted> HTTP_SCRIPT;
extern std::vector<std::string> HTTP_LOG;
extern std::vector<std::pair<std::string, int>> SCAN;
static int fails = 0;
static void check(const char* n, bool c, const std::string& x = "") { printf("%s %s%s\n", c ? "PASS" : "FAIL", n, c ? "" : (" | " + x).c_str()); if (!c) fails++; }
void M5Canvas::pushSprite(int, int) {}
static void at(long epoch) { struct timeval tv = { (time_t)epoch, 0 }; settimeofday(&tv, nullptr); }
static const long NOWEPOCH = 1793390000;

static const char* STATE1_TEXT_PTR();
#define STATE1_TEXT STATE1_TEXT_PTR()
static void script(int code, const std::string& body) { HTTP_SCRIPT.push_back({ code, body }); }
static std::string lastReq() { return HTTP_LOG.empty() ? "" : HTTP_LOG.back(); }
static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
static void pollNow() { bridgeNextPoll = 0; bridgeTick(); }
static std::string stateV(int v) { std::string s = STATE1_TEXT; s.replace(s.find("\"v\":1"), 5, "\"v\":" + std::to_string(v)); return s; }
static int countUsed() { int n = 0; for (auto& a : approvals) if (a.used) n++; return n; }

static const char* STATE1 = R"({"v":1,"now":1793390000,"bridge":1,"remote":true,"remote_setting":"auto","pinned":"",
 "sessions":[{"id":"sess-a","project":"api-server@main","state":"working","detail":"Edit main.py","since_s":65,"plan_done_n":0,
   "todo":{"done":1,"total":3,"skip":0,"now":"Write tests","items":[["c","Plan"],["i","Write tests"],["p","Ship"]]},
   "usage":{"in":10,"out":20,"cr":300,"cw":40,"ctx":5000,"model":"sonnet"},"ctx_pct":42.5,"model_name":"Opus 5.5"},
  {"id":"sess-b","project":"docs","state":"done","detail":"","since_s":3,"plan_done_n":0}],
 "tokens_today":12345,
 "limits":{"five":{"pct":61,"resets":1793393600},"seven":{"pct":12,"resets":1793890000},"age_s":30},
 "approvals":[{"id":1000001,"tool":"Bash","detail":"npm run build","project":"api-server@main","session":"sess-a","src":"wifi","always":"npm run *","risk":"","risk_why":""}],
 "choices":[{"id":1000002,"kind":"done","project":"docs","session":"sess-b","title":"Done","last":"All tests pass","options":["Continue","Commit"]}],
 "macros":["Continue","Keep going","Commit"]})";

static const char* STATE1_TEXT_PTR() { return STATE1; }

int main() {
  // ---- boot with a bridge saved
  PREFS.clear(); PREFS["setup"] = "1"; PREFS["ssid"] = "home-wifi"; PREFS["brurl"] = "https://stick.example.com"; PREFS["brtok"] = "tok123";
  NOW = 100000; setup(); WiFi.st = WL_CONNECTED; at(NOWEPOCH);
  check("bridge address + token loaded from settings", bridgeUrl == "https://stick.example.com" && bridgeTok == "tok123" && bridgeConfigured());
  check("not connected until the first poll", !bridgeUp);

  // ---- first poll
  script(200, STATE1);
  TONES = 0; HTTP_LOG.clear(); pollNow();
  std::string r = lastReq();
  check("polls GET /state with version and the lite flag", has(r, "GET https://stick.example.com/state?n=1&v=0"), r);
  check("...sending the token and who it is", has(r, "X-Token: tok123") && has(r, "X-Client: stick"), r);
  check("connected", bridgeUp && bridgeFails == 0 && bridgeV == 1);
  Session* a = findSession("sess-a", false);
  check("sessions mirrored, flagged as bridge", a && a->bridge && a->project == "api-server@main" && a->state == "working" && a->detail == "Edit main.py");
  check("state age carried over", a && NOW - a->stateSince >= 64000 && NOW - a->stateSince <= 66000);
  check("to-do mirrored", a && a->todoTotal == 3 && a->todoDone == 1 && a->nTodo == 3 && a->todoNow == "Write tests" && a->todo[1].st == 'i');
  check("usage + context mirrored", a && a->hasUsage && a->tokCR == 300 && a->ctx == 5000 && a->ctxPct > 42.4f && a->ctxPct < 42.6f && a->modelName == "Opus 5.5");
  check("limits mirrored", lim.set && lim.five.pct == 61 && lim.seven.pct == 12 && lim.five.resets == 1793393600UL);
  check("limits keep their age", NOW - lim.at >= 29000 && NOW - lim.at <= 31000);
  check("tokens today come from the bridge", tokensToday() == 12345, std::to_string(tokensToday()));
  check("remote mode comes from the bridge", remoteMode && bridgeRemote);
  Approval* ap = topApproval();
  check("approval mirrored", ap && ap->bridge && ap->id == 1000001 && ap->tool == "Bash" && ap->detail == "npm run build" &&
                             ap->always == "npm run *" && ap->session == "sess-a" && ap->project == "api-server@main");
  StopReq* st = topStop();
  check("Done card mirrored with the bridge's options", st && st->bridge && st->id == 1000002 && st->nMacros == 2 &&
                                                       st->macros[1].label == "Commit" && st->session == "sess-b");
  check("bridge macro labels kept apart from the Stick's own", nBridgeMacro == 3 && bridgeMacro[2] == "Commit");
  check("first look is a baseline: no chime for what was already there", TONES == 0, std::to_string(TONES));

  // ---- nothing changed: tiny reply, nothing disturbed
  script(200, R"({"v":1,"now":1793390001,"same":true})");
  pollNow();
  check("asks for the version it holds", has(lastReq(), "v=1"), lastReq());
  check("'same' changes nothing", countUsed() == 1 && findSession("sess-a", false) && bridgeUp);
  check("polling slows down while the screen is dimmed", [&] { dimmed = true; script(200, R"({"v":1,"same":true})"); pollNow(); uint32_t d = bridgeNextPoll - millis(); dimmed = false; return d >= BRIDGE_POLL_DIM_MS - 50; }());

  // ---- answering an approval
  HTTP_LOG.clear(); script(200, R"({"ok":true})");
  decide(ap, D_ALLOW);
  check("approve -> POST /act with the approval id", has(lastReq(), "POST https://stick.example.com/act") && has(lastReq(), "\"action\":\"approve\"") &&
                                                      has(lastReq(), "\"id\":1000001") && has(lastReq(), "Content-Type: application/json"), lastReq());
  check("...and the card goes away at once", !topApproval());
  check("...then the next poll is immediate", bridgeNextPoll == 0);

  script(200, STATE1); pollNow();   // the bridge still lists it (e.g. we answered a moment too late): it comes back
  ap = topApproval();
  check("an approval the bridge still lists is shown again", ap && ap->id == 1000001);
  HTTP_LOG.clear(); script(200, R"({"ok":true})");
  ap->always = ""; decide(ap, D_ALWAYS);
  check("always when none is offered -> plain approve", has(lastReq(), "\"action\":\"approve\""), lastReq());

  script(200, STATE1); pollNow(); ap = topApproval();
  HTTP_LOG.clear(); script(200, R"({"ok":true})");
  ap->reason = "not that folder"; decide(ap, D_DENY);
  check("deny carries what you said", has(lastReq(), "\"action\":\"deny\"") && has(lastReq(), "\"text\":\"not that folder\""), lastReq());

  script(200, STATE1); pollNow(); ap = topApproval();
  HTTP_LOG.clear(); script(200, R"({"ok":true})");
  decide(ap, D_SKIP);
  check("answer-at-keyboard -> 'keyboard'", has(lastReq(), "\"action\":\"keyboard\""), lastReq());

  script(200, STATE1); pollNow(); ap = topApproval();
  script(404, R"({"ok":false})");
  decide(ap, D_ALLOW);
  check("404 = someone else already answered: card goes, no error", !topApproval() && !(noticeText.length() && noticeText.startsWith("error")));

  // an unreachable bridge keeps the card up and says so
  script(200, STATE1); pollNow(); ap = topApproval();
  script(-1, ""); noticeUntil = 0; int stats0 = stats.appr;
  decide(ap, D_ALLOW);
  check("can't reach the bridge: the card stays", topApproval() && topApproval()->id == 1000001);
  check("...with a notice", noticeUntil != 0 && noticeTitle == "bridge", noticeText.c_str());
  check("...and nothing counted as approved", stats.appr == stats0);
  script(401, R"({"error":"bad token"})"); noticeUntil = 0;
  decide(topApproval(), D_ALLOW);
  check("token rejected on an answer: says so", noticeText == "token rejected", noticeText.c_str());

  // ---- Done card
  st = topStop(); menuIdx = 1;
  HTTP_LOG.clear(); script(200, R"({"ok":true})");
  check("choosing an option", stopResult(st, 1));
  check("-> POST /act choose with the index", has(lastReq(), "\"action\":\"choose\"") && has(lastReq(), "\"id\":1000002") && has(lastReq(), "\"index\":1"), lastReq());
  check("...and the card goes away", !topStop());
  script(200, STATE1); pollNow(); st = topStop();
  HTTP_LOG.clear(); script(200, R"({"ok":true})");
  stopResult(st, -2);
  check("dismiss", has(lastReq(), "\"action\":\"dismiss\""), lastReq());
  script(200, STATE1); pollNow(); st = topStop();
  script(-1, "");
  check("choose fails: the card stays", !stopResult(st, 0) && topStop());

  // ---- talking to a session, stopping it, macros
  Session* sa = findSession("sess-a", false);
  HTTP_LOG.clear(); script(200, R"({"ok":true})");
  check("say", deliverText(sa, "also update the docs"));
  check("-> POST /act say with the session and text", has(lastReq(), "\"action\":\"say\"") && has(lastReq(), "\"session\":\"sess-a\"") && has(lastReq(), "also update the docs"), lastReq());
  HTTP_LOG.clear(); script(200, R"({"ok":true})");
  sessionStop(sa);
  check("stop now", has(lastReq(), "\"action\":\"stop\"") && has(lastReq(), "\"session\":\"sess-a\""), lastReq());
  check("...no local stop flag (the bridge hands it to the hook)", !sa->stopFlag);
  HTTP_LOG.clear(); script(200, R"({"ok":true})");
  bridgeSessionAct(sa, "macro", 2);
  check("macro by index", has(lastReq(), "\"action\":\"macro\"") && has(lastReq(), "\"index\":2"), lastReq());
  focusSession = -1;
  MenuEntry ents[MAX_ITEMS]; String labels[MAX_ITEMS];
  int n = menuEntries(ents, labels);
  bool bridgeMacros = false; for (int i = 0; i < n; i++) if (ents[i].kind == MI_MACRO && labels[i] == "> Keep going") bridgeMacros = true;
  check("the actions menu lists the bridge's macros for a bridge session", bridgeMacros);
  HTTP_LOG.clear(); script(200, R"({"ok":true})"); setRemoteSetting(1);
  check("remote setting goes to the bridge", has(lastReq(), "\"action\":\"remote\"") && has(lastReq(), "\"setting\":\"on\""), lastReq());
  HTTP_LOG.clear(); script(200, R"({"ok":true})"); bridgePin("sess-a");
  check("tie to session goes to the bridge", has(lastReq(), "\"action\":\"pin\"") && has(lastReq(), "\"session\":\"sess-a\""), lastReq());

  // ---- changes since the last poll: alerts, removals, pinned
  std::string s2 = STATE1;
  s2.replace(s2.find("\"v\":1"), 5, "\"v\":2");
  s2.replace(s2.find("\"state\":\"working\""), 17, "\"state\":\"done\"");
  s2.replace(s2.find("\"plan_done_n\":0"), 15, "\"plan_done_n\":1");
  s2.replace(s2.find("\"pinned\":\"\""), 11, "\"pinned\":\"sess-a\"");
  TONES = 0; script(200, s2); pollNow();
  check("a session finishing chimes", TONES > 0);
  check("to-do completing chimes too", sa->planDone == 1);
  check("pinned follows the bridge", pinned == "sess-a");
  s2.replace(s2.find("\"v\":2"), 5, "\"v\":3");
  s2.replace(s2.find("\"pinned\":\"sess-a\""), 17, "\"pinned\":\"\"");
  script(200, s2); pollNow();
  check("unpinned on the bridge -> unpinned here", pinned == "");
  // an approval answered elsewhere disappears
  std::string s3 = R"({"v":4,"now":1793390100,"remote":false,"sessions":[],"approvals":[],"choices":[],"macros":[]})";
  script(200, s3); pollNow();
  check("approvals answered elsewhere disappear", !topApproval() && !topStop());
  check("sessions that end disappear", !findSession("sess-a", false) && !findSession("sess-b", false));
  check("remote follows the bridge (off)", !remoteMode);
  check("limits stay (account-wide, and they carry their age)", lim.set && lim.five.pct == 61);
  check("tokens today follow the bridge", tokensToday() == 0);

  // ---- bridge-origin entries are never expired by the hook-side timers
  script(200, stateV(5)); pollNow();
  NOW += 20UL * 60 * 1000;   // 20 minutes of silence from the bridge's own session, no hook polling approvals
  expireStale();
  check("hook-style expiry leaves bridge cards and sessions alone", topApproval() && topStop() && findSession("sess-a", false));
  check("a session tied to a vanished one is untied", [&] { pinned = "gone"; expireStale(); return pinned == ""; }());

  // ---- failure: two misses drop everything from the bridge; recovery re-baselines quietly
  script(-1, ""); pollNow();
  check("one miss: still showing what we had, retrying soon", bridgeUp && bridgeFails == 1 && topApproval());
  script(-1, ""); pollNow();
  check("two misses: offline, bridge items gone (their prompts expired at home too)", !bridgeUp && !topApproval() && !topStop() && !findSession("sess-a", false));
  check("offline state is visible (red icon source)", bridgeFails == 2 && bridgeConfigured());
  check("back-off after two misses: 6 s", bridgeNextPoll - millis() == 6000, std::to_string(bridgeNextPoll - millis()));
  script(-1, ""); pollNow();
  check("...12 s after three", bridgeNextPoll - millis() == 12000, std::to_string(bridgeNextPoll - millis()));
  for (int i = 0; i < 12; i++) { script(-1, ""); pollNow(); }
  check("back-off is capped at a minute", bridgeNextPoll - millis() == BRIDGE_POLL_MAX_MS, std::to_string(bridgeNextPoll - millis()));
  TONES = 0;
  script(200, stateV(6)); pollNow();
  check("reconnected", bridgeUp && bridgeFails == 0 && topApproval());
  check("reconnecting says so with one click, and doesn't chime for old news", TONES == 1 && noticeText == "connected again", std::to_string(TONES) + " " + noticeText.c_str());

  // ---- losing the link when nobody is watching the screen
  script(200, stateV(7)); pollNow();
  TONES = 0; noticeUntil = 0; script(-1, ""); pollNow();
  check("one miss is quiet", TONES == 0 && noticeUntil == 0);
  script(-1, ""); pollNow();
  check("two misses: a notice and a tone, because Claude may be waiting on you", noticeTitle == "bridge" && noticeText.startsWith("lost:") && TONES > 0, noticeText.c_str());

  // ---- error replies
  script(401, R"({"error":"bad token"})"); pollNow();
  check("401 -> 'token rejected'", bridgeErr == "token rejected", bridgeErr.c_str());
  script(429, R"({"error":"too many failed attempts"})"); pollNow();
  check("429 -> waits", has(bridgeErr.c_str(), "too many attempts"), bridgeErr.c_str());
  script(200, "<html>nginx welcome</html>"); pollNow();
  check("a web server that isn't a bridge is recognised", bridgeErr == "not a ClaudeStick bridge", bridgeErr.c_str());
  script(200, "{\"x\":1}"); pollNow();
  check("JSON without a version too", bridgeErr == "not a ClaudeStick bridge", bridgeErr.c_str());
  script(200, STATE1); pollNow();
  check("and it recovers", bridgeUp && bridgeErr == "");

  // ---- clock, Wi-Fi, memory, plain http
  HTTP_LOG.clear(); at(1); bridgeFails = 0; pollNow();
  check("https waits for the clock (certificates have dates)", HTTP_LOG.empty() && bridgeErr == "waiting for the clock", bridgeErr.c_str());
  at(NOWEPOCH);
  HEAP_FREE = 20000; TLS_UP = false; HTTP_LOG.clear(); pollNow();
  check("no TLS session without enough free memory", HTTP_LOG.empty() && bridgeErr == "low memory", bridgeErr.c_str());
  HEAP_FREE = 150000;
  script(200, stateV(8)); pollNow(); noticeUntil = 0; TONES = 0;
  WiFi.st = WL_IDLE_STATUS; HTTP_LOG.clear(); TLS_UP = true; int stops0 = TLS_STOPS;
  bridgeTick();
  check("Wi-Fi dropping says so", noticeText == "lost: Wi-Fi dropped" && TONES > 0, noticeText.c_str());
  check("Wi-Fi down: no requests, bridge items dropped, connection released", HTTP_LOG.empty() && !bridgeUp && !topApproval() && TLS_STOPS > stops0);
  WiFi.st = WL_CONNECTED;
  bridgeSave("http://192.168.1.20:7890", "tok123"); at(1);
  script(200, R"({"v":1,"now":1,"remote":false,"sessions":[],"approvals":[],"choices":[],"macros":[]})"); HTTP_LOG.clear(); pollNow();
  check("plain http to a LAN bridge needs no clock and no TLS", bridgeUp && has(lastReq(), "GET http://192.168.1.20:7890/state"), lastReq());
  bridgeSave("https://stick.example.com", "tok123"); at(NOWEPOCH);
  check("saving the address drops the old connection and state", !bridgeUp && bridgeV == 0 && PREFS["brurl"] == "https://stick.example.com");

  // ---- the setup page's endpoints
  check("address cleanup", bridgeCleanUrl(" https://a.example.com/ ") == "https://a.example.com" && bridgeCleanUrl("http://10.0.0.5:7890") == "http://10.0.0.5:7890" &&
                           bridgeCleanUrl("a.example.com") == "" && bridgeCleanUrl("https://a.example.com/path") == "" && bridgeCleanUrl("ftp://x") == "" &&
                           bridgeCleanUrl("https://u:p@a.example.com") == "" && bridgeCleanUrl("https://a b") == "");
  token = ""; setupDone = true;
  REQ_BODY = R"({"url":"not a url","token":"x"})"; hSetupBridge();
  check("/setup/bridge: bad address -> 400", LAST_CODE == 400 && has(LAST_SENT, "https://stick.example.com"), LAST_SENT);
  REQ_BODY = R"({"url":"https://new.example.com","token":""})"; hSetupBridge();
  check("/setup/bridge: a new address needs a token", LAST_CODE == 400 && has(LAST_SENT, "token"), LAST_SENT);
  script(200, STATE1); REQ_BODY = R"({"url":"https://new.example.com/","token":"newtok"})"; hSetupBridge();
  check("/setup/bridge: saves, tries it, reports success", has(LAST_SENT, "\"ok\":true") && PREFS["brurl"] == "https://new.example.com" && PREFS["brtok"] == "newtok" && bridgeUp, LAST_SENT);
  script(401, "{}"); REQ_BODY = R"({"url":"https://new.example.com","token":"wrong"})"; hSetupBridge();
  check("/setup/bridge: wrong token -> a clear message", has(LAST_SENT, "\"ok\":false") && has(LAST_SENT, "rejected that token"), LAST_SENT);
  script(200, STATE1); REQ_BODY = R"({"url":"https://new.example.com","token":""})"; hSetupBridge();
  check("/setup/bridge: blank token keeps the saved one for the same address", PREFS["brtok"] == "wrong" && has(LAST_SENT, "\"ok\":true"), PREFS["brtok"]);
  script(404, "{}"); REQ_BODY = R"({"url":"https://elsewhere.example.com","token":"t"})"; hSetupBridge();
  check("/setup/bridge: a site that isn't a bridge", has(LAST_SENT, "isn't a ClaudeStick bridge"), LAST_SENT);
  WiFi.st = WL_IDLE_STATUS; REQ_BODY = R"({"url":"https://elsewhere.example.com","token":"t"})"; hSetupBridge();
  check("/setup/bridge: not on Wi-Fi yet -> says so", has(LAST_SENT, "isn't on Wi-Fi"), LAST_SENT);
  WiFi.st = WL_CONNECTED;
  hSetupInfo();
  check("/setup/info shows the bridge but never its token", has(LAST_SENT, "\"link\":{") && has(LAST_SENT, "elsewhere.example.com") && !has(LAST_SENT, "brtok") &&
                                                           !has(LAST_SENT, "tok123") && !has(LAST_SENT, "\"wrong\""), LAST_SENT);
  REQ_BODY = R"({"url":""})"; hSetupBridge();
  check("/setup/bridge: empty address turns it off", !bridgeConfigured() && PREFS["brurl"] == "" && has(LAST_SENT, "\"set\":false"), LAST_SENT);

  // ---- speech to text through the bridge
  bridgeSave("https://stick.example.com", "tok123"); bridgeUp = true;
  String text; uint8_t wav[64] = {};
  script(200, R"({"text":"  run the tests  "})");
  check("bridgeStt: text back, trimmed", bridgeStt(wav, sizeof wav, text) && text == "run the tests");
  check("...as a WAV POST to /stt", has(lastReq(), "POST https://stick.example.com/stt") && has(lastReq(), "Content-Type: audio/wav"), lastReq());
  script(503, R"({"error":"voice isn't set up on the bridge: add a transcription URL in its settings"})");
  check("bridgeStt: the bridge's own message is shown", !bridgeStt(wav, sizeof wav, text) && has(voiceErr.c_str(), "voice isn't set up on the bridge"), voiceErr.c_str());
  bridgeUp = false;

  // ---- what a real bridge sends (tests/py/gen_bridge_fixture.py writes it from the bridge's own code)
  {
    FILE* f = fopen("out/bridge_state.json", "rb");
    check("real-bridge fixture present (tests/host/run.sh writes it)", f != nullptr);
    if (f) {
      std::string body; char buf[4096]; size_t n;
      while ((n = fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, n);
      fclose(f);
      bridgeSave("https://stick.example.com", "tok123"); bridgeDrop(); pinned = ""; WiFi.st = WL_CONNECTED; at(NOWEPOCH);
      script(200, body); TONES = 0; pollNow();
      Session* r1 = findSession("real-1", false); Session* r2 = findSession("real-2", false);
      check("real bridge: connected", bridgeUp, bridgeErr.c_str());
      check("real bridge: sessions", r1 && r2 && r1->bridge && r1->project == "api-server@main" && r1->state == "approve" && r2->project == "docs" && r2->state == "done");
      check("real bridge: to-do", r1 && r1->todoTotal == 3 && r1->todoDone == 1 && r1->todoNow == "Write tests" && r1->nTodo == 3 && r1->todo[2].st == 'p' && r1->todo[0].text == "Plan");
      check("real bridge: usage, context and model", r1 && r1->hasUsage && r1->tokOut == 20 && r1->tokCW == 40 && r1->ctx == 5000 && r1->model == "sonnet" &&
                                                    r1->ctxPct > 42.4f && r1->ctxPct < 42.6f && r1->modelName == "Opus 5.5");
      check("real bridge: plan-done counter", r2 && r2->planDone == 1);
      check("real bridge: limits and tokens", lim.set && lim.five.pct == 61 && lim.seven.resets == 1793890000UL && tokensToday() == 4242, std::to_string(tokensToday()));
      check("real bridge: remote on, pinned", remoteMode && pinned == "real-1");
      Approval* ra = topApproval();
      check("real bridge: approval", ra && ra->bridge && ra->id == 1000000 && ra->tool == "Bash" && ra->detail == "npm run build" && ra->always == "npm run *" &&
                                     ra->session == "real-1" && ra->project == "api-server@main");
      StopReq* rs = topStop();
      check("real bridge: Done card with the bridge's macros", rs && rs->kind == "done" && rs->session == "real-2" && rs->last == "All tests pass" &&
                                                              rs->nMacros == 6 && rs->macros[5].label == "Commit");
      check("real bridge: macro labels", nBridgeMacro == 6 && bridgeMacro[0] == "Continue");
      check("real bridge: first look is quiet (just the reconnect click)", TONES <= 1, std::to_string(TONES));
      HTTP_LOG.clear(); script(200, R"({"ok":true})");
      decide(ra, D_ALWAYS);
      check("real bridge: the answer names the id the bridge gave", has(lastReq(), "\"id\":1000000") && has(lastReq(), "\"action\":\"always\""), lastReq());
      bridgeDrop(); pinned = ""; bridgeUp = false;
    }
  }

  // ---- several Wi-Fi networks
  PREFS.clear(); PREFS["setup"] = "1"; PREFS["ssid"] = "home-wifi"; PREFS["pass"] = "goodpass";
  NOW = 200000; setup(); WiFi.st = WL_CONNECTED; token = "";
  check("one saved network (older firmware) becomes the list", nNets == 1 && nets[0].ssid == "home-wifi");
  REQ_BODY = R"({"ssid":"Phone hotspot","pass":"hotspotpw"})"; hSetupWifiAdd();
  check("add another network", has(LAST_SENT, "\"ok\":true") && nNets == 2 && nets[0].ssid == "home-wifi" && nets[1].ssid == "Phone hotspot");
  check("...saved, primary still kept where older firmware looks", PREFS["ssid"] == "home-wifi" && has(PREFS["nets"], "Phone hotspot"));
  REQ_BODY = R"({"ssid":"Phone hotspot","pass":"newpw"})"; hSetupWifiAdd();
  check("adding the same name updates its password", nNets == 2 && nets[1].pass == "newpw");
  REQ_BODY = R"({"ssid":"","pass":"x"})"; hSetupWifiAdd();
  check("empty name -> 400", LAST_CODE == 400);
  addNet("Office", "pw3", false); addNet("Cafe", "pw4", false);
  REQ_BODY = R"({"ssid":"Fifth","pass":"pw5"})"; hSetupWifiAdd();
  check("four is the limit (409)", LAST_CODE == 409 && nNets == 4);
  hSetupInfo();
  check("/setup/info lists the saved names, not their passwords", has(LAST_SENT, "\"saved\":[\"home-wifi\",\"Phone hotspot\",\"Office\",\"Cafe\"]") && !has(LAST_SENT, "newpw"), LAST_SENT);
  REQ_BODY = R"({"ssid":"Office"})"; hSetupWifiForget();
  check("forget one", nNets == 3 && nets[2].ssid == "Cafe");
  ssid = "Cafe"; pass = "pw4"; forgetNet("Cafe");
  check("forgetting the network we're aiming at retargets the next one", nNets == 2 && ssid == "home-wifi" && pass == "goodpass", ssid.c_str());
  addNet("Cafe", "pw4", false);
  // at home: both in range, home is stronger
  SCAN = { { "Phone hotspot", -60 }, { "home-wifi", -45 }, { "stranger", -30 } };
  pickNetAtBoot();
  check("boot picks the strongest known network", ssid == "home-wifi" && pass == "goodpass", ssid.c_str());
  // at work: only the hotspot is in range, and we're disconnected
  SCAN = { { "stranger", -30 }, { "Phone hotspot", -70 } };
  WiFi.st = WL_IDLE_STATUS; NOW += 20000; SCAN_PENDING = true; SCANS_STARTED = 0;
  wifiTick();
  check("lost the network: scans (without freezing the screen)", SCANS_STARTED == 1);
  wifiTick();
  check("...and waits for the scan to finish", ssid == "home-wifi" || ssid == "Phone hotspot");
  SCAN_PENDING = false; wifiTick();
  check("...then joins the known network that is in range", ssid == "Phone hotspot" && pass == "newpw", ssid.c_str());
  // nothing known in sight: take turns instead of getting stuck on one
  SCAN = { { "stranger", -30 } }; WiFi.st = WL_IDLE_STATUS; NOW += 20000; SCAN_PENDING = true; wifiTick(); SCAN_PENDING = false;
  std::string first = ssid.c_str(); wifiTick();
  check("nothing known in sight: tries the next one", std::string(ssid.c_str()) != first, ssid.c_str());
  // setting up a new primary
  connectWifi("Brand new", "pw", 100);
  check("a network set up on the Stick becomes the first choice", nets[0].ssid == "Brand new" && PREFS["ssid"] == "Brand new" && nNets == 4);
  forgetAllNets();
  check("forget all", nNets == 0 && PREFS.count("nets") == 0 && PREFS.count("ssid") == 0 && ssid == "");

  printf("\nbridge client: %s\n", fails ? "FAILED" : "all passed");
  return fails;
}
