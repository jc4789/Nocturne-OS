"""Reproduce the freestanding FFmpeg subset from the user's unmodified source.

Requires the repository's bundled MSYS2 tools and an already-built Nocturne
build/sysroot/usr/lib/libc.a (used ONLY by configure's link feature probes).
Runtime builds use the imported sources/config, never this host-side process.
"""
import argparse, os, pathlib, shutil, subprocess

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--source',required=True)
    p.add_argument('--import-vendor',action='store_true')
    a=p.parse_args()
    root=pathlib.Path(__file__).resolve().parents[2]
    src=pathlib.Path(a.source).resolve()
    if (src/'RELEASE').read_text(encoding='utf-8').strip()!='9.0.2':raise SystemExit('expected release 9.0.2')
    out=root/'build/nocturne-platform/media/reproduce-stage'
    out.mkdir(parents=True,exist_ok=True)
    names=['configure','Makefile','RELEASE','VERSION','LICENSE.md','COPYING.LGPLv2.1','ffbuild','compat','libavutil','libavcodec','libavformat','libavdevice','libavfilter','libswscale','libswresample','fftools','doc','tests','tools']
    for name in names:
        target=out/name
        if target.exists():continue
        if(src/name).is_dir():shutil.copytree(src/name,target)
        else:shutil.copy2(src/name,target)
    env=os.environ.copy()
    env['PATH']=str(root/'tools/msys64/ucrt64/bin')+os.pathsep+str(root/'tools/msys64/usr/bin')+os.pathsep+env['PATH']
    args=[str(root/'tools/msys64/usr/bin/bash.exe'),'./configure',
        '--enable-cross-compile','--target-os=none','--arch=x86_64','--cc=clang','--ld=ld.lld','--ar=ar','--nm=nm',
        '--disable-autodetect','--disable-everything','--disable-programs','--disable-doc','--disable-network','--disable-protocols',
        '--disable-avdevice','--disable-avfilter','--disable-swscale','--disable-swresample','--disable-pthreads','--disable-w32threads',
        '--disable-os2threads','--disable-asm','--disable-inline-asm','--disable-x86asm','--disable-runtime-cpudetect','--disable-debug',
        '--enable-small','--enable-static','--disable-shared','--enable-avcodec','--enable-avformat','--enable-avutil',
        '--enable-decoder=mp3,flac,mjpeg,h264,aac,rawvideo,pcm_s16le,pcm_s24le,pcm_s32le,pcm_u8,pcm_f32le',
        '--enable-demuxer=mp3,flac,wav,avi,mov,aac','--enable-parser=mpegaudio,flac,mjpeg,h264,aac',
        '--extra-cflags=--target=x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -msse2 -I../../../../ports/ffmpeg/include -I../../../../user/include -I../../../../common',
        '--extra-ldflags=-m elf_x86_64 --entry=main','--extra-libs=../../../../build/sysroot/usr/lib/libc.a']
    with(out/'configure-output.log').open('w',encoding='utf-8')as log:subprocess.run(args,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    # Native mmap/fcntl/mkstemp exist, but are not POSIX file helper contracts.
    # These overrides remove unused upstream helper assumptions, not new syscalls.
    config=out/'config.h';text=config.read_text(encoding='utf-8')
    for token in ['MMAP','FCNTL','MKSTEMP']:text=text.replace('#define HAVE_'+token+' 1','#define HAVE_'+token+' 0')
    config.write_text(text,encoding='utf-8')
    with(out/'build-output.log').open('w',encoding='utf-8')as log:
        subprocess.run([str(root/'tools/msys64/usr/bin/make.exe'),'-j8','libavcodec/libavcodec.a','libavformat/libavformat.a','libavutil/libavutil.a'],cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
    if a.import_vendor:subprocess.run([str(root/'tools/msys64/ucrt64/bin/python.exe'),str(root/'ports/ffmpeg/vendor.py'),'--source',str(src),'--stage',str(out)],check=True)
    print('reproduced Nocturne FFmpeg 9.0.2; logs:',out)
if __name__=='__main__':main()
