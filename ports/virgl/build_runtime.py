"""固定版 virgl の未修正/一行修理 QEMU を build 内だけに作る。インストールしない。"""
from pathlib import Path
import argparse,hashlib,io,json,os,re,shutil,subprocess,sys,tarfile

ROOT=Path(__file__).resolve().parents[2]
URL='https://chromium.googlesource.com/chromiumos/third_party/virglrenderer'
REVISION='2cb2065b6a0515c5accfa3e44bcb7ce57d2f9983'
PKGCONF_SHA256='74b6fe685d84ec6ea19b9ee285d752d122e31b6b93038f933627b05324d8f2a3'
CONFIG=['--buildtype=release','-Dplatforms=egl','-Ddrm-renderers=[]','-Dtests=false',
        '-Dvideo=false','-Dvenus=false','-Dunstable-apis=true','-Db_lto=false']

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--stage',required=True,help='新規 build/nocturne-virgl/<stage> 名。上書き不可')
    ap.add_argument('--python-tools',required=True,help='手動準備した workspace 内 Meson/Ninja/PyYAML ディレクトリ')
    ap.add_argument('--pkgconf-archive',required=True,help='README で指定した固定 MSYS2 package（展開・実行は build 内のみ）')
    ap.add_argument('--qemu',default='C:/Program Files/qemu/qemu-system-x86_64.exe',help='読み取り専用の元 QEMU')
    a=ap.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}',a.stage):ap.error('stage 名が不正です')
    buildroot=(ROOT/'build').resolve()
    stage=(buildroot/'nocturne-virgl'/a.stage).resolve()
    python_tools=Path(a.python_tools).resolve();archive=Path(a.pkgconf_archive).resolve()
    qemu=Path(a.qemu).resolve();compiler=ROOT/'tools/msys64/ucrt64/bin'
    if not buildroot.is_relative_to(ROOT) or not stage.is_relative_to(buildroot):ap.error('出力が workspace build 外です')
    if not python_tools.is_relative_to(buildroot) or not archive.is_relative_to(buildroot):ap.error('依存準備は workspace build 内に限定します')
    if stage.exists():ap.error('既存 stage は上書きできません')
    for p in [qemu,compiler/'gcc.exe',compiler/'g++.exe',python_tools/'bin/ninja.exe',archive]:
        if not p.is_file():ap.error('必要ファイルがありません: '+str(p))
    if digest(archive)!=PKGCONF_SHA256:ap.error('pkgconf package の固定 hash が一致しません')
    if not (qemu.parent/'share/bios-256k.bin').is_file():ap.error('元 QEMU の読み取り専用 BIOS がありません')
    # 解決済み stage 内の新規 checkout にしかパッチを適用しない。
    stage.mkdir(parents=True,exist_ok=False)
    source=stage/'source';build=stage/'compile';pkgconf=stage/'pkgconf';pkgconf.mkdir()
    from compression import zstd  # Python 3.14。システムへの tool installer は使わない。
    with tarfile.open(fileobj=io.BytesIO(zstd.decompress(archive.read_bytes())),mode='r:') as package:
        for name,dst in [('ucrt64/bin/pkgconf.exe','pkg-config.exe'),
                         ('ucrt64/bin/libpkgconf-8.dll','libpkgconf-8.dll'),
                         ('ucrt64/share/doc/pkgconf/COPYING','COPYING')]:
            member=package.getmember(name)
            if not member.isfile():raise RuntimeError('固定 package 内に通常ファイルがありません: '+name)
            with package.extractfile(member) as f:(pkgconf/dst).write_bytes(f.read())
    env=dict(os.environ,PATH=str(compiler)+os.pathsep+str(pkgconf)+os.pathsep+os.environ.get('PATH',''),
             PYTHONPATH=str(python_tools),PYTHONUTF8='1',CC=str(compiler/'gcc.exe'),CXX=str(compiler/'g++.exe'),
             PKG_CONFIG=str(pkgconf/'pkg-config.exe'),PKG_CONFIG_PATH=str(compiler.parent/'lib/pkgconfig'),
             NINJA=str(python_tools/'bin/ninja.exe'))
    def run(name,args):
        r=subprocess.run([str(x) for x in args],cwd=ROOT,env=env,capture_output=True,text=True,encoding='utf-8',errors='replace')
        (stage/(name+'.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
        print(name,'終了コード',r.returncode,flush=True)
        if r.returncode:raise RuntimeError('失敗: '+name+'。ログを確認してください')
        return r.stdout
    run('git-init',['git','init',source])
    run('git-remote',['git','-C',source,'remote','add','origin',URL])
    run('git-fetch',['git','-C',source,'fetch','--depth=1','origin',REVISION])
    run('git-checkout',['git','-C',source,'checkout','--detach','FETCH_HEAD'])
    if run('source-revision',['git','-C',source,'rev-parse','HEAD']).strip()!=REVISION:raise RuntimeError('固定 revision が一致しません')
    if not re.search(r"version:\s*'1\.1\.0'",(source/'meson.build').read_text(encoding='utf-8')):raise RuntimeError('source version が一致しません')
    run('pkgconf-check',[env['PKG_CONFIG'],'--cflags','--libs','epoxy'])
    run('configure',[sys.executable,'-m','mesonbuild.mesonmain','setup',build,source,*CONFIG])
    run('compile-unmodified',[env['NINJA'],'-C',build,'-j','4'])
    original=stage/'libvirglrenderer-unmodified.dll';shutil.copyfile(build/'src/libvirglrenderer-1.dll',original)
    patch=Path(__file__).with_name('angle-fence-flush.patch')
    run('patch-check',['git','-C',source,'apply','--check',patch])
    run('patch-apply',['git','-C',source,'apply',patch])
    diff=run('source-diff',['git','-C',source,'diff','--numstat'])
    if diff.strip()!='1\t1\tsrc/vrend_renderer.c':raise RuntimeError('一行以外の source 差分があります')
    run('compile-fixed',[env['NINJA'],'-C',build,'-j','4'])
    fixed=stage/'libvirglrenderer-fixed.dll';shutil.copyfile(build/'src/libvirglrenderer-1.dll',fixed)
    files=[Path(p) for p in run('qemu-files',['rg','--files','-g','*.dll',qemu.parent]).splitlines()]
    files=[p for p in files if p.parent.resolve()==qemu.parent]
    files.extend([qemu,qemu.parent/'COPYING',qemu.parent/'COPYING.LIB'])
    before={p.name:digest(p) for p in files}
    notices=run('copyright-notices',['rg','-n','-i','copyright',source])
    for variant,dll in [('unmodified',original),('fixed',fixed)]:
        runtime=stage/('qemu-'+variant);runtime.mkdir(exist_ok=False)
        for p in files:shutil.copyfile(p,runtime/p.name)
        shutil.copyfile(dll,runtime/'libvirglrenderer-1.dll')
        shutil.copyfile(source/'COPYING',runtime/'VIRGL-COPYING')
        shutil.copyfile(patch,runtime/'angle-fence-flush.patch')
        (runtime/'VIRGL-COPYRIGHT-NOTICES.txt').write_text(notices,encoding='utf-8')
        hashes={p.name:digest(runtime/p.name) for p in files}
        if any(hashes[n]!=h for n,h in before.items() if n!='libvirglrenderer-1.dll'):raise RuntimeError('意図しない QEMU 差分があります')
        version=run('qemu-version-'+variant,[runtime/qemu.name,'--version']).strip()
        metadata={'variant':variant,'source_url':URL,'revision':REVISION,'source_version':'1.1.0',
                  'patch_sha256':digest(patch),'source_path':str(source),'configure':CONFIG,
                  'original_qemu':str(qemu),'qemu_version':version,'original_file_hashes':before,
                  'runtime_file_hashes':hashes,'bios_readonly_path':str(qemu.parent/'share'),
                  'installed_files_changed':False,'note':'機能成立と速度向上は別に実 guest で判定する'}
        (runtime/'provenance.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2),encoding='utf-8')
    if before!={p.name:digest(p) for p in files}:raise RuntimeError('元 QEMU の hash が変化しました')
    print('専用ランタイム完成:',stage,'。ゲスト・通常期限・画素・速度の受入れは別途必要です。')

if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    try:main()
    except (RuntimeError,OSError) as e:print('失敗:',e,file=sys.stderr);raise SystemExit(1)
