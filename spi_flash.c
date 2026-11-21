/*
 * File:   spi_flash.c
 * Author: thassio lima
 *
 * Driver simples para a memoria SPI W25Q16 (2 MB).
 */

#include "hw.h"
#include "spi_flash.h"

#define CMD_WRITE_ENABLE   0x06
#define CMD_READ_STATUS1   0x05
#define CMD_READ_DATA      0x03
#define CMD_PAGE_PROGRAM   0x02
#define CMD_SECTOR_ERASE   0x20
#define CMD_JEDEC_ID       0x9F
#define CMD_RELEASE_PD     0xAB

void spi_init(void)
{
    /* MOSI, CLK, SS(LED) e /CS como saida; MISO entrada */
    DDRB |= (1 << PB3) | (1 << PB5) | (1 << LED_PIN) | (1 << FLASH_CS_PIN);
    DDRB &= ~(1 << PB4);
    FLASH_DESELECT();
    /* mestre, modo 0, fosc/2 = 4 MHz, MSB primeiro */
    SPCR = (1 << SPE) | (1 << MSTR);
    SPSR = (1 << SPI2X);

    /* acorda a memoria caso esteja em power-down */
    FLASH_SELECT();
    spi_troca(CMD_RELEASE_PD);
    FLASH_DESELECT();
}

uint8_t spi_troca(uint8_t dado)
{
    SPDR = dado;
    while (!(SPSR & (1 << SPIF)));
    return SPDR;
}

static void comando_endereco(uint8_t cmd, uint32_t endereco)
{
    spi_troca(cmd);
    spi_troca((uint8_t)(endereco >> 16));
    spi_troca((uint8_t)(endereco >> 8));
    spi_troca((uint8_t)endereco);
}

static void aguardar(void)
{
    FLASH_SELECT();
    spi_troca(CMD_READ_STATUS1);
    while (spi_troca(0xFF) & 0x01);   /* BUSY */
    FLASH_DESELECT();
}

static void habilitar_escrita(void)
{
    FLASH_SELECT();
    spi_troca(CMD_WRITE_ENABLE);
    FLASH_DESELECT();
}

uint32_t flash_ler_id(void)
{
    uint32_t id;
    SPCR &= ~(1 << DORD);
    FLASH_SELECT();
    spi_troca(CMD_JEDEC_ID);
    id = (uint32_t)spi_troca(0xFF) << 16;
    id |= (uint16_t)spi_troca(0xFF) << 8;
    id |= spi_troca(0xFF);
    FLASH_DESELECT();
    return id;
}

void flash_ler(uint32_t endereco, uint8_t *destino, uint16_t n)
{
    SPCR &= ~(1 << DORD);
    FLASH_SELECT();
    comando_endereco(CMD_READ_DATA, endereco);
    while (n--)
        *destino++ = spi_troca(0xFF);
    FLASH_DESELECT();
}

uint8_t flash_ler_byte(uint32_t endereco)
{
    uint8_t b;
    flash_ler(endereco, &b, 1);
    return b;
}

void flash_apagar_setor(uint32_t endereco)
{
    SPCR &= ~(1 << DORD);
    habilitar_escrita();
    FLASH_SELECT();
    comando_endereco(CMD_SECTOR_ERASE, endereco);
    FLASH_DESELECT();
    aguardar();
}

void flash_gravar_pagina(uint32_t endereco, const uint8_t *dados, uint16_t n)
{
    SPCR &= ~(1 << DORD);
    habilitar_escrita();
    FLASH_SELECT();
    comando_endereco(CMD_PAGE_PROGRAM, endereco);
    while (n--)
        spi_troca(*dados++);
    FLASH_DESELECT();
    aguardar();
}
