#include "../../firmware/src/main.cpp"
#include <cstdio>
void M5Canvas::pushSprite(int,int){}
static int bad = 0;
static void chk(const char* in, const char* want) { String g = asciiFold(in); bool ok = g == want; if (!ok) bad++; printf("%s %s -> [%s]\n", ok ? "PASS" : "FAIL", in, g.c_str()); }
int main() {
  chk("plain ascii", "plain ascii");
  chk("let\xe2\x80\x99s \xe2\x80\x9cgo\xe2\x80\x9d \xe2\x80\x94 now\xe2\x80\xa6", "let's \"go\" - now...");
  chk("caf\xc3\xa9 na\xc3\xafve \xc3\x85ngstr\xc3\xb6m", "cafe naive Angstrom");
  chk("done \xf0\x9f\x9a\x80\xf0\x9f\x8e\x89 ok", "done ? ok");
  chk("a\tb\r\nc", "a  b\nc");
  chk("\xe4\xbd\xa0\xe5\xa5\xbd", "?");
  chk("trunc \xe2\x80", "trunc ?");
  return bad;
}
