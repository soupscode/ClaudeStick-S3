#include "../../firmware/src/main.cpp"
#include <string>
extern unsigned long NOW;
static int fails = 0;
static void check(const char* n, bool c, const std::string& x = "") { printf("%s %s%s\n", c ? "PASS" : "FAIL", n, c ? "" : (" | " + x).c_str()); if (!c) fails++; }
void M5Canvas::pushSprite(int,int){}
// feed a steady signal for n seconds, return the bars at the end
static int settle(int rssi, int secs = 12) { WiFi.rssi = rssi; int b = 0; for (int i = 0; i < secs; i++) { NOW += 1000; b = wifiBars(); } return b; }
int main() {
  setup(); WiFi.st = WL_CONNECTED; NOW = 100000;
  check("-45 dBm -> 4 bars", settle(-45) == 4);
  check("-60 dBm -> 3 bars", settle(-60) == 3);
  check("-70 dBm -> 2 bars", settle(-70) == 2);
  check("-80 dBm -> 1 bar", settle(-80) == 1);
  check("-92 dBm -> still 1 bar", settle(-92) == 1);
  check("back to -50 -> 4 bars", settle(-50) == 4);
  // hysteresis: hovering around the 4/3 boundary (-55) must not flicker
  settle(-54, 15); int flips = 0, last = wifiBars();
  for (int i = 0; i < 40; i++) { WiFi.rssi = (i % 2) ? -53 : -58; NOW += 1000; int b = wifiBars(); if (b != last) flips++; last = b; }
  check("no flicker at a threshold", flips <= 2, std::to_string(flips));
  // a single noisy sample doesn't move the bars
  settle(-50, 15); WiFi.rssi = -66; NOW += 1000; int b1 = wifiBars(); WiFi.rssi = -50; NOW += 1000;
  check("one weak sample keeps 4 bars", b1 == 4, std::to_string(b1));
  // an invalid reading (0) is ignored
  settle(-50, 15); WiFi.rssi = 0; NOW += 1000; check("rssi 0 ignored", wifiBars() == 4);
  // disconnected -> 0 bars, reconnect starts fresh
  WiFi.st = WL_IDLE_STATUS; NOW += 1000; check("disconnected -> 0 bars", wifiBars() == 0);
  WiFi.st = WL_CONNECTED; WiFi.rssi = -72; NOW += 1000; check("reconnect adopts the new level", wifiBars() == 2);
  return fails;
}
