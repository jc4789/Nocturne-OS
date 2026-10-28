"""Unicode 18.0.0 の原 UCD から、HTML の方向性（「text node directionality」と
auto-directionality）が読む強い双方向の型（L と、R・AL）の範囲の表を生成する。

型は UnicodeData.txt の Bidi_Class の欄（4 番目）。欄の無い未割当ての符号点は
強い型を持たないものとして扱う（UAX #9 の既定値を当てない）。"""
import ucd

OUTPUT = ucd.ROOT / "user/libc/web/unicode_bidi_data.h"
SOURCES = ("UnicodeData.txt",)


def strong_types():
    types = bytearray(ucd.LIMIT)
    for cp, fields in ucd.unicode_data():
        bidi = fields[4]
        if bidi == "L":
            types[cp] = 1
        elif bidi in ("R", "AL"):
            types[cp] = 2
    return types


def runs(types):
    out = []
    start = None
    for cp in range(ucd.LIMIT + 1):
        value = types[cp] if cp < ucd.LIMIT else 0
        if start is not None and value != types[start]:
            out.append((start, cp - 1, types[start]))
            start = None
        if start is None and value:
            start = cp
    return out


def generate():
    table = runs(strong_types())
    lines = ucd.header("unicode_bidi.py",
                       f"Unicode {ucd.VERSION} の強い双方向の型（1: L、2: R または AL）の範囲。昇順で重ならない。",
                       SOURCES, "NOCTURNE_UNICODE_BIDI_DATA_H")
    lines.extend(["struct unicode_bidi_run { uint32_t first, last; uint8_t type; };",
                  f"static const struct unicode_bidi_run unicode_bidi_strong[{len(table)}] = {{"])
    for first, last, kind in table:
        lines.append(f"    {{0x{first:X}, 0x{last:X}, {kind}}},")
    lines.extend(["};", "#endif", ""])
    return "\n".join(lines)


if __name__ == "__main__":
    ucd.main(__doc__, generate, OUTPUT, "双方向の強い型の表")
