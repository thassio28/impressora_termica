#!/usr/bin/env python3
"""
Gera uma fonte bitmap no mesmo formato da fonte Consolas do font.h
(formato FontEdit):

  - caracteres ASCII 32 (' ') ate 126 ('~'), 95 caracteres;
  - cada caractere ocupa  altura * (largura/8)  bytes;
  - linha a linha, de cima para baixo;
  - dentro de cada linha, primeiro byte = 8 pixels da esquerda;
  - dentro de cada byte, bit 0 (LSB) = pixel mais a esquerda
    (o SPI envia com DORD=1, LSB primeiro).

Uso:
  python3 gerar_fonte.py ARQUIVO.ttf LARGURA ALTURA NOME [--tamanho N] [--y OFFSET]

Aceita fontes TrueType (.ttf) e fontes bitmap X11 (.pcf / .pcf.gz).

Exemplo (fontes usadas no font.h, pacote Debian/Ubuntu xfonts-terminus,
licenca SIL OFL 1.1):
  python3 gerar_fonte.py /usr/share/fonts/X11/misc/ter-u16b_iso-8859-1.pcf.gz 8 16 terminus_8x16
  python3 gerar_fonte.py /usr/share/fonts/X11/misc/ter-u32b_iso-8859-1.pcf.gz 16 32 terminus_16x32

Use --preview para ver os caracteres no terminal antes de gerar.
"""
import argparse

from PIL import Image, ImageDraw, ImageFont


def carregar(arquivo, tamanho):
    """Carrega uma fonte TrueType (.ttf) ou bitmap X11 (.pcf / .pcf.gz)."""
    if ".pcf" in arquivo:
        import gzip, os, tempfile
        from PIL import PcfFontFile
        abrir = gzip.open if arquivo.endswith(".gz") else open
        with abrir(arquivo, "rb") as fp:
            pcf = PcfFontFile.PcfFontFile(fp)
        base = os.path.join(tempfile.mkdtemp(), "fonte")
        pcf.save(base)
        return ImageFont.load(base + ".pil"), True
    return ImageFont.truetype(arquivo, tamanho), False


def gerar(ttf, w, h, tamanho, dy, limiar=110):
    fonte, bitmap = carregar(ttf, tamanho)
    if bitmap:
        topo = dy  # fontes bitmap ja vem no tamanho da celula
    else:
        ascent, descent = fonte.getmetrics()
        # centraliza verticalmente a caixa ascent+descent na celula
        topo = (h - (ascent + descent)) // 2 + dy
    glifos = []
    for code in range(32, 127):
        ch = chr(code)
        img = Image.new("L", (w, h), 0)
        d = ImageDraw.Draw(img)
        x = 0 if bitmap else (w - fonte.getlength(ch)) / 2
        d.text((x, topo), ch, fill=255, font=fonte)
        px = img.load()
        linhas = []
        for y in range(h):
            linha = [1 if px[x_, y] >= limiar else 0 for x_ in range(w)]
            linhas.append(linha)
        glifos.append(linhas)
    return glifos


def para_bytes(glifo, w):
    out = []
    bpr = (w + 7) // 8
    for linha in glifo:
        for b in range(bpr):
            v = 0
            for bit in range(8):
                x = b * 8 + bit
                if x < w and linha[x]:
                    v |= 1 << bit  # LSB = pixel da esquerda
            out.append(v)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ttf")
    ap.add_argument("largura", type=int)
    ap.add_argument("altura", type=int)
    ap.add_argument("nome")
    ap.add_argument("--tamanho", type=int, default=None)
    ap.add_argument("--y", type=int, default=0, help="desloca o desenho na vertical")
    ap.add_argument("--limiar", type=int, default=110)
    ap.add_argument("--preview", action="store_true")
    a = ap.parse_args()
    if a.largura % 8:
        raise SystemExit("a largura deve ser multipla de 8")
    tamanho = a.tamanho or int(a.altura * 0.8)
    glifos = gerar(a.ttf, a.largura, a.altura, tamanho, a.y, a.limiar)

    if a.preview:
        for code, g in zip(range(32, 127), glifos):
            print("'%s'" % chr(code))
            for linha in g:
                print("".join("#" if p else "." for p in linha))
        return

    print("//")
    print("// %s" % a.nome)
    print("// Font Size: %dx%dpx (gerada por tools/gerar_fonte.py a partir de %s, tamanho %d)"
          % (a.largura, a.altura, a.ttf.split("/")[-1], tamanho))
    print("// Formato: linha a linha, %d byte(s) por linha, LSB = pixel da esquerda" % (a.largura // 8))
    print("//")
    print("const uint8_t font_%s[] PROGMEM = {" % a.nome)
    for code, g in zip(range(32, 127), glifos):
        dados = para_bytes(g, a.largura)
        for i in range(0, len(dados), 16):
            print("\t" + ",".join("0x%02X" % v for v in dados[i:i + 16]) + ",")
        c = chr(code)
        if c == "\\":
            c = "backslash"
        print("\t // Character 0x%02x (%d: '%s')" % (code, code, c))
    print("};")


if __name__ == "__main__":
    main()
