/*
 * File:   oled.h
 * Author: thassio lima
 *
 * Display OLED SSD1306 128x64 via I2C (TWI do ATmega328, PC4/PC5).
 * Nao usa framebuffer (a RAM do ATmega328 e pequena): escreve direto nas
 * paginas do display.
 *
 * Texto usa a fonte Terminus 8x16 do font.h: 4 linhas x 16 caracteres.
 */

#ifndef OLED_H
#define OLED_H

#include <stdint.h>

#define OLED_ENDERECO  0x3C   /* alguns modulos usam 0x3D */
#define OLED_LARGURA   128
#define OLED_PAGINAS   8
#define OLED_LINHAS    4      /* linhas de texto de 16 pixels */
#define OLED_COLUNAS   16     /* caracteres por linha */

uint8_t oled_init(void);      /* retorna 1 se o display respondeu */
void oled_limpar(void);
void oled_pos(uint8_t pagina, uint8_t coluna);
void oled_dados_inicio(void);
void oled_dado(uint8_t d);
void oled_dados_fim(void);

/* escreve uma linha de texto (0..3), completa com espacos ate o fim */
void oled_texto(uint8_t linha, const char *s, uint8_t inverter);
void oled_texto_P(uint8_t linha, const char *s, uint8_t inverter);

#endif
