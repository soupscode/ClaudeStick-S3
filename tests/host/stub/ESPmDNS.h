#pragma once
#include <Arduino.h>
struct MDNSResponder { bool begin(const char*); void end(); bool addService(const char*, const char*, uint16_t); void addServiceTxt(const char*, const char*, const char*, const char*); };
extern MDNSResponder MDNS;
