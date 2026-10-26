"""Unicode 18.0.0 の原 UCD から拡張書記素境界の必要プロパティだけを生成する。"""
import ucd

OUTPUT = ucd.ROOT / "user/libc/web/unicode_grapheme_data.h"
SOURCES = ("GraphemeBreakProperty.txt", "DerivedCoreProperties.txt", "emoji-data.txt")
GCB = {name: index for index, name in enumerate((
    "Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator",
    "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT"))}
INCB = {"None": 0, "Extend": 16, "Linker": 32, "Consonant": 48}


def generate():
    properties = bytearray(ucd.LIMIT)
    for first, last, fields in ucd.records("GraphemeBreakProperty.txt"):
        value = GCB[fields[0]]
        properties[first:last + 1] = bytes([value]) * (last - first + 1)
    for first, last, fields in ucd.records("DerivedCoreProperties.txt"):
        if fields[0] == "InCB":
            value = INCB[fields[1]]
            for cp in range(first, last + 1):
                properties[cp] |= value
    for first, last, fields in ucd.records("emoji-data.txt"):
        if fields[0] == "Extended_Pictographic":
            for cp in range(first, last + 1):
                properties[cp] |= 64
    # LV/LVT の全11,172音節は規範的な Hangul 算式へ置換する。
    # 別プロパティが重なる版では生成を失敗させ、静かに落とさない。
    for cp in range(0xAC00, 0xD7A4):
        expected = GCB["LV"] if (cp - 0xAC00) % 28 == 0 else GCB["LVT"]
        if properties[cp] != expected:
            raise ValueError(f"Hangul property changed at U+{cp:04X}")
        properties[cp] = 0
    lines = ucd.header("unicode_grapheme.py",
                       f"Unicode {ucd.VERSION} / UAX #29 revision 49 の GCB・InCB・Extended_Pictographic。",
                       SOURCES, "NOCTURNE_UNICODE_GRAPHEME_DATA_H")
    lines.extend([f"#define WF_GRAPHEME_UNICODE_VERSION \"{ucd.VERSION}\"",
                  "enum wf_grapheme_property {"])
    for name, value in GCB.items():
        lines.append(f"    WF_GCB_{name.upper()} = {value},")
    lines.extend(["    WF_INCB_EXTEND = 16, WF_INCB_LINKER = 32, WF_INCB_CONSONANT = 48,",
                  "    WF_EXTENDED_PICTOGRAPHIC = 64", "};",
                  "struct wf_grapheme_range { uint32_t first, last; uint8_t properties; };",
                  "static const struct wf_grapheme_range wf_grapheme_ranges[] = {"])
    first = 0
    while first < len(properties):
        value = properties[first]
        last = first + 1
        while last < len(properties) and properties[last] == value:
            last += 1
        if value:
            lines.append(f"    {{0x{first:X}, 0x{last - 1:X}, {value}}},")
        first = last
    lines.extend(["};", "#endif", ""])
    return "\n".join(lines)


if __name__ == "__main__":
    ucd.main(__doc__, generate, OUTPUT, "書記素表")
