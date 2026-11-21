#include <stdint.h>
#define EEMEM
static inline uint8_t eeprom_read_byte(const uint8_t *p) { return *p; }
static inline void eeprom_update_byte(uint8_t *p, uint8_t v) { *p = v; }
