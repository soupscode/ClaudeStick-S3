#pragma once
#include <Arduino.h>
#include <map>
#include <string>
extern std::map<std::string,std::string> PREFS;
struct Preferences {
 bool begin(const char*, bool){return true;}
 String getString(const char* k, String d = String()){ auto i=PREFS.find(k); return i==PREFS.end()?d:String(i->second.c_str()); }
 size_t putString(const char* k, String v){ PREFS[k]=v.c_str(); return v.length(); }
 bool remove(const char* k){ PREFS.erase(k); return true; }
 bool getBool(const char* k, bool d=false){ auto i=PREFS.find(k); return i==PREFS.end()?d:i->second=="1"; }
 size_t putBool(const char* k, bool v){ PREFS[k]=v?"1":"0"; return 1; }
 int8_t getChar(const char* k, int8_t d=0){ auto i=PREFS.find(k); return i==PREFS.end()?d:(int8_t)atoi(i->second.c_str()); }
 size_t putChar(const char* k, int8_t v){ PREFS[k]=std::to_string(v); return 1; }
 uint8_t getUChar(const char* k, uint8_t d=0){ auto i=PREFS.find(k); return i==PREFS.end()?d:(uint8_t)atoi(i->second.c_str()); }
 size_t putUChar(const char* k, uint8_t v){ PREFS[k]=std::to_string(v); return 1; }
 int32_t getInt(const char* k, int32_t d=0){ auto i=PREFS.find(k); return i==PREFS.end()?d:atoi(i->second.c_str()); }
 size_t putInt(const char* k, int32_t v){ PREFS[k]=std::to_string(v); return 4; }
 size_t putBytes(const char* k, const void* p, size_t n){ PREFS[k]=std::string((const char*)p,n); return n; }
 size_t getBytes(const char* k, void* p, size_t n){ auto i=PREFS.find(k); if(i==PREFS.end()) return 0; memcpy(p,i->second.data(),std::min(n,i->second.size())); return std::min(n,i->second.size()); }
 size_t getBytesLength(const char* k){ auto i=PREFS.find(k); return i==PREFS.end()?0:i->second.size(); }
};
