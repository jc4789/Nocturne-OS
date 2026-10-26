"""Unicode 18.0.0 の原 UCD から、UTS #46 の妥当性の検査が IDNA の対応表の外に要る性質を生成する。

Lexbor の unicode/idna.c が使う（third_party/lexbor/NOCTURNE_PATCHES.json）。Bidi_Class は
CheckBidi（RFC 5893 第 2 節）、General_Category=Mark は「ラベルを結合記号で始めない」に使う。
ヴィラーマ（Canonical_Combining_Class 9）は Lexbor の正規化表が既に持つ。
"""
import ucd

OUTPUT = ucd.ROOT / "third_party/lexbor/source/lexbor/unicode/idna_validity_res.h"
SOURCES = ("UnicodeData.txt",)
# 0 が L：未割り当ての符号点は IDNA の対応表で disallowed になり、この表まで届かない。
BIDI = ("L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN", "B", "S", "WS", "ON",
        "LRE", "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI")
MARK = 1 << 5


def generate():
    bidi = {name: index for index, name in enumerate(BIDI)}
    values = bytearray(ucd.LIMIT)
    for cp, fields in ucd.unicode_data():
        values[cp] = bidi[fields[4]] | (MARK if fields[2].startswith("M") else 0)
    shift, blocks, index, index_bytes = ucd.two_level(values)
    lines = ucd.header("unicode_idna.py",
                       f"Unicode {ucd.VERSION} の Bidi_Class と General_Category=Mark（UTS #46 の妥当性の検査）。",
                       SOURCES, "LXB_UNICODE_IDNA_VALIDITY_RES_H")
    lines.append("enum {")
    for name, value in bidi.items():
        lines.append(f"    LXB_UNICODE_IDNA_BIDI_{name} = {value},")
    lines.extend([f"    LXB_UNICODE_IDNA_BIDI_MASK = {MARK - 1},", f"    LXB_UNICODE_IDNA_MARK = {MARK}", "};",
                  f"#define LXB_UNICODE_IDNA_VALIDITY_SHIFT {shift}",
                  f"static const uint{8 * index_bytes}_t lxb_unicode_idna_validity_index[{len(index)}] = {{"])
    ucd.emit_bytes(lines, index)
    lines.append("};")
    lines.append(f"static const uint8_t lxb_unicode_idna_validity_blocks[{len(blocks) << shift}] = {{")
    ucd.emit_bytes(lines, [value for block in blocks for value in block])
    lines.extend(["};", "#endif", ""])
    return "\n".join(lines)


if __name__ == "__main__":
    ucd.main(__doc__, generate, OUTPUT, "IDNA 妥当性表")
