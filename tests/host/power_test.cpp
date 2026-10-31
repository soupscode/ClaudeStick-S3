// Battery meter, the Bluetooth setting and the radio-sleep switch: what an away-from-home Stick needs to last.
#include "../../firmware/src/main.cpp"
#include <string>
#include <map>
#include <vector>
extern std::map<std::string, std::string> PREFS;
extern std::string REQ_BODY, LAST_SENT;
extern int LAST_CODE, BATT_PCT, RESTARTS;
extern bool BATT_CHARGING;
struct HttpScripted { int code; std::string body; };
extern std::vector<HttpScripted> HTTP_SCRIPT;
extern std::vector<std::string> HTTP_LOG;
extern unsigned long NOW;
static int fails = 0;
static void check(const char* n, bool c, const std::string& x = "") { printf("%s %s%s\n", c ? "PASS" : "FAIL", n, c ? "" : (" | " + x).c_str()); if (!c) fails++; }
void M5Canvas::pushSprite(int, int) {}
static bool has(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

int main() {
  PREFS.clear(); PREFS["setup"] = "1"; PREFS["ssid"] = "home-wifi";
  NOW = 100000; BATT_PCT = 80; BATT_CHARGING = false;
  setup(); WiFi.st = WL_CONNECTED; token = "";

  // ---- battery drain meter
  drainTick();
  check("starts measuring when unplugged", drain.pct0 == 80);
  check("no estimate yet", drainPerHour() < 0 && batteryLine().endsWith("measuring"), batteryLine().c_str());
  NOW += 11UL * 60 * 1000; BATT_PCT = 79;
  check("one point in 11 minutes is too little to go on", drainPerHour() < 0);
  NOW += 49UL * 60 * 1000; BATT_PCT = 74;   // 6 points in 60 minutes
  float r = drainPerHour();
  check("6 points in an hour = 6 %/h", r > 5.9f && r < 6.1f, std::to_string(r));
  String line = batteryLine();
  check("the info line shows level, voltage, rate and time left", line.startsWith("74% 4.01V") && has(line.c_str(), "6.0%/h") && has(line.c_str(), "~12h"), line.c_str());
  NOW += 1000; drainTick();
  check("ticks don't restart the measurement", drain.pct0 == 80);
  BATT_CHARGING = true; NOW += 31000; drainTick();
  check("plugging in resets it and says so", drain.pct0 == -1 && batteryLine().endsWith("charging"), batteryLine().c_str());
  BATT_CHARGING = false; BATT_PCT = 100; NOW += 31000; drainTick();
  check("unplugging starts a new measurement", drain.pct0 == 100);
  BATT_PCT = 100; NOW += 60UL * 60 * 1000;
  check("a full battery that hasn't dropped shows no rate", drainPerHour() < 0);
  BATT_PCT = 60; NOW += 31000; drainTick(); drain.pct0 = 50;   // a charger the gauge didn't report: the level went up
  NOW += 31000; drainTick();
  check("a level that went up restarts the measurement from there", drain.pct0 == 60, std::to_string(drain.pct0));
  BATT_PCT = 95; drain.pct0 = 100;
  screen = SCR_INFO; draw();   // the info screen draws with the battery row
  check("info screen draws", true);
  Serial.out.clear();
  textCommand("bat");
  check("serial `bat` reports it", has(Serial.out, "batt 95%") && has(Serial.out, "charging=0"), Serial.out);

  // ---- Bluetooth setting
  check("Bluetooth is on by default", btPref && !btOff());
  REQ_BODY = R"({"bt":false})"; RESTARTS = 0; restartAt = 0; hSetupSave();
  check("turning it off in setup saves it and restarts (it starts at boot)", !btPref && PREFS["bt"] == "0" && restartAt != 0 && has(LAST_SENT, "\"restart\":true"), LAST_SENT);
  check("off means off: the icon and BLE init are skipped", btOff());
  hSetupInfo();
  check("/setup/info reports it", has(LAST_SENT, "\"bt_on\":false"), LAST_SENT);
  REQ_BODY = R"({"bt":false})"; restartAt = 0; hSetupSave();
  check("saving the same value again doesn't restart", restartAt == 0, LAST_SENT);
  REQ_BODY = R"({"name":"Claude-X"})"; hSetupSave();   // an older client that doesn't send bt keeps the setting
  check("a save without 'bt' leaves it alone", !btPref);
  PREFS["setup"] = "1"; setup(); WiFi.st = WL_CONNECTED; token = "";
  check("the setting survives a reboot", !btPref && btOff());
  safeMode = false; btPref = true; check("safe mode still wins over the setting", ([&] { safeMode = true; bool o = btOff(); safeMode = false; return o; })());
  // menu entry
  MenuEntry ents[MAX_ITEMS]; String labels[MAX_ITEMS];
  int n = menuEntries(ents, labels); bool found = false; int at = -1;
  for (int i = 0; i < n; i++) if (ents[i].kind == MI_BT) { found = true; at = i; check("menu: bluetooth entry says what it does", labels[i] == "bluetooth: on (restarts)", labels[i].c_str()); }
  check("menu: has a bluetooth entry", found);
  check("menu fits the list", n <= MAX_ITEMS);
  screen = SCR_ACTIONS; menuIdx = at; M5.BtnA.c = true; restartAt = 0; buttons(); M5.BtnA.c = false;
  check("menu: choosing it flips the setting and schedules a restart", !btPref && PREFS["bt"] == "0" && restartAt != 0);
  btPref = true;

  // ---- radio sleep (experimental, off by default)
  check("standard modem sleep by default", radioSleep == 0 && WiFi.ps == WIFI_PS_MIN_MODEM);
  Serial.out.clear(); textCommand("sleep max");
  check("`sleep max` switches and remembers", radioSleep == 1 && WiFi.ps == WIFI_PS_MAX_MODEM && PREFS["wps"] == "1", Serial.out);
  PREFS["setup"] = "1"; setup(); token = "";
  check("it is applied again after a reboot", radioSleep == 1 && WiFi.ps == WIFI_PS_MAX_MODEM);
  textCommand("sleep min");
  check("`sleep min` goes back", radioSleep == 0 && WiFi.ps == WIFI_PS_MIN_MODEM && PREFS["wps"] == "0");
  Serial.out.clear(); textCommand("sleep banana");
  check("anything else prints usage and changes nothing", radioSleep == 0 && has(Serial.out, "sleep max|min"));

  // ---- remote mode chosen on the setup page reaches the bridge (it owns the setting while connected)
  bridgeSave("http://192.168.1.20:7890", "tok"); bridgeUp = true; HTTP_LOG.clear();
  HTTP_SCRIPT.push_back({ 200, "{\"ok\":true}" });
  REQ_BODY = R"({"remote":"on"})"; hSetupSave();
  check("setup page: remote mode is sent to the bridge", !HTTP_LOG.empty() && has(HTTP_LOG.back(), "\"action\":\"remote\"") && has(HTTP_LOG.back(), "\"setting\":\"on\""), HTTP_LOG.empty() ? "" : HTTP_LOG.back());
  HTTP_LOG.clear(); REQ_BODY = R"({"name":"Claude-X"})"; hSetupSave();
  check("a save that doesn't mention remote mode doesn't touch the bridge", HTTP_LOG.empty());
  bridgeUp = false;

  printf("\n%s\n", fails ? "power: FAILED" : "power: all passed");
  return fails;
}
