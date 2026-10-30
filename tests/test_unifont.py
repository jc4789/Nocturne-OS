"""全BDF字形の独立照合：common/unifont.bin を上流の BDF と再生成の両方に照らす。"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    vendor = ROOT / 'third_party/unifont'
    manifest = json.loads((vendor / 'manifest.json').read_text(encoding='utf-8'))
    compressed = (vendor / manifest['compressed_filename']).read_bytes()
    raw = gzip.decompress(compressed)
    assert hashlib.sha256(raw).hexdigest() == manifest['source_sha256']
    assert hashlib.sha256(compressed).hexdigest() == manifest['compressed_sha256']
    assert hashlib.sha256((vendor / 'OFL-1.1.txt').read_bytes()).hexdigest() == manifest['license_sha256']
    data = (ROOT / 'common/unifont.bin').read_bytes()
    assert len(data) == 65536 + 65536 * 32
    glyphs = {}
    cp = width = None
    rows = None
    for line in raw.decode('utf-8').splitlines():
        if line.startswith('ENCODING '):
            cp = int(line.split()[1])
        elif line.startswith('DWIDTH '):
            width = int(line.split()[1])
        elif line.startswith('BITMAP'):
            rows = []
        elif line == 'ENDCHAR':
            assert cp not in glyphs and 0 <= cp < 65536
            assert width in (8, 16) and len(rows) == 16
            expected = b''.join((r << (16 - width)).to_bytes(2, 'big') for r in rows)
            assert data[cp] == width, hex(cp)
            assert data[65536 + cp * 32:65536 + (cp + 1) * 32] == expected, hex(cp)
            glyphs[cp] = width
            rows = None
        elif rows is not None:
            rows.append(int(line.strip(), 16))
    assert len(glyphs) == 57086 == manifest['glyph_count']
    for cp in range(65536):
        if cp not in glyphs:
            assert data[cp] == 0 and data[65536 + cp * 32:65536 + (cp + 1) * 32] == bytes(32)
    sys.path.insert(0, str(ROOT / 'scripts'))
    from unifont2bin import convert
    regenerated, count = convert(vendor / manifest['compressed_filename'])
    assert count == len(glyphs) and regenerated == data
    print('全57,086字形の幅・全16行一致、未収録8,450位置は空、原本SHA-256・再生成一致。')


if __name__ == '__main__':
    argparse.ArgumentParser(description=__doc__).parse_args()
    check_source()
