#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
enum followRedirects_t { HTTPC_DISABLE_FOLLOW_REDIRECTS, HTTPC_STRICT_FOLLOW_REDIRECTS, HTTPC_FORCE_FOLLOW_REDIRECTS };
struct HTTPClient { bool begin(WiFiClient&, const String&); void setFollowRedirects(followRedirects_t); int GET(); bool begin(const String&);  void setTimeout(uint16_t);
 void setConnectTimeout(int32_t); void setReuse(bool); int sendRequest(const char*, uint8_t*, size_t); int getSize();
 void addHeader(const String&, const String&); int POST(uint8_t*, size_t); String getString(); void end(); static String errorToString(int); };
