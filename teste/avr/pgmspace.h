#include <string.h>
#include <stdint.h>
#define PROGMEM
#define PSTR(s) (s)
#define pgm_read_byte(p) (*(const uint8_t *)(p))
#define memcpy_P memcpy
#define strcpy_P strcpy
#define strcat_P strcat
