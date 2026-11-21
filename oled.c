/*
 * File:   oled.c
 * Author: thassio lima
 *
 * Driver SSD1306 128x64 via I2C, sem framebuffer.
 */

#include "hw.h"
#include "oled.h"
#include <avr/pgmspace.h>
#include <util/twi.h>

/* definida em font.h (incluido apenas pelo main.c) */
extern const uint8_t font_terminus_8x16[] PROGMEM;

#define TWI_TIMEOUT 2000

static uint8_t presente = 0;

static uint8_t twi_esperar(void)
{
    uint16_t t = TWI_TIMEOUT;
    while (!(TWCR & (1 << TWINT)))
        if (--t == 0)
            return 0;
    return 1;
}

static uint8_t twi_inicio(uint8_t endereco)
{
    TWCR = (1 << TWINT) | (1 << TWSTA) | (1 << TWEN);
    if (!twi_esperar())
        return 0;
    TWDR = endereco << 1;
    TWCR = (1 << TWINT) | (1 << TWEN);
    if (!twi_esperar())
        return 0;
    return (TW_STATUS == TW_MT_SLA_ACK);
}

static void twi_escrever(uint8_t d)
{
    TWDR = d;
    TWCR = (1 << TWINT) | (1 << TWEN);
    twi_esperar();
}

static void twi_fim(void)
{
    TWCR = (1 << TWINT) | (1 << TWSTO) | (1 << TWEN);
}

static void comandos(const uint8_t *lista, uint8_t n)
{
    if (!twi_inicio(OLED_ENDERECO)) {
        twi_fim();
        return;
    }
    twi_escrever(0x00);  /* sequencia de comandos */
    while (n--)
        twi_escrever(pgm_read_byte(lista++));
    twi_fim();
}

static const uint8_t init_seq[] PROGMEM = {
    0xAE,       /* display off */
    0xD5, 0x80, /* clock */
    0xA8, 0x3F, /* multiplex 64 */
    0xD3, 0x00, /* offset */
    0x40,       /* linha inicial 0 */
    0x8D, 0x14, /* charge pump ligado */
    0x20, 0x02, /* enderecamento por pagina */
    0xA1,       /* espelho horizontal */
    0xC8,       /* varredura de cima para baixo */
    0xDA, 0x12,
    0x81, 0xCF, /* contraste */
    0xD9, 0xF1,
    0xDB, 0x40,
    0xA4,
    0xA6,       /* normal (nao invertido) */
    0xAF        /* display on */
};

uint8_t oled_init(void)
{
    /* pull-ups internos (os modulos normalmente ja tem resistores) */
    PORTC |= (1 << PC4) | (1 << PC5);
    TWSR = 0;
    TWBR = 2;   /* 8 MHz / (16 + 2*2) = 400 kHz */

    presente = twi_inicio(OLED_ENDERECO);
    twi_fim();
    if (!presente)
        return 0;
    comandos(init_seq, sizeof(init_seq));
    oled_limpar();
    return 1;
}

void oled_pos(uint8_t pagina, uint8_t coluna)
{
    uint8_t c[3];
    if (!presente)
        return;
    c[0] = 0xB0 | pagina;
    c[1] = 0x00 | (coluna & 0x0F);
    c[2] = 0x10 | (coluna >> 4);
    if (!twi_inicio(OLED_ENDERECO)) {
        twi_fim();
        return;
    }
    twi_escrever(0x00);
    twi_escrever(c[0]);
    twi_escrever(c[1]);
    twi_escrever(c[2]);
    twi_fim();
}

static uint8_t transmitindo = 0;

void oled_dados_inicio(void)
{
    transmitindo = 0;
    if (!presente)
        return;
    if (twi_inicio(OLED_ENDERECO)) {
        twi_escrever(0x40);  /* dados */
        transmitindo = 1;
    }
}

void oled_dado(uint8_t d)
{
    if (transmitindo)
        twi_escrever(d);
}

void oled_dados_fim(void)
{
    if (presente)
        twi_fim();
    transmitindo = 0;
}

void oled_limpar(void)
{
    uint8_t p, x;
    for (p = 0; p < OLED_PAGINAS; p++) {
        oled_pos(p, 0);
        oled_dados_inicio();
        for (x = 0; x < OLED_LARGURA; x++)
            oled_dado(0);
        oled_dados_fim();
    }
}

/* metade (0 = de cima, 1 = de baixo) de um caractere 8x16 em formato de coluna */
static void caractere(char c, uint8_t metade, uint8_t inverter)
{
    const uint8_t *g;
    uint8_t x, y, col;
    if (c < 32 || c > 126)
        c = ' ';
    g = font_terminus_8x16 + (uint16_t)(c - 32) * 16 + metade * 8;
    for (x = 0; x < 8; x++) {
        col = 0;
        for (y = 0; y < 8; y++)
            if (pgm_read_byte(g + y) & (1 << x))   /* LSB = pixel da esquerda */
                col |= (1 << y);
        oled_dado(inverter ? (uint8_t)~col : col);
    }
}

static void texto(uint8_t linha, const char *s, uint8_t inverter, uint8_t flash)
{
    uint8_t metade, n;
    const char *p;
    char c;
    if (!presente || linha >= OLED_LINHAS)
        return;
    for (metade = 0; metade < 2; metade++) {
        oled_pos(linha * 2 + metade, 0);
        oled_dados_inicio();
        p = s;
        for (n = 0; n < OLED_COLUNAS; n++) {
            c = flash ? pgm_read_byte(p) : *p;
            if (c)
                p++;
            else
                c = ' ';
            caractere(c, metade, inverter);
        }
        oled_dados_fim();
    }
}

void oled_texto(uint8_t linha, const char *s, uint8_t inverter)
{
    texto(linha, s, inverter, 0);
}

void oled_texto_P(uint8_t linha, const char *s, uint8_t inverter)
{
    texto(linha, s, inverter, 1);
}
