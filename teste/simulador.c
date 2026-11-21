/*
 * Simulador no PC do firmware da impressora termica V2.
 *
 * Compila o main.c e o spi_flash.c de verdade, trocando o hardware por
 * modelos simples:
 *   - memoria W25Q16 (2 MB em RAM, comandos 03/02/20/06/05/9F/AB);
 *   - cabeca termica (cada 48 bytes enviados com /CS da memoria em alto
 *     viram uma linha impressa);
 *   - display SSD1306 (framebuffer 128x64);
 *   - UART com um "PC" que respeita XON/XOFF.
 *
 * Gera imagens .pbm em teste/saida/ com o que seria impresso e o que
 * apareceria no display. Uso: ver teste/rodar.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static char *utoa(unsigned v, char *s, int base)
{
    (void)base;
    sprintf(s, "%u", v);
    return s;
}

/* ---------------- registradores ---------------- */
#include "avr/io.h"
volatile uint8_t DDRB, PORTB, PINB, DDRC, PORTC, DDRD, PORTD, PIND;
volatile uint8_t SPCR, UCSR0A = 0xFF, UCSR0B, UCSR0C, EICRA, EIMSK, EIFR;
volatile uint8_t ADMUX, ADCSRA, ADCSRB, ADCH = 200, OCR0A, OCR0B, TCCR0A, TCCR0B;
volatile uint8_t TIMSK0, TCNT0, TCCR2A, TCCR2B, OCR2A, TIMSK2, SREG;
volatile uint16_t UBRR0;

/* ---------------- memoria + cabeca ---------------- */
static uint8_t memoria[2 * 1024 * 1024];
static int cs_baixo = 0;
static int fl_n = 0, fl_cmd = 0;
static uint32_t fl_end = 0;
static int fl_wel = 0;

static void sim_cs(int baixo)
{
    if (!baixo && cs_baixo) {
        if (fl_cmd == 0x20 && fl_n >= 4 && fl_wel) {
            memset(memoria + (fl_end & ~0xFFFu), 0xFF, 4096);
            fl_wel = 0;
        }
        if (fl_cmd == 0x02)
            fl_wel = 0;
    }
    if (baixo && !cs_baixo)
        fl_n = 0;
    cs_baixo = baixo;
}
#define FLASH_SELECT()   (PORTB &= ~1, sim_cs(1))
#define FLASH_DESELECT() (PORTB |= 1, sim_cs(0))

#define MAX_LINHAS 20000
static uint8_t impresso[MAX_LINHAS][48];   /* MSB = primeiro bit enviado */
static long impresso_passo[MAX_LINHAS];    /* passos do motor no envio */
static int n_impresso = 0;
static long passos_motor = 0;
static int max_pontos_envio = 0;
static uint8_t linha_cab[48];
static int n_cab = 0;
static int erro_dord = 0;

static uint8_t flash_byte(uint8_t b)
{
    uint8_t r = 0xFF;
    if (fl_n == 0) {
        fl_cmd = b;
        if (b == 0x06)
            fl_wel = 1;
    } else if (fl_n <= 3 && (fl_cmd == 0x03 || fl_cmd == 0x02 || fl_cmd == 0x20)) {
        fl_end = (fl_end << 8 | b) & 0xFFFFFF;
    } else if (fl_cmd == 0x03) {
        r = memoria[fl_end];
        fl_end = (fl_end + 1) & 0x1FFFFF;
    } else if (fl_cmd == 0x02 && fl_wel) {
        memoria[fl_end] &= b;
        fl_end = (fl_end & ~0xFFu) | ((fl_end + 1) & 0xFF);
    } else if (fl_cmd == 0x9F) {
        static const uint8_t id[3] = { 0xEF, 0x40, 0x15 };
        r = id[(fl_n - 1) % 3];
    } else if (fl_cmd == 0x05) {
        r = 0;
    }
    if (fl_n == 3 && fl_cmd == 0x03)
        ;
    fl_n++;
    return r;
}

static uint8_t inverte(uint8_t v)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++)
        if (v & (1 << i))
            r |= 0x80 >> i;
    return r;
}

static uint8_t spi_byte(uint8_t b)
{
    if (cs_baixo)
        return flash_byte(b);
    /* cabeca: guarda na ordem em que os bits entram */
    if (SPCR & (1 << DORD)) {
        b = inverte(b);
        erro_dord = 1;
    }
    linha_cab[n_cab++] = b;
    if (n_cab == 48) {
        int pontos = 0;
        for (int i = 0; i < 48; i++)
            pontos += __builtin_popcount(linha_cab[i]);
        if (pontos > max_pontos_envio)
            max_pontos_envio = pontos;
        if (n_impresso < MAX_LINHAS) {
            impresso_passo[n_impresso] = passos_motor;
            memcpy(impresso[n_impresso++], linha_cab, 48);
        }
        n_cab = 0;
    }
    return 0;
}

#define SENT 0xFFFF
static uint16_t spdr_var = SENT, spdr_resp;
uint16_t *sim_spdr(void)
{
    if (spdr_var != SENT) {
        spdr_resp = spi_byte((uint8_t)spdr_var);
        spdr_var = SENT;
        return &spdr_resp;
    }
    return &spdr_var;
}
static uint8_t spsr_var;
uint8_t *sim_spsr(void)
{
    spsr_var |= (1 << SPIF);
    return &spsr_var;
}

/* ---------------- UART ---------------- */
static uint16_t udr_var = SENT;
static int em_rx = 0;
static int host_pausado = 0;
static const uint8_t *host_dados;
static long host_n = 0, host_pos = 0;
static char resposta[4096];
static int n_resposta = 0;
static long n_xoff = 0;

static void udr_flush(void)
{
    if (em_rx || udr_var == SENT)
        return;
    uint8_t b = udr_var;
    udr_var = SENT;
    if (b == 0x13) {
        host_pausado = 1;
        n_xoff++;
    } else if (b == 0x11) {
        host_pausado = 0;
    } else if (n_resposta < (int)sizeof(resposta) - 1) {
        resposta[n_resposta++] = b;
        resposta[n_resposta] = 0;
    }
}

uint16_t *sim_udr(void)
{
    if (em_rx)
        return &udr_var;   /* leitura do byte recebido */
    udr_flush();
    return &udr_var;
}

static void host_enviar(const void *d, long n)
{
    host_dados = d;
    host_n = n;
    host_pos = 0;
}

/* ---------------- display ---------------- */
#include "../oled.h"
static uint8_t tela_oled[8][128];
static uint8_t o_pag, o_col;
uint8_t oled_init(void) { memset(tela_oled, 0, sizeof(tela_oled)); return 1; }
void oled_limpar(void) { memset(tela_oled, 0, sizeof(tela_oled)); }
void oled_pos(uint8_t p, uint8_t c) { o_pag = p; o_col = c; }
void oled_dados_inicio(void) {}
void oled_dado(uint8_t d) { if (o_col < 128) tela_oled[o_pag][o_col++] = d; }
void oled_dados_fim(void) {}
extern const uint8_t font_terminus_8x16[];
static void o_texto(uint8_t linha, const char *s, uint8_t inv)
{
    for (int m = 0; m < 2; m++) {
        oled_pos(linha * 2 + m, 0);
        const char *p = s;
        for (int n = 0; n < 16; n++) {
            char c = *p ? *p++ : ' ';
            if (c < 32 || c > 126) c = ' ';
            const uint8_t *g = font_terminus_8x16 + (c - 32) * 16 + m * 8;
            for (int x = 0; x < 8; x++) {
                uint8_t col = 0;
                for (int y = 0; y < 8; y++)
                    if (g[y] & (1 << x)) col |= 1 << y;
                oled_dado(inv ? ~col : col);
            }
        }
    }
}
void oled_texto(uint8_t l, const char *s, uint8_t inv) { o_texto(l, s, inv); }
void oled_texto_P(uint8_t l, const char *s, uint8_t inv) { o_texto(l, s, inv); }

/* ---------------- firmware ---------------- */
#define main firmware_main
#include "../main.c"
#undef main
#include "../spi_flash.c"

/* chamado a cada cli(): passa o tempo e o PC envia um byte */
void sim_tick(void)
{
    static int div = 0;
    udr_flush();
    if (++div >= 4) {
        div = 0;
        ms++;
    }
    if (!host_pausado && host_pos < host_n) {
        uint16_t prox = (rx_fim + 1) & (RX_TAM - 1);
        if (prox == rx_ini) {
            fprintf(stderr, "ESTOURO do buffer de recepcao!\n");
            exit(1);
        }
        em_rx = 1;
        udr_var = host_dados[host_pos++];
        USART_RX_vect();
        udr_var = SENT;
        em_rx = 0;
    }
}

/* ---------------- utilitarios ---------------- */
/* junta os envios em linhas fisicas do papel: o papel anda 1 linha a cada
 * 4 passos do motor; partes enviadas com o motor parado caem na mesma linha */
static uint8_t papel[MAX_LINHAS][48];
static int n_papel = 0;

static void montar_papel(void)
{
    memset(papel, 0, sizeof(papel));
    n_papel = 0;
    for (int i = 0; i < n_impresso; i++) {
        int y = (int)(impresso_passo[i] / 4);
        if (y >= MAX_LINHAS) break;
        for (int k = 0; k < 48; k++)
            papel[y][k] |= impresso[i][k];
        if (y + 1 > n_papel) n_papel = y + 1;
    }
}

static void salvar_impresso(const char *nome)
{
    FILE *f = fopen(nome, "wb");
    fprintf(f, "P4\n384 %d\n", n_papel);
    for (int i = 0; i < n_papel; i++)
        fwrite(papel[i], 1, 48, f);
    fclose(f);
}

static void salvar_oled(const char *nome)
{
    FILE *f = fopen(nome, "wb");
    fprintf(f, "P4\n128 64\n");
    for (int y = 0; y < 64; y++)
        for (int xb = 0; xb < 16; xb++) {
            uint8_t v = 0;
            for (int b = 0; b < 8; b++)
                if (tela_oled[y / 8][xb * 8 + b] & (1 << (y & 7)))
                    v |= 0x80 >> b;
            fputc(v, f);
        }
    fclose(f);
}

static void processar_tudo(void)
{
    for (int i = 0; i < 200000 && (host_pos < host_n || rx_disponivel()); i++)
        processar_serial();
}

static int imprimir_ate_fim(void)
{
    long passos = 0;
    while (imprimindo && passos < 2000000) {
        uint8_t antes = PORTC & 0x0F;
        TIMER0_OVF_vect();
        if ((PORTC & 0x0F) != antes && (PORTC & 0x0F))
            passos_motor++;
        passos++;
    }
    montar_papel();
    if (fim_impressao) {
        fim_impressao = 0;
        mensagem_resultado();
    }
    return (int)passos;
}

static void reiniciar_impresso(void) { n_impresso = 0; n_cab = 0; passos_motor = 0; max_pontos_envio = 0; }

int main(int argc, char **argv)
{
    const char *bmp_arquivo = argc > 1 ? argv[1] : NULL;
    int falhas = 0;
    char nome[128];

    memset(memoria, 0xFF, sizeof(memoria));
    PIND = (1 << PD2) | (1 << PD7);     /* papel presente, botao solto */
    PORTB = (1 << LAT_PIN) | (1 << LED_PIN) | 1;
    spi_init();
    carregar_fonte(0);
    oled_ok = oled_init();
    menu_desenhar();
    salvar_oled("saida/oled_menu.pbm");

    /* ---------- texto ---------- */
    static const char txt[] =
        "Impressora termica V2: texto recebido pela serial com quebra "
        "automatica de linha. Acentuação: São João, ç ü é ã!\n"
        "Nova linha apos \\n. 0123456789 {}[]()<>#$%&@\r\n";
    host_enviar(txt, sizeof(txt) - 1);
    processar_tudo();
    printf("texto: %u caracteres: \"%s\"\n", texto_len, texto);
    if (strstr(texto, "Acentuacao: Sao Joao, c u e a!") == NULL) {
        printf("FALHA: acentos\n");
        falhas++;
    }
    salvar_oled("saida/oled_texto_recebido.pbm");

    for (uint8_t f = 0; f < NUM_FONTES; f++) {
        char cmd[3] = { 0x1B, 'F', '0' + f };
        n_resposta = 0;
        host_enviar(cmd, 3);
        processar_tudo();
        reiniciar_impresso();
        n_resposta = 0;
        acao_imprimir_texto();
        int passos = imprimir_ate_fim();
        printf("fonte %u (%s): %d linhas no papel, %d envios, %d periodos, max %d pontos/envio\n",
               f, fonte.nome, n_papel, n_impresso, passos, max_pontos_envio);
        if (max_pontos_envio > MAX_PONTOS) {
            printf("FALHA: mais de %d pontos acionados juntos\n", MAX_PONTOS);
            falhas++;
        }
        sprintf(nome, "saida/texto_fonte%u.pbm", f);
        salvar_impresso(nome);

        previa_abrir(JOB_TEXTO);
        sprintf(nome, "saida/oled_previa_texto_fonte%u.pbm", f);
        salvar_oled(nome);
        tela = TELA_MENU;
    }

    /* ---------- bitmap ---------- */
    static uint8_t bmp[BMP_TAMANHO];
    if (bmp_arquivo) {
        FILE *f = fopen(bmp_arquivo, "rb");
        if (!f || fread(bmp, 1, BMP_TAMANHO, f) != BMP_TAMANHO) {
            printf("nao consegui ler %s\n", bmp_arquivo);
            return 1;
        }
        fclose(f);
    } else {
        for (int y = 0; y < BMP_ALTURA; y++)
            for (int x = 0; x < BMP_LARGURA; x++)
                if (((x / 20) + (y / 20)) & 1)
                    bmp[y * 40 + x / 8] |= 0x80 >> (x & 7);
    }

    static uint8_t pacote[3 + BMP_TAMANHO];
    pacote[0] = 0x1B; pacote[1] = 'B'; pacote[2] = '2';
    memcpy(pacote + 3, bmp, BMP_TAMANHO);
    n_resposta = 0;
    host_enviar(pacote, sizeof(pacote));
    processar_tudo();
    printf("upload: resposta \"%s\" (XOFF enviados: %ld)\n", resposta, n_xoff);
    if (memcmp(memoria + SLOT_ENDERECO(2), bmp, BMP_TAMANHO) != 0) {
        printf("FALHA: memoria diferente do bitmap enviado\n");
        falhas++;
    }
    if (!slot_ocupado(2)) {
        printf("FALHA: slot 2 nao marcado\n");
        falhas++;
    }

    n_resposta = 0;
    host_enviar("\x1bS", 2);
    processar_tudo();
    printf("status: %s", resposta);

    reiniciar_impresso();
    n_resposta = 0;
    host_enviar("\x1bP2", 3);
    processar_tudo();
    imprimir_ate_fim();
    printf("bitmap: %d linhas no papel, %d envios, max %d pontos/envio, resposta: %s",
           n_papel, n_impresso, max_pontos_envio, resposta);
    if (max_pontos_envio > MAX_PONTOS) {
        printf("FALHA: mais de %d pontos acionados juntos\n", MAX_PONTOS);
        falhas++;
    }
    salvar_impresso("saida/bitmap.pbm");
    {
        /* confere linha a linha no papel: avanco inicial e depois o bitmap */
        int erros = 0;
        for (int y = 0; y < BMP_ALTURA; y++) {
            uint8_t *l = papel[LINHAS_AVANCO_INICIO + y];
            for (int i = 0; i < 4; i++)
                if (l[i] || l[44 + i]) erros++;
            if (memcmp(l + 4, bmp + y * 40, 40)) erros++;
        }
        if (n_papel != LINHAS_AVANCO_INICIO + BMP_ALTURA + LINHAS_AVANCO_FIM)
            erros++;
        printf("bitmap impresso %s\n", erros ? "DIFERENTE do enviado" : "igual ao enviado");
        falhas += erros != 0;
    }

    previa_abrir(JOB_BITMAP);
    salvar_oled("saida/oled_previa_bitmap.pbm");
    tela = TELA_MENU;

    /* slot vazio */
    n_resposta = 0;
    host_enviar("\x1bP5", 3);
    processar_tudo();
    printf("slot vazio: %s", resposta);

    /* sem papel */
    PIND &= ~(1 << PD2);
    n_resposta = 0;
    host_enviar("\x1bT", 2);
    processar_tudo();
    printf("sem papel: %s", strstr(resposta, "ERRO") ? strstr(resposta, "ERRO") : resposta);
    PIND |= (1 << PD2);

    menu_sel = M_SLOT;
    menu_desenhar();
    salvar_oled("saida/oled_menu_slot.pbm");

    if (erro_dord)
        printf("AVISO: dados enviados para a cabeca com DORD=1\n");
    printf("%s\n", falhas ? "TESTES COM FALHA" : "TODOS OS TESTES OK");
    return falhas != 0;
}
