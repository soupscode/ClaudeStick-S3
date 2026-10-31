#include "../../firmware/src/main.cpp"
#include <map>
#include <string>
extern unsigned long NOW; extern std::map<std::string,std::string> PREFS; extern std::string HTTP_BODY, LAST_URL, UPDATE_URL;
extern int HTTP_CODE, UPDATE_RESULT, RESTARTS, BOOT_SET; extern bool OTHER_VALID;
static int fails = 0;
static void check(const char* n, bool c, const std::string& x = "") { printf("%s %s%s\n", c ? "PASS" : "FAIL", n, c ? "" : (" | " + x).c_str()); if (!c) fails++; }
static std::string OUTP; void M5Canvas::pushSprite(int,int){
  if (OUTP.empty()) return; FILE* f=fopen(OUTP.c_str(),"wb"); fprintf(f,"P6 %d %d 255\n",w,h);
  for(int i=0;i<w*h;i++){uint16_t c=fb[i]; unsigned char rgb[3]={(unsigned char)(((c>>11)&31)*255/31),(unsigned char)(((c>>5)&63)*255/63),(unsigned char)((c&31)*255/31)}; fwrite(rgb,1,3,f);} fclose(f); }
static void shot(const char* n){ OUTP = std::string("out/") + n + ".ppm"; draw(); OUTP.clear(); }
int main() {
  // ---- boot guard: two failed boots, then rollback on the third
  PREFS.clear(); RESTARTS = 0;
  prefs.begin("cstick", false);
  bootGuard(); check("boot 1 counted", PREFS["bootfail"] == "1");
  bootGuard(); check("boot 2 counted", PREFS["bootfail"] == "2" && BOOT_SET == -1);
  bootGuard(); check("boot 3 -> switch to previous slot + restart", BOOT_SET == 1 && RESTARTS == 1, std::to_string(BOOT_SET));
  check("failed version recorded and skipped", PREFS["rolledback"] == FW_VERSION && PREFS["skipver"] == FW_VERSION);
  PREFS.clear(); BOOT_SET = -1; OTHER_VALID = false; RESTARTS = 0;
  bootGuard(); bootGuard(); bootGuard();
  check("no valid previous slot: keep running, no restart", BOOT_SET == -1 && RESTARTS == 0);
  OTHER_VALID = true;
  // ---- healthy boot clears the counter
  PREFS.clear(); bootGuard(); NOW = 25000; bootHealthy = false; bootHealthTick();
  check("20 s of uptime marks the boot healthy", PREFS["bootfail"] == "0");
  // ---- normal setup
  PREFS.clear(); PREFS["setup"] = "1"; PREFS["ssid"] = "home-wifi";
  NOW = 100000; setup(); RESTARTS = 0; WiFi.st = WL_CONNECTED;
  conf.updateUrl = "https://example.github.io/app/";
  HTTP_BODY = "{\"version\":\"0.9.5\",\"app\":\"firmware/claudestick-app.bin\",\"notes\":\"- Tilt fix\\n- Louder approval chime\\n- Battery: screen sleeps sooner\"}";
  checkForUpdate(false);
  check("fetches ota.json from the update URL", LAST_URL == "https://example.github.io/app/ota.json", LAST_URL);
  check("newer version -> prompt due", upd.available && updatePromptDue() && upd.version == "0.9.5");
  shot("23_update_prompt");
  // remind me later
  menuIdx = 1; M5.BtnA.c = true; buttons(); M5.BtnA.c = false;
  check("remind me later hides the prompt", !updatePromptDue());
  NOW += 24UL * 3600 * 1000 + 1000;
  check("...and it returns after 24 h", updatePromptDue());
  // skip this version
  menuIdx = 2; M5.BtnA.c = true; buttons(); M5.BtnA.c = false;
  check("skip this version", !upd.available && PREFS["skipver"] == "0.9.5");
  checkForUpdate(false); check("skipped version not offered again", !upd.available);
  HTTP_BODY = "{\"version\":\"0.9.6\",\"app\":\"firmware/claudestick-app.bin\",\"notes\":\"- Fix\"}";
  checkForUpdate(false); check("a newer version is offered again", upd.available && upd.version == "0.9.6");
  // same version manual check
  HTTP_BODY = std::string("{\"version\":\"") + FW_VERSION + "\"}"; upd.available = false;
  checkForUpdate(true); check("manual check: up to date notice", !upd.available && noticeText.startsWith("up to date"), noticeText.c_str());
  noticeUntil = 0;
  HTTP_CODE = 404; checkForUpdate(true); check("manual check: unreachable notice", noticeText.indexOf("couldn't reach") >= 0, noticeText.c_str());
  HTTP_CODE = 200; noticeUntil = 0;
  // install
  HTTP_BODY = "{\"version\":\"0.9.6\",\"app\":\"firmware/claudestick-app.bin\",\"notes\":\"- Fix\"}"; checkForUpdate(false);
  menuIdx = 0; M5.BtnA.c = true;
  OUTP = "out/24_update_progress.ppm";   // last progress frame lands here
  buttons(); M5.BtnA.c = false; OUTP.clear();
  check("downloads the app image", UPDATE_URL == "https://example.github.io/app/firmware/claudestick-app.bin", UPDATE_URL);
  check("progress reaches 100 and a restart is scheduled", upd.progress == 100 && restartAt > 0);
  check("remembers which version it's moving to", PREFS["upd_to"] == "0.9.6");
  // failed install
  upd.progress = -1; restartAt = 0; UPDATE_RESULT = HTTP_UPDATE_FAILED; installUpdate();
  check("failed install shows a notice, no restart", restartAt == 0 && noticeTitle == "update failed");
  shot("25_update_failed"); noticeUntil = 0;
  // post-update / rollback notices
  PREFS["upd_to"] = FW_VERSION; bootNotices(); check("after update: 'updated!' notice", noticeTitle == "updated!"); shot("26_updated_notice");
  noticeUntil = 0; PREFS["rolledback"] = "0.9.6"; bootNotices(); check("after rollback: notice", noticeTitle == "rolled back"); shot("27_rolled_back");
  // ---- hardening
  conf.updateUrl = "https://example.github.io/app/"; HTTP_CODE = 200; upd.available = false; upd.lastCheck = 0; noticeUntil = 0; upd.remindAt = 0;
  auto setTime = [](long e) { struct timeval tv = { (time_t)e, 0 }; settimeofday(&tv, nullptr); };
  setTime(1);   // no clock yet
  HTTP_BODY = "{\"version\":\"0.9.9\",\"app\":\"firmware/claudestick-app.bin\"}"; LAST_URL.clear();
  checkForUpdate(true);
  check("https check waits for the clock", LAST_URL.empty() && !upd.available && upd.lastCheck == 0 && noticeText.indexOf("clock") >= 0, noticeText.c_str());
  noticeUntil = 0; NOW = 400000; lastActivity = 0; updateTick(); LAST_URL.clear();
  check("background tick: nothing without a clock", LAST_URL.empty());
  setTime(1793390000);
  // a pending approval or recent button press holds the background check back
  NOW = 500000; lastActivity = NOW - 60000; Approval* ap = newApproval(); ap->src = SRC_WIFI; ap->tool = "Bash";
  updateTick(); check("background check waits for a pending approval", LAST_URL.empty());
  ap->used = false;
  lastActivity = NOW - 1000; updateTick(); check("background check waits while you're using it", LAST_URL.empty());
  NOW += 20000; lastActivity = NOW - 60000; updateTick();
  check("background check runs when idle", LAST_URL == "https://example.github.io/app/ota.json" && upd.available && upd.version == "0.9.9", LAST_URL);
  updateTick(); LAST_URL.clear(); NOW += 16000; updateTick();
  check("and not again for hours", LAST_URL.empty());
  // update source paths must stay relative
  upd.available = false; upd.version = ""; upd.lastCheck = 0;
  HTTP_BODY = "{\"version\":\"0.9.9\",\"app\":\"https://evil.example/x.bin\"}"; checkForUpdate(false);
  check("absolute app URL ignored", upd.app == "firmware/claudestick-app.bin", upd.app.c_str());
  upd.available = false; upd.version = ""; HTTP_BODY = "{\"version\":\"0.9.9\",\"app\":\"../../x.bin\"}"; checkForUpdate(false);
  check("path traversal ignored", upd.app == "firmware/claudestick-app.bin", upd.app.c_str());
  // not enough memory for TLS: explain instead of failing mid-handshake
  upd.progress = -1; restartAt = 0; UPDATE_URL.clear(); HEAP_FREE = 20000; noticeUntil = 0; installUpdate(); HEAP_FREE = 200000;
  check("low memory: install refused with a clear notice", UPDATE_URL.empty() && upd.progress == -1 && noticeText.indexOf("memory") >= 0, noticeText.c_str());
  setTime(1); noticeUntil = 0; installUpdate();
  check("no clock: install refused", UPDATE_URL.empty() && noticeText.indexOf("clock") >= 0, noticeText.c_str());
  setTime(1793390000);
  printf("%s\n", fails ? "SOME FAILED" : "ALL PASS");
  return fails;
}
