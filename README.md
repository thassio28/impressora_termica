# impressora_termica
<h1>Impressora térmica  com atmega328p e cabeça térmica LTP02-245.</h1>

Para fazer uma impressão deve ser enviado o texto por meio da porta serial com o comando CR (13) "\r" ao final da frase, ao apertar o botão o texto é impresso e é retornado pela serial o texto recebido.

Tensão comunicação 3,3V
BAUD = 9600
Tensão máxima da cabeça térmica (VP) 8V.
Clock interno 8 Mhz

Código e esquema ainda em fase de desenvolvimento.

Versão 1
<img width="1949" height="1981" alt="Versão 1" src="https://github.com/user-attachments/assets/d2e6456f-f486-4e7d-9f64-287764aa1da3" />

Video impresão de texto: https://youtube.com/shorts/1QFV2fXhjIA?feature=share

Atualização futura, impressão de imagem: https://youtube.com/shorts/7ARQ239Vu7w?feature=share

Versão 2

<img width="4160" height="3120" alt="Versão 2" src="https://github.com/user-attachments/assets/fd3a9a09-96c1-4e6e-9e8e-ab20bfaf2698" />
<img width="4160" height="3120" alt="Versão 2" src="https://github.com/user-attachments/assets/09d7b933-807f-4980-a1aa-9955c01f84ba" />

O controle da cabeça térmica funciona da mesma forma entre a versão 1 e 2, na versão 2 foi adicionado um conversor buck para fazer a alimentação da cabeça térmica e uma memória para guardas alguns bitmaps.

## Firmware da versão 2

Novas funções: display OLED SSD1306 128x64 (I2C) com menu, seleção de fonte, impressão de bitmap 320x240 gravado na memória SPI 25Q16 (W25Q16) e pré-visualização de texto e bitmap no display.

### Arquivos

| Arquivo | Conteúdo |
|---|---|
| `main.c` | motor de impressão, menu, protocolo serial, recepção de bitmap |
| `hw.h` | pinos da V2 (conforme `esquema_v2.pdf`) e tamanhos (bitmap, slots) |
| `spi_flash.c/.h` | SPI compartilhado e driver da memória W25Q16 |
| `oled.c/.h` | driver do SSD1306 sem framebuffer (a RAM do ATmega328 é pequena) |
| `font.h` | fontes de impressão e tabela `fontes[]` usada no menu |
| `tools/enviar.py` | envia texto, comandos e imagens pela serial (converte jpg/png para bitmap) |
| `tools/gerar_fonte.py` | gera novas fontes no formato do `font.h` |
| `teste/` | simulador no PC: roda o firmware com memória, cabeça e display simulados |

No MPLAB X, adicione `spi_flash.c` e `oled.c` ao projeto junto com o `main.c`. Também dá para compilar com `make` (avr-gcc). Ocupa cerca de 22 KB de flash e 1,4 KB de RAM.

### Uso pelo botão (com display)

- **Clique curto:** próxima opção.
- **Clique longo (> 0,7 s):** seleciona a opção.

Opções do menu:

- **Imprimir texto**
- **Fonte:** troca a fonte. A escolha fica salva na EEPROM.
- **Ver texto:** pré-visualização. Clique curto vai para a próxima tela, clique longo volta ao menu.
- **Imprimir bitmap**
- **Slot:** troca o slot da memória.
- **Ver bitmap**
- **Receber bitmap:** espera o bitmap pela serial.
- **Avançar papel**

Durante a impressão, um clique longo cancela. Sem display, o botão funciona como na V1 e imprime o texto.

### Fontes

| Nº | Nome | Tamanho | Caracteres por linha |
|---|---|---|---|
| 0 | Consolas | 16x32 | 24 |
| 1 | Terminus | 16x32 | 24 |
| 2 | Pequena (Terminus) | 8x16 | 48 |
| 3 | Grande (Consolas 2x) | 32x64 | 12 |

O texto quebra a linha entre palavras e aceita `\n`. Letras acentuadas (UTF-8) são impressas sem acento. Para adicionar uma fonte, veja o comentário no início do `font.h`.

### Bitmap

- **Formato:** 320x240 pixels, 1 bit por pixel (8 pixels por byte), linha a linha, MSB = pixel da esquerda, 1 = ponto preto, total de 9600 bytes. É o formato "horizontal" do image2cpp.
- **Slots:** 8 slots de 12 KB na memória.
- **Recepção:** os dados chegam pela UART num buffer circular de 512 bytes na RAM e são gravados na memória de 256 em 256 bytes (uma página). O tráfego da UART é controlado com XON/XOFF.
- **Impressão:** cada linha é lida da memória e enviada à cabeça centralizada (32 pontos de margem de cada lado).
- **SPI compartilhado:** a memória e a cabeça usam o mesmo SPI. Durante a impressão o SPI só é usado dentro da interrupção do Timer0, e o /CS da memória fica em nível alto quando os dados vão para a cabeça.
- **Limite de pontos:** linhas com mais de 45 pontos (limite do datasheet) são impressas em partes, com o motor parado. O valor pode ser ajustado em `MAX_PONTOS` no `main.c`.

```
pip install pyserial pillow
python3 tools/enviar.py --porta COM3 --bitmap foto.jpg --slot 0 --imprimir
python3 tools/enviar.py --porta COM3 --fonte 2 --texto "Ola mundo" --imprimir-texto
```

### Protocolo serial (9600 8N1, ESC = 0x1B)

| Comando | Função |
|---|---|
| `texto` + CR | guarda o texto (igual à V1), até 480 caracteres |
| ESC `T` | imprime o texto |
| ESC `F` n | seleciona a fonte n (`0`..`3`) |
| ESC `B` n | recebe bitmap no slot n (`0`..`7`): responde `PRONTO`, recebe 9600 bytes, responde `OK` |
| ESC `P` n | imprime o bitmap do slot n |
| ESC `V` n | mostra o bitmap do slot n no display |
| ESC `S` | responde `FONTE=f SLOT=s SLOTS=00000000` (1 = slot ocupado) |

As impressões respondem `OK`, `CANCELADO`, `ERRO sem papel` ou `ERRO temperatura`.

### Diferenças de hardware tratadas no código

- **Buck:** EN_BUCK (PD3) em nível alto desliga o conversor buck. O firmware só liga o buck durante a impressão.
- **Fim da impressão:** o DST é desconectado do timer e mantido em nível baixo, e as fases do motor são desligadas.
- **Sensor de papel:** usa borda de descida (na V1 era nível baixo, o que travava o programa sem papel).

### Simulador

`sh teste/rodar.sh` compila o `main.c` real no PC com o hardware simulado e confere os resultados:

- texto nas 4 fontes;
- recepção de bitmap com XON/XOFF;
- bitmap impresso igual ao enviado;
- limite de pontos por acionamento.

As imagens do papel e do display ficam em `teste/saida/`.
