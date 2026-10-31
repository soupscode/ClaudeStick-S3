#pragma once
#include <Arduino.h>
struct DNSServer { bool start(uint16_t, const String&, IPAddress){return true;} void stop(){} void processNextRequest(){} };
