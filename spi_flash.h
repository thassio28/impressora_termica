/*
 * File:   spi_flash.h
 * Author: thassio lima
 *
 * SPI compartilhado entre a memoria W25Q16 (25Q16) e a cabeca termica.
 *
 * A cabeca termica nao tem chip select: ela desloca o MOSI a cada pulso de
 * CLK, inclusive durante os acessos a memoria. Isso nao causa problema porque
 * o conteudo do registrador de deslocamento so e usado quando o LAT e
 * pulsado, e isso so e feito logo depois de enviar uma linha completa
 * (48 bytes) para a cabeca. Regras para evitar conflito:
 *   - /CS da memoria sempre em nivel alto quando enviando dados para a cabeca;
 *   - durante a impressao o SPI e usado apenas dentro da interrupcao do
 *     Timer0 (o programa principal nao acessa a memoria enquanto imprime).
 */

#ifndef SPI_FLASH_H
#define SPI_FLASH_H

#include <stdint.h>

void spi_init(void);
uint8_t spi_troca(uint8_t dado);

uint32_t flash_ler_id(void);           /* JEDEC ID, W25Q16 = 0xEF4015 */
void flash_ler(uint32_t endereco, uint8_t *destino, uint16_t n);
uint8_t flash_ler_byte(uint32_t endereco);
void flash_apagar_setor(uint32_t endereco);   /* 4 KB */
/* grava ate 256 bytes; nao pode atravessar o limite de uma pagina */
void flash_gravar_pagina(uint32_t endereco, const uint8_t *dados, uint16_t n);

#endif
