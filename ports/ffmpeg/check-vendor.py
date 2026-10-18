"""Verify imported bytes and optionally compare all unmodified source files."""
import argparse, hashlib, json, pathlib, re
p=argparse.ArgumentParser();p.add_argument('--source');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2];vendor=root/'third_party/ffmpeg'
manifest=json.loads((vendor/'manifest.json').read_text(encoding='utf-8'))
count=0
for item in manifest['files']:
    path=(vendor/item['path']).resolve();path.relative_to(vendor.resolve())
    actual=hashlib.sha256(path.read_bytes()).hexdigest()
    if actual!=item['sha256']:raise SystemExit('modified vendor: '+item['path'])
    if a.source and item['origin']=='upstream-unmodified':
        source=pathlib.Path(a.source)/item['path']
        if hashlib.sha256(source.read_bytes()).hexdigest()!=actual:raise SystemExit('source mismatch: '+item['path'])
    count+=1
components=(vendor/'config_components.h').read_text(encoding='utf-8')
for field,suffix in [('decoders','DECODER'),('demuxers','DEMUXER'),('parsers','PARSER'),('bitstream_filters','BSF')]:
    actual=sorted(x.lower() for x in re.findall(r'^#define CONFIG_(\w+)_'+suffix+r' 1$',components,re.M))
    if field in manifest and actual!=manifest[field]:raise SystemExit('configuration mismatch: '+field)
mk=(vendor/'sources.mk').read_text(encoding='utf-8')
sources=re.findall(r'third_party/ffmpeg/(\S+\.c)(?=\s|$)',mk)
asm=re.findall(r'third_party/ffmpeg/(\S+\.asm)(?=\s|$)',mk)
listed={x['path'] for x in manifest['files']}
if len(sources)!=manifest['compiled_sources'] or len(set(sources))!=len(sources) or not set(sources)<=listed:
    raise SystemExit('compiled source closure mismatch')
if len(asm)!=manifest.get('compiled_asm_sources',0) or len(set(asm))!=len(asm) or not set(asm)<=listed:
    raise SystemExit('compiled asm closure mismatch')
if asm and not {'config.asm','libavutil/x86/x86inc.asm','libavutil/x86/x86util.asm'}<=listed:
    raise SystemExit('asm include closure incomplete')
print('FFMPEG_VENDOR verified',count,'files; release',manifest['release'],'C',manifest['compiled_sources'],'asm',manifest.get('compiled_asm_sources',0))
