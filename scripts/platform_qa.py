"""Nocturneの隔離実機経路検証。既存dataとHyper-V VMは接続しない。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / 'tools/msys64/ucrt64/bin'
USR = ROOT / 'tools/msys64/usr/bin'
ENV = dict(os.environ, PATH=str(BIN)+os.pathsep+str(USR)+os.pathsep+os.environ.get('PATH', ''),
           MSYSTEM='UCRT64', CHERE_INVOKING='1', MTOOLS_SKIP_CHECK='1')


def run(*args):
    subprocess.run([str(x) for x in args], cwd=ROOT, env=ENV, check=True)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Monitor:
    def __init__(self, process, port):
        for _ in range(100):
            if process.poll() is not None:
                raise RuntimeError('QEMUがmonitor開始前に終了しました')
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=.2)
                break
            except OSError:
                time.sleep(.1)
        else:
            raise RuntimeError('隔離QEMUのmonitorを開けません')
        self.s.settimeout(5)
        self.read()

    def read(self):
        data = b''
        while not data.endswith(b'(qemu) '):
            chunk = self.s.recv(65536)
            if not chunk:
                break
            data += chunk
        return data

    def cmd(self, command):
        self.s.sendall(command.encode('ascii') + b'\n')
        return self.read().decode('utf-8', 'replace')


class SerialTail:
    """Consume each complete line once; pointer feedback must not scan MB/s."""
    def __init__(self, path):
        self.path, self.offset, self.partial = path, 0, b''

    def lines(self):
        try:
            with self.path.open('rb') as stream:
                stream.seek(self.offset)
                data = stream.read()
                self.offset = stream.tell()
        except FileNotFoundError:
            return []
        lines = (self.partial+data).split(b'\n')
        self.partial = lines.pop()
        return [line.decode('utf-8', 'replace') for line in lines]


def move_native_pointer(mon, serial, marker, x, y, deadline):
    """Real PS/2 feedback, never DOM injection or optimistic delta counting.

    Native dequeue acknowledgements also work while author hover callbacks
    are deferred. Wait within this QA run's lifetime, not a two-second page
    assumption. A packet is sent only after the preceding real position.
    """
    pattern = re.compile(re.escape(marker) + r' ([0-9]+) (-?[0-9]+) (-?[0-9]+)')
    tail, position = SerialTail(serial), None

    def latest():
        nonlocal position
        for line in tail.lines():
            found = pattern.search(line)
            if found:
                position = tuple(map(int, found.groups()))
        return position

    def after(sequence):
        while time.monotonic() < deadline:
            actual = latest()
            if actual and actual[0] > sequence:
                return actual
            time.sleep(.01)
        raise RuntimeError('native pointer受信が実観察期間内に到達しません')

    actual = latest()
    if actual is None:
        # The compositor starts at screen centre. Obtain a real client event
        # rather than sweeping through the page header from a fictitious 0,0.
        mon.cmd('mouse_move 0 1')
        actual = after(-1)
    start = list(actual)
    steps = 0
    while time.monotonic() < deadline:
        sequence, px, py = actual
        dx, dy = x-px, y-py
        if abs(dx) <= 1 and abs(dy) <= 1:
            return {'start': start, 'arrived': list(actual), 'steps': steps}
        # No acceleration or PS/2 signed-byte overflow. Acknowledging every
        # packet avoids guest input-queue loss/coalescing accumulating drift.
        dx, dy = max(-5, min(5, dx)), max(-5, min(5, dy))
        mon.cmd(f'mouse_move {dx} {dy}')
        steps += 1
        actual = after(sequence)
    raise RuntimeError('native pointerが実観察期間内に目標へ到達しません')


def confirm_native_target(serial, marker, ident, x, y, deadline):
    """Observer confirms live geometry/hit only after trusted hover delivery."""
    pattern = re.compile(re.escape(marker) + r' ([0-9]+) ([0-9]+) ([0-9]+)')
    tail = SerialTail(serial)
    while time.monotonic() < deadline:
        for line in tail.lines():
            found = pattern.search(line)
            if found and tuple(found.groups()) == (ident, str(x), str(y)):
                return
        time.sleep(.02)
    raise RuntimeError('native hover後の実target確認がありません。誤クリックを拒否しました')


def png_from_ppm(ppm, png):
    data, i, tokens = ppm.read_bytes(), 0, []
    while len(tokens) < 4:
        while i < len(data) and data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b'#':
            end = data.find(b'\n', i)
            if end < 0:
                raise ValueError('PPMコメントが途切れています')
            i = end + 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace():
            j += 1
        if j == i:
            raise ValueError('PPM headerが途切れています')
        tokens.append(data[i:j])
        i = j
    if tokens[0] != b'P6' or tokens[3] != b'255':
        raise ValueError('未対応のPPM形式です')
    i += 1
    w, h = map(int, tokens[1:3])
    if not 0 < w <= 0x7fffffff or not 0 < h <= 0x7fffffff or len(data)-i != w*h*3:
        raise ValueError('PPM画素数が一致しません')
    pixels = data[i:]
    scan = b''.join(b'\0'+pixels[y*w*3:(y+1)*w*3] for y in range(h))

    def chunk(kind, contents):
        return (struct.pack('>I', len(contents)) + kind + contents +
                struct.pack('>I', zlib.crc32(kind+contents) & 0xffffffff))

    png.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(scan, 6)) + chunk(b'IEND', b''))
    return [w, h]


def main():
    # Serial is UTF-8; never pass Japanese/Chinese evidence through CP932.
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--label', required=True)
    ap.add_argument('--url')
    ap.add_argument('--command', action='append', default=[])
    ap.add_argument('--guest-file', action='append', default=[], help='明示したconsole入力を隔離dataの/testsへコピー（サイトfixture用ではない）')
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--resolution', help='隔離bootの通常entryだけに渡す幅x高さx32。元boot設定は変更しない')
    ap.add_argument('--seconds', type=float, default=60)
    ap.add_argument('--ready-marker', help='この実guest serial文字列の到達後から観察時間と入力時刻を計る')
    ap.add_argument('--startup-seconds', type=float, default=600, help='ready-marker待ちの起動期限。サイトの容量制限ではない')
    ap.add_argument('--screen-interval', type=float, default=30, help='画面観察間隔（秒、最小1秒）')
    ap.add_argument('--defer-screen-png', action='store_true', help='再生中のhost圧縮負荷を避け、VM停止後に画面をPNG化')
    ap.add_argument('--memory', type=int, default=2048)
    ap.add_argument('--cpus', type=int, default=4)
    ap.add_argument('--cpu-model', help='この専用QEMUだけに渡すCPUモデルと機能指定')
    ap.add_argument('--accel', choices=['tcg', 'whpx'], default='tcg')
    ap.add_argument('--vga', choices=['std', 'vmware', 'virtio'], default='std')
    ap.add_argument('--gl', action='store_true')
    ap.add_argument('--trace-gpu', action='store_true')
    ap.add_argument('--virgl-disable-mt', action='store_true', help='このQEMU子プロセスだけでvirglのthread同期を無効化して比較')
    ap.add_argument('--audio', action='store_true')
    ap.add_argument('--qemu', default='C:/Program Files/qemu/qemu-system-x86_64.exe')
    ap.add_argument('--qemu-data-dir', help='専用QEMUコピーが読み込むBIOSデータのディレクトリ')
    ap.add_argument('--key', action='append', default=[])
    ap.add_argument('--timed-key', action='append', default=[], help='秒:QEMUキー。実ページの準備後にconsole等を操作する')
    ap.add_argument('--mouse-marker', help='serialの「接頭辞 識別番号 x y」に応じて隔離PS/2で実クリック')
    ap.add_argument('--mouse-move-marker', help='同形式のmarkerで実PS/2移動のみを行い、hoverを観察する')
    ap.add_argument('--mouse-keys', help='markerクリック後の実PS/2キー列（カンマ区切り、最大128キー）')
    ap.add_argument('--mouse-origin', default='0:0', help='marker座標に足す画面内原点 x:y')
    ap.add_argument('--mouse-position-marker', help='native mousemove の「接頭辞 x y」で加速・coalescingのずれを補正')
    ap.add_argument('--native-pointer-marker', help='browser native受信の「接頭辞 seq x y」。DOM callback待ちを位置ackにしない')
    ap.add_argument('--mouse-confirm-marker', help='実trusted hover後の読取observerが「接頭辞 id x y」で最終targetを確認')
    ap.add_argument('--scroll-pages', type=int, default=0, help='実ページをクリックしてから結果一覧を順に下へ送り画面を保存')
    ap.add_argument('--scroll-interval', type=int, default=6, help='実ページ送りの間隔、1～60秒。クリック対象の確認期限は変更しない')
    ap.add_argument('--no-focus-click', action='store_true', help='既にactiveなbrowserへPgDnだけ送る。ページ中央のselect等を誤操作しない')
    ap.add_argument('--expect', action='append', default=[], help='serialに必要な文字列（複数可）')
    ap.add_argument('--reject', action='append', default=[], help='serialに出てはいけない文字列（複数可）')
    ap.add_argument('--allow-panic', action='store_true', help='期待panicの負例検証だけで使用')
    ap.add_argument('--stop-when-expected', action='store_true')
    a = ap.parse_args()
    if a.url and any(re.match(r'\s*(?:/data/bin/)?browser(?:\s|$)', command) for command in a.command):
        ap.error('--urlとbrowser起動commandは併用できません。操作console付き起動は--commandだけ指定してください')
    if not re.fullmatch('[a-zA-Z0-9_-]+', a.label) or not 1 <= a.cpus <= 16 or not 256 <= a.memory <= 16384:
        ap.error('不正なラベル、CPU数またはRAMです')
    if not 1 <= a.seconds <= 3600 or any(x in a.cmdline for x in '\r\n\0'):
        ap.error('不正な実行時間またはboot引数です')
    if a.resolution:
        dimensions = re.fullmatch(r'([1-9][0-9]*)x([1-9][0-9]*)x32', a.resolution)
        if not dimensions or any(int(x) > 0x7fffffff for x in dimensions.groups()):
            ap.error('解像度は正の32bit寸法による 幅x高さx32 です')
    if not 1 <= a.startup_seconds <= 3600 or (a.ready_marker and any(x in a.ready_marker for x in '\r\n\0')):
        ap.error('不正な起動期限またはready-markerです')
    if not 1 <= a.screen_interval <= 3600:
        ap.error('画面間隔は1から3600秒です')
    if any(not re.fullmatch('[a-zA-Z0-9_-]+', x) for x in a.key):
        ap.error('不正なQEMUキー名です')
    timed_keys = []
    for item in a.timed_key:
        match = re.fullmatch(r'([0-9]+):([a-zA-Z0-9_-]+)', item)
        if not match or not 0 <= int(match[1]) < a.seconds:
            ap.error('時刻付きキーは実行期間内の秒:キー名です')
        timed_keys.append((int(match[1]), match[2]))
    timed_keys.sort()
    if any(marker and not re.fullmatch('[a-zA-Z0-9_]+', marker)
           for marker in (a.mouse_marker,a.mouse_move_marker)):
        ap.error('不正なマウスmarkerです')
    mouse_keys = a.mouse_keys.split(',') if a.mouse_keys else []
    if mouse_keys and (not a.mouse_marker or len(mouse_keys)>128 or
                      any(not re.fullmatch('[a-zA-Z0-9_-]+',x) for x in mouse_keys)):
        ap.error('mouse-keysはmarkerと有効な128個以内のキー名が必要です')
    if a.mouse_position_marker and not re.fullmatch('[a-zA-Z0-9_]+', a.mouse_position_marker):
        ap.error('不正なマウス位置markerです')
    if any(marker and not re.fullmatch('[a-zA-Z0-9_]+', marker)
           for marker in (a.native_pointer_marker, a.mouse_confirm_marker)):
        ap.error('不正なnative pointer/target確認markerです')
    if a.mouse_confirm_marker and not a.native_pointer_marker:
        ap.error('最終target確認にはnative pointer受信が必要です')
    origin = re.fullmatch(r'([0-9]{1,5}):([0-9]{1,5})', a.mouse_origin)
    if not origin:
        ap.error('マウス原点は x:y です')
    mouse_origin = tuple(map(int, origin.groups()))
    mouse_seen, mouse_actions = set(), []
    if not 0 <= a.scroll_pages <= 40:
        ap.error('スクロール回数は0から40です')
    if not 1 <= a.scroll_interval <= 60:
        ap.error('ページ送りの間隔は1から60秒です')
    if a.cpu_model is not None and not re.fullmatch('[a-zA-Z0-9_.,=+_-]+', a.cpu_model):
        ap.error('不正なQEMU CPUモデルです')
    if a.url and any(x in a.url for x in '\n\r\0 |&;`$'):
        ap.error('この検証URLにshell構文は使用できません')
    if a.stop_when_expected and not a.expect:
        ap.error('--stop-when-expectedには--expectが必要です')
    if a.virgl_disable_mt and not a.gl:
        ap.error('--virgl-disable-mtには--glが必要です')
    here = ROOT / 'build/nocturne-platform' / a.label
    here.mkdir(parents=True, exist_ok=False)
    boot = here / 'boot.img'
    shutil.copyfile(ROOT / 'build/nocturne.img', boot)
    boot_part = boot.relative_to(ROOT).as_posix()+'@@1048576'
    hashes = {}
    for name in ('kernel.elf', 'initrd.tar'):
        extracted = here / ('extracted-'+name)
        run(BIN / 'mcopy.exe', '-i', boot_part, '::/boot/'+name, extracted.relative_to(ROOT).as_posix())
        hashes[name] = digest(extracted)
        if hashes[name] != digest(ROOT / 'build' / name):
            raise RuntimeError('起動媒体と最新buildが不一致: '+name)
        extracted.unlink()
    if a.cmdline or a.resolution:
        config = (ROOT / 'boot/limine.conf').read_text(encoding='utf-8')
        needle = '    path: boot():/boot/kernel.elf'
        first_entry = config.split('\n/')[1].split('\n/')[0] if '\n/' in config else ''
        if needle not in first_entry or 'cmdline:' in first_entry:
            raise RuntimeError('予期しないLimine構成です')
        updated = first_entry
        if a.cmdline:
            updated = updated.replace(needle, needle+'\n    cmdline: '+a.cmdline, 1)
        if a.resolution:
            updated, replacements = re.subn(r'(?m)^    resolution: [^\n]+$', '    resolution: '+a.resolution, updated)
            if replacements != 1:
                raise RuntimeError('通常entryの解像度指定が一意でありません')
        conf = here / 'limine.conf'
        conf.write_text(config.replace(first_entry, updated, 1), encoding='utf-8', newline='\n')
        run(BIN / 'mcopy.exe', '-o', '-i', boot_part, conf.relative_to(ROOT).as_posix(), '::/boot/limine/limine.conf')
    disk = here / 'data.img'
    run(USR / 'bash.exe', 'scripts/mkdata.sh', disk.relative_to(ROOT).as_posix(), '128')
    part = disk.relative_to(ROOT).as_posix()+'@@1048576'
    run(BIN / 'mcopy.exe', '-i', part, '-s', 'tests', '::/')
    guest_hashes = {}
    for source in a.guest_file:
        local = Path(source).resolve(strict=True)
        if not local.is_file() or local.stat().st_size > 16384 or not re.fullmatch('[a-zA-Z0-9_.-]+',local.name):
            ap.error('console入力は16KiB以内の通常ファイルです')
        run(BIN / 'mcopy.exe','-o','-i',part,local,'::/tests/'+local.name)
        guest_hashes[local.name] = digest(local)
    commands = ['free', 'ps', *a.command]
    if a.url:
        commands += ['browser --debug-js '+a.url]
    script = here / 'autorun.sh'
    script.write_text('\n'.join(commands)+'\n', encoding='utf-8', newline='\n')
    run(BIN / 'mcopy.exe', '-i', part, script.relative_to(ROOT).as_posix(), '::/tests/autorun.sh')
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
    serial = here / 'serial.log'
    args = [a.qemu, '-M', 'pc', '-m', str(a.memory)+'M', '-smp', str(a.cpus), '-accel', a.accel,
            '-display', 'egl-headless' if a.gl else 'none', '-vga', a.vga, '-no-reboot',
            '-serial', 'file:'+str(serial), '-monitor', f'tcp:127.0.0.1:{port},server,nowait',
            '-drive', f'file={boot},format=raw,if=ide,index=0,snapshot=on',
            '-drive', f'file={disk},format=raw,if=ide,index=1,snapshot=on',
            '-netdev', 'user,id=qa', '-device', 'e1000,netdev=qa']
    if a.gl:
        args += ['-device', 'virtio-gpu-gl-pci']
    if a.cpu_model:
        args += ['-cpu', a.cpu_model]
    if a.qemu_data_dir:
        args += ['-L', a.qemu_data_dir]
    if a.trace_gpu:
        args += ['-trace', f'enable=virtio_gpu*,file={here / "gpu-trace.log"}', '-d', 'guest_errors']
    if a.audio:
        args += ['-audiodev', f'wav,id=snd,path={here / "audio.wav"}', '-device', 'AC97,audiodev=snd']
    metadata = {'arguments': args, 'boot_payload_sha256': hashes, 'user_data_attached': False,
                'boot_snapshot': True, 'scratch_snapshot': True, 'requested_seconds': a.seconds,
                'requested_resolution': a.resolution,
                'requested_scroll_interval': a.scroll_interval,
                'expected_serial': a.expect, 'rejected_serial': a.reject,
                'guest_console_sha256': guest_hashes, 'guest_autorun_sha256': digest(script)}
    err = (here / 'qemu-stderr.log').open('wb')
    child_env = dict(ENV)
    if a.virgl_disable_mt:
        child_env['VIRGL_DISABLE_MT'] = '1'
    metadata['virgl_disable_mt'] = a.virgl_disable_mt
    p = subprocess.Popen(args, cwd=ROOT, env=child_env, stderr=err, stdout=err,
                         creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    metadata['pid'] = p.pid
    start = time.monotonic()
    ready_at = None if a.ready_marker else start
    deadline = start + (a.startup_seconds if a.ready_marker else a.seconds)
    metadata['ready_marker'] = a.ready_marker
    metadata['startup_seconds'] = a.startup_seconds
    captures = []
    metadata_path = here / 'metadata.json'
    metadata_path.write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding='utf-8')

    def shot(mon, name):
        ppm, png = here / (name+'.ppm'), here / (name+'.png')
        mon.cmd('screendump '+ppm.relative_to(ROOT).as_posix())
        captures.append({'name':name, 'elapsed':round(time.monotonic()-start, 3)})
        if not a.defer_screen_png:
            captures[-1]['resolution'] = png_from_ppm(ppm, png)

    failure = None
    mon = None
    try:
        mon = Monitor(p, port)
        print('隔離QEMU 起動:', p.pid, here, flush=True)
        next_shot, index, scroll_at, scroll_count = 15, 0, 45, 0
        startup_shot, startup_index = 15, 0
        while time.monotonic() < deadline and p.poll() is None:
            if ready_at is None:
                if serial.exists() and a.ready_marker in serial.read_text(encoding='utf-8', errors='replace'):
                    ready_at = time.monotonic()
                    deadline = ready_at + a.seconds
                    metadata['ready_elapsed_seconds'] = round(ready_at-start, 3)
                    metadata_path.write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding='utf-8')
                    print('実guest観察開始:', round(ready_at-start, 3), flush=True)
                else:
                    # Loading or a failed observation hook must stay visible;
                    # author readiness controls input timing, never capture.
                    if time.monotonic()-start >= startup_shot:
                        shot(mon, 'startup-'+str(startup_index))
                        startup_index += 1
                        startup_shot += a.screen_interval
                    time.sleep(.2)
                    continue
            elapsed = time.monotonic()-ready_at
            while timed_keys and elapsed >= timed_keys[0][0]:
                _, key = timed_keys.pop(0)
                mon.cmd('sendkey '+key)
            if (a.mouse_marker or a.mouse_move_marker) and serial.exists():
                log = serial.read_text(encoding='utf-8', errors='replace')
                matches=[]
                for kind,marker in (('click',a.mouse_marker),('move',a.mouse_move_marker)):
                    if marker:
                        for match in re.finditer(re.escape(marker)+r' ([0-9]+) ([0-9]+) ([0-9]+)',log):
                            matches.append((match.start(),kind,*match.groups()))
                for _,kind,ident,x,y in sorted(matches):
                    action_id=kind+':'+ident
                    if action_id in mouse_seen:
                        continue
                    mouse_seen.add(action_id)
                    sx, sy = int(x)+mouse_origin[0], int(y)+mouse_origin[1]
                    dimensions = metadata.get('screen_captures', captures)
                    bounds = next((i['resolution'] for i in reversed(dimensions) if 'resolution' in i), None)
                    if bounds is None and a.resolution:
                        bounds = list(map(int, a.resolution.split('x')[:2]))
                    if bounds is None:
                        raise ValueError('実pointerには観測した画面寸法または--resolutionが必要です')
                    if not 0 <= sx < bounds[0] or not 0 <= sy < bounds[1]:
                        raise ValueError('実マウスの画面座標が範囲外です')
                    action={'id':ident,'kind':kind,'screen':[sx,sy], 'marker':[int(x),int(y)],
                            'keys':mouse_keys if kind=='click' else [], 'stage':'requested'}
                    mouse_actions.append(action)
                    (here/'mouse-actions.json').write_text(json.dumps(mouse_actions, indent=2), encoding='utf-8')
                    if a.native_pointer_marker:
                        # A stale author marker must not monopolize the run:
                        # keep captures and keyboard scrolling observable even
                        # when layout moved the target. Never click without ACK.
                        try:
                            action['native_pointer']=move_native_pointer(mon, serial, a.native_pointer_marker,
                                                                         int(x), int(y), min(deadline, time.monotonic()+12))
                        except RuntimeError as input_error:
                            action.update(stage='refused', error=str(input_error))
                            (here/'mouse-actions.json').write_text(json.dumps(mouse_actions, indent=2), encoding='utf-8')
                            shot(mon, 'mouse-refused-'+ident)
                            continue
                        action['stage']='arrived'
                        (here/'mouse-actions.json').write_text(json.dumps(mouse_actions, indent=2), encoding='utf-8')
                    # Clamp at the corner, then use <=5 deltas to avoid the
                    # compositor's relative-pointer acceleration. One monitor
                    # owns all input and screenshots; no second HMP connection.
                    else:
                        for _ in range((max(bounds)+199)//200):
                            mon.cmd('mouse_move -100 -100')
                            time.sleep(.02)
                        xx = yy = 0
                        while xx < sx or yy < sy:
                            dx, dy = min(5, sx-xx), min(5, sy-yy)
                            mon.cmd(f'mouse_move {dx} {dy}')
                            xx += dx; yy += dy
                            time.sleep(.02)
                        time.sleep(.3)
                    if a.mouse_position_marker and not a.native_pointer_marker:
                        pattern = re.escape(a.mouse_position_marker)+r' (-?[0-9]+) (-?[0-9]+)'
                        for correction in range(40):
                            positions = re.findall(pattern, serial.read_text(encoding='utf-8', errors='replace'))
                            if not positions:
                                raise RuntimeError('native mousemove の座標を受信できません')
                            px, py = map(int, positions[-1])
                            dx, dy = int(x)-px, int(y)-py
                            if abs(dx) <= 1 and abs(dy) <= 1:
                                break
                            dx, dy = max(-5, min(5, dx)), max(-5, min(5, dy))
                            mon.cmd(f'mouse_move {dx} {dy}')
                            end = min(deadline, time.monotonic()+2)
                            while time.monotonic() < end:
                                time.sleep(.05)
                                after = re.findall(pattern, serial.read_text(encoding='utf-8', errors='replace'))
                                if len(after) > len(positions):
                                    break
                            else:
                                raise RuntimeError('native mousemove の応答がありません')
                        else:
                            raise RuntimeError('native mousemove の有限補正が一致しません')
                    if kind=='click' and a.mouse_confirm_marker:
                        try:
                            confirm_native_target(serial, a.mouse_confirm_marker, ident, int(x), int(y),
                                                  min(deadline, time.monotonic()+12))
                        except RuntimeError as input_error:
                            action.update(stage='refused', error=str(input_error))
                            (here/'mouse-actions.json').write_text(json.dumps(mouse_actions, indent=2), encoding='utf-8')
                            shot(mon, 'mouse-refused-'+ident)
                            continue
                        action['stage']='confirmed'
                    shot(mon, 'mouse-'+kind+'-'+ident)
                    if kind=='click':
                        mon.cmd('mouse_button 1')
                        time.sleep(.1)
                        mon.cmd('mouse_button 0')
                        for key in mouse_keys:
                            mon.cmd('sendkey '+key)
                            time.sleep(.2)
                    action['stage']='sent'
                    (here/'mouse-actions.json').write_text(json.dumps(mouse_actions, indent=2), encoding='utf-8')
                    print('隔離PS/2'+('クリック' if kind=='click' else '移動')+':', ident, sx, sy, flush=True)
            if elapsed >= next_shot:
                shot(mon, 'screen-'+str(index))
                index += 1
                next_shot += a.screen_interval
                if index == 1:
                    if (a.key or a.scroll_pages) and not a.no_focus_click:
                        mon.cmd('mouse_button 1')
                        mon.cmd('mouse_button 0')
                    for key in a.key:
                        mon.cmd('sendkey '+key)
            if a.scroll_pages and elapsed >= scroll_at and scroll_count < a.scroll_pages:
                if scroll_count == 0 and not a.no_focus_click:
                    # The first 15s frame may still be Limine loading the
                    # larger rootfs. Focus the actual page at scrolling time.
                    mon.cmd('mouse_button 1')
                    mon.cmd('mouse_button 0')
                shot(mon, 'page-'+str(scroll_count))
                mon.cmd('sendkey pgdn')
                scroll_count += 1
                scroll_at += a.scroll_interval
            if a.stop_when_expected and serial.exists():
                log = serial.read_text(encoding='utf-8', errors='replace')
                if all(x in log for x in a.expect):
                    break
            time.sleep(.2)
        if p.poll() is None:
            shot(mon, 'screen-final')
            (here / 'registers.txt').write_text(mon.cmd('info registers -a'), encoding='utf-8')
            try:
                mon.cmd('quit')
            except (ConnectionResetError, BrokenPipeError):
                pass
    except Exception as e:
        failure = str(e)
        if mon is not None and p.poll() is None:
            try:
                shot(mon, 'screen-error')
            except Exception as capture_error:
                metadata['error_capture_error'] = str(capture_error)
        # autorun may power off immediately after its final marker, racing a
        # screendump/register/quit command. Accept only a confirmed clean
        # guest shutdown with every requested marker; other monitor errors
        # remain errors. Keep the observation visible in metadata.
        if isinstance(e, (ConnectionResetError, BrokenPipeError)):
            try:
                code = p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                code = None
            log = serial.read_text(encoding='utf-8', errors='replace') if serial.exists() else ''
            if code == 0 and a.expect and all(x in log for x in a.expect) and 'power: shutting down' in log:
                metadata['clean_shutdown_monitor_race'] = str(e)
                failure = None
    finally:
        if p.poll() is None:
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.terminate()
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        if mon is not None:
            mon.s.close()
        err.close()
        if a.defer_screen_png:
            for capture in captures:
                ppm = here / (capture['name']+'.ppm')
                capture['resolution'] = png_from_ppm(ppm, ppm.with_suffix('.png'))
        metadata['screen_captures'] = captures
        if a.resolution:
            expected_dimensions = list(map(int, a.resolution.split('x')[:2]))
            # Limine uses its own boot video mode before the guest switches to
            # the requested desktop mode. Keep those early pictures as evidence,
            # but check the settled guest screen rather than the boot loader.
            metadata['resolution_matched'] = bool(captures) and captures[-1].get('resolution') == expected_dimensions
            if not metadata['resolution_matched']:
                failure = (failure+'; ' if failure else '')+'要求解像度と実QEMU画面寸法が不一致です'
        log = serial.read_text(encoding='utf-8', errors='replace') if serial.exists() else ''
        missing = [x for x in a.expect if x not in log]
        if a.ready_marker and ready_at is None:
            missing.append(a.ready_marker)
        rejected = [x for x in a.reject if x in log]
        refused = [x for x in mouse_actions if x.get('stage') == 'refused']
        if refused:
            failure = (failure+'; ' if failure else '')+'実pointer操作を拒否しました（座標またはhover未確認）'
        ok = failure is None and p.returncode == 0 and not missing and not rejected and (a.allow_panic or 'KERNEL PANIC' not in log)
        metadata.update(exit_code=p.returncode, elapsed_seconds=round(time.monotonic()-start, 2),
                         process_stopped=True, harness_accepted=ok, missing_serial=missing, rejected_serial_found=rejected, harness_error=failure,
                         refused_pointer_actions=refused,
                        site_acceptance='画面と操作結果の別確認が必要')
        metadata_path.write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding='utf-8')
        print('隔離QEMU 終了:', p.returncode, '検査:', '成功' if ok else '失敗', flush=True)
        print(log[-6500:])
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
