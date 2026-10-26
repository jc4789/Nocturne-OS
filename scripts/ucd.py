"""原 UCD（third_party/unicode/<版>/）から C の表を正規生成する生成器の共通部分。

unicode_grapheme.py・unicode_wctype.py・unicode_idna.py が使う。各生成器は
生成物の全文を返す generate() を持ち、main() が書き出しと --check を受け持つ。
"""
from hashlib import sha256
from pathlib import Path
import argparse

ROOT = Path(__file__).resolve().parent.parent
VERSION = "18.0.0"
DATA = ROOT / "third_party/unicode" / VERSION
LIMIT = 0x110000


def records(name):
    """「符号点[..符号点]; 値; …」の行を (先頭, 末尾, 残りの欄) で返す。"""
    for line in (DATA / name).read_text(encoding="utf-8").splitlines():
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        fields = [part.strip() for part in body.split(";")]
        ends = fields[0].split("..")
        yield int(ends[0], 16), int(ends[-1], 16), fields[1:]


def unicode_data():
    """UnicodeData.txt の (符号点, 欄)。<…, First>／<…, Last> の組は範囲の全符号点に広げる。"""
    first = None
    for line in (DATA / "UnicodeData.txt").read_text(encoding="utf-8").splitlines():
        fields = line.split(";")
        cp, name = int(fields[0], 16), fields[1]
        if name.endswith(", First>"):
            first = cp
            continue
        start = first if name.endswith(", Last>") else cp
        first = None
        for c in range(start, cp + 1):
            yield c, fields


def header(script, summary, sources, guard):
    """出典と各原データの SHA256 を持つ注釈と include guard の行。"""
    lines = [f"/* 正規生成物: python -X utf8 scripts/{script}",
             f" * {summary}",
             " * Copyright © 1991-2026 Unicode, Inc. / UNICODE LICENSE V3。",
             " * 許可条件は third_party/unicode/LICENSE を参照。"]
    for name in sources:
        lines.append(f" * {name} SHA256 {sha256((DATA / name).read_bytes()).hexdigest()}")
    lines.extend([" */", f"#ifndef {guard}", f"#define {guard}"])
    return lines


def two_level(values):
    """各ブロックを一度だけ持つ二段の表。大きさが最小になるブロック長を選ぶ。"""
    best = None
    for shift in range(4, 12):
        size = 1 << shift
        blocks, index = {}, []
        for start in range(0, LIMIT, size):
            block = bytes(values[start:start + size])
            index.append(blocks.setdefault(block, len(blocks)))
        index_bytes = 1 if len(blocks) <= 256 else 2
        total = len(index) * index_bytes + len(blocks) * size
        if best is None or total < best[0]:
            best = (total, shift, list(blocks), index, index_bytes)
    return best[1:]


def emit_bytes(lines, values, per_line=24):
    for start in range(0, len(values), per_line):
        lines.append("    " + ", ".join(str(v) for v in values[start:start + per_line]) + ",")


def main(description, generate, output, label):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--check", action="store_true", help="生成済み表との一致だけを確認")
    args = parser.parse_args()
    generated = generate()
    if args.check:
        if output.read_text(encoding="utf-8") != generated:
            raise SystemExit(f"{label}が原UCDと一致しません")
        print(f"Unicode {VERSION} {label}: 原UCDと一致")
    else:
        output.write_text(generated, encoding="utf-8", newline="\n")
        print(f"Unicode {VERSION} {label}を正規生成: {output.relative_to(ROOT)}")
