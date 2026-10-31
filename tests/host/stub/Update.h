#pragma once
#include <Arduino.h>
#define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF
struct UpdateClass { bool begin(size_t); size_t write(uint8_t*, size_t); bool end(bool); bool hasError(); bool isFinished(); void abort(); };
extern UpdateClass Update;
