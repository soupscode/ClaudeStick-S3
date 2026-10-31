#pragma once
#include <Arduino.h>
enum wl_status_t { WL_IDLE_STATUS=0, WL_CONNECTED=3 };
#define WIFI_STA 1
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_AP_STA 3
enum wifi_ps_type_t { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM };
enum wifi_auth_mode_t { WIFI_AUTH_OPEN=0, WIFI_AUTH_WPA2_PSK=3 };
struct WiFiClass { int8_t rssi=-52; int8_t RSSI(){return rssi;} bool softAP(const char*); IPAddress softAPIP(); bool softAPdisconnect(bool); int16_t scanNetworks(bool async = false); int16_t scanComplete(); String SSID(uint8_t); int32_t RSSI(uint8_t); wifi_auth_mode_t encryptionType(uint8_t); void scanDelete(); wl_status_t st=WL_CONNECTED; wl_status_t status(){return st;} void begin(const char*, const char*); bool disconnect(bool=false){ st=WL_IDLE_STATUS; return true;}
 void mode(int){} bool setHostname(const char*){return true;} bool setSleep(bool){return true;} bool setSleep(wifi_ps_type_t t){ ps=t; return true; } wifi_ps_type_t ps = WIFI_PS_MIN_MODEM; IPAddress localIP(){return IPAddress(192,168,1,50);} void macAddress(uint8_t* m){memset(m,0xAB,6);} };
struct WiFiExtra {};
extern WiFiClass WiFi;
