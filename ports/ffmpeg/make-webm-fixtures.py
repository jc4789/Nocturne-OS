"""New actual compressed WebM/Ogg fixtures; legacy fixture bytes are untouched."""
import hashlib, json, pathlib, subprocess

root=pathlib.Path(__file__).resolve().parents[2]
dest=root/'tests/media-fixtures'
version=subprocess.check_output(['ffmpeg','-version'],text=True,encoding='utf-8').splitlines()[0]
audio=['-f','lavfi','-i','aevalsrc=0.3*sin(2*PI*440*t)|0.3*sin(2*PI*660*t):s=48000:d=0.2']
video=['-f','lavfi','-i','testsrc2=size=96x64:rate=10:duration=0.4']
vp9=['-c:v','libvpx-vp9','-profile:v','0','-pix_fmt','yuv420p','-threads','1','-row-mt','0','-tile-columns','0','-g','10','-deadline','good','-cpu-used','4','-b:v','80k']
specs={
    'stereo.opus':audio+['-c:a','libopus','-application','audio','-b:a','96k'],
    'stereo-silk.opus':audio+['-c:a','libopus','-application','voip','-ar','16000','-b:a','24k'],
    'stereo.ogg':audio+['-c:a','libvorbis','-q:a','4'],
    'opus.webm':audio+['-c:a','libopus','-application','audio','-b:a','96k'],
    'vorbis.webm':audio+['-c:a','libvorbis','-q:a','4'],
    'vp9.webm':video+['-an']+vp9,
    'vp9-opus.webm':video+audio+vp9+['-c:a','libopus','-b:a','96k'],
    'vp9-vorbis.webm':video+audio+vp9+['-c:a','libvorbis','-q:a','4'],
    'vp9-high10.webm':video+['-an','-c:v','libvpx-vp9','-profile:v','2','-pix_fmt','yuv420p10le','-threads','1','-deadline','good','-cpu-used','4','-b:v','80k'],
    'unsupported-av1.webm':video+['-an','-c:v','libaom-av1','-pix_fmt','yuv420p','-threads','1','-cpu-used','8','-crf','40'],
}
records=[]
for name,args in specs.items():
    command=['ffmpeg','-hide_banner','-loglevel','error','-y']+args+[str(dest/name)]
    subprocess.run(command,check=True)
    data=(dest/name).read_bytes()
    probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-show_format','-of','json',str(dest/name)],text=True,encoding='utf-8'))
    record={'path':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'arguments':args,
            'streams':[{k:s.get(k) for k in ['codec_name','profile','codec_type','sample_rate','channels','width','height','pix_fmt']} for s in probe['streams']]}
    if any(s['codec_type']=='audio' for s in probe['streams']):
        pcm=subprocess.check_output(['ffmpeg','-v','error','-i',str(dest/name),'-map','0:a:0','-ar','48000','-ac','2','-f','s16le','pipe:1'])
        record['host_reference_frames']=len(pcm)//4
    records.append(record)
(dest/'webm-manifest.json').write_text(json.dumps({'generator':version,'authorship':'new self-authored synthetic tone/pattern; legacy manifest and inputs preserved','fixtures':records},indent=2)+'\n',encoding='utf-8',newline='\n')
print('generated WebM/Ogg',len(records),'fixtures',sum(x['bytes'] for x in records),'bytes')
