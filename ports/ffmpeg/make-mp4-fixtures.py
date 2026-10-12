"""Generate new self-authored BMFF inputs; preserve every existing fixture."""
import hashlib, json, pathlib, subprocess
root=pathlib.Path(__file__).resolve().parents[2]
dest=root/'tests/media-fixtures'
video=['-f','lavfi','-i','testsrc2=size=96x64:rate=10:duration=0.4','-an']
audio=['-f','lavfi','-i','aevalsrc=0.3*sin(2*PI*440*t)|0.3*sin(2*PI*660*t):s=48000:d=0.2']
hevc=['-c:v','libx265','-threads','1','-x265-params','pools=none:frame-threads=1:log-level=error','-tag:v','hvc1']
specs={
    'mp4-opus.mp4':audio+['-c:a','libopus','-b:a','96k'],
    'mp4-ac3-delayed.mp4':audio+['-c:a','ac3','-b:a','192k'],
    'mp4-eac3-delayed.mp4':audio+['-c:a','eac3','-b:a','192k'],
    'mp4-vp9.mp4':video+['-c:v','libvpx-vp9','-pix_fmt','yuv420p','-threads','1','-deadline','good','-cpu-used','4'],
    'mp4-hevc.mp4':video+hevc+['-pix_fmt','yuv420p'],
    'mp4-hevc10.mp4':video+hevc+['-pix_fmt','yuv420p10le'],
}
records=[]
version=subprocess.check_output(['ffmpeg','-version'],text=True,encoding='utf-8').splitlines()[0]
for name,args in specs.items():
    output=dest/name
    flags='frag_keyframe+empty_moov+default_base_moof'+('+delay_moov' if 'delayed' in name else '')
    args=args+['-movflags',flags]
    # A partial failed run may have completed earlier outputs. Inspect those
    # exact bytes instead of regenerating them; never delete or overwrite.
    if not output.exists():subprocess.run(['ffmpeg','-hide_banner','-loglevel','error']+args+[str(output)],check=True)
    data=output.read_bytes()
    probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(output)],text=True,encoding='utf-8'))
    record={'path':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'arguments':args,
        'streams':[{k:s.get(k) for k in ['codec_name','profile','codec_type','sample_rate','channels','width','height','pix_fmt']}for s in probe['streams']]}
    if any(s['codec_type']=='audio' for s in probe['streams']):
        pcm=subprocess.check_output(['ffmpeg','-v','error','-i',str(output),'-map','0:a:0','-ar','48000','-ac','2','-f','s16le','pipe:1'])
        record['host_reference_frames']=len(pcm)//4
    else:
        frames=json.loads(subprocess.check_output(['ffprobe','-v','error','-count_frames','-show_entries','stream=nb_read_frames','-of','json',str(output)],text=True,encoding='utf-8'))
        record['host_reference_pictures']=int(frames['streams'][0]['nb_read_frames'])
    records.append(record)
(dest/'mp4-new-manifest.json').write_text(json.dumps({'generator':version,'authorship':'self-authored synthetic tone/pattern; host encoder is not a runtime dependency','fixtures':records},indent=2)+'\n',encoding='utf-8',newline='\n')
print('new fragmented BMFF fixtures:',len(records),'bytes:',sum(r['bytes']for r in records))
