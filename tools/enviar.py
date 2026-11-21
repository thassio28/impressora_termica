#!/usr/bin/env python3
"""
Envia texto, bitmaps e comandos para a impressora termica V2 pela serial.

Requer: pip install pyserial pillow

Exemplos:
  # converte uma imagem (jpg, png...) para 320x240 1 bit e grava no slot 0
  python3 enviar.py --porta COM3 --bitmap foto.jpg --slot 0

  # grava e ja imprime
  python3 enviar.py --porta /dev/ttyUSB0 --bitmap logo.png --slot 1 --imprimir

  # so converte, salva o .bin (9600 bytes) e uma previa .png, sem enviar
  python3 enviar.py --bitmap foto.jpg --salvar foto.bin

  # texto, fonte e impressao
  python3 enviar.py --porta COM3 --fonte 2 --texto "Ola mundo" --imprimir-texto

  # imprime um bitmap ja gravado / consulta o estado
  python3 enviar.py --porta COM3 --imprimir-slot 1
  python3 enviar.py --porta COM3 --status

Formatos aceitos em --bitmap:
  - imagens (png, jpg, bmp, ...): redimensionada para caber em 320x240,
    convertida para preto e branco com pontilhado (dithering);
  - .bin com 9600 bytes: 320x240, 1 bit por pixel (MSB = esquerda, 1 = preto);
  - .bin com 76800 bytes: 320x240, 8 bits por pixel (tons de cinza, 0 = preto);
  - .h / .c / .txt com um vetor em C (ex.: gerado pelo image2cpp) com 9600
    valores 0x.. no formato horizontal 1 bit por pixel.
"""
import argparse
import re
import sys
import time

LARGURA, ALTURA = 320, 240
TAMANHO = LARGURA * ALTURA // 8
ESC = b"\x1b"


def imagem_para_bytes(caminho, inverter=False, limiar=None, girar=False):
    from PIL import Image, ImageOps

    im = Image.open(caminho)
    im = ImageOps.exif_transpose(im).convert("L")
    if girar or (im.height > im.width):
        im = im.rotate(90, expand=True)
    im.thumbnail((LARGURA, ALTURA), Image.LANCZOS)
    fundo = Image.new("L", (LARGURA, ALTURA), 255)
    fundo.paste(im, ((LARGURA - im.width) // 2, (ALTURA - im.height) // 2))
    return cinza_para_bytes(fundo, inverter, limiar)


def cinza_para_bytes(im, inverter=False, limiar=None):
    from PIL import Image, ImageOps

    if inverter:
        im = ImageOps.invert(im)
    if limiar is None:
        bw = im.convert("1")  # Floyd-Steinberg
    else:
        bw = im.point(lambda v: 255 if v >= limiar else 0).convert("1", dither=Image.NONE)
    # no PIL "1": bit 1 = branco. Na impressora: bit 1 = ponto preto.
    dados = bytes(b ^ 0xFF for b in bw.tobytes())
    assert len(dados) == TAMANHO
    return dados


def carregar_bitmap(caminho, inverter=False, limiar=None, girar=False):
    nome = caminho.lower()
    if nome.endswith(".bin"):
        dados = open(caminho, "rb").read()
        if len(dados) == TAMANHO:
            return bytes(b ^ 0xFF for b in dados) if inverter else dados
        if len(dados) == LARGURA * ALTURA:
            from PIL import Image
            return cinza_para_bytes(Image.frombytes("L", (LARGURA, ALTURA), dados),
                                    inverter, limiar)
        sys.exit("arquivo .bin deve ter %d (1 bit) ou %d (8 bits) bytes, tem %d"
                 % (TAMANHO, LARGURA * ALTURA, len(dados)))
    if nome.endswith((".h", ".c", ".txt")):
        texto = re.sub(r"//[^\n]*|/\*.*?\*/", "", open(caminho).read(), flags=re.S)
        valores = [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{1,2})\b", texto)]
        if len(valores) != TAMANHO:
            sys.exit("o vetor deve ter %d bytes, tem %d" % (TAMANHO, len(valores)))
        dados = bytes(valores)
        return bytes(b ^ 0xFF for b in dados) if inverter else dados
    return imagem_para_bytes(caminho, inverter, limiar, girar)


def salvar_previa(dados, caminho_png):
    from PIL import Image
    im = Image.frombytes("1", (LARGURA, ALTURA), bytes(b ^ 0xFF for b in dados))
    im.save(caminho_png)


class Impressora:
    def __init__(self, porta, baud=9600):
        import serial
        self.s = serial.Serial(porta, baud, timeout=0.2, xonxoff=True)
        time.sleep(0.1)
        self.s.reset_input_buffer()

    def ler_linha(self, timeout):
        fim = time.time() + timeout
        linha = b""
        while time.time() < fim:
            c = self.s.read(1)
            if not c:
                continue
            if c in (b"\x11", b"\x13"):
                continue
            if c == b"\n":
                return linha.decode(errors="replace").strip()
            linha += c
        return None

    def esperar(self, palavras, timeout):
        fim = time.time() + timeout
        while time.time() < fim:
            linha = self.ler_linha(fim - time.time())
            if linha is None:
                break
            if linha:
                print("<", linha)
            for p in palavras:
                if linha.startswith(p):
                    return linha
        return None

    def enviar_bitmap(self, dados, slot):
        self.s.write(ESC + b"B" + str(slot).encode())
        r = self.esperar(["PRONTO", "ERRO"], 10)
        if not r or not r.startswith("PRONTO"):
            sys.exit("a impressora nao respondeu PRONTO")
        enviado = 0
        bloco = 64
        inicio = time.time()
        while enviado < len(dados):
            self.s.write(dados[enviado:enviado + bloco])
            enviado += bloco
            print("\r  enviando %3d%%" % (min(enviado, len(dados)) * 100 // len(dados)),
                  end="", flush=True)
        self.s.flush()
        print("  (%.1f s)" % (time.time() - inicio))
        r = self.esperar(["OK", "ERRO"], 10)
        if not r or not r.startswith("OK"):
            sys.exit("falha ao gravar o bitmap")

    def comando(self, cmd, esperar=("OK", "ERRO"), timeout=5):
        self.s.write(ESC + cmd)
        return self.esperar(list(esperar), timeout)

    def texto(self, t):
        t = t.replace("\\n", "\n")
        self.s.write(t.encode("utf-8") + b"\r")
        time.sleep(len(t) / 900 + 0.2)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--porta", help="porta serial (COM3, /dev/ttyUSB0...)")
    ap.add_argument("--bitmap", help="imagem / .bin / vetor C a enviar")
    ap.add_argument("--slot", type=int, default=0, help="slot da memoria (0..7)")
    ap.add_argument("--imprimir", action="store_true", help="imprime o bitmap depois de enviar")
    ap.add_argument("--imprimir-slot", type=int, help="imprime um bitmap ja gravado")
    ap.add_argument("--inverter", action="store_true", help="inverte preto e branco")
    ap.add_argument("--limiar", type=int, help="usa limiar fixo (0..255) em vez de pontilhado")
    ap.add_argument("--girar", action="store_true", help="gira a imagem 90 graus")
    ap.add_argument("--salvar", help="salva o bitmap convertido (.bin) e uma previa (.png)")
    ap.add_argument("--texto", help="texto a enviar (use \\n para quebrar linha)")
    ap.add_argument("--fonte", type=int, help="seleciona a fonte (0..3)")
    ap.add_argument("--imprimir-texto", action="store_true")
    ap.add_argument("--status", action="store_true")
    a = ap.parse_args()

    dados = None
    if a.bitmap:
        dados = carregar_bitmap(a.bitmap, a.inverter, a.limiar, a.girar)
        if a.salvar:
            open(a.salvar, "wb").write(dados)
            previa = re.sub(r"\.bin$", "", a.salvar) + ".png"
            salvar_previa(dados, previa)
            print("salvo %s e %s" % (a.salvar, previa))

    if not a.porta:
        if not a.salvar:
            ap.error("informe --porta (ou --salvar para so converter)")
        return

    imp = Impressora(a.porta)
    if a.fonte is not None:
        imp.comando(b"F" + str(a.fonte).encode())
    if a.texto:
        imp.texto(a.texto)
    if dados is not None:
        imp.enviar_bitmap(dados, a.slot)
        if a.imprimir:
            imp.comando(b"P" + str(a.slot).encode(), timeout=60)
    if a.imprimir_slot is not None:
        imp.comando(b"P" + str(a.imprimir_slot).encode(), timeout=60)
    if a.imprimir_texto:
        imp.comando(b"T", timeout=120)
    if a.status:
        imp.comando(b"S", esperar=("FONTE",))


if __name__ == "__main__":
    main()
