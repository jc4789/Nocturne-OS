"""Regenerate the checked-in Intl bundle from the vendored, pinned sources.

Host tool only: esbuild 0.28.2 is required. No package manager or network access
is used by this script or by the Nocturne build/runtime.
"""
import argparse
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--esbuild', default='esbuild')
args = parser.parse_args()
version = subprocess.check_output([args.esbuild, '--version'], text=True, encoding='utf-8').strip()
if version != '0.28.2':
    raise SystemExit('Expected esbuild 0.28.2, found ' + version)
vendor = root / 'third_party/formatjs'
output = root / 'user/libc/web/js_intl.js'
env = dict(os.environ, NODE_PATH=str(vendor / 'source'))
subprocess.run([args.esbuild, str(vendor / 'entry.js'), '--bundle', '--minify',
                '--target=es2022', '--platform=neutral', '--format=iife',
                '--legal-comments=inline', '--outfile=' + str(output)],
               cwd=root, env=env, check=True)
banner = '''/* Generated Intl implementation: FormatJS (MIT), CLDR (Unicode), IANA tzdb.
 * Pinned sources, licenses and Nocturne's lazy timezone adapter:
 * third_party/formatjs/. Regenerate with scripts/build_intl.py.
 * Locales: en, ja. See README there for implementation limits.
 */
'''
output.write_text(banner + output.read_text(encoding='utf-8'), encoding='utf-8', newline='\n')
print(output.relative_to(root), output.stat().st_size, 'bytes')
