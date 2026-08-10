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

Atualização futura 2, talvez display OLED.
