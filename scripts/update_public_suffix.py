"""公式PSLを全件A-label化してNocturneの信頼済みデータとして保存する。

開発ホスト専用。Python idna (IDNA2008/UTS #46) が必要。
実行: python scripts/update_public_suffix.py
OS実行時の自動ダウンロードや依存追加は行わない。
"""
from datetime import datetime, timezone
from hashlib import sha256
from pathlib import Path
import urllib.request

import idna

SOURCE = "https://publicsuffix.org/list/public_suffix_list.dat"
DEST = Path(__file__).resolve().parents[1] / "rootfs/usr/share/browser/public_suffix_list.dat"

def convert(raw: bytes) -> tuple[bytes, int]:
    lines = []
    count = 0
    for original in raw.decode("utf-8").splitlines():
        line = original.strip()
        if not line or line.startswith("//"):
            lines.append(original)
            continue
        prefix = "!" if line.startswith("!") else "*." if line.startswith("*.") else ""
        host = line[len(prefix):]
        encoded = idna.encode(host, uts46=True, transitional=False, std3_rules=True).decode("ascii").lower()
        # 失敗は例外で全処理停止。未知/非ASCII規則を落とした部分表は配布しない。
        lines.append(prefix + encoded)
        count += 1
    if count < 9000 or not any("BEGIN PRIVATE DOMAINS" in x for x in lines):
        raise ValueError("公式PSLの完全性確認に失敗")
    header = ["// Nocturne: Unicode rule labels converted to ASCII using IDNA2008/UTS #46.",
              "// Source: " + SOURCE,
              "// Retrieved UTC: " + datetime.now(timezone.utc).isoformat(),
              "// Source SHA256: " + sha256(raw).hexdigest(),
              "// Converter: scripts/update_public_suffix.py; Python idna " + idna.__version__]
    return ("\n".join(header + lines) + "\n").encode("utf-8"), count

if __name__ == "__main__":
    with urllib.request.urlopen(SOURCE, timeout=30) as response:
        raw = response.read(2 * 1024 * 1024 + 1)
    if len(raw) > 2 * 1024 * 1024:
        raise ValueError("PSL入力上限超過")
    result, count = convert(raw)
    DEST.parent.mkdir(parents=True, exist_ok=True)
    DEST.write_bytes(result)
    print(f"PSL: {count} rules, {len(result)} bytes, source SHA256 {sha256(raw).hexdigest()}")
