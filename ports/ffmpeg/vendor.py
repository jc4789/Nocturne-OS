"""Import only the proven C/include closure from a successful Nocturne build.

Usage: python ports/ffmpeg/vendor.py --source PATH --stage PATH
The source and stage are read-only; source licensing headers remain unchanged.
"""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

# POSIX fd helpers that the Nocturne native API replaced (README "ネイティブAPI移行"); never imported.
NATIVE_DROPPED = {'libavutil/file_open.c', 'libavutil/file_open.h', 'libavformat/os_support.c'}
SOURCE_SUFFIXES = ('.c', '.asm')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def enabled(text, suffix):
    return sorted(x.lower() for x in re.findall(r'^#define CONFIG_(\w+)_'+suffix+r' 1$',text,re.M))

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source', required=True)
    p.add_argument('--stage', required=True)
    a = p.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    src, stage = pathlib.Path(a.source).resolve(), pathlib.Path(a.stage).resolve()
    if (src/'RELEASE').read_text(encoding='utf-8').strip() != '9.0.2':
        raise SystemExit('expected FFmpeg RELEASE 9.0.2')
    dest = root/'third_party/ffmpeg'
    dest.mkdir(parents=True, exist_ok=True)
    files = set(['config.h','config_components.h','libavutil/avconfig.h','libavutil/ffversion.h'])
    sources = set()
    asm_sources = set()
    # Files already ported to the native API stay as they are (the stage was built from them).
    native = set()
    if (dest/'manifest.json').is_file():
        native = {x['path'] for x in json.loads((dest/'manifest.json').read_text(encoding='utf-8'))['files']
                  if x['origin'] == 'nocturne-native-port'}
    config=(stage/'config.h').read_text(encoding='utf-8')
    components=(stage/'config_components.h').read_text(encoding='utf-8')
    if any(re.search(r'^#define CONFIG_'+x+r' 1$',config,re.M) for x in ['GPL','NONFREE','VERSION3','NETWORK']):
        raise SystemExit('unexpected license/network configuration')
    libraries=['libavutil','libavcodec','libavformat']
    if '#define CONFIG_SWRESAMPLE 1' in config:libraries+=['libswresample']
    if '#define CONFIG_AVFILTER 1' in config:libraries+=['libavfilter']
    for library in libraries:
        if not (stage/library/(library+'.a')).is_file():raise SystemExit('missing compiled archive: '+library)
    deps = subprocess.check_output(['rg','--files','-g','*.d']+libraries,cwd=stage,text=True,encoding='utf-8').splitlines()
    for name in deps:
        text = (stage/name).read_text(encoding='utf-8').replace('\\\n',' ')
        header, _, body = text.partition(':')
        if not header.strip().endswith('.o'): continue
        words = body.split()
        if not words or not words[0].endswith(SOURCE_SUFFIXES): continue
        first=pathlib.PurePosixPath(words[0])
        if first.is_absolute() or '..' in first.parts or not first.parts or first.parts[0] not in libraries:
            raise SystemExit('unexpected dependency source: '+words[0])
        if words[0] in NATIVE_DROPPED: continue
        (asm_sources if words[0].endswith('.asm') else sources).add(words[0])
        for word in words:
            path = pathlib.PurePosixPath(word)
            if '..' in path.parts or path.is_absolute(): continue
            if (stage/word).is_file() and word.endswith(('.h','.c','.asm')) and word not in NATIVE_DROPPED:
                files.add(word)
    files.update(sources)
    files.update(asm_sources)
    if asm_sources:
        if '#define HAVE_X86ASM 1' not in config: raise SystemExit('asm sources without HAVE_X86ASM')
        files.update(['config.asm'])
        if (stage/'config_components.asm').is_file(): files.add('config_components.asm')
    # Public headers used by the adapter are also part of the closure.
    for name in ['libavformat/avformat.h','libavcodec/avcodec.h','libavutil/avutil.h','libavutil/mem.h','libavutil/samplefmt.h','libavutil/pixfmt.h']:
        files.add(name)
    if '#define CONFIG_AVFILTER 1' in config:
        files.update(['libavfilter/avfilter.h','libavfilter/buffersrc.h','libavfilter/buffersink.h'])
    licenses = ['RELEASE','VERSION','LICENSE.md','COPYING.LGPLv2.1','COPYING.LGPLv3','COPYING.GPLv2','COPYING.GPLv3']
    # Reject a modified upstream file BEFORE copying any new vendor bytes.
    # A failure must not leave a partially overwritten source/license closure.
    identical_sources={}
    for name in sorted(files):
        original,built=src/name,stage/name
        if name in native:
            if not built.is_file() or sha(built) != sha(dest/name):
                raise SystemExit('native-port file differs from the stage build: '+name)
            continue
        identical_sources[name]=original.is_file() and sha(original)==sha(built)
        if original.is_file() and not identical_sources[name]:
            raise SystemExit('upstream source unexpectedly modified: '+name)
    manifest = []
    for name in sorted(files):
        original = src/name
        built = stage/name
        output = dest/name
        output.parent.mkdir(parents=True,exist_ok=True)
        if name in native:
            manifest.append({'path':name,'sha256':sha(output),'origin':'nocturne-native-port'})
            continue
        shutil.copy2(built,output)
        identical = identical_sources[name]
        manifest.append({'path':name,'sha256':sha(output),'origin':'upstream-unmodified' if identical else 'configure-generated'})
    for name in licenses:
        shutil.copy2(src/name,dest/name)
        manifest.append({'path':name,'sha256':sha(dest/name),'origin':'upstream-unmodified'})
    separator = ' ' + chr(92) + '\n    '
    mk = '# Generated by ports/ffmpeg/vendor.py; Nocturne FFmpeg 9.0.2 subset.\nFFMPEG_C := ' + separator.join('third_party/ffmpeg/'+x for x in sorted(sources))+'\n'
    if asm_sources:
        mk += '# x86-64 NASM sources: SIMD selected at run time by libavutil/x86/cpu.c (README.nocturne.md).\nFFMPEG_ASM := ' + separator.join('third_party/ffmpeg/'+x for x in sorted(asm_sources))+'\n'
    (dest/'sources.mk').write_text(mk,encoding='utf-8',newline='\n')
    simd = 'x86asm + inline asm with runtime cpudetect, AVX-512 off' if asm_sources else 'asm disabled'
    data = {'release':'9.0.2','license':'LGPL-2.1-or-later','compiled_sources':len(sources),'compiled_asm_sources':len(asm_sources),'files':manifest,
        'configuration':'freestanding x86_64 target-os=none; network/protocols/devices/encoders/threads disabled; '+simd+'; filters limited to configured native PCM transforms',
        'effective_overrides':{'HAVE_MMAP':0,'HAVE_FCNTL':0,'HAVE_MKSTEMP':0},
        'libraries':libraries,'decoders':enabled(components,'DECODER'),
        'demuxers':enabled(components,'DEMUXER'),'parsers':enabled(components,'PARSER'),
        'bitstream_filters':enabled(components,'BSF')}
    (dest/'manifest.json').write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    print('imported',len(sources),'C sources,',len(asm_sources),'asm sources,',len(manifest),'files,',sum((dest/x['path']).stat().st_size for x in manifest),'bytes')
if __name__ == '__main__': main()
