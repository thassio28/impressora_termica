#!/bin/sh
# Compila e roda o simulador do firmware no PC (gcc).
# Uso: sh teste/rodar.sh [bitmap.bin]
#   bitmap.bin: 9600 bytes gerados com  tools/enviar.py --bitmap foto.jpg --salvar bitmap.bin
# As imagens do que seria impresso e do display ficam em teste/saida/*.pbm
cd "$(dirname "$0")" || exit 1
mkdir -p saida
gcc -O1 -w -I. -o saida/simulador simulador.c || exit 1
./saida/simulador "$@"
