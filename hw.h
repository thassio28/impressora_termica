/*
 * File:   hw.h
 * Author: thassio lima
 *
 * Definicoes de hardware da versao 2 (ver Versão 2/esquema_v2.pdf).
 *
 *  ATmega328P - clock interno 8 MHz - 3,3 V
 *
 *  PB0  /CS  memoria W25Q16 (25Q16)
 *  PB1  EXP4 (livre)
 *  PB2  LED  (ativo em nivel baixo) - tambem e o SS do SPI, deve ser saida
 *  PB3  MOSI (memoria + cabeca termica, DI)
 *  PB4  MISO (memoria)
 *  PB5  CLK  (memoria + cabeca termica)
 *  PB6  LAT  cabeca termica (ativo em nivel baixo)
 *  PB7  PWR  liga VP (cabeca/motor) e o SLEEP do DRV8833
 *  PC0..PC3  A, A', B, B' do motor (DRV8833)
 *  PC4  SDA  display OLED SSD1306 (J2)
 *  PC5  SCL  display OLED SSD1306 (J2)
 *  PD0/PD1  UART (J3)
 *  PD2  PS   sensor de papel (INT0) - nivel baixo = sem papel
 *  PD3  EN_BUCK  nivel ALTO DESLIGA o conversor buck (Q101 puxa OCSET)
 *  PD4  EXP2 (livre)
 *  PD5  DST  strobe da cabeca termica (OC0B)
 *  PD7  BUTTON (botao para GND, usa pull-up interno)
 *  ADC7 TH   termistor da cabeca termica
 */

#ifndef HW_H
#define HW_H

#ifndef F_CPU
#define F_CPU 8000000UL
#endif

#include <avr/io.h>

#define FLASH_CS_PIN   PB0
#define LED_PIN        PB2
#define LAT_PIN        PB6
#define PWR_PIN        PB7
#define PS_PIN         PD2
#define EN_BUCK_PIN    PD3
#define DST_PIN        PD5
#define BUTTON_PIN     PD7

#define LED_ON()       (PORTB &= ~(1 << LED_PIN))
#define LED_OFF()      (PORTB |=  (1 << LED_PIN))

#ifndef FLASH_SELECT
#define FLASH_SELECT()   (PORTB &= ~(1 << FLASH_CS_PIN))
#define FLASH_DESELECT() (PORTB |=  (1 << FLASH_CS_PIN))
#endif

#define BUCK_ON()      (PORTD &= ~(1 << EN_BUCK_PIN))
#define BUCK_OFF()     (PORTD |=  (1 << EN_BUCK_PIN))

#define PWR_ON()       (PORTB |=  (1 << PWR_PIN))
#define PWR_OFF()      (PORTB &= ~(1 << PWR_PIN))

#define TEM_PAPEL()    (PIND & (1 << PS_PIN))
#define BOTAO_APERTADO() (!(PIND & (1 << BUTTON_PIN)))

/* Cabeca termica LTP02-245: 384 pontos por linha = 48 bytes */
#define CABECA_PONTOS  384
#define CABECA_BYTES   (CABECA_PONTOS / 8)

/* Bitmap: 320 x 240 pixels, 1 bit por pixel (8 pixels por byte),
 * linha a linha, MSB = pixel da esquerda, 1 = ponto preto (imprime).
 * 320 / 8 = 40 bytes por linha, 40 * 240 = 9600 bytes. */
#define BMP_LARGURA    320
#define BMP_ALTURA     240
#define BMP_BYTES_LINHA (BMP_LARGURA / 8)
#define BMP_TAMANHO    ((uint16_t)BMP_BYTES_LINHA * BMP_ALTURA)
/* margem para centralizar 320 pontos em 384: (48 - 40) / 2 = 4 bytes */
#define BMP_MARGEM     ((CABECA_BYTES - BMP_BYTES_LINHA) / 2)

/* Slots na memoria: cada slot ocupa 3 setores de 4 KB (12 KB) */
#define FLASH_SETOR       4096UL
#define SLOT_SETORES      3
#define SLOT_TAMANHO      (FLASH_SETOR * SLOT_SETORES)
#define NUM_SLOTS         8
#define SLOT_ENDERECO(s)  ((uint32_t)(s) * SLOT_TAMANHO)
/* marcador gravado logo apos os dados quando o bitmap foi recebido completo */
#define SLOT_MARCADOR_END(s) (SLOT_ENDERECO(s) + BMP_TAMANHO)
#define SLOT_MARCADOR_0   0xB1
#define SLOT_MARCADOR_1   0x7A

#endif
