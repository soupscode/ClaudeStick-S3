#pragma once
#include <Arduino.h>
struct WiFiClient { bool connected(); void stop(); };
struct WiFiClientSecure : WiFiClient { void setInsecure(); void setCACertBundle(const uint8_t*); void setHandshakeTimeout(unsigned long); };
