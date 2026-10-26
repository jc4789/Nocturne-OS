"""Unicode 18.0.0 の原 UCD から libc の <wctype.h> の分類と単純大文字小文字変換の表を生成する。"""
import ucd

OUTPUT = ucd.ROOT / "user/libc/wctype_data.h"
SOURCES = ("UnicodeData.txt", "PropList.txt", "DerivedCoreProperties.txt")
# 1 符号点 1 バイトの分類ビット。print は graph と blank から導く（wctype.c）。
BITS = {name: 1 << index for index, name in enumerate((
    "ALPHA", "UPPER", "LOWER", "SPACE", "BLANK", "CNTRL", "PUNCT", "GRAPH"))}
LIMIT = ucd.LIMIT


def unicode_data():
    """一般カテゴリーと単純大文字・小文字の写像。"""
    category = ["Cn"] * LIMIT
    upper, lower = {}, {}
    for cp, fields in ucd.unicode_data():
        category[cp] = fields[2]
        if fields[12]:
            upper[cp] = int(fields[12], 16)
        if fields[13]:
            lower[cp] = int(fields[13], 16)
    return category, upper, lower


def property_set(name, wanted):
    members = set()
    for first, last, fields in ucd.records(name):
        if fields[0] == wanted:
            members.update(range(first, last + 1))
    return members


def classify():
    category, upper_map, lower_map = unicode_data()
    alphabetic = property_set("DerivedCoreProperties.txt", "Alphabetic")
    uppercase = property_set("DerivedCoreProperties.txt", "Uppercase")
    lowercase = property_set("DerivedCoreProperties.txt", "Lowercase")
    white = property_set("PropList.txt", "White_Space")
    # UTS #18 付録 C の互換プロパティ。ISO C は iswdigit を 0-9 に限るので、
    # 他の十進数字は alpha に入れて alnum を保つ。
    not_blank = {0x0A, 0x0B, 0x0C, 0x0D, 0x85}
    classes = bytearray(LIMIT)
    for cp in range(LIMIT):
        gc = category[cp]
        alpha = cp in alphabetic or (gc == "Nd" and not 0x30 <= cp <= 0x39)
        graph = cp not in white and gc not in ("Cc", "Cs", "Cn")
        bits = 0
        if alpha:
            bits |= BITS["ALPHA"]
        if cp in uppercase:
            bits |= BITS["UPPER"]
        if cp in lowercase:
            bits |= BITS["LOWER"]
        if cp in white:
            bits |= BITS["SPACE"]
            if cp not in not_blank and gc not in ("Zl", "Zp"):
                bits |= BITS["BLANK"]
        if gc == "Cc":
            bits |= BITS["CNTRL"]
        if graph:
            bits |= BITS["GRAPH"]
            if gc[0] in "PS" and not alpha:
                bits |= BITS["PUNCT"]
        classes[cp] = bits
    check(classes)
    return classes, upper_map, lower_map


def check(classes):
    """ISO C 7.30.2.1 の分類間の制約と、ASCII が <ctype.h> の "C" ロケールと一致することを確かめる。"""
    b = BITS
    for cp, bits in enumerate(classes):
        alpha = bits & b["ALPHA"]
        problems = []
        if bits & (b["UPPER"] | b["LOWER"]) and not alpha:
            problems.append("upper/lower が alpha でない")
        if alpha and bits & (b["CNTRL"] | b["SPACE"] | b["PUNCT"]):
            problems.append("alpha が cntrl/space/punct と重なる")
        if alpha and 0x30 <= cp <= 0x39:
            problems.append("alpha が digit と重なる")
        if alpha and not bits & b["GRAPH"]:
            problems.append("alpha が graph でない")
        if bits & b["BLANK"] and not bits & b["SPACE"]:
            problems.append("blank が space でない")
        if bits & b["SPACE"] and bits & b["GRAPH"]:
            problems.append("space が graph と重なる")
        if bits & b["CNTRL"] and bits & b["GRAPH"]:
            problems.append("cntrl が graph と重なる")
        if problems:
            raise ValueError(f"U+{cp:04X}: " + "、".join(problems))
    for cp in range(0x80):
        ch = chr(cp)
        expected = 0
        if ch.isalpha():
            expected |= b["ALPHA"]
        if "A" <= ch <= "Z":
            expected |= b["UPPER"]
        if "a" <= ch <= "z":
            expected |= b["LOWER"]
        if ch in " \t\n\v\f\r":
            expected |= b["SPACE"]
        if ch in " \t":
            expected |= b["BLANK"]
        if cp < 0x20 or cp == 0x7F:
            expected |= b["CNTRL"]
        if 0x21 <= cp <= 0x7E:
            expected |= b["GRAPH"]
            if not ch.isalnum():
                expected |= b["PUNCT"]
        if classes[cp] != expected:
            raise ValueError(f"U+{cp:04X} の分類が <ctype.h> と一致しない")


def case_runs(mapping):
    """写像を (先頭, 個数, 間隔, 差) の連なりにまとめる。間隔 2 は大小が交互に並ぶ区画。"""
    runs = []
    for cp in sorted(mapping):
        delta = mapping[cp] - cp
        if runs:
            first, count, step, run_delta = runs[-1]
            last = first + (count - 1) * step
            if run_delta == delta and (cp - last == step or (count == 1 and cp - last in (1, 2))):
                runs[-1] = (first, count + 1, cp - last, delta)
                continue
        runs.append((cp, 1, 1, delta))
    # 連なりは互いに重ならず昇順なので wctype.c は二分探索できる。展開して元の写像と比べる。
    expanded = {}
    for first, count, step, delta in runs:
        if count > 0xFFFF or step > 0xFF:
            raise ValueError(f"U+{first:04X} の連なりが表の型に収まらない")
        for i in range(count):
            expanded[first + i * step] = first + i * step + delta
    if expanded != mapping or any(a[0] + (a[1] - 1) * a[2] >= b[0] for a, b in zip(runs, runs[1:])):
        raise ValueError("大文字小文字の連なりが写像を再現しない")
    return runs


def generate():
    classes, upper_map, lower_map = classify()
    shift, blocks, index, index_bytes = ucd.two_level(classes)
    lines = ucd.header("unicode_wctype.py",
                       f"Unicode {ucd.VERSION} の <wctype.h> の分類（UTS #18 付録 C）と単純大文字小文字変換。",
                       SOURCES, "NOCTURNE_WCTYPE_DATA_H")
    lines.extend([f"#define WCTYPE_UNICODE_VERSION \"{ucd.VERSION}\"", "enum wctype_bit {"])
    for name, value in BITS.items():
        lines.append(f"    WC_{name} = {value},")
    lines.extend(["};", f"#define WCTYPE_SHIFT {shift}",
                  f"static const uint{8 * index_bytes}_t wctype_index[{len(index)}] = {{"])
    ucd.emit_bytes(lines, index)
    lines.append("};")
    lines.append(f"static const uint8_t wctype_blocks[{len(blocks) << shift}] = {{")
    ucd.emit_bytes(lines, [value for block in blocks for value in block])
    lines.append("};")
    lines.append("struct wctype_case_run { uint32_t first; uint16_t count; uint8_t step; int32_t delta; };")
    for name, mapping in (("upper", upper_map), ("lower", lower_map)):
        lines.append(f"static const struct wctype_case_run wctype_to_{name}[] = {{")
        for first, count, step, delta in case_runs(mapping):
            lines.append(f"    {{0x{first:X}, {count}, {step}, {delta}}},")
        lines.append("};")
    lines.extend(["#endif", ""])
    return "\n".join(lines)


if __name__ == "__main__":
    ucd.main(__doc__, generate, OUTPUT, "wctype 表")
