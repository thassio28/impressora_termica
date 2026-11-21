/* eeprom.h falso para o simulador: 1 KB de EEPROM em RAM (apagada = 0xFF) */
#include <stdint.h>
#include <string.h>
#define EEMEM
static uint8_t sim_eeprom[1024];
static int sim_eeprom_ok = 0;
static inline uint8_t eeprom_read_byte(const uint8_t *p)
{
    if (!sim_eeprom_ok) { memset(sim_eeprom, 0xFF, sizeof(sim_eeprom)); sim_eeprom_ok = 1; }
    return sim_eeprom[(uintptr_t)p & 1023];
}
static inline void eeprom_update_byte(uint8_t *p, uint8_t v)
{
    if (!sim_eeprom_ok) { memset(sim_eeprom, 0xFF, sizeof(sim_eeprom)); sim_eeprom_ok = 1; }
    sim_eeprom[(uintptr_t)p & 1023] = v;
}
