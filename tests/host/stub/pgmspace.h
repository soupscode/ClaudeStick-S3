#pragma once
#define PROGMEM
#define PGM_P const char*
#define PSTR(s) (s)
#define pgm_read_byte(a) (*(const unsigned char*)(a))
#define strlen_P strlen
#define strcpy_P strcpy
#define memcpy_P memcpy
