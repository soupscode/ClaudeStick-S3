// ------------------------------------------------------- bridge client ---
// Away from your home network the Stick can't be reached by your machines' hooks, so it asks a
// ClaudeStick bridge instead (bridge/claudestick_bridge.py, running at home, usually behind an HTTPS
// reverse proxy). The Stick polls the bridge's /state, shows what it finds exactly as if the hooks
// had talked to it directly, and sends your answers back with POST /act.
// Included from main.cpp; shares its state. Everything here runs on the main loop (no tasks, no locks).

static const uint32_t BRIDGE_POLL_ACTIVE_MS = 1500;    // screen on: approvals show up within a couple of seconds
static const uint32_t BRIDGE_POLL_DIM_MS    = 4000;    // dimmed: spare the battery
static const uint32_t BRIDGE_POLL_MAX_MS    = 60000;   // longest back-off while the bridge can't be reached
static const size_t   BRIDGE_MIN_BLOCK      = 32768;   // internal RAM needed for a TLS session (same bar as OTA)
static const size_t   BRIDGE_MAX_BODY       = 48 * 1024;

static String   bridgeUrl, bridgeTok;       // "https://stick.example.com" (no trailing slash) and its token
// bridgeUp (the last polls worked) and bridgeRemote (the bridge's verdict on remote mode) are declared in main.cpp.
static bool     bridgeEverUp = false;       // it has worked at least once since boot (so "back again" is worth a notice)
static bool     bridgeSynced = false;       // first state since (re)connecting is a baseline: no alerts for it
static uint32_t bridgeV = 0;                // version of the state we hold (0 = none)
static uint32_t bridgeNextPoll = 0;
static uint8_t  bridgeFails = 0, bridgeGen = 0;
static String   bridgeErr;                  // short reason, for the setup page and the info screen
static uint32_t bridgeTokens = 0;           // tokens today, as the bridge counts them
static String   bridgeMacro[MAX_MACROS];    // the bridge's macro labels (the Stick's own list is for direct hooks)
static int      nBridgeMacro = 0;

static WiFiClientSecure bridgeTls;
static WiFiClient       bridgePlain;
static HTTPClient       bridgeHttp;
static bool             bridgeTlsReady = false;

static bool bridgeConfigured() { return bridgeUrl.length() && bridgeTok.length(); }

// "https://host[:port]" or "http://host[:port]", no path. Returns "" if it isn't one.
static String bridgeCleanUrl(String u) {
  u.trim();
  while (u.endsWith("/")) u.remove(u.length() - 1);
  if (u.length() > 200 || !(u.startsWith("https://") || u.startsWith("http://"))) return "";
  String rest = u.substring(u.indexOf("://") + 3);
  if (!rest.length() || rest.indexOf('/') >= 0 || rest.indexOf(' ') >= 0 || rest.indexOf('?') >= 0 || rest.indexOf('@') >= 0) return "";
  return u;
}

static void bridgeSave(const String& url, const String& tok) {
  bridgeUrl = url; bridgeTok = tok;
  prefs.putString("brurl", bridgeUrl); prefs.putString("brtok", bridgeTok);
  bridgeUp = false; bridgeSynced = false; bridgeV = 0; bridgeFails = 0; bridgeNextPoll = 0; bridgeErr = "";
  bridgeHttp.end(); bridgeTls.stop(); bridgePlain.stop();
}

// One request to the bridge. Returns the HTTP status, or a negative number when nothing was exchanged.
static int bridgeRequest(const char* method, const String& path, const String& body, String& out,
                         uint16_t timeoutMs = 3500, const char* ctype = "application/json", const uint8_t* raw = nullptr, size_t rawLen = 0,
                         bool auth = true) {
  bool https = bridgeUrl.startsWith("https://");
  if (https) {
    if (!clockOk()) { bridgeErr = "waiting for the clock"; return -100; }   // certificates need the date
    if (!bridgeTls.connected()) {
      bridgeTls.stop();   // frees what a dropped session left behind
      if (internalBlock() < BRIDGE_MIN_BLOCK) { bridgeErr = "low memory"; return -101; }
    }
    if (!bridgeTlsReady) {
      bridgeTls.setCACertBundle(rootca_crt_bundle_start);
      bridgeTls.setHandshakeTimeout(8000);   // the default is two minutes: far too long to freeze the screen
      bridgeTlsReady = true;
    }
  } else if (!bridgePlain.connected()) bridgePlain.stop();
  bool ok = https ? bridgeHttp.begin(bridgeTls, bridgeUrl + path) : bridgeHttp.begin(bridgePlain, bridgeUrl + path);
  if (!ok) { bridgeErr = "bad address"; return -1; }
  bridgeHttp.setReuse(true);                     // keep the connection (and its TLS session) between polls
  bridgeHttp.setConnectTimeout(4000);
  bridgeHttp.setTimeout(timeoutMs);
  if (auth) bridgeHttp.addHeader("X-Token", bridgeTok);   // trading a pairing code for the token is the one call without it
  bridgeHttp.addHeader("X-Client", "stick");
  if (body.length() || rawLen) bridgeHttp.addHeader("Content-Type", ctype);
  int code;
  if (rawLen) code = bridgeHttp.sendRequest(method, (uint8_t*)raw, rawLen);
  else code = bridgeHttp.sendRequest(method, (uint8_t*)body.c_str(), body.length());
  if (code > 0) {
    int len = bridgeHttp.getSize();
    if (len > (int)BRIDGE_MAX_BODY) { bridgeHttp.end(); bridgeTls.stop(); bridgePlain.stop(); bridgeErr = "reply too large"; return -2; }
    out = bridgeHttp.getString();
  } else bridgeErr = HTTPClient::errorToString(code);
  bridgeHttp.end();
  // A TLS session holds ~20 KB of internal RAM. If memory is getting tight, let it go and reconnect next time.
  if (https && internalBlock() < 14000) bridgeTls.stop();
  return code;
}

static bool bridgePost(JsonDocument& body, const char* path = "/act") {
  String in, out; serializeJson(body, in);
  int code = bridgeRequest("POST", path, in, out);
  if (code == 200) return true;
  if (code == 404 && String(path) == "/act") return true;   // already handled somewhere else; the next poll clears it
  notice("bridge", code == 401 ? "token rejected" : code > 0 ? "error " + String(code) : "can't reach it: " + (bridgeErr.length() ? bridgeErr : String("offline")), C_BAD_U, 3500);
  return false;
}

// -- answers going back to the bridge (the Stick's UI calls these where it would otherwise answer locally)

static bool bridgeDecide(Approval* a, int8_t d) {
  JsonDocument j;
  j["action"] = d == D_ALLOW ? "approve" : d == D_ALWAYS ? "always" : d == D_DENY ? "deny" : "keyboard";
  j["id"] = a->id;
  if (d == D_DENY && a->reason.length()) j["text"] = a->reason;
  bool ok = bridgePost(j);
  if (ok) bridgeNextPoll = 0;   // look again right away
  return ok;
}

static bool bridgeStopResult(StopReq* r, int result) {   // result: -2 dismiss, >= 0 option
  JsonDocument j;
  j["action"] = result == -2 ? "dismiss" : "choose";
  j["id"] = r->id;
  if (result >= 0) j["index"] = result;
  bool ok = bridgePost(j);
  if (ok) bridgeNextPoll = 0;
  return ok;
}

static bool bridgeSessionAct(Session* s, const char* action, int index = -1, const String& text = String()) {
  JsonDocument j;
  j["action"] = action; j["session"] = s->id;
  if (index >= 0) j["index"] = index;
  if (text.length()) j["text"] = text;
  bool ok = bridgePost(j);
  if (ok) bridgeNextPoll = 0;
  return ok;
}

static void bridgeSetRemote(uint8_t r) {
  JsonDocument j; j["action"] = "remote"; j["setting"] = remoteName(r);
  if (bridgePost(j)) bridgeNextPoll = 0;
}

static void bridgePin(const String& sid) {
  JsonDocument j; j["action"] = "pin"; j["session"] = sid;
  if (bridgePost(j)) bridgeNextPoll = 0;
}

// -- state coming from the bridge

static Session* bridgeSession(const String& id) {
  Session* s = findSession(id, true);
  if (!s->bridge) { String keep = s->id; *s = Session(); s->used = true; s->id = keep; s->bridge = true; }
  return s;
}

static void bridgeApply(JsonDocument& d) {
  uint32_t now = millis();
  bool quiet = !bridgeSynced;      // first look after connecting: show it, don't chime for it
  bridgeGen++;

  if (!clockOk() && d["now"].is<long>()) setClock(d["now"].as<long>(), 0);
  bridgeRemote = d["remote"] | false;
  { String rs = d["remote_setting"] | ""; uint8_t r = rs == "off" ? 0 : rs == "on" ? 1 : 2;
    if (rs.length() && r != conf.remote) { conf.remote = r; saveConfig(); } }
  bridgeTokens = d["tokens_today"] | 0UL;

  // sessions: the newest MAX_SESSIONS of them
  JsonArray ss = d["sessions"].as<JsonArray>();
  size_t first = ss.size() > (size_t)MAX_SESSIONS ? ss.size() - MAX_SESSIONS : 0, idx = 0;
  for (JsonObject o : ss) {
    if (idx++ < first) continue;
    Session* s = bridgeSession(o["id"] | "");
    String prev = s->state, st = o["state"] | "";
    bool fresh = prev.length() == 0;
    s->gen = bridgeGen; s->lastSeen = now;
    s->project = (const char*)(o["project"] | ""); if (!s->project.length()) s->project = shortId(s->id);
    s->state = st; s->detail = (const char*)(o["detail"] | "");
    if (prev != st) {
      s->stateSince = now - 1000UL * (o["since_s"] | 0UL);
      if (!quiet && !fresh) {   // the same alerts a direct hook would raise
        if (st == "done") { beep(2); touch(); }
        else if (st == "waiting") { beep(1); touch(); }
      }
    }
    uint16_t pd = o["plan_done_n"] | 0;
    if (!quiet && pd > s->planDone) { beep(2); heartUntil = now + 3000; touch(); }
    s->planDone = pd;
    s->todoTotal = s->todoDone = s->todoSkip = s->nTodo = 0; s->todoNow = "";
    if (o["todo"].is<JsonObject>()) {
      JsonObject t = o["todo"];
      s->todoTotal = t["total"] | 0; s->todoDone = t["done"] | 0; s->todoSkip = t["skip"] | 0;
      s->todoNow = (const char*)(t["now"] | "");
      for (JsonArray it : t["items"].as<JsonArray>()) {
        if (s->nTodo >= MAX_TODO) break;
        String k = it[0] | "p";
        s->todo[s->nTodo].st = k.length() ? k[0] : 'p';
        s->todo[s->nTodo].text = (const char*)(it[1] | "");
        s->nTodo++;
      }
    }
    s->hasUsage = o["usage"].is<JsonObject>();
    if (s->hasUsage) {
      JsonObject u = o["usage"];
      s->tokIn = u["in"] | 0UL; s->tokOut = u["out"] | 0UL; s->tokCR = u["cr"] | 0UL; s->tokCW = u["cw"] | 0UL; s->ctx = u["ctx"] | 0UL;
      s->model = (const char*)(u["model"] | "");
    }
    s->ctxPct = o["ctx_pct"].is<float>() ? o["ctx_pct"].as<float>() : -1;
    s->modelName = (const char*)(o["model_name"] | "");
  }
  for (auto& s : sessions) if (s.used && s.bridge && s.gen != bridgeGen) s.used = false;

  // approvals waiting for you
  for (auto& a : approvals) if (a.used && a.bridge) a.decision = D_SKIP;   // provisional: whatever the bridge still lists is set back below
  for (JsonObject o : d["approvals"].as<JsonArray>()) {
    uint32_t id = o["id"] | 0UL;
    Approval* a = nullptr;
    for (auto& x : approvals) if (x.used && x.bridge && x.id == id) a = &x;
    if (!a) {
      for (auto& x : approvals) if (!x.used) { a = &x; break; }
      if (!a) continue;   // queue full: the rest show up as these get answered
      *a = Approval(); a->used = true; a->bridge = true; a->id = id; a->created = now;
    }
    a->decision = D_PENDING; a->lastPoll = now;
    a->tool = (const char*)(o["tool"] | "tool"); a->detail = (const char*)(o["detail"] | "");
    a->project = (const char*)(o["project"] | ""); a->session = (const char*)(o["session"] | "");
    a->always = (const char*)(o["always"] | ""); a->risk = (const char*)(o["risk"] | ""); a->riskWhy = (const char*)(o["risk_why"] | "");
  }
  for (auto& a : approvals) if (a.used && a.bridge && a.decision != D_PENDING) a.used = false;

  // Done / Question cards
  for (auto& r : stops) if (r.used && r.bridge) r.lastPoll = 0;   // provisional, as above
  for (JsonObject o : d["choices"].as<JsonArray>()) {
    uint32_t id = o["id"] | 0UL;
    StopReq* r = nullptr;
    for (auto& x : stops) if (x.used && x.bridge && x.id == id) r = &x;
    if (!r) {
      for (auto& x : stops) if (!x.used) { r = &x; break; }
      if (!r) continue;
      *r = StopReq(); r->used = true; r->bridge = true; r->id = id; r->created = now;
    }
    r->lastPoll = now;
    r->kind = (const char*)(o["kind"] | "done"); r->session = (const char*)(o["session"] | "");
    r->project = (const char*)(o["project"] | ""); r->title = (const char*)(o["title"] | ""); r->last = (const char*)(o["last"] | "");
    r->nMacros = 0;
    for (JsonVariant l : o["options"].as<JsonArray>()) {
      if (r->nMacros >= MAX_CHOICES) break;
      r->macros[r->nMacros].label = (const char*)(l | ""); r->macros[r->nMacros].text = "";
      r->nMacros++;
    }
  }
  for (auto& r : stops) if (r.used && r.bridge && r.lastPoll == 0) r.used = false;

  nBridgeMacro = 0;
  for (JsonVariant l : d["macros"].as<JsonArray>()) if (nBridgeMacro < MAX_MACROS) bridgeMacro[nBridgeMacro++] = (const char*)(l | "");

  if (d["limits"].is<JsonObject>()) {
    JsonObject l = d["limits"];
    for (int w = 0; w < 2; w++) {
      JsonObject o = l[w ? "seven" : "five"];
      if (o.isNull()) continue;
      Limit& L = w ? lim.seven : lim.five;
      L.pct = o["pct"] | -1.0f; L.resets = o["resets"] | 0UL;
    }
    lim.at = now - 1000UL * (l["age_s"] | 0UL); lim.set = true;
  }

  String bp = d["pinned"] | "";
  if (bp.length()) pinned = bp;
  else if (pinned.length()) { Session* ps = findSession(pinned, false); if (ps && ps->bridge) pinned = ""; }

  bridgeSynced = true;
  updateRemote();
  dirty = true;
}

// Everything that came from the bridge goes away when we lose it: the hooks give up on their prompts
// after about 15 s, so showing them any longer would offer buttons that do nothing.
static void bridgeDrop() {
  for (auto& s : sessions) if (s.used && s.bridge) s.used = false;
  for (auto& a : approvals) if (a.used && a.bridge) a.used = false;
  for (auto& r : stops) if (r.used && r.bridge) r.used = false;
  bridgeTokens = 0; nBridgeMacro = 0; bridgeSynced = false; bridgeV = 0; bridgeRemote = false;
  updateRemote();
  dirty = true;
}

// Away from home nobody is watching the Stick's screen: say so when the link goes, because Claude may be waiting on you.
static void bridgeLost(const String& why) {
  notice("bridge", "lost: " + why, C_BAD_U, 5000);
  beep(3);
}

static void bridgeFail(const String& why) {
  if (why.length()) bridgeErr = why;
  if (bridgeFails < 250) bridgeFails++;
  if (bridgeUp && bridgeFails >= 2) { bridgeUp = false; bridgeDrop(); bridgeLost(bridgeErr.length() ? bridgeErr : String("no reply")); }
  uint32_t back = BRIDGE_POLL_ACTIVE_MS << (bridgeFails > 6 ? 6 : bridgeFails);   // 3 s, 6 s, ... capped
  bridgeNextPoll = millis() + (back > BRIDGE_POLL_MAX_MS ? BRIDGE_POLL_MAX_MS : back);
  dirty = true;
}

static void bridgePoll() {
  String out;
  int code = bridgeRequest("GET", "/state?n=1&v=" + String(bridgeV), "", out);
  if (code == 200) {
    JsonDocument d;
    if (deserializeJson(d, out) || !d["v"].is<uint32_t>()) { bridgeFail("not a ClaudeStick bridge"); return; }
    if (!bridgeUp) {
      bridgeUp = true; dirty = true;
      if (bridgeEverUp) { notice("bridge", "connected again", C_OK_U, 2500); beep(0); }
      bridgeEverUp = true;
    }
    bridgeFails = 0; bridgeErr = "";
    bridgeNextPoll = millis() + (dimmed ? BRIDGE_POLL_DIM_MS : BRIDGE_POLL_ACTIVE_MS);
    if (d["same"] | false) { bridgeSynced = true; return; }
    bridgeApply(d);
    bridgeV = d["v"] | 0UL;
    return;
  }
  if (code == 401) bridgeFail("token rejected");
  else if (code == 429) bridgeFail("too many attempts, waiting");
  else if (code > 0) bridgeFail("error " + String(code));
  else bridgeFail("");
}

static void bridgeTick() {
  if (!bridgeConfigured() || !setupDone) return;
  if (WiFi.status() != WL_CONNECTED) {
    if (bridgeUp) { bridgeUp = false; bridgeDrop(); bridgeLost("Wi-Fi dropped"); }
    bridgeTls.stop(); bridgePlain.stop();   // the connection died with the Wi-Fi: give its memory back
    bridgeNextPoll = 0;
    return;
  }
  if ((int32_t)(millis() - bridgeNextPoll) < 0) return;
  if (upd.progress >= 0) { bridgeNextPoll = millis() + 5000; return; }   // not while an update installs
  bridgePoll();
}

// Small cloud for the status bar: green = connected, red = can't reach it, dim = connecting.
static void iconBridge(int x, int y, uint16_t c) {   // 7 x 5
  static const uint8_t cloud[5] = { 0x18, 0x3C, 0x7E, 0x7F, 0x3E };
  for (int r = 0; r < 5; r++)
    for (int b = 0; b < 7; b++)
      if (cloud[r] & (1 << (6 - b))) spr.drawPixel(x + b, y + r, c);
}
