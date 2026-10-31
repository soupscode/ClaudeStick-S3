#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {
char* itoa(int v, char* s, int b){ if(b==16) sprintf(s,"%x",v); else sprintf(s,"%d",v); return s; }
char* ltoa(long v, char* s, int b){ if(b==16) sprintf(s,"%lx",v); else sprintf(s,"%ld",v); return s; }
char* utoa(unsigned v, char* s, int b){ if(b==16) sprintf(s,"%x",v); else sprintf(s,"%u",v); return s; }
char* ultoa(unsigned long v, char* s, int b){ if(b==16) sprintf(s,"%lx",v); else sprintf(s,"%lu",v); return s; }
char* lltoa(long long v, char* s, int b){ sprintf(s,"%lld",v); return s; }
char* ulltoa(unsigned long long v, char* s, int b){ sprintf(s,"%llu",v); return s; }
char* dtostrf(double v, signed char w, unsigned char p, char* s){ sprintf(s,"%*.*f",w,p,v); return s; }
}
