#include "../../firmware/src/main.cpp"
#include <string>
#include <map>
extern std::string REQ_BODY, LAST_SENT; extern int LAST_CODE; extern unsigned long NOW;
static int fails = 0;
static void check(const char* n, bool c, const std::string& x = "") { printf("%s %s%s\n", c ? "PASS" : "FAIL", n, c ? "" : (" | " + x).c_str()); if (!c) fails++; }
void M5Canvas::pushSprite(int,int){}
static void at(long epoch) { struct timeval tv = { (time_t)epoch, 0 }; settimeofday(&tv, nullptr); }
int main() {
  setup(); token = ""; NOW = 200000; at(1793390000);
  Session* s = findSession("s1", true); s->project = "api-server"; s->state = "tool";
  REQ_BODY = R"({"session":"s1","ctx_pct":42.5,"cost":1.84,"model":"Opus 5.5","five":{"pct":63.2,"resets":1793398040},"seven":{"pct":28,"resets":1793663600}})";
  hUsage();
  check("limits stored", lim.five.pct > 63.1f && lim.five.pct < 63.3f && lim.seven.pct == 28 && lim.five.resets == 1793398040UL);
  check("session context % stored", s->ctxPct > 42.4f && s->ctxPct < 42.6f && s->cost > 1.83f && s->modelName == "Opus 5.5");
  check("fmtReset 5h", fmtReset(lim.five.resets) == "2h14m", fmtReset(lim.five.resets).c_str());
  check("fmtReset 7d", fmtReset(lim.seven.resets) == "3d4h", fmtReset(lim.seven.resets).c_str());
  check("fmtReset minutes", fmtReset(1793390000 + 125) == "2m");
  check("fresh limits aren't stale", !limitsStale());
  NOW += 16UL * 60 * 1000; check("limits go stale after 15 min", limitsStale());
  // a window that has reset is back at 0
  at(1793398040 + 10); check("reset window reads 0%", limitPct(lim.five) == 0 && limitPct(lim.seven) == 28);
  at(1793390000);
  // unknown session: limits still taken, no session created
  int before = 0; for (auto& x : sessions) if (x.used) before++;
  REQ_BODY = R"({"session":"ghost","ctx_pct":10,"five":{"pct":70,"resets":1793398040}})"; hUsage();
  int after = 0; for (auto& x : sessions) if (x.used) after++;
  check("never creates a session", after == before && findSession("ghost", false) == nullptr);
  check("limits updated from any session", lim.five.pct == 70);
  // missing windows (API users): limits untouched, no crash
  REQ_BODY = R"({"session":"s1","ctx_pct":50})"; hUsage();
  check("ctx-only update keeps limits", s->ctxPct == 50 && lim.five.pct == 70);
  // /state exposes them
  hState();
  check("/state has limits and ctx_pct", LAST_SENT.find("\"limits\"") != std::string::npos && LAST_SENT.find("\"ctx_pct\":50") != std::string::npos, LAST_SENT.substr(0, 200));
  // percent colours
  check("colours: green / amber / red", pctColor(40) == C_OK && pctColor(75) == C_YEL && pctColor(95) == C_BAD);
  check("haveUsage with limits", haveUsage());
  return fails;
}
