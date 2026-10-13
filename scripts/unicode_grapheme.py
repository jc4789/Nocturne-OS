"""Unicode 18.0.0 の原 UCD から拡張書記素境界の必要プロパティだけを生成する。"""
from hashlib import sha256
from pathlib import Path
import argparse

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "third_party/unicode/18.0.0"
OUTPUT = ROOT / "user/libc/web/unicode_grapheme_data.h"
GCB = {name: index for index, name in enumerate((
    "Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator",
    "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT"))}
INCB = {"None": 0, "Extend": 16, "Linker": 32, "Consonant": 48}


def records(name):
    for line in (DATA / name).read_text(encoding="utf-8").splitlines():
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        fields = [part.strip() for part in body.split(";")]
        ends = fields[0].split("..")
        yield int(ends[0], 16), int(ends[-1], 16), fields[1:]


def generate():
    properties = bytearray(0x110000)
    for first, last, fields in records("GraphemeBreakProperty.txt"):
        value = GCB[fields[0]]
        properties[first:last + 1] = bytes([value]) * (last - first + 1)
    for first, last, fields in records("DerivedCoreProperties.txt"):
        if fields[0] == "InCB":
            value = INCB[fields[1]]
            for cp in range(first, last + 1):
                properties[cp] |= value
    for first, last, fields in records("emoji-data.txt"):
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
    lines = ["/* 正規生成物: python -X utf8 scripts/unicode_grapheme.py",
             " * Unicode 18.0.0 / UAX #29 revision 49 の GCB・InCB・Extended_Pictographic。",
             " * Copyright © 1991-2026 Unicode, Inc. / UNICODE LICENSE V3。",
             " * 許可条件は third_party/unicode/LICENSE を参照。"]
    for name in ("GraphemeBreakProperty.txt", "DerivedCoreProperties.txt", "emoji-data.txt"):
        lines.append(f" * {name} SHA256 {sha256((DATA / name).read_bytes()).hexdigest()}")
    lines.extend([" */", "#ifndef NOCTURNE_UNICODE_GRAPHEME_DATA_H",
                  "#define NOCTURNE_UNICODE_GRAPHEME_DATA_H",
                  "#define WF_GRAPHEME_UNICODE_VERSION \"18.0.0\"",
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="生成済み表との一致だけを確認")
    args = parser.parse_args()
    generated = generate()
    if args.check:
        if OUTPUT.read_text(encoding="utf-8") != generated:
            raise SystemExit("書記素表が原UCDと一致しません")
        print("Unicode 18.0.0 書記素表: 原UCDと一致")
    else:
        OUTPUT.write_text(generated, encoding="utf-8", newline="\n")
        print(f"Unicode 18.0.0 書記素表を正規生成: {OUTPUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
