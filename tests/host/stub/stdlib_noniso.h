#pragma once
extern "C" {
char* itoa(int, char*, int); char* ltoa(long, char*, int); char* utoa(unsigned, char*, int); char* ultoa(unsigned long, char*, int);
char* lltoa(long long, char*, int); char* ulltoa(unsigned long long, char*, int); char* dtostrf(double, signed char, unsigned char, char*);
}
