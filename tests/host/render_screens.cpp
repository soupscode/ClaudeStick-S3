#include "../../firmware/src/main.cpp"
#include <string>
extern unsigned long NOW; extern bool BLE_ON; extern uint32_t PK;
// ---- output
static std::string OUT;
void M5Canvas::pushSprite(int,int){
  FILE* f = fopen(OUT.c_str(), "wb"); fprintf(f, "P6 %d %d 255\n", w, h);
  for(int i=0;i<w*h;i++){ uint16_t c=fb[i]; unsigned char rgb[3]={ (unsigned char)(((c>>11)&31)*255/31), (unsigned char)(((c>>5)&63)*255/63), (unsigned char)((c&31)*255/31)}; fwrite(rgb,1,3,f);} fclose(f);
}
static void shot(const char* name, int ticks = 1){ for (int i=0;i<ticks;i++){ NOW += 200; } OUT = std::string("out/") + name + ".ppm"; draw(); }
static Session* mk(const char* id, const char* proj, const char* st, const char* det){ Session* s=findSession(id,true); s->project=proj; s->state=st; s->detail=det; s->lastSeen=NOW; s->stateSince=NOW; return s; }

int main(){
  setup();
  clockSet = true; tzOffset = -5*3600;
  { struct timeval tv0 = { 1793390000, 0 }; settimeofday(&tv0, nullptr); }
  WiFi.st = WL_IDLE_STATUS; ssid = ""; startAP(); shot("00a_ftue_hotspot", 3);
  ssid = "home-wifi"; WiFi.st = WL_CONNECTED; shot("00b_ftue_almost", 3);
  stopAP();
  setupDone = true; ssid = "home-wifi"; token = "a1b2c3d4e5f60789"; clockSet = true; tzOffset = -5*3600;
  struct timeval tv = { 1793390000, 0 }; settimeofday(&tv, nullptr);
  buddySetSpeciesIdx(0);
  lim.five.pct = 63; lim.five.resets = 1793390000 + 2*3600 + 14*60; lim.seven.pct = 28; lim.seven.resets = 1793390000 + 3*86400 + 4*3600; lim.at = NOW; lim.set = true;
  shot("01_home_empty", 3);
  mk("aaaa1111", "api-server", "tool", "Bash: npm run build");
  mk("bbbb2222", "web-app", "done", "Updated 3 components in /ui");
  NOW += 20000;
  Session* p = mk("cccc3333", "docs-site", "working", "Updating the API docs"); (void)p;
  sessions[0].lastSeen = NOW; sessions[0].ctxPct = 42; sessions[0].cost = 1.84; sessions[0].hasUsage = true; sessions[0].ctx = 84213;
  shot("02_home_sessions", 2);
  Approval* a = newApproval(); a->src = SRC_WIFI; a->tool = "Bash"; a->project = "api-server";
  a->detail = "rm -rf node_modules/.cache build/ && npm ci && npm run build 2>&1 | tail -20 && echo 'rebuilt the production bundle after clearing the cache'";
  Approval* a2 = newApproval(); a2->src = SRC_WIFI; a2->tool = "Edit"; a2->project = "docs-site"; a2->detail = "/etc/nginx/nginx.conf";
  shot("03_approval", 4);
  scrollLine = 4; shot("04_approval_scrolled", 1); scrollLine = 0;
  a->used = false; a2->used = false;
  StopReq* r = &stops[0]; *r = StopReq(); r->used = true; r->id = 50; r->kind = "question"; r->project = "api-server"; r->session = "aaaa1111";
  r->title = "Which database should the tests use?"; r->nMacros = 3;
  r->macros[0] = {"sqlite (in-memory)", "sqlite"}; r->macros[1] = {"postgres (docker)", "postgres"}; r->macros[2] = {"mock client", "mock"};
  menuIdx = 1; shot("05_question", 3);
  r->kind = "done"; r->id = 51; r->title = ""; r->last = "Migrated the auth module to the new session store and verified it with a dry run. Nothing destructive ran.";
  r->nMacros = 6;
  r->macros[0] = {"Run the full suite","x"}; r->macros[1] = {"Add token refresh","x"}; r->macros[2] = {"Stop here","x"};
  r->macros[3] = {"Continue","x"}; r->macros[4] = {"Keep going","x"}; r->macros[5] = {"Run tests + fix","x"};
  menuIdx = 0; shot("06_done_menu", 3);
  r->used = false;
  nMacros = 4; macros[0] = {"Continue","x"}; macros[1] = {"Keep going","x"}; macros[2] = {"Next phase","x"}; macros[3] = {"Run tests + fix","x"};
  remoteMode = true;
  screen = SCR_ACTIONS; menuIdx = 0; shot("07_menu", 2);
  screen = SCR_SESSIONS; menuIdx = 1; pinned = "aaaa1111"; shot("08_tie_session", 2);
  screen = SCR_INFO; shot("09_info", 2);
  screen = SCR_HOME; heartUntil = NOW + 5000; shot("10_home_remote_heart", 2);
  heartUntil = 0; sessions[0].state = "error"; sessions[0].stateSince = NOW; focusSession = 0; shot("11_home_error", 2);
  sessions[0].state = "tool"; BLE_ON = true; PK = 482913; shot("12_pairing", 1); PK = 0;
  // ---- v0.5 screens
  heartUntil = 0; PK = 0; screen = SCR_HOME; remoteMode = false; conf.remote = 2;
  sessions[0].project = "api-server@fix-auth"; sessions[0].state = "tool"; sessions[0].detail = "Bash: npm test";
  Approval* d = newApproval(); d->src = SRC_WIFI; d->tool = "Bash"; d->project = "api-server@fix-auth";
  d->risk = "high"; d->riskWhy = "recursive force delete"; d->detail = "rm -rf ./build ./dist";
  shot("13_danger", 4);
  holdHintUntil = NOW + 3000; shot("14_danger_tapped", 1); holdHintUntil = 0;
  d->used = false;
  Approval* e2 = newApproval(); e2->src = SRC_WIFI; e2->tool = "Edit"; e2->project = "api-server@fix-auth";
  e2->always = "auto-accept edits (session)";
  e2->detail = "/srv/app/config.py\n- RETRIES = 3\n- TIMEOUT = 5\n+ RETRIES = 5\n+ TIMEOUT = 30\n+ VERIFY_TLS = True";
  shot("15_edit_diff_always", 4);
  e2->used = false;
  Approval* g = newApproval(); g->src = SRC_WIFI; g->tool = "Bash"; g->project = "api-server@fix-auth";
  g->always = "Bash(git status *) here"; g->detail = "git status --short";
  shot("16_bash_always", 3); g->used = false;
  conf.sttUrl = "http://whisper.lan:8000/v1/audio/transcriptions";
  voiceState = V_REC; voiceLevel = 7000; OUT = "out/17_voice_listening.ppm"; drawVoiceRec(3400);
  voiceState = V_THINK; OUT = "out/18_voice_thinking.ppm"; drawVoiceRec(0);
  voiceState = V_CONFIRM; voiceTarget = "aaaa1111"; voiceStopReq = 0;
  voiceText = "use postgres for the integration tests and keep the sqlite ones for unit tests";
  shot("19_voice_confirm", 2); voiceState = V_IDLE;
  screen = SCR_ACTIONS; menuIdx = nMacros + 1; shot("20_menu_v05", 2);
  screen = SCR_HELP; shot("21_help", 2);
  screen = SCR_HOME; conf.remote = 2; lastStickAt = NOW; lastKbdAt = 0; updateRemote(); shot("22_home_auto_remote", 2);
  screen = SCR_TEST; M5.BtnA.c = false; shot("28_hw_test", 2); screen = SCR_HOME;
  safeMode = true; shot("29_home_safe_mode", 2); safeMode = false;

  // ---- v0.7 screens
  {
    Session* h = &sessions[0]; h->project = "api-server@fix-auth"; h->state = "tool"; h->detail = "Bash: npm test";
    const char* items[][2] = {{"c","Audit the session code"},{"c","Pick a session store"},{"i","Rewrite auth.py for Redis"},
                              {"p","Add token refresh"},{"p","Run the unit tests"},{"p","Run the full suite"},{"p","Update the API docs"}};
    h->nTodo = 7; for (int i = 0; i < 7; i++) { h->todo[i].st = items[i][0][0]; h->todo[i].text = items[i][1]; }
    h->todoTotal = 7; h->todoDone = 2; h->todoNow = "Rewriting auth.py";
    h->tokIn = 18234; h->tokOut = 61200; h->tokCW = 340000; h->tokCR = 4200000; h->ctx = 84213; h->hasUsage = true; h->model = "opus-5-5";
    noteLedger(h->id, "2026-10-30", 1834000); noteLedger("zzzz", "2026-10-30", 2210000);
    focusSession = 0; screen = SCR_HOME; shot("30_home_progress", 2);
    screen = SCR_DETAIL; shot("31_detail_todo", 2);
    screen = SCR_DETAIL; focusSession = 1; shot("32_detail_no_todo", 2); focusSession = 0;
    screen = SCR_HOME;
    Approval* g2 = newApproval(); g2->src = SRC_WIFI; g2->tool = "Bash"; g2->project = "api-server@fix-auth"; g2->session = h->id;
    g2->always = "Bash(git status *) here"; g2->detail = "git status --short";
    shot("33_approval_2x", 3);
    Approval* d2 = newApproval(); d2->src = SRC_WIFI; d2->tool = "Bash"; d2->project = "api-server"; d2->risk = "high";
    d2->riskWhy = "recursive force delete"; d2->detail = "rm -rf ./build"; g2->used = false;
    holdHintUntil = NOW + 3000; shot("34_danger_tap_hint", 1); holdHintUntil = 0;
    voiceState = V_CONFIRM; voiceTarget = h->id; voiceApproval = d2->id; voiceText = "no, only delete the build cache, not dist";
    shot("35_voice_deny", 2); voiceState = V_IDLE; voiceApproval = 0; d2->used = false;
    screen = SCR_HELP; shot("36_help_v07", 2);
    screen = SCR_INFO; shot("37_info_tokens", 2); screen = SCR_HOME;
  }
  screen = SCR_PET; buddySetSpeciesIdx(0); shot("40_pet_picker", 2); screen = SCR_HOME;
  { sessions[0].state = "done"; sessions[0].stateSince = NOW; shot("41_home_done", 2); sessions[0].state = "tool"; }
  // remote access through a bridge: the link row on the info screen and the status-bar cloud
  bridgeUrl = "https://stick.example.com"; bridgeTok = "x"; bridgeUp = true; bridgeFails = 0;
  screen = SCR_INFO; shot("42_info_bridge", 2);
  screen = SCR_HOME; shot("43_home_bridge_up", 2);
  bridgeUp = false; bridgeFails = 3; bridgeErr = "token rejected";
  screen = SCR_INFO; shot("44_info_bridge_offline", 2);
  screen = SCR_HOME; shot("45_home_bridge_offline", 2);
  bridgeUrl = ""; bridgeTok = ""; bridgeFails = 0; bridgeErr = "";
  // buddy gallery
  for (int i = 0; i < buddySpeciesCount(); i++) { buddySetSpeciesIdx(i); char n[64]; snprintf(n, sizeof n, "sp_%02d_%s", i, buddySpeciesName()); screen = SCR_HOME; sessions[0].state="tool"; shot(n, 7); }
  return 0;
}
