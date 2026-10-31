#include "../../firmware/src/main.cpp"
#include <iostream>
#include <iterator>
extern unsigned long NOW;
void M5Canvas::pushSprite(int,int){}
int main(){
  setup();
  Serial.out.clear();
  std::string in((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  Serial.in = in;
  for (int i = 0; i < 50; i++) serialTick();
  fwrite(Serial.out.data(), 1, Serial.out.size(), stdout);
  fprintf(stderr, "ssid=%s pass=%s wifi=%d setupDone=%d\n", ssid.c_str(), pass.c_str(), WiFi.status(), setupDone);
  return 0;
}
