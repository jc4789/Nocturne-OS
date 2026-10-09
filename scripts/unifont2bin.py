#!/usr/bin/env python3
"""提供Unifont BDFの全字形を、BMP直接索引の幅表と無変形ビットマップへ変換する。"""
import argparse
from pathlib import Path
from bdf2c import parse_bdf, render

CODEPOINTS = 0x10000
HEIGHT = 16
ROW_BYTES = 2


def convert(src):
    box, ascent, glyphs = parse_bdf(src)
    if box != [16, 16, 0, -2] or ascent != 14:
        raise ValueError("Unifontの16px BDFではありません")
    widths = bytearray(CODEPOINTS)
    rows = bytearray(CODEPOINTS * HEIGHT * ROW_BYTES)
    for cp, (bbx, bitmap) in glyphs.items():
        if not 0 <= cp < CODEPOINTS:
            raise ValueError("BMP外の字形はこの形式では扱えません: U+%X" % cp)
        if bbx not in ([8, 16, 0, -2], [16, 16, 0, -2]) or len(bitmap) != HEIGHT:
            raise ValueError("想定外の字形サイズ: U+%04X" % cp)
        widths[cp] = bbx[0]
        offset = cp * HEIGHT * ROW_BYTES
        rows[offset:offset + HEIGHT * ROW_BYTES] = bytes(render(box, ascent, (bbx, bitmap)))
    if not widths[0xFFFD]:
        raise ValueError("置換文字U+FFFDがありません")
    return bytes(widths + rows), len(glyphs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("src")
    parser.add_argument("out")
    args = parser.parse_args()
    data, count = convert(args.src)
    Path(args.out).write_bytes(data)
    print("Unifont: %d字形を無変形で収録、%dバイト" % (count, len(data)))


if __name__ == "__main__":
    main()
