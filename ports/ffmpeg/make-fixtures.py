"""Generate short self-authored regression media, not real-site acceptance."""
import hashlib, json, pathlib, subprocess
root = pathlib.Path(__file__).resolve().parents[2]
dest = root/'tests/media-fixtures'
dest.mkdir(parents=True,exist_ok=True)
version = subprocess.check_output(['ffmpeg','-version'],text=True,encoding='utf-8').splitlines()[0]
audio = ['-f','lavfi','-i','aevalsrc=0.3*sin(2*PI*440*t)|0.3*sin(2*PI*660*t):s=48000:d=0.2']
video = ['-f','lavfi','-i','testsrc2=size=96x64:rate=10:duration=0.4']
specs = {
    'stereo.wav': audio+['-c:a','pcm_s16le'],
    'stereo.flac':audio+['-c:a','flac'],
    'stereo.mp3':audio+['-c:a','libmp3lame','-b:a','128k'],
    'stereo.aac':audio+['-c:a','aac','-b:a','128k'],
    'mjpeg.avi':video+['-an','-c:v','mjpeg','-q:v','3','-pix_fmt','yuvj420p'],
    'h264.mp4':video+['-an','-c:v','libx264','-profile:v','baseline','-pix_fmt','yuv420p','-movflags','+faststart'],
    'av.mp4':video+audio+['-c:v','libx264','-profile:v','baseline','-pix_fmt','yuv420p','-c:a','aac','-movflags','+faststart'],
    'unsupported.webm':video+['-an','-c:v','libvpx','-b:v','80k'],
}
records=[]
for name, args in specs.items():
    command=['ffmpeg','-hide_banner','-loglevel','error','-y']+args+[str(dest/name)]
    subprocess.run(command,check=True)
    data=(dest/name).read_bytes()
    records.append({'path':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'arguments':args})
(dest/'invalid.mp4').write_bytes(b'not a container\x00\xff\x00')
(dest/'manifest.json').write_text(json.dumps({'generator':version,'authorship':'synthetic lavfi tone/pattern generated for Nocturne regression','fixtures':records},indent=2)+'\n',encoding='utf-8')
print('generated',len(records),'fixtures',sum(x['bytes'] for x in records),'bytes')
