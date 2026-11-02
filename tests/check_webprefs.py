"""user/libc/webprefs.c（ブラウザーの設定）のホストの試験。

製品の webprefs.c をそのまま組み、nocturne_settings.h の口だけを tests/webprefs_host.c の記憶の中の値の表に
差し替える。範囲を限った裏付けで、サービス settings・ゲストのブラウザーでの受け入れではない。

目録の tag は、同梱の formatjs の CLDR の表（言語は likelySubtags、地域は英語の地域名、別名は
languageAlias・territoryAlias）でも照らす：言語と地域の subtag が CLDR の知る物で、別名（廃止された
tag）でないこと。IANA の登録簿そのものではなく、その代わりの裏付け。
"""
from pathlib import Path
import os
import re
import subprocess
import url_owner

root = Path(__file__).resolve().parent.parent
out = root / 'build/webprefs-host'
out.mkdir(parents=True, exist_ok=True)
problems = []

product = (root / 'user/libc/webprefs.c').read_text(encoding='utf-8')
block = product[product.index('catalog[] = {'):product.index('};', product.index('catalog[] = {'))]
tags = re.findall(r'^\s*\{"([^"]+)", "[^"]*", "[^"]*"\},$', block, re.M)
cldr = (root / 'third_party/formatjs/source/@formatjs/intl-getcanonicallocales/index.js').read_text(encoding='utf-8')


def table(name):
    start = cldr.index(f'const {name} = {{')
    return set(re.findall(r'^\t"([^"]+)":', cldr[start:cldr.index('\n};', start)], re.M))


likely, language_alias, territory_alias = table('likelySubtags'), table('languageAlias'), table('territoryAlias')
# 地域：likelySubtags は推せる地域を省くので、CLDR の英語の地域名の表で照らす。
display = (root / 'third_party/formatjs/source/@formatjs/intl-displaynames/locale-data/en.js').read_text(encoding='utf-8')
start = display.index('"region": {')
regions = set(re.findall(r'^\s+"([0-9A-Z]{2,3})": "', display[start:display.index('"script": {', start)], re.M))
for tag in tags:
    parts = tag.split('-')
    language, region = parts[0], parts[1] if len(parts) > 1 else None
    if language not in likely or language in language_alias:
        problems.append(f'目録の {tag}：言語の subtag {language} が CLDR の likelySubtags に無いか別名')
    if region and (region not in regions or region in territory_alias):
        problems.append(f'目録の {tag}：地域の subtag {region} が CLDR の地域名の表に無いか別名')

cc = url_owner.clang(root)
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get('PATH', ''))
exe = out / 'webprefs_host.exe'
# A URL's host comes from its record: the URL parser's own objects (tests/url_owner.py).
owner = url_owner.objects(root, cc)
sources = ['user/libc/webprefs.c', 'tests/webprefs_host.c']
built = subprocess.run([str(cc), '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter',
                        '-idirafter', 'user/include', '-idirafter', 'common'] + sources + owner + ['-o', str(exe)],
                       cwd=root, env=env)
code = built.returncode or subprocess.run([str(exe)], cwd=root, env=env, timeout=20).returncode
for problem in problems:
    print('失敗 ' + problem)
print(f'目録の tag {len(tags)} 件を CLDR の表で照らした')
raise SystemExit(code or (1 if problems else 0))
