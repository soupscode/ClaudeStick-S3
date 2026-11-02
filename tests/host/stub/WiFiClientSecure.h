#pragma once
#include <Arduino.h>
struct WiFiClient { bool connected(); void stop(); };
extern int TLS_ERR_CODE; extern const char* TLS_ERR_TEXT;   // tests set what mbedTLS "said"
struct WiFiClientSecure : WiFiClient { void setInsecure(); void setCACertBundle(const uint8_t*); void setHandshakeTimeout(unsigned long);
  int lastError(char* buf, const size_t size) { if (TLS_ERR_CODE && size) { strncpy(buf, TLS_ERR_TEXT, size - 1); buf[size - 1] = 0; } return TLS_ERR_CODE; } };
