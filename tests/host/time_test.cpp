#include "../../firmware/src/main.cpp"
#include <string>
#include <map>
extern std::map<std::string,std::string> PREFS; extern std::string REQ_BODY, LAST_SENT, NTP_TZ; extern int LAST_CODE, NTP_CALLS; extern unsigned long NOW;
static int fails = 0;
static void check(const char* n, bool c, const std::string& x = "") { printf("%s %s%s\n", c ? "PASS" : "FAIL", n, c ? "" : (" | " + x).c_str()); if (!c) fails++; }
void M5Canvas::pushSprite(int,int){}
static void at(long epoch) { struct timeval tv = { (time_t)epoch, 0 }; settimeofday(&tv, nullptr); }
// 2026-10-30 19:53:20 UTC, 2026-11-02 19:53:20 UTC (after US DST ends), 2026-07-01 12:00 UTC
static const long OCT30 = 1793390000, NOV2 = 1793390000 + 3 * 86400, JUL1 = 1782907200;
int main() {
  setup(); WiFi.st = WL_CONNECTED; NOW = 100000;
  check("no time yet -> empty clock", [&]{ at(1); return clockStr() == "" ; }());
  // hook / desktop time with a fixed offset, no zone chosen
  setClock(OCT30, -5 * 3600);
  check("fixed offset 12-hour", clockStr() == "2:53p", clockStr().c_str());
  clock24 = true; check("24-hour", clockStr() == "14:53", clockStr().c_str()); clock24 = false;
  // chosen zone: daylight saving handled by the rule
  REQ_BODY = R"({"tz":"CST6CDT,M3.2.0,M11.1.0","tz_name":"America/Chicago","clock24":false})";
  hSetupTime();
  check("/setup/time ok", LAST_CODE == 200 || LAST_SENT.find("\"ok\":true") != std::string::npos, LAST_SENT);
  check("zone persisted", PREFS["tz"] == "CST6CDT,M3.2.0,M11.1.0" && PREFS["tzname"] == "America/Chicago");
  at(OCT30); check("Chicago in CDT: 2:53p", clockStr() == "2:53p", clockStr().c_str());
  at(NOV2);  check("Chicago after DST ends: 1:53p", clockStr() == "1:53p", clockStr().c_str());
  at(JUL1);  check("Chicago in July: 7:00a", clockStr() == "7:00a", clockStr().c_str());
  at(1793422800); check("midnight is 12:00a", clockStr() == "12:00a", clockStr().c_str());   // 2026-10-31 05:00 UTC = 00:00 CDT
  at(1793466000); check("noon is 12:00p", clockStr() == "12:00p", clockStr().c_str());      // 2026-10-31 17:00 UTC = 12:00 CDT
  // the sender's offset no longer overrides the chosen zone
  setClock(OCT30, 3600); check("chosen zone beats sender offset", clockStr() == "2:53p", clockStr().c_str());
  // SNTP restarts with the zone rule
  int calls = NTP_CALLS; ntpTick();
  check("NTP started with the rule", NTP_CALLS == calls + 1 && NTP_TZ == "CST6CDT,M3.2.0,M11.1.0", NTP_TZ);
  ntpTick(); check("NTP started once", NTP_CALLS == calls + 1);
  // 24-hour toggle over HTTP
  REQ_BODY = R"({"clock24":true})"; hSetupTime();
  at(OCT30); check("24-hour via /setup/time", clockStr() == "14:53" && PREFS["clk24"] == "1", clockStr().c_str());
  // bad zone rejected, nothing changed
  REQ_BODY = R"({"tz":"CST6; rm -rf /"})"; hSetupTime();
  check("bad zone rejected", LAST_CODE == 400 && PREFS["tz"] == "CST6CDT,M3.2.0,M11.1.0", std::to_string(LAST_CODE));
  // browser time is used only when nothing else has set the clock
  at(1); REQ_BODY = R"({"epoch":1793390000})"; hSetupTime();
  check("browser epoch sets an unset clock", clockOk() && clockStr() == "14:53", clockStr().c_str());
  REQ_BODY = R"({"epoch":1700000100})"; hSetupTime();
  check("browser epoch ignored once the clock is set", clockStr() == "14:53", clockStr().c_str());
  // fixed-offset rule from the browser fallback (no DST), half-hour zone
  REQ_BODY = R"({"tz":"<+0530>-5:30","tz_name":"UTC+05:30"})"; hSetupTime(); at(OCT30);
  check("fixed +05:30 rule", clockStr() == "01:23", clockStr().c_str());   // 24h still on
  // boot restores everything
  tzPosix = ""; tzName = ""; clock24 = false;
  setup(); at(OCT30);
  check("settings survive a reboot", tzPosix == "<+0530>-5:30" && clock24 == true && clockStr() == "01:23", tzPosix.c_str());
  // serial commands
  tzPosix = ""; textCommand("tz EST5EDT,M3.2.0,M11.1.0"); textCommand("clock 12"); at(OCT30);
  check("serial tz + clock", clockStr() == "3:53p", clockStr().c_str());
  return fails;
}
