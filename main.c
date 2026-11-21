/*
 * File:   main.c
 * Author: thassio lima
 *
 * Created on February 9, 2025, 10:07 PM
 *
 * Impressora termica - versao 2
 * ATmega328P (8 MHz interno), cabeca termica LTP02-245, memoria W25Q16,
 * display OLED SSD1306 128x64 (I2C).
 *
 * Funcoes:
 *  - impressao de texto recebido pela serial (terminado com CR, igual a V1);
 *  - selecao de fonte (fontes em font.h, escolha salva na EEPROM);
 *  - recepcao de bitmap 320x240 (1 bit por pixel, 9600 bytes) pela serial,
 *    gravado em slots na memoria externa com controle de fluxo XON/XOFF;
 *  - impressao de bitmap lido da memoria externa;
 *  - pre-visualizacao do texto ou do bitmap no display OLED;
 *  - menu no display controlado pelo botao:
 *        clique curto = proxima opcao, clique longo (>0,7 s) = selecionar.
 *
 * Protocolo serial (9600 8N1):
 *  texto<CR>         guarda o texto para impressao (ate 480 caracteres;
 *                    '\n' dentro do texto quebra a linha)
 *  ESC 'T'           imprime o texto guardado
 *  ESC 'F' n         seleciona a fonte n ('0'..'3')
 *  ESC 'B' n         recebe bitmap no slot n ('0'..'7'): o dispositivo apaga
 *                    o slot, responde "PRONTO" e espera 9600 bytes.
 *                    Responde "OK" ao terminar ou "ERRO ..." em caso de falha.
 *  ESC 'P' n         imprime o bitmap do slot n
 *  ESC 'V' n         mostra o bitmap do slot n no display
 *  ESC 'S'           responde "FONTE=f SLOT=s SLOTS=10000000" (1 = ocupado)
 *  ESC = 0x1B. Veja tools/enviar.py.
 */

#include "hw.h"

#include <stdlib.h>
#include <string.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <avr/pgmspace.h>
#include <util/delay.h>

#include "font.h"
#include "spi_flash.h"
#include "oled.h"

/* ------------------------------------------------------------------------ */
/* Configuracoes                                                            */
/* ------------------------------------------------------------------------ */

#define PASSOS_POR_LINHA       4    /* passos do motor por linha de pontos (igual a V1) */
#define LINHAS_AVANCO_INICIO   12   /* 48 passos antes de imprimir (datasheet 3.3.6) */
#define LINHAS_AVANCO_FIM      96   /* avanco no fim para destacar o papel */
#define TEMPERATURA_LIMITE     25   /* ADCH < 25 -> cabeca acima de ~75 C (V1) */
/* maximo de pontos acionados ao mesmo tempo (datasheet: 45). Cada ponto
 * consome ~VP/162 ohm (~42 mA a 6,8 V); 45 pontos ~1,9 A. Linhas com mais
 * pontos sao divididas em partes impressas com o motor parado. A V1 nao tinha
 * esse limite; aumentar deixa a impressao mais rapida, se a fonte aguentar
 * (384 = sem divisao). */
#define MAX_PONTOS             45

#define TEXTO_MAX              480
#define RX_TAM                 512  /* potencia de 2 */
#define RX_XOFF                384  /* envia XOFF acima disso */
#define RX_XON                 128  /* envia XON abaixo disso */
#define XON                    0x11
#define XOFF                   0x13
#define ESC                    0x1B

#define TEMPO_CLIQUE_LONGO     700  /* ms */
#define TEMPO_MENSAGEM         1500 /* ms */
#define TEMPO_BYTE_BITMAP      3000 /* ms sem receber nada durante o bitmap */
#define TEMPO_INICIO_SERIAL    5000 /* ms para comecar a enviar apos ESC B */
#define TEMPO_INICIO_MENU      60000U /* ms esperando o bitmap pelo menu */

/* ------------------------------------------------------------------------ */
/* Variaveis                                                                */
/* ------------------------------------------------------------------------ */

/* sequencia do motor (igual a V1) */
static const uint8_t passo[8] = {
    0b00001010, 0b00000010, 0b00000110, 0b00000100,
    0b00000101, 0b00000001, 0b00001001, 0b00001000
};

static char texto[TEXTO_MAX + 1];
static uint16_t texto_len = 0;   /* tamanho do texto pronto para imprimir */
static uint16_t rx_len = 0;      /* caracteres do texto sendo recebido */

static volatile uint8_t rx_buf[RX_TAM];
static volatile uint16_t rx_ini = 0, rx_fim = 0;
static uint8_t xoff_enviado = 0;

/* buffer de 256 bytes: pagina da memoria durante a recepcao do bitmap e
 * area de trabalho da pre-visualizacao (nunca usados ao mesmo tempo) */
static uint8_t pagina_buf[256];
static uint8_t linha_buf[CABECA_BYTES];
static uint8_t parte_buf[CABECA_BYTES];   /* parte da linha enviada a cabeca */

static volatile uint16_t ms = 0;

static uint8_t fonte_sel = 0;
static uint8_t slot_sel = 0;
static fonte_t fonte;            /* copia em RAM da fonte selecionada */
static uint8_t oled_ok = 0;

static uint8_t EEMEM ee_fonte = 0;
static uint8_t EEMEM ee_slot = 0;

/* ---- impressao ---- */
enum { JOB_NADA, JOB_TEXTO, JOB_BITMAP };
enum { FASE_INICIO, FASE_DADOS, FASE_FIM };
enum { IMP_OK, IMP_SEM_PAPEL, IMP_TEMPERATURA, IMP_CANCELADO };

static volatile uint8_t imprimindo = 0;
static volatile uint8_t fim_impressao = 0;
static volatile uint8_t resultado = IMP_OK;
static volatile uint8_t cancelar = 0;
static uint8_t fase;
static uint8_t avanco;
static uint8_t cont = 0;         /* passo do motor */
static uint8_t n_lat = 0;
static uint8_t resto_linha = 0;  /* ainda ha pontos da linha atual a imprimir */
static uint8_t segurar_motor = 0;

/* estado do "trabalho" que gera as linhas (texto ou bitmap) */
static uint8_t job_tipo;
static uint16_t job_linha_atual;  /* bitmap: linha 0..239 / texto: linha do glifo */
static uint32_t job_endereco;     /* bitmap: endereco do slot */
static uint16_t txt_ini, txt_prox;
static uint8_t txt_n, txt_colunas;

/* ------------------------------------------------------------------------ */
/* Copia de dados da flash (PROGMEM)                                        */
/* Substituem strcpy_P/strcat_P/memcpy_P, que existem na avr-libc mas nao   */
/* na biblioteca do XC8.                                                    */
/* ------------------------------------------------------------------------ */

static char *copiar_P(char *destino, const char *origem)
{
    char *d = destino;
    while ((*d = pgm_read_byte(origem++)))
        d++;
    return destino;
}

static char *juntar_P(char *destino, const char *origem)
{
    copiar_P(destino + strlen(destino), origem);
    return destino;
}

static void copiar_bytes_P(void *destino, const void *origem, uint8_t n)
{
    uint8_t *d = destino;
    const uint8_t *o = origem;
    while (n--)
        *d++ = pgm_read_byte(o++);
}

/* ------------------------------------------------------------------------ */
/* Tempo                                                                    */
/* ------------------------------------------------------------------------ */

ISR(TIMER2_COMPA_vect)
{
    ms++;
}

static uint16_t agora(void)
{
    uint16_t t;
    uint8_t s = SREG;
    cli();
    t = ms;
    SREG = s;
    return t;
}

/* ------------------------------------------------------------------------ */
/* UART                                                                     */
/* ------------------------------------------------------------------------ */

ISR(USART_RX_vect)
{
    uint8_t b = UDR0;
    uint16_t prox = (rx_fim + 1) & (RX_TAM - 1);
    if (prox != rx_ini) {
        rx_buf[rx_fim] = b;
        rx_fim = prox;
    }
}

static uint16_t rx_disponivel(void)
{
    uint16_t n;
    uint8_t s = SREG;
    cli();
    n = (rx_fim - rx_ini) & (RX_TAM - 1);
    SREG = s;
    return n;
}

/* so chamar com rx_disponivel() > 0 */
static uint8_t rx_ler(void)
{
    uint8_t b = rx_buf[rx_ini];
    uint8_t s = SREG;
    cli();
    rx_ini = (rx_ini + 1) & (RX_TAM - 1);
    SREG = s;
    return b;
}

static void uart_tx(uint8_t b)
{
    while (!(UCSR0A & (1 << UDRE0)));
    UDR0 = b;
}

static void uart_P(const char *s)
{
    char c;
    while ((c = pgm_read_byte(s++)))
        uart_tx(c);
}

/* converte n para texto decimal em s (s precisa de 6 bytes).
 * Substitui utoa(), que existe na avr-libc mas nao no XC8. */
static char *numero_texto(uint16_t n, char *s)
{
    char tmp[5];
    uint8_t i = 0;
    char *p = s;
    do {
        tmp[i++] = '0' + (n % 10);
        n /= 10;
    } while (n);
    while (i)
        *p++ = tmp[--i];
    *p = 0;
    return s;
}

static void uart_num(uint16_t n)
{
    char b[6];
    char *p = b;
    numero_texto(n, b);
    while (*p)
        uart_tx(*p++);
}

/* controle de fluxo XON/XOFF */
static void fluxo(void)
{
    uint16_t n = rx_disponivel();
    if (!xoff_enviado && n > RX_XOFF) {
        uart_tx(XOFF);
        xoff_enviado = 1;
    } else if (xoff_enviado && n < RX_XON) {
        uart_tx(XON);
        xoff_enviado = 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Cabeca termica                                                           */
/* ------------------------------------------------------------------------ */

/* envia 48 bytes (MSB = ponto da esquerda) e grava no latch.
 * Com dados = 0 envia uma linha em branco. */
static void cabeca_enviar(const uint8_t *dados)
{
    uint8_t i;
    FLASH_DESELECT();
    SPCR &= ~(1 << DORD);
    for (i = 0; i < CABECA_BYTES; i++)
        spi_troca(dados ? dados[i] : 0);
    PORTB &= ~(1 << LAT_PIN);   /* LAT ativo em nivel baixo, >120 ns */
    __asm__ __volatile__("nop\n\tnop");
    PORTB |= (1 << LAT_PIN);
}

/* inverte a ordem dos bits (fonte: LSB = esquerda -> MSB = esquerda) */
static const uint8_t inv4[16] PROGMEM = {
    0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
    0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF
};

static uint8_t inverter_bits(uint8_t v)
{
    return (pgm_read_byte(&inv4[v & 0x0F]) << 4) | pgm_read_byte(&inv4[v >> 4]);
}

/* 4 pixels (bit 0 = esquerda) -> 8 pixels dobrados (MSB = esquerda) */
static const uint8_t dobra4[16] PROGMEM = {
    0x00, 0xC0, 0x30, 0xF0, 0x0C, 0xCC, 0x3C, 0xFC,
    0x03, 0xC3, 0x33, 0xF3, 0x0F, 0xCF, 0x3F, 0xFF
};

/* ------------------------------------------------------------------------ */
/* Geracao das linhas (texto / bitmap)                                      */
/* ------------------------------------------------------------------------ */

static uint8_t eh_espaco(uint16_t i)
{
    return i >= texto_len || texto[i] == ' ' || texto[i] == '\n';
}

/* calcula quantos caracteres cabem na linha que comeca em txt_ini,
 * quebrando no '\n' ou no ultimo espaco (para nao cortar palavras) */
static void texto_layout(void)
{
    uint16_t i = txt_ini;
    uint8_t n = 0, k;

    while (n < txt_colunas && i + n < texto_len && texto[i + n] != '\n')
        n++;
    txt_prox = i + n;

    if (txt_prox < texto_len) {
        if (texto[txt_prox] == '\n' || texto[txt_prox] == ' ') {
            txt_prox++;         /* nao comeca a proxima linha com espaco */
        } else if (n == txt_colunas) {
            /* palavra cortada: volta ate o ultimo espaco da linha */
            for (k = n; k > 0 && !eh_espaco(i + k - 1); k--);
            if (k > 1) {
                n = k - 1;
                txt_prox = i + k;
            }
        }
    }
    txt_n = n;
}

static void job_reiniciar(void)
{
    job_linha_atual = 0;
    if (job_tipo == JOB_TEXTO) {
        txt_colunas = CABECA_PONTOS / (fonte.largura * fonte.escala);
        txt_ini = 0;
        texto_layout();
    }
}

static void job_iniciar(uint8_t tipo, uint8_t slot)
{
    job_tipo = tipo;
    job_endereco = SLOT_ENDERECO(slot);
    job_reiniciar();
}

/* gera a proxima linha de pontos em buf (48 bytes, MSB = esquerda).
 * Retorna 0 quando acabou. */
static uint8_t job_linha(uint8_t *buf)
{
    if (job_tipo == JOB_BITMAP) {
        if (job_linha_atual >= BMP_ALTURA)
            return 0;
        memset(buf, 0, CABECA_BYTES);
        flash_ler(job_endereco + (uint32_t)job_linha_atual * BMP_BYTES_LINHA,
                  buf + BMP_MARGEM, BMP_BYTES_LINHA);
        job_linha_atual++;
        return 1;
    }

    if (job_tipo == JOB_TEXTO) {
        uint8_t bpl = fonte.largura / 8;               /* bytes por linha do glifo */
        uint16_t bpc = (uint16_t)fonte.altura * bpl;   /* bytes por caractere */
        uint8_t lin = job_linha_atual / fonte.escala;  /* linha dentro do glifo */
        uint8_t c, b, j = 0;
        char ch;
        const uint8_t *p;
        uint8_t v;

        if (txt_ini >= texto_len)
            return 0;

        for (c = 0; c < txt_colunas; c++) {
            ch = (c < txt_n) ? texto[txt_ini + c] : ' ';
            if (ch < 32 || ch > 126)
                ch = ' ';
            p = fonte.dados + (uint16_t)(ch - 32) * bpc + (uint16_t)lin * bpl;
            for (b = 0; b < bpl; b++) {
                v = pgm_read_byte(p + b);
                if (fonte.escala == 2) {
                    if (j < CABECA_BYTES - 1) {
                        buf[j++] = pgm_read_byte(&dobra4[v & 0x0F]);
                        buf[j++] = pgm_read_byte(&dobra4[v >> 4]);
                    }
                } else if (j < CABECA_BYTES) {
                    buf[j++] = inverter_bits(v);
                }
            }
        }
        while (j < CABECA_BYTES)
            buf[j++] = 0;

        if (++job_linha_atual >= (uint16_t)fonte.altura * fonte.escala) {
            job_linha_atual = 0;
            txt_ini = txt_prox;
            texto_layout();
        }
        return 1;
    }

    return 0;   /* JOB_NADA: so avanca o papel */
}

/* ------------------------------------------------------------------------ */
/* Motor / impressao                                                        */
/* ------------------------------------------------------------------------ */

static void parar_impressao(uint8_t res)
{
    TCCR0B = 0;                      /* para o timer */
    TCCR0A = 0;                      /* desconecta OC0B */
    PORTD &= ~(1 << DST_PIN);        /* DST sempre baixo fora da impressao */
    PORTC &= 0xF0;                   /* desliga as fases do motor */
    PWR_OFF();
    BUCK_OFF();
    LED_OFF();
    EIMSK &= ~(1 << INT0);
    resultado = res;
    imprimindo = 0;
    fim_impressao = 1;
}

/* numero de bits em 1 de um byte */
static uint8_t contar_bits(uint8_t v)
{
    v = v - ((v >> 1) & 0x55);
    v = (v & 0x33) + ((v >> 2) & 0x33);
    return (v + (v >> 4)) & 0x0F;
}

/* tira de "linha" ate MAX_PONTOS pontos e coloca em "parte".
 * Retorna 1 se ainda sobraram pontos na linha. */
static uint8_t dividir_linha(uint8_t *linha, uint8_t *parte)
{
    uint8_t i, b, m, p, n, cnt = 0, sobra = 0;
    for (i = 0; i < CABECA_BYTES; i++) {
        b = linha[i];
        p = 0;
        if (b) {
            n = contar_bits(b);
            if (cnt + n <= MAX_PONTOS) {
                p = b;
                cnt += n;
            } else {
                for (m = 0x80; m && cnt < MAX_PONTOS; m >>= 1)
                    if (b & m) {
                        p |= m;
                        cnt++;
                    }
            }
            linha[i] = b & ~p;
            sobra |= linha[i];
        }
        parte[i] = p;
    }
    return sobra != 0;
}

/* chamada a cada PASSOS_POR_LINHA passos do motor (dentro da interrupcao) */
static void proxima_linha(void)
{
    segurar_motor = 0;

    if (cancelar && fase != FASE_FIM) {
        fase = FASE_FIM;
        avanco = LINHAS_AVANCO_INICIO;
    }

    if (fase == FASE_INICIO) {
        if (avanco) {
            avanco--;
            cabeca_enviar(0);
            return;
        }
        fase = FASE_DADOS;
    }

    if (fase == FASE_DADOS) {
        if (resto_linha || job_linha(linha_buf)) {
            /* linhas com muitos pontos sao impressas em partes; o motor so
             * avanca depois da ultima parte */
            resto_linha = dividir_linha(linha_buf, parte_buf);
            segurar_motor = resto_linha;
            cabeca_enviar(parte_buf);
            return;
        }
        fase = FASE_FIM;
        avanco = LINHAS_AVANCO_FIM;
    }

    /* FASE_FIM: avanca o papel sem imprimir (no minimo 1 linha apos o
     * ultimo acionamento da cabeca, datasheet 3.3.6) */
    if (avanco) {
        avanco--;
        cabeca_enviar(0);
    } else {
        parar_impressao(cancelar ? IMP_CANCELADO : IMP_OK);
    }
}

ISR(TIMER0_OVF_vect)
{
    if (!segurar_motor) {
        PORTC = (PORTC & 0xF0) | passo[cont];
        cont = (cont + 1) & 7;
    }
    TCCR0A ^= (1 << COM0B0) | (1 << COM0B1);   /* DST a cada 2 periodos */
    if (++n_lat >= PASSOS_POR_LINHA) {
        n_lat = 0;
        proxima_linha();
    }
}

/* sensor de papel: borda de descida = acabou o papel */
ISR(INT0_vect)
{
    if (imprimindo)
        parar_impressao(IMP_SEM_PAPEL);
}

/* termistor (conversao disparada pelo overflow do Timer0) */
ISR(ADC_vect)
{
    if (imprimindo && ADCH < TEMPERATURA_LIMITE)
        parar_impressao(IMP_TEMPERATURA);
}

static uint8_t iniciar_impressao(uint8_t tipo)
{
    if (imprimindo)
        return 0;
    if (!TEM_PAPEL()) {
        resultado = IMP_SEM_PAPEL;
        return 0;
    }

    job_iniciar(tipo, slot_sel);

    LED_ON();
    BUCK_ON();
    _delay_ms(20);                 /* partida do conversor */
    PWR_ON();
    _delay_ms(5);

    cabeca_enviar(0);              /* limpa o latch (evita reimprimir a ultima linha) */

    fase = FASE_INICIO;
    avanco = LINHAS_AVANCO_INICIO;
    cont = 0;
    n_lat = 0;
    resto_linha = 0;
    segurar_motor = 0;
    cancelar = 0;
    resultado = IMP_OK;
    fim_impressao = 0;

    PORTC = (PORTC & 0xF0) | passo[0];   /* passo inicial */
    _delay_ms(4);

    imprimindo = 1;
    EIFR = (1 << INTF0);
    EIMSK |= (1 << INT0);

    TCNT0 = 0;
    TCCR0A = (1 << COM0B0) | (1 << COM0B1) | (1 << WGM00) | (1 << WGM01);
    TCCR0B = (1 << WGM02) | (1 << CS00) | (1 << CS01);   /* clk/64 */
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Slots / fontes                                                           */
/* ------------------------------------------------------------------------ */

static uint8_t slot_ocupado(uint8_t s)
{
    uint8_t m[2];
    flash_ler(SLOT_MARCADOR_END(s), m, 2);
    return m[0] == SLOT_MARCADOR_0 && m[1] == SLOT_MARCADOR_1;
}

static void carregar_fonte(uint8_t n)
{
    if (n >= NUM_FONTES)
        n = 0;
    fonte_sel = n;
    copiar_bytes_P(&fonte, &fontes[n], sizeof(fonte_t));
}

static void selecionar_fonte(uint8_t n)
{
    carregar_fonte(n);
    eeprom_update_byte(&ee_fonte, fonte_sel);
}

static void selecionar_slot(uint8_t s)
{
    slot_sel = (s < NUM_SLOTS) ? s : 0;
    eeprom_update_byte(&ee_slot, slot_sel);
}

/* ------------------------------------------------------------------------ */
/* Display                                                                  */
/* ------------------------------------------------------------------------ */

static uint16_t msg_inicio = 0;
static uint8_t msg_ativa = 0;

static void mensagem(const char *l1, const char *l2)
{
    if (!oled_ok)
        return;
    oled_texto_P(0, PSTR(""), 0);
    oled_texto_P(1, l1, 0);
    if (l2)
        oled_texto(2, l2, 0);
    else
        oled_texto_P(2, PSTR(""), 0);
    oled_texto_P(3, PSTR(""), 0);
    msg_inicio = agora();
    msg_ativa = 1;
}

static void mensagem_resultado(void)
{
    switch (resultado) {
    case IMP_SEM_PAPEL:
        mensagem(PSTR("Sem papel!"), 0);
        uart_P(PSTR("ERRO sem papel\r\n"));
        break;
    case IMP_TEMPERATURA:
        mensagem(PSTR("Temperatura alta"), 0);
        uart_P(PSTR("ERRO temperatura\r\n"));
        break;
    case IMP_CANCELADO:
        mensagem(PSTR("Cancelado"), 0);
        uart_P(PSTR("CANCELADO\r\n"));
        break;
    default:
        mensagem(PSTR("Impresso"), 0);
        uart_P(PSTR("OK\r\n"));
        break;
    }
}

/*
 * Pre-visualizacao: reduz a imagem de 384 pontos de largura por "escala"
 * (3 -> 128 px, 4 -> 96 px) nas duas direcoes. Um pixel do display acende
 * quando pelo menos "limiar" pontos do bloco escala x escala sao pretos.
 * tela = quantas telas (64 linhas do display) pular.
 * Retorna 1 se ainda ha conteudo depois desta tela.
 */
static uint8_t previsualizar(uint8_t escala, uint8_t limiar, uint8_t tela)
{
    uint8_t *cnt = pagina_buf;          /* 128 contadores */
    uint8_t *saida = pagina_buf + 128;  /* 128 colunas da pagina */
    uint8_t largura = CABECA_PONTOS / escala;
    uint8_t margem = (OLED_LARGURA - largura) / 2;
    uint8_t fim = 0;
    uint8_t p, k, t, ox, i, x;
    uint16_t n, xx;

    job_reiniciar();
    for (n = (uint16_t)tela * 64 * escala; n && !fim; n--)
        if (!job_linha(linha_buf))
            fim = 1;

    for (p = 0; p < OLED_PAGINAS; p++) {
        memset(saida, 0, 128);
        for (k = 0; k < 8; k++) {
            memset(cnt, 0, 128);
            for (t = 0; t < escala; t++) {
                if (fim || !job_linha(linha_buf)) {
                    fim = 1;
                    break;
                }
                xx = 0;
                for (ox = 0; ox < largura; ox++)
                    for (i = 0; i < escala; i++, xx++)
                        if (linha_buf[xx >> 3] & (0x80 >> (xx & 7)))
                            cnt[ox]++;
            }
            for (ox = 0; ox < largura; ox++)
                if (cnt[ox] >= limiar)
                    saida[ox] |= (1 << k);
        }
        oled_pos(p, 0);
        oled_dados_inicio();
        for (x = 0; x < OLED_LARGURA; x++)
            oled_dado((x >= margem && x - margem < largura) ? saida[x - margem] : 0);
        oled_dados_fim();
    }
    if (!fim && !job_linha(linha_buf))
        fim = 1;
    return !fim;
}

/* ------------------------------------------------------------------------ */
/* Botao                                                                    */
/* ------------------------------------------------------------------------ */

enum { BT_NADA, BT_CURTO, BT_LONGO };

static uint8_t ler_botao(void)
{
    static uint8_t estado = 0;
    static uint16_t t;
    uint8_t apertado = BOTAO_APERTADO();

    switch (estado) {
    case 0:
        if (apertado) {
            t = agora();
            estado = 1;
        }
        break;
    case 1:
        if (!apertado) {
            estado = 0;
            if ((uint16_t)(agora() - t) > 30)
                return BT_CURTO;
        } else if ((uint16_t)(agora() - t) > TEMPO_CLIQUE_LONGO) {
            estado = 2;
            return BT_LONGO;
        }
        break;
    default:
        if (!apertado)
            estado = 0;
        break;
    }
    return BT_NADA;
}

/* ------------------------------------------------------------------------ */
/* Recepcao de bitmap                                                       */
/* ------------------------------------------------------------------------ */

static void texto_slot(char *s, const char *prefixo_P, uint8_t slot)
{
    copiar_P(s, prefixo_P);
    s += strlen(s);
    *s++ = '0' + slot;
    *s = 0;
}

/* recebe BMP_TAMANHO bytes pela serial e grava no slot */
static uint8_t receber_bitmap(uint8_t slot, uint16_t espera_inicial)
{
    char s[17];
    uint32_t base;
    uint16_t cont_bytes = 0, t0;
    uint8_t m[2] = { SLOT_MARCADOR_0, SLOT_MARCADOR_1 };
    uint8_t i, ok = 0;

    if (slot >= NUM_SLOTS) {
        uart_P(PSTR("ERRO slot\r\n"));
        return 0;
    }
    base = SLOT_ENDERECO(slot);

    /* segura o envio enquanto apaga (pode levar algumas centenas de ms) */
    uart_tx(XOFF);
    xoff_enviado = 1;
    texto_slot(s, PSTR("Slot "), slot);
    mensagem(PSTR("Apagando..."), s);
    LED_ON();
    for (i = 0; i < SLOT_SETORES; i++)
        flash_apagar_setor(base + i * FLASH_SETOR);

    mensagem(PSTR("Aguardando bmp"), s);
    uart_P(PSTR("PRONTO\r\n"));
    uart_tx(XON);
    xoff_enviado = 0;

    t0 = agora();
    while (1) {
        fluxo();
        if (rx_disponivel()) {
            pagina_buf[cont_bytes & 0xFF] = rx_ler();
            cont_bytes++;
            t0 = agora();
            if ((cont_bytes & 0xFF) == 0 || cont_bytes == BMP_TAMANHO) {
                uint16_t ini = (cont_bytes - 1) & ~0xFF;
                flash_gravar_pagina(base + ini, pagina_buf, cont_bytes - ini);
                if (oled_ok) {
                    copiar_P(s, PSTR("Recebendo "));
                    numero_texto((uint16_t)((uint32_t)cont_bytes * 100 / BMP_TAMANHO), s + strlen(s));
                    juntar_P(s, PSTR("%"));
                    oled_texto(1, s, 0);
                }
                if (cont_bytes == BMP_TAMANHO) {
                    ok = 1;
                    break;
                }
            }
        } else {
            uint16_t limite = cont_bytes ? TEMPO_BYTE_BITMAP : espera_inicial;
            if ((uint16_t)(agora() - t0) > limite) {
                mensagem(PSTR("Tempo esgotado"), 0);
                uart_P(PSTR("ERRO tempo esgotado, recebidos "));
                uart_num(cont_bytes);
                uart_P(PSTR("\r\n"));
                break;
            }
            if (ler_botao() != BT_NADA) {
                mensagem(PSTR("Cancelado"), 0);
                uart_P(PSTR("ERRO cancelado\r\n"));
                break;
            }
        }
    }

    if (xoff_enviado) {
        uart_tx(XON);
        xoff_enviado = 0;
    }
    LED_OFF();
    if (!ok)
        return 0;

    flash_gravar_pagina(SLOT_MARCADOR_END(slot), m, 2);
    texto_slot(s, PSTR("no slot "), slot);
    mensagem(PSTR("Bitmap gravado"), s);
    uart_P(PSTR("OK\r\n"));
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Acoes                                                                    */
/* ------------------------------------------------------------------------ */

static void acao_imprimir_texto(void)
{
    uint16_t i;
    if (!texto_len) {
        mensagem(PSTR("Sem texto"), 0);
        uart_P(PSTR("ERRO sem texto\r\n"));
        return;
    }
    /* igual a V1: devolve pela serial o texto que sera impresso */
    for (i = 0; i < texto_len; i++)
        uart_tx(texto[i]);
    uart_P(PSTR("\r\n"));
    if (iniciar_impressao(JOB_TEXTO))
        mensagem(PSTR("Imprimindo..."), 0);
    else
        mensagem_resultado();
}

static void acao_imprimir_bitmap(uint8_t slot)
{
    if (slot >= NUM_SLOTS || !slot_ocupado(slot)) {
        mensagem(PSTR("Slot vazio"), 0);
        uart_P(PSTR("ERRO slot vazio\r\n"));
        return;
    }
    slot_sel = slot;
    if (iniciar_impressao(JOB_BITMAP))
        mensagem(PSTR("Imprimindo bmp"), 0);
    else
        mensagem_resultado();
}

static void acao_avancar(void)
{
    if (iniciar_impressao(JOB_NADA))
        mensagem(PSTR("Avancando..."), 0);
    else
        mensagem_resultado();
}

/* ------------------------------------------------------------------------ */
/* Menu                                                                     */
/* ------------------------------------------------------------------------ */

enum {
    M_IMP_TEXTO, M_FONTE, M_VER_TEXTO, M_IMP_BMP, M_SLOT, M_VER_BMP,
    M_RECEBER, M_AVANCAR, M_TOTAL
};

enum { TELA_MENU, TELA_PREVIA };

static uint8_t menu_sel = 0;
static uint8_t tela = TELA_MENU;
static uint8_t previa_tipo, previa_pagina, previa_mais;

static void menu_rotulo(uint8_t item, char *s)
{
    switch (item) {
    case M_IMP_TEXTO: copiar_P(s, PSTR("Imprimir texto")); break;
    case M_FONTE:
        copiar_P(s, PSTR("Fonte:"));
        juntar_P(s, fonte.nome);
        break;
    case M_VER_TEXTO: copiar_P(s, PSTR("Ver texto")); break;
    case M_IMP_BMP:   copiar_P(s, PSTR("Imprimir bitmap")); break;
    case M_SLOT:
        texto_slot(s, PSTR("Slot "), slot_sel);
        juntar_P(s, slot_ocupado(slot_sel) ? PSTR(": ocupado") : PSTR(": vazio"));
        break;
    case M_VER_BMP:   copiar_P(s, PSTR("Ver bitmap")); break;
    case M_RECEBER:   copiar_P(s, PSTR("Receber bitmap")); break;
    default:          copiar_P(s, PSTR("Avancar papel")); break;
    }
}

static void menu_desenhar(void)
{
    char s[24];
    uint8_t topo, l;
    if (!oled_ok)
        return;
    topo = (menu_sel < OLED_LINHAS) ? 0 : menu_sel - (OLED_LINHAS - 1);
    for (l = 0; l < OLED_LINHAS; l++) {
        menu_rotulo(topo + l, s);
        s[OLED_COLUNAS] = 0;
        oled_texto(l, s, (topo + l) == menu_sel);
    }
}

static void previa_mostrar(void)
{
    if (previa_tipo == JOB_TEXTO) {
        job_iniciar(JOB_TEXTO, slot_sel);
        previa_mais = previsualizar(3, 2, previa_pagina);   /* largura inteira, 2 de 9 */
    } else {
        job_iniciar(JOB_BITMAP, slot_sel);
        previa_mais = previsualizar(4, 8, 0);   /* 320x240 -> 80x60, 8 de 16 */
    }
}

static void previa_abrir(uint8_t tipo)
{
    if (!oled_ok)
        return;
    if (tipo == JOB_TEXTO && !texto_len) {
        mensagem(PSTR("Sem texto"), 0);
        return;
    }
    if (tipo == JOB_BITMAP && !slot_ocupado(slot_sel)) {
        mensagem(PSTR("Slot vazio"), 0);
        return;
    }
    previa_tipo = tipo;
    previa_pagina = 0;
    tela = TELA_PREVIA;
    msg_ativa = 0;
    previa_mostrar();
}

static void menu_selecionar(void)
{
    switch (menu_sel) {
    case M_IMP_TEXTO: acao_imprimir_texto(); break;
    case M_FONTE:
        selecionar_fonte(fonte_sel + 1 < NUM_FONTES ? fonte_sel + 1 : 0);
        menu_desenhar();
        break;
    case M_VER_TEXTO: previa_abrir(JOB_TEXTO); break;
    case M_IMP_BMP:   acao_imprimir_bitmap(slot_sel); break;
    case M_SLOT:
        selecionar_slot(slot_sel + 1);
        menu_desenhar();
        break;
    case M_VER_BMP:   previa_abrir(JOB_BITMAP); break;
    case M_RECEBER:   receber_bitmap(slot_sel, TEMPO_INICIO_MENU); break;
    case M_AVANCAR:   acao_avancar(); break;
    }
}

static void tratar_botao(uint8_t ev)
{
    if (ev == BT_NADA)
        return;

    if (!oled_ok) {
        /* sem display: funciona como a V1, o botao imprime o texto */
        if (ev == BT_CURTO)
            acao_imprimir_texto();
        return;
    }

    if (msg_ativa) {            /* qualquer clique fecha a mensagem */
        msg_ativa = 0;
        if (tela == TELA_MENU)
            menu_desenhar();
        return;
    }

    if (tela == TELA_PREVIA) {
        if (ev == BT_CURTO && previa_tipo == JOB_TEXTO) {
            previa_pagina = previa_mais ? previa_pagina + 1 : 0;
            previa_mostrar();
        } else {
            tela = TELA_MENU;
            menu_desenhar();
        }
        return;
    }

    if (ev == BT_CURTO) {
        menu_sel = (menu_sel + 1) % M_TOTAL;
        menu_desenhar();
    } else {
        menu_selecionar();
    }
}

/* ------------------------------------------------------------------------ */
/* Serial                                                                   */
/* ------------------------------------------------------------------------ */

/* UTF-8 0xC3 0x80..0xBF (A-crase .. y-trema) -> letra sem acento */
static const char sem_acento[64] PROGMEM =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
    "aaaaaaaceeeeiiiidnooooo/ouuuuypy";

static uint8_t argumento(uint8_t b)
{
    return (b >= '0' && b <= '9') ? b - '0' : b;
}

static void responder_status(void)
{
    uint8_t s;
    uart_P(PSTR("FONTE="));
    uart_num(fonte_sel);
    uart_P(PSTR(" SLOT="));
    uart_num(slot_sel);
    uart_P(PSTR(" SLOTS="));
    for (s = 0; s < NUM_SLOTS; s++)
        uart_tx(slot_ocupado(s) ? '1' : '0');
    uart_P(PSTR("\r\n"));
}

static void processar_serial(void)
{
    static uint8_t estado = 0;      /* 0 texto, 1 depois do ESC, 2 argumento */
    static uint8_t comando;
    static uint8_t novo_texto = 1;
    static uint8_t utf8_c3 = 0, utf8_pular = 0;
    uint8_t b;

    /* durante a impressao o SPI e da interrupcao: os bytes ficam no buffer */
    while (!imprimindo && rx_disponivel()) {
        b = rx_ler();

        if (estado == 1) {          /* byte depois do ESC */
            estado = 0;
            switch (b) {
            case 'B': case 'P': case 'F': case 'V':
                comando = b;
                estado = 2;
                break;
            case 'T':
                acao_imprimir_texto();
                break;
            case 'S':
                responder_status();
                break;
            default:
                uart_P(PSTR("ERRO comando\r\n"));
                break;
            }
            continue;
        }

        if (estado == 2) {          /* argumento do comando */
            estado = 0;
            b = argumento(b);
            switch (comando) {
            case 'B':
                if (b < NUM_SLOTS)
                    selecionar_slot(b);
                receber_bitmap(b, TEMPO_INICIO_SERIAL);
                break;
            case 'P':
                acao_imprimir_bitmap(b);
                break;
            case 'F':
                if (b < NUM_FONTES) {
                    selecionar_fonte(b);
                    uart_P(PSTR("OK\r\n"));
                    if (tela == TELA_MENU && !msg_ativa)
                        menu_desenhar();
                } else {
                    uart_P(PSTR("ERRO fonte\r\n"));
                }
                break;
            case 'V':
                if (b < NUM_SLOTS) {
                    selecionar_slot(b);
                    previa_abrir(JOB_BITMAP);
                    uart_P(PSTR("OK\r\n"));
                } else {
                    uart_P(PSTR("ERRO slot\r\n"));
                }
                break;
            }
            continue;
        }

        if (b == ESC) {
            estado = 1;
            continue;
        }

        if (b == '\r') {            /* fim do texto (igual a V1) */
            if (!novo_texto) {
                texto[rx_len] = 0;
                texto_len = rx_len;
                novo_texto = 1;
                if (tela == TELA_MENU) {
                    char s[17];
                    numero_texto(texto_len, s);
                    juntar_P(s, PSTR(" caracteres"));
                    mensagem(PSTR("Texto recebido"), s);
                }
            }
            continue;
        }

        if (novo_texto) {
            if (b == '\n')          /* LF logo depois do CR */
                continue;
            novo_texto = 0;
            rx_len = 0;
            texto_len = 0;
        }

        /* acentos (UTF-8) viram letras sem acento; outros simbolos viram '?' */
        if (utf8_pular) {
            utf8_pular--;
            continue;
        }
        if (utf8_c3) {
            utf8_c3 = 0;
            b = (b >= 0x80 && b < 0xC0) ? pgm_read_byte(&sem_acento[b - 0x80]) : '?';
        } else if (b == 0xC3) {
            utf8_c3 = 1;
            continue;
        } else if (b >= 0x80) {
            if (b >= 0xF0)
                utf8_pular = 3;
            else if (b >= 0xE0)
                utf8_pular = 2;
            else if (b >= 0xC0)
                utf8_pular = 1;
            else
                continue;
            b = '?';
        }

        if (rx_len < TEXTO_MAX)
            texto[rx_len++] = b;
    }
}

/* ------------------------------------------------------------------------ */
/* main                                                                     */
/* ------------------------------------------------------------------------ */

int main(void)
{
    uint8_t ev;

    /* PORTAS */
    DDRB = 0xFF & ~(1 << PB4);         /* tudo saida, menos MISO */
    PORTB = (1 << LAT_PIN) | (1 << LED_PIN) | (1 << FLASH_CS_PIN);
    DDRC = 0x0F;                       /* motor */
    PORTC = 0;
    DDRD = (1 << EN_BUCK_PIN) | (1 << DST_PIN) | (1 << PD6);
    PORTD = (1 << EN_BUCK_PIN)         /* buck desligado */
          | (1 << PS_PIN)              /* pull-up sensor */
          | (1 << BUTTON_PIN);         /* pull-up botao */

    spi_init();

    /* UART 9600 */
    UBRR0 = 51;
    UCSR0A = 0;
    UCSR0B = (1 << RXCIE0) | (1 << RXEN0) | (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);

    /* INT0: borda de descida (sensor de papel), habilitada so ao imprimir */
    EICRA = (1 << ISC01);
    EIMSK = 0;

    /* ADC7 (termistor), disparado pelo overflow do Timer0 */
    ADMUX = (1 << REFS0) | (1 << MUX0) | (1 << MUX1) | (1 << MUX2) | (1 << ADLAR);
    ADCSRA = (1 << ADEN) | (1 << ADSC) | (1 << ADATE) | (1 << ADPS0) | (1 << ADPS1)
           | (1 << ADPS2) | (1 << ADIE);
    ADCSRB = (1 << ADTS2);

    /* Timer0: motor + DST (mesmos valores da V1), parado */
    OCR0A = 115;                       /* periodo do passo */
    OCR0B = 70;                        /* largura do DST */
    TCCR0A = 0;
    TCCR0B = 0;
    TIMSK0 = (1 << TOIE0);

    /* Timer2: base de tempo de 1 ms */
    TCCR2A = (1 << WGM21);
    TCCR2B = (1 << CS22);              /* clk/64 */
    OCR2A = 124;
    TIMSK2 = (1 << OCIE2A);

    carregar_fonte(eeprom_read_byte(&ee_fonte));
    slot_sel = eeprom_read_byte(&ee_slot);
    if (slot_sel >= NUM_SLOTS)
        slot_sel = 0;

    sei();

    oled_ok = oled_init();
    if (oled_ok) {
        oled_texto_P(0, PSTR("Impressora"), 0);
        oled_texto_P(1, PSTR("termica V2"), 0);
        oled_texto_P(2, (flash_ler_id() & 0xFFFF) == 0x4015
                        ? PSTR("Memoria OK") : PSTR("Memoria ERRO"), 0);
        _delay_ms(1200);
        menu_desenhar();
    }
    uart_P(PSTR("Impressora V2\r\n"));

    while (1) {
        fluxo();
        ev = ler_botao();

        if (imprimindo) {
            if (ev == BT_LONGO)        /* clique longo cancela a impressao */
                cancelar = 1;
            continue;
        }

        if (fim_impressao) {
            fim_impressao = 0;
            mensagem_resultado();
        }

        processar_serial();
        tratar_botao(ev);

        if (msg_ativa && (uint16_t)(agora() - msg_inicio) > TEMPO_MENSAGEM) {
            msg_ativa = 0;
            if (tela == TELA_MENU)
                menu_desenhar();
        }
    }
    return 0;
}
