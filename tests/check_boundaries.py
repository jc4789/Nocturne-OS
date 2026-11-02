"""カーネルの境界の検査。ソースだけを読む。一つでも破れていれば、場所を全部書いて 1 で終わる。

  1. pid の比較で権限を決めない：init_pid の名、DENIED を返す pid の比較を拒む。
  2. カーネルに設定の文書を持ち込まない：kernel/src の文字列の .conf を拒む。
  3. dev/ から UI を呼ばない：gui/ の include と wm_・desktop_・screen_modes_ の呼出しを拒む。
  4. VM の処理は vm/vm.h の後ろ：kernel/src/vm の外から vm/vm.h 以外の vm/ を include しない。

  python -X utf8 tests/check_boundaries.py
"""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parent.parent
kernel = root / "kernel/src"


def code_of(path):
    """コメントを同じ行数の空白へ置き換えた本文（文字列はそのまま）。"""
    text = path.read_text(encoding="utf-8", errors="replace")
    out, at, n = [], 0, len(text)
    while at < n:
        c = text[at]
        if c == '"':
            end = at + 1
            while end < n and text[end] != '"' and text[end] != "\n":
                end += 2 if text[end] == "\\" else 1
            out.append(text[at:end + 1])
            at = end + 1
        elif text.startswith("/*", at):
            end = text.find("*/", at + 2)
            end = n if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[at:end]))
            at = end
        elif text.startswith("//", at):
            end = text.find("\n", at)
            end = n if end < 0 else end
            at = end
        else:
            out.append(c)
            at += 1
    return "".join(out)


PID_COMPARE = re.compile(r"\bpid\s*[!=]=|[!=]=\s*\w*(->|\.)?pid\b")
UI_INCLUDE = re.compile(r'#\s*include\s+"(\.\./)?gui/')
UI_CALL = re.compile(r"\b(wm_|desktop_|screen_modes_)\w*\s*\(")
VM_INCLUDE = re.compile(r'#\s*include\s+"(\.\./)?vm/([^"]+)"')
CONF = re.compile(r'"[^"\n]*\.conf"')

broken = []


def report(path, number, rule, line):
    broken.append(f"{path.relative_to(root).as_posix()}:{number}: {rule}: {line.strip()}")


for path in sorted([*kernel.rglob("*.c"), *kernel.rglob("*.h")]):
    relative = path.relative_to(kernel).as_posix()
    lines = code_of(path).split("\n")
    for i, line in enumerate(lines):
        number = i + 1
        if "init_pid" in line:
            report(path, number, "pid の権限", line)
        if PID_COMPARE.search(line) and any("N_ERR_DENIED" in near for near in lines[i:i + 3]):
            report(path, number, "pid の権限", line)
        if CONF.search(line):
            report(path, number, "カーネルの .conf", line)
        if relative.startswith("dev/") and (UI_INCLUDE.search(line) or UI_CALL.search(line)):
            report(path, number, "dev/ から UI", line)
        included = VM_INCLUDE.search(line)
        if included and not relative.startswith("vm/") and included.group(2) != "vm.h":
            report(path, number, "vm/ の外の VM", line)

for line in broken:
    print(line)
print(f"check_boundaries: {len(broken)} 件")
sys.exit(1 if broken else 0)
