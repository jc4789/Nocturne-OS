"""設定の宣言の表と、設定の文書を拒む境界のホストの検査。

  1. user/apps/settings_table.h の宣言：領域と鍵の組の重複、名前の重複、名前の字、既定の型と範囲。
  2. 宣言の無い鍵：カーネルの領域の鍵（common/settings_abi.h の enum）は全部表にある。
  3. user/・rootfs/ のシステムの設定の文書：`.conf` の文字列と置いたファイル、`AppData\\Nocturne` を拒む。

  python -X utf8 tests/check_settings.py
"""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parent.parent
problems = []

table = (root / 'user/apps/settings_table.h').read_text(encoding='utf-8')
abi = (root / 'common/settings_abi.h').read_text(encoding='utf-8')
body = table[table.index('settings_table[] = {'):table.index('};', table.index('settings_table[] = {'))]

# BOOL_SETTING(domain, key, scope, apply, name, label, about, fallback)
# INT_SETTING(domain, key, scope, apply, name, label, about, min, max, fallback)
# {.domain = ..., .key = ..., .type = ..., ...}
TEXT = r'"(?:[^"\\]|\\.)*"'
BOOL = re.compile(rf'BOOL_SETTING\(\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*({TEXT}),\s*{TEXT},\s*{TEXT},\s*(-?\w+)\s*\)')
INT = re.compile(rf'INT_SETTING\(\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*({TEXT}),\s*{TEXT},\s*{TEXT},\s*(-?\w+),\s*(-?\w+),\s*(-?\w+)\s*\)')
FULL = re.compile(r'\{\s*\.domain\s*=\s*(\w+),\s*\.key\s*=\s*(\w+),\s*\.type\s*=\s*(\w+),(.*?)\n?\s*\},?', re.S)

CONSTANTS = {'INT32_MAX': 2 ** 31 - 1, 'INT32_MIN': -2 ** 31}


def number(text):
    return CONSTANTS[text] if text in CONSTANTS else int(text, 0)


entries = []
for m in BOOL.finditer(body):
    domain, key, scope, apply, name, fallback = m.groups()
    entries.append((domain, key, name.strip('"')))
    if fallback not in ('0', '1'):
        problems.append(f'{key}：BOOL の既定 {fallback} は 0 か 1 でない')
for m in INT.finditer(body):
    domain, key, scope, apply, name, low, high, fallback = m.groups()
    entries.append((domain, key, name.strip('"')))
    if not number(low) <= number(fallback) <= number(high):
        problems.append(f'{key}：INT の既定 {fallback} が範囲 {low}..{high} の外')
for m in FULL.finditer(body):
    domain, key, kind, rest = m.groups()
    name = re.search(rf'\.name\s*=\s*({TEXT})', rest)
    if not name:
        problems.append(f'{key}：名前が無い')
        continue
    entries.append((domain, key, name.group(1).strip('"')))
    if kind == 'N_SETTING_ENUM':
        options = re.search(rf'\.options\s*=\s*({TEXT})', rest)
        high = re.search(r'\.max\s*=\s*(\d+)', rest)
        if not options or not high:
            problems.append(f'{key}：ENUM に選択肢か上限が無い')
        elif options.group(1).count('\\0') + 1 != int(high.group(1)) + 1:
            problems.append(f'{key}：ENUM の選択肢の数が上限と合わない')

if body.count('_SETTING(') + len(FULL.findall(body)) != len(entries):
    problems.append('表の行に読めないものがある：検査の正規表現を表の形に揃える')

seen_keys, seen_names = {}, {}
for domain, key, name in entries:
    if (domain, key) in seen_keys:
        problems.append(f'{domain}・{key} の重複')
    seen_keys[(domain, key)] = name
    if name in seen_names:
        problems.append(f'名前 {name} の重複')
    seen_names[name] = key
    if not re.fullmatch(r'[a-z0-9.-]+', name) or len(name) >= 48:
        problems.append(f'名前 {name}：ASCII の小文字、数字、「.」、「-」で 47 バイトまで')

# カーネルの領域の鍵は全部宣言される。
DOMAINS = {'n_boot_key': 'N_DOMAIN_BOOT', 'n_audio_key': 'N_DOMAIN_AUDIO', 'n_display_key': 'N_DOMAIN_DISPLAY',
           'n_window_key': 'N_DOMAIN_WINDOW', 'n_input_key': 'N_DOMAIN_INPUT', 'n_power_key': 'N_DOMAIN_POWER',
           'n_network_key': 'N_DOMAIN_NETWORK'}
for enum, domain in DOMAINS.items():
    block = re.search(rf'enum {enum} \{{(.*?)\}};', abi, re.S)
    if not block:
        problems.append(f'settings_abi.h に enum {enum} が無い')
        continue
    for key in re.findall(r'^\s*(N_\w+),', block.group(1), re.M):
        if not key.endswith('_KEY_COUNT') and (domain, key) not in seen_keys:
            problems.append(f'{key} の宣言が表に無い')

# システムの設定の文書。limine.conf は起動の道具の書式で、C: の設定ではない。
CONF = re.compile(r'"[^"\n]*\.conf"|AppData\\+Nocturne')
for folder in ('user', 'rootfs'):
    for path in (root / folder).rglob('*'):
        if not path.is_file():
            continue
        where = path.relative_to(root).as_posix()
        if path.suffix == '.conf':
            problems.append(f'{where}：置いた設定の文書')
        if path.suffix not in ('.c', '.h', '.cpp', '.js', '.ts'):
            continue
        for number_, line in enumerate(path.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
            if CONF.search(line):
                problems.append(f'{where}:{number_}：設定の文書 {line.strip()[:80]}')

for problem in problems:
    print('失敗 ' + problem)
print(f'check_settings: 宣言 {len(entries)} 件、失敗 {len(problems)} 件')
sys.exit(1 if problems else 0)
