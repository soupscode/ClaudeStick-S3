#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <string>
#include <vector>
M5Unified M5; WiFiClass WiFi; HWSerialStub Serial; EspClass ESP; MDNSResponder MDNS;
float IMU_ACC[3] = {0, 0, 1};
int TLS_ERR_CODE = 0; const char* TLS_ERR_TEXT = "";
unsigned long NOW = 100000; int TONES = 0; int BATT_PCT = 87; bool BATT_CHARGING = false;
std::string GOOD_PASS = "goodpass";
unsigned long millis(){ return NOW; } void delay(unsigned long d){ NOW += d; } uint32_t esp_random(){ return 0x1234abcd; }
void WiFiClass::begin(const char* s, const char* p){ st = (p && GOOD_PASS == p) ? WL_CONNECTED : WL_IDLE_STATUS; }
bool WiFiClass::softAP(const char*){return true;} IPAddress WiFiClass::softAPIP(){return IPAddress(192,168,4,1);} bool WiFiClass::softAPdisconnect(bool){return true;}
// Tests can set SCAN to what the radio "sees"; SCAN_PENDING makes an async scan stay unfinished.
std::vector<std::pair<std::string,int>> SCAN; bool SCAN_PENDING = false; int SCANS_STARTED = 0;
int16_t WiFiClass::scanNetworks(bool async){ SCANS_STARTED++; if (async) { SCAN_PENDING = true; return WIFI_SCAN_RUNNING; } return SCAN.empty() ? 2 : (int16_t)SCAN.size(); }
int16_t WiFiClass::scanComplete(){ if (SCAN_PENDING) return WIFI_SCAN_RUNNING; return SCAN.empty() ? 2 : (int16_t)SCAN.size(); }
String WiFiClass::SSID(uint8_t i){ if (!SCAN.empty()) return String(SCAN[i].first.c_str()); return i?"neighbors":"home-wifi"; }
int32_t WiFiClass::RSSI(uint8_t i){ if (!SCAN.empty()) return SCAN[i].second; return i?-80:-48; }
wifi_auth_mode_t WiFiClass::encryptionType(uint8_t i){return i?WIFI_AUTH_OPEN:WIFI_AUTH_WPA2_PSK;} void WiFiClass::scanDelete(){}
WebServer::WebServer(int){} void WebServer::on(const String&, HTTPMethod, THandlerFunction){} void WebServer::onNotFound(THandlerFunction){}
void WebServer::begin(){} void WebServer::stop(){} void WebServer::handleClient(){} std::string REQ_BODY, LAST_SENT; int LAST_CODE = 0;
String WebServer::arg(const String& n){ return n == "plain" ? String(REQ_BODY.c_str()) : String(""); } String WebServer::header(const String&){return "";}
HTTPMethod WebServer::method(){return HTTP_GET;} void WebServer::send(int c, const char*, const String& b){ LAST_CODE = c; LAST_SENT = b.c_str(); } void WebServer::send_P(int,const char*,const char*){}
void WebServer::collectHeaders(const char**, size_t){} void WebServer::sendHeader(const String&, const String&){}
bool MDNSResponder::begin(const char*){return true;} void MDNSResponder::end(){} bool MDNSResponder::addService(const char*,const char*,uint16_t){return true;}
bool BLE_ON = false; uint32_t PK = 0;
void bleInit(const char*){} bool bleConnected(){return BLE_ON;} bool bleSecure(){return true;} uint32_t blePasskey(){return PK;}
void bleClearBonds(){} size_t bleAvailable(){return 0;} int bleRead(){return -1;} size_t bleWrite(const uint8_t*, size_t n){return n;}
#include <HTTPClient.h>
#include <string>
extern std::string HTTP_BODY, LAST_URL; extern int HTTP_CODE;
#include <Update.h>
UpdateClass Update;
bool UpdateClass::begin(size_t){return true;} size_t UpdateClass::write(uint8_t*, size_t n){return n;} bool UpdateClass::end(bool){return true;}
bool UpdateClass::hasError(){return false;} bool UpdateClass::isFinished(){return true;} void UpdateClass::abort(){}
bool HTTPClient::begin(const String&){return true;} void HTTPClient::setTimeout(uint16_t){}
// A bridge client test scripts replies with HTTP_SCRIPT (one {status, body} per request, in order) and reads HTTP_LOG back.
struct HttpScripted { int code; std::string body; };
std::vector<HttpScripted> HTTP_SCRIPT; std::vector<std::string> HTTP_LOG; std::string HTTP_HEADERS, HTTP_RESP; bool HTTP_RESP_SET = false;
bool TLS_UP = false; int TLS_STOPS = 0, TLS_CONNECTS = 0, HTTP_REUSE = 0;
void HTTPClient::addHeader(const String& k, const String& v){ HTTP_HEADERS += std::string(k.c_str()) + ": " + v.c_str() + "\n"; }
int HTTPClient::POST(uint8_t*, size_t){return 200;} String HTTPClient::getString(){return String((HTTP_RESP_SET ? HTTP_RESP : HTTP_BODY).c_str());} void HTTPClient::end(){ HTTP_HEADERS.clear(); }
void HTTPClient::setConnectTimeout(int32_t){} void HTTPClient::setReuse(bool r){ HTTP_REUSE += r; } int HTTPClient::getSize(){ return (int)(HTTP_RESP_SET ? HTTP_RESP : HTTP_BODY).size(); }
int HTTPClient::sendRequest(const char* m, uint8_t* body, size_t n){
  HTTP_LOG.push_back(std::string(m) + " " + LAST_URL + "\n" + HTTP_HEADERS + std::string((const char*)(body ? body : (uint8_t*)""), body ? n : 0));
  if (HTTP_SCRIPT.empty()) { HTTP_RESP_SET = false; return HTTP_CODE; }
  HttpScripted r = HTTP_SCRIPT.front(); HTTP_SCRIPT.erase(HTTP_SCRIPT.begin());
  if (r.code > 0) { TLS_UP = true; HTTP_RESP = r.body; HTTP_RESP_SET = true; } else TLS_UP = false;
  return r.code;
}
bool WiFiClient::connected(){ return TLS_UP; } void WiFiClient::stop(){ if (TLS_UP) TLS_STOPS++; TLS_UP = false; }
void WiFiClientSecure::setHandshakeTimeout(unsigned long){}
String HTTPClient::errorToString(int){return "err";} void WiFiClientSecure::setInsecure(){}
bool setCpuFrequencyMhz(uint32_t){return true;} uint32_t getCpuFrequencyMhz(){return 240;}
void MDNSResponder::addServiceTxt(const char*, const char*, const char*, const char*){}
bool Mic_Class::begin(){return true;} void Mic_Class::end(){} bool Mic_Class::record(int16_t*, size_t, uint32_t){return true;} size_t Mic_Class::isRecording(){return 0;}
void Speaker_Class::end(){}
void WebServer::on(const String&, HTTPMethod, THandlerFunction, THandlerFunction){} HTTPUpload& WebServer::upload(){ static HTTPUpload u; return u; }

#include <map>
#include <HTTPUpdate.h>
#include <esp_ota_ops.h>
std::map<std::string,std::string> PREFS;
std::string HTTP_BODY = "{}"; int HTTP_CODE = 200; std::string LAST_URL;
bool HTTPClient::begin(WiFiClient&, const String& u){ LAST_URL=u.c_str(); return true; }
void HTTPClient::setFollowRedirects(followRedirects_t){} int HTTPClient::GET(){ return HTTP_CODE; }
void WiFiClientSecure::setCACertBundle(const uint8_t*){}
extern const uint8_t rootca_crt_bundle_start[1] asm("_binary_data_cert_x509_crt_bundle_bin_start");
const uint8_t rootca_crt_bundle_start[1] = {0};
HTTPUpdate httpUpdate; std::function<void(int,int)> PROG; int UPDATE_RESULT = HTTP_UPDATE_OK; std::string UPDATE_URL;
void HTTPUpdate::rebootOnUpdate(bool){} void HTTPUpdate::setFollowRedirects(followRedirects_t){} void HTTPUpdate::onProgress(std::function<void(int,int)> f){ PROG=f; }
t_httpUpdate_return HTTPUpdate::update(WiFiClient&, const String& u){ UPDATE_URL=u.c_str(); if(PROG){ for(int i=0;i<=10;i++) PROG(i*150000, 1500000);} return (t_httpUpdate_return)UPDATE_RESULT; }
String HTTPUpdate::getLastErrorString(){ return "HTTP error: 404"; }
esp_partition_t P0{0}, P1{1}; bool OTHER_VALID = true; int BOOT_SET = -1;
const esp_partition_t* esp_ota_get_running_partition(){ return &P0; } const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*){ return &P1; }
esp_err_t esp_ota_get_partition_description(const esp_partition_t*, esp_app_desc_t*){ return OTHER_VALID ? ESP_OK : -1; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* p){ BOOT_SET = p->x; return ESP_OK; }

int RESTARTS = 0, HEAP_FREE = 200000;

// ---- time: never touch the real system clock. settimeofday() freezes a fake epoch for time().
#include <ctime>
#include <sys/time.h>
std::string NTP_TZ; int NTP_CALLS = 0;
void configTzTime(const char* tz, const char*, const char*, const char*) { NTP_TZ = tz; NTP_CALLS++; }
extern "C" {
time_t __real_time(time_t*);
static time_t FAKE_EPOCH = 0;
int __wrap_settimeofday(const struct timeval* tv, const struct timezone*) { FAKE_EPOCH = tv->tv_sec; return 0; }
time_t __wrap_time(time_t* t) { time_t r = FAKE_EPOCH ? FAKE_EPOCH : __real_time(nullptr); if (t) *t = r; return r; }
}
