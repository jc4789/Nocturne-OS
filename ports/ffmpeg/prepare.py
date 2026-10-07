"""Reproduce the freestanding FFmpeg subset from the user's unmodified source.

Requires the repository's bundled MSYS2 tools and an already-built Nocturne
build/sysroot/usr/lib/libc.a (used ONLY by configure's link feature probes).
Runtime builds use the imported sources/config, never this host-side process.
"""
import argparse, hashlib, json, os, pathlib, shutil, subprocess

LEGACY_DECODERS='mp3,flac,mjpeg,h264,aac,rawvideo,pcm_s16le,pcm_s24le,pcm_s32le,pcm_u8,pcm_f32le'
LEGACY_DEMUXERS='mp3,flac,wav,avi,mov,aac'
LEGACY_PARSERS='mpegaudio,flac,mjpeg,h264,aac'

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--source',required=True)
    p.add_argument('--import-vendor',action='store_true')
    p.add_argument('--profile',choices=['legacy','webm'],default='legacy')
    p.add_argument('--stage',help='Fresh directory below repository build/; existing stages are never overwritten')
    a=p.parse_args()
    root=pathlib.Path(__file__).resolve().parents[2]
    src=pathlib.Path(a.source).resolve()
    if (src/'RELEASE').read_text(encoding='utf-8').strip()!='9.0.2':raise SystemExit('expected release 9.0.2')
    default='build/media-stage2/reproduce-stage' if a.profile=='webm' else 'build/nocturne-platform/media/reproduce-stage'
    out=(root/(a.stage or default)).resolve()
    out.relative_to((root/'build').resolve())
    if out.exists():raise SystemExit('stage already exists; choose a fresh --stage (old evidence is preserved): '+str(out))
    if not (root/'build/sysroot/usr/lib/libc.a').is_file():raise SystemExit('build the Nocturne sysroot libc.a first')
    out.mkdir(parents=True)
    names=['configure','Makefile','RELEASE','VERSION','LICENSE.md','COPYING.LGPLv2.1','ffbuild','compat','libavutil','libavcodec','libavformat','libavdevice','libavfilter','libswscale','libswresample','fftools','doc','tests','tools']
    for name in names:
        target=out/name
        if(src/name).is_dir():shutil.copytree(src/name,target)
        else:shutil.copy2(src/name,target)
    env=os.environ.copy()
    env['PATH']=str(root/'tools/msys64/ucrt64/bin')+os.pathsep+str(root/'tools/msys64/usr/bin')+os.pathsep+env['PATH']
    relative=pathlib.PurePosixPath(os.path.relpath(root,out).replace('\\','/'))
    prefix=str(relative)
    args=[str(root/'tools/msys64/usr/bin/bash.exe'),'./configure',
        '--enable-cross-compile','--target-os=none','--arch=x86_64','--cc=clang','--ld=ld.lld','--ar=ar','--nm=nm',
        '--disable-autodetect','--disable-everything','--disable-programs','--disable-doc','--disable-network','--disable-protocols',
        '--disable-avdevice','--disable-avfilter','--disable-swscale',
        '--enable-swresample' if a.profile=='webm' else '--disable-swresample','--disable-pthreads','--disable-w32threads',
        '--disable-os2threads','--disable-asm','--disable-inline-asm','--disable-x86asm','--disable-runtime-cpudetect','--disable-debug',
        '--enable-small','--enable-static','--disable-shared','--enable-avcodec','--enable-avformat','--enable-avutil',
        '--enable-decoder='+LEGACY_DECODERS+(',vp9,opus,vorbis' if a.profile=='webm' else ''),
        '--enable-demuxer='+LEGACY_DEMUXERS+(',matroska,ogg' if a.profile=='webm' else ''),
        '--enable-parser='+LEGACY_PARSERS+(',vp9,opus,vorbis' if a.profile=='webm' else ''),
        '--extra-cflags=--target=x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -msse2 -I'+prefix+'/ports/ffmpeg/include -I'+prefix+'/user/include -I'+prefix+'/common',
        '--extra-ldflags=-m elf_x86_64 --entry=main','--extra-libs='+prefix+'/build/sysroot/usr/lib/libc.a']
    (out/'nocturne-prepare.json').write_text(json.dumps({'profile':a.profile,'source':str(src),'release_sha256':hashlib.sha256((src/'RELEASE').read_bytes()).hexdigest(),'configure_arguments':args},indent=2)+'\n',encoding='utf-8',newline='\n')
    with(out/'configure-output.log').open('w',encoding='utf-8')as log:subprocess.run(args,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    # Native mmap/fcntl/mkstemp exist, but are not POSIX file helper contracts.
    # These overrides remove unused upstream helper assumptions, not new syscalls.
    config=out/'config.h';text=config.read_text(encoding='utf-8')
    for token in ['MMAP','FCNTL','MKSTEMP']:text=text.replace('#define HAVE_'+token+' 1','#define HAVE_'+token+' 0')
    # configure emits an empty EXTERN_ASM macro with a trailing space.
    # Normalize generated configuration only, never upstream C/license bytes.
    text='\n'.join(line.rstrip(' \t') for line in text.splitlines())+'\n'
    config.write_text(text,encoding='utf-8',newline='\n')
    with(out/'build-output.log').open('w',encoding='utf-8')as log:
        libraries=['libavcodec/libavcodec.a','libavformat/libavformat.a','libavutil/libavutil.a']
        if a.profile=='webm':libraries+=['libswresample/libswresample.a']
        subprocess.run([str(root/'tools/msys64/usr/bin/make.exe'),'-j8']+libraries,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    if a.import_vendor:subprocess.run([str(root/'tools/msys64/ucrt64/bin/python.exe'),str(root/'ports/ffmpeg/vendor.py'),'--source',str(src),'--stage',str(out)],check=True)
    print('reproduced Nocturne FFmpeg 9.0.2; logs:',out)
if __name__=='__main__':main()
