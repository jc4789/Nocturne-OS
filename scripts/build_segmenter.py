"""Regenerate Intl.Segmenter from pinned, vendored FormatJS sources offline."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--esbuild', default='esbuild')
args = parser.parse_args()
version = subprocess.check_output([args.esbuild, '--version'], text=True,
                                  encoding='utf-8').strip()
if version != '0.28.2':
    raise SystemExit('Expected esbuild 0.28.2, found ' + version)
vendor = root / 'third_party/formatjs-segmenter'
output = root / 'user/libc/web/js_segmenter.js'
manifest = json.loads((vendor / 'manifest.json').read_text(encoding='utf-8'))
for item in manifest['files']:
    source = vendor / item['path']
    if hashlib.sha256(source.read_bytes()).hexdigest() != item['sha256']:
        raise SystemExit('Vendored source hash mismatch: ' + str(source))
paths = [vendor / 'source', root / 'third_party/formatjs/source']
env = dict(os.environ, NODE_PATH=os.pathsep.join(map(str, paths)))
subprocess.run([args.esbuild, str(vendor / 'entry.js'), '--bundle', '--minify',
                '--target=es2022', '--platform=neutral', '--format=iife',
                '--legal-comments=inline', '--outfile=' + str(output)],
               cwd=root, env=env, check=True)
banner = '''/* Generated Intl.Segmenter: FormatJS (MIT), Unicode/CLDR.
 * Pinned sources and licenses: third_party/formatjs-segmenter/.
 * Regenerate offline with scripts/build_segmenter.py (esbuild 0.28.2).
 * CLDR root/tailored boundaries; no dictionary-based segmentation engine.
 */
'''
output.write_text(banner + output.read_text(encoding='utf-8'),
                  encoding='utf-8', newline='\n')
print(output.relative_to(root), output.stat().st_size, 'bytes')
