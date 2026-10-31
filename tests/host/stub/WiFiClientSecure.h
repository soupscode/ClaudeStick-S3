#pragma once
#include <Arduino.h>
struct WiFiClient {};
struct WiFiClientSecure : WiFiClient { void setInsecure(); void setCACertBundle(const uint8_t*); };
