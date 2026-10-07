"""Verify imported bytes and optionally compare all unmodified source files."""
import argparse, hashlib, json, pathlib
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
print('FFMPEG_VENDOR verified',count,'files; release',manifest['release'],'C',manifest['compiled_sources'])
