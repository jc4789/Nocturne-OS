"""Regenerate the checked-in Intl bundle from the vendored, pinned sources.

Host tool only: esbuild 0.28.2 is required. No package manager or network access
is used by this script or by the Nocturne build/runtime. The vendored upstream
files are checked against manifest.json and copied to build/nocturne-intl,
where Nocturne's patches (third_party/formatjs/patches, in name order) are
applied; the vendored files themselves stay as published. --check compares the
result with the checked-in bundle instead of writing it.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--esbuild', default='esbuild')
parser.add_argument('--check', action='store_true')
args = parser.parse_args()
vendor = root / 'third_party/formatjs'
manifest = json.loads((vendor / 'manifest.json').read_text(encoding='utf-8'))
version = subprocess.check_output([args.esbuild, '--version'], text=True, encoding='utf-8').strip()
if version != manifest['esbuild']:
    raise SystemExit('Expected esbuild ' + manifest['esbuild'] + ', found ' + version)
stage = root / 'build/nocturne-intl'
shutil.rmtree(stage, ignore_errors=True)
for name, expected in manifest['upstreamFilesSha256'].items():
    source = vendor / name
    if hashlib.sha256(source.read_bytes()).hexdigest() != expected:
        raise SystemExit('Vendored upstream file differs from manifest.json: ' + name)
    (stage / name).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, stage / name)
for name in ('entry.js', 'lazy-tz.js'):
    shutil.copyfile(vendor / name, stage / name)
for patch in sorted((vendor / 'patches').glob('*.patch')):
    subprocess.run(['git', 'apply', '--directory=' + stage.relative_to(root).as_posix(), str(patch)],
                   cwd=root, check=True)
bundle = stage / 'js_intl.js'
env = dict(os.environ, NODE_PATH=str(stage / 'source'))
subprocess.run([args.esbuild, str(stage / 'entry.js'), '--bundle', '--minify',
                '--target=es2022', '--platform=neutral', '--format=iife',
                '--legal-comments=inline', '--outfile=' + str(bundle)],
               cwd=root, env=env, check=True)
banner = '''/* Generated Intl implementation: FormatJS (MIT), CLDR (Unicode), IANA tzdb.
 * Pinned sources, licenses, Nocturne's lazy timezone adapter and patches:
 * third_party/formatjs/. Regenerate with scripts/build_intl.py.
 * Locales: en, ja. See README there for implementation limits.
 */
'''
data = (banner + bundle.read_text(encoding='utf-8')).encode('utf-8')
output = root / 'user/libc/web/js_intl.js'
if args.check:
    if output.read_bytes() != data:
        raise SystemExit(str(output.relative_to(root)) + ' differs from the regenerated bundle')
else:
    output.write_bytes(data)
print(output.relative_to(root), len(data), 'bytes SHA-256', hashlib.sha256(data).hexdigest())
