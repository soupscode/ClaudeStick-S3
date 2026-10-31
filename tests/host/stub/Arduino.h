#pragma once
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <algorithm>
using std::min; using std::max;
typedef bool boolean; typedef uint8_t byte;
#include "WString.h"
#include "Print.h"
#include "Stream.h"
#include "IPAddress.h"
static const uint8_t TX = 43, RX = 44, SDA = 8, SCL = 9, SS = 10, MOSI = 11, MISO = 13, SCK = 12;   // pins_arduino.h (esp32s3)
unsigned long millis(); void delay(unsigned long); void configTzTime(const char* tz, const char* s1, const char* s2 = nullptr, const char* s3 = nullptr);
uint32_t esp_random();
enum esp_reset_reason_t { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO };
inline esp_reset_reason_t esp_reset_reason(){ return ESP_RST_POWERON; }
inline void* ps_malloc(size_t n){ return malloc(n); }
bool setCpuFrequencyMhz(uint32_t); uint32_t getCpuFrequencyMhz();
#include <string>
struct HWSerialStub : public Stream { using Print::write; std::string in, out; size_t rp=0;
  int available() override {return in.size()-rp;} int read() override {return rp<in.size()?(uint8_t)in[rp++]:-1;} int peek() override {return rp<in.size()?(uint8_t)in[rp]:-1;}
  size_t write(uint8_t c) override {out.push_back((char)c); return 1;} void begin(unsigned long){} };
extern HWSerialStub Serial;
extern int RESTARTS, HEAP_FREE; struct EspClass { uint32_t getFreeHeap(){return HEAP_FREE;} uint32_t getFreePsram(){return 7000000;} uint32_t getMinFreeHeap(){return 150000;} void restart(){ RESTARTS++; } }; extern EspClass ESP;
