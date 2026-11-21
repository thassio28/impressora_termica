# Compilacao com avr-gcc (alternativa ao MPLAB X)
#   make            gera impressora_termica_v2.hex
#   make gravar     grava com avrdude (ajuste PROGRAMADOR/PORTA)
#   make tamanho    mostra uso de flash e RAM

MCU         = atmega328p
F_CPU       = 8000000UL
ALVO        = impressora_termica_v2
FONTES      = main.c spi_flash.c oled.c
PROGRAMADOR ?= usbasp
PORTA       ?= usb

CC      = avr-gcc
OBJCOPY = avr-objcopy
SIZE    = avr-size
CFLAGS  = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os -std=gnu99 -Wall -Wextra \
          -ffunction-sections -fdata-sections -fshort-enums
LDFLAGS = -mmcu=$(MCU) -Wl,--gc-sections

OBJ = $(FONTES:.c=.o)

all: $(ALVO).hex tamanho

$(ALVO).elf: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c hw.h spi_flash.h oled.h font.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(ALVO).hex: $(ALVO).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

tamanho: $(ALVO).elf
	$(SIZE) --mcu=$(MCU) -C $< 2>/dev/null || $(SIZE) $<

# fusiveis da V1/V2: clock interno 8 MHz sem divisor (lfuse 0xE2)
gravar: $(ALVO).hex
	avrdude -c $(PROGRAMADOR) -P $(PORTA) -p m328p -U flash:w:$<:i

clean:
	rm -f $(OBJ) $(ALVO).elf $(ALVO).hex

.PHONY: all tamanho gravar clean
