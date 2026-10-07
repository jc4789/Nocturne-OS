"""Explicit host-only download of the pinned official ICU 78.1 generator.

Never invoked by the Nocturne build. Only DLLs for table generation are unpacked
under the specified host directory; nothing is added to the guest image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BASE = 'https://github.com/unicode-org/icu/releases/download/release-78.1/'
NAME = 'icu4c-78.1-Win64-MSVC2022.zip'


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--host-dir', default=str(ROOT / 'build/collator-icu78-host'))
    args = parser.parse_args()
    target = Path(args.host_dir).resolve()
    target.mkdir(parents=True, exist_ok=True)
    checksum = urllib.request.urlopen(BASE + 'SHASUM512.txt').read()
    expected = next(line.split()[0] for line in checksum.decode('ascii').splitlines() if NAME in line)
    archive = target / NAME
    if not archive.exists():
        with urllib.request.urlopen(BASE + NAME) as source, archive.open('wb') as out:
            while chunk := source.read(1024 * 1024):
                out.write(chunk)
    raw = archive.read_bytes()
    if hashlib.sha512(raw).hexdigest() != expected:
        raise SystemExit('Official ICU archive checksum mismatch')
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            if Path(info.filename).name in ['icuuc78.dll','icuin78.dll','icudt78.dll']:
                (target / Path(info.filename).name).write_bytes(z.read(info))
    vendor = ROOT / 'third_party/unicode_collation'
    vendor.mkdir(parents=True, exist_ok=True)
    license_url = 'https://raw.githubusercontent.com/unicode-org/icu/release-78.1/LICENSE'
    (vendor / 'LICENSE').write_bytes(urllib.request.urlopen(license_url).read())
    manifest = {'archive':BASE+NAME, 'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),
                'sha512':expected,'checksumSource':BASE+'SHASUM512.txt','licenseSource':license_url}
    (vendor / 'upstream.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    print(target, len(raw), 'bytes; verified official SHA-512', flush=True)


if __name__ == '__main__':
    main()
