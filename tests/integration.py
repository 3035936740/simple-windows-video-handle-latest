"""End-to-end tests execute the same C++ core as the Win32 GUI. No Python video processing in production."""
import argparse
import hashlib
import json
import math
import shutil
import statistics
import subprocess
import time
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--cli', type=Path, required=True)
p.add_argument('--work', type=Path, default=Path('test-output/integration'))
p.add_argument('--gpu', action='store_true')
a = p.parse_args()
a.cli = a.cli.resolve()
a.work = a.work.resolve()
a.work.mkdir(parents=True, exist_ok=True)
FFMPEG = str(a.cli.parent / 'ffmpeg.exe') if (a.cli.parent / 'ffmpeg.exe').exists() else shutil.which('ffmpeg')
FFPROBE = str(a.cli.parent / 'ffprobe.exe') if (a.cli.parent / 'ffprobe.exe').exists() else shutil.which('ffprobe')
if not FFMPEG or not FFPROBE:
    raise RuntimeError('ffmpeg / ffprobe required')
report = []

def run(args, expected=0, timeout=120):
    r = subprocess.run([str(x) for x in args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if r.returncode != expected:
        raise AssertionError(f'Exit {r.returncode}, expected {expected}: {args}\n{r.stdout.decode("utf8", "replace")}\n{r.stderr.decode("utf8", "replace")}')
    return r

def ff(*args):
    return run([FFMPEG, '-hide_banner', '-loglevel', 'error', '-y', *args])

def make(path, w=1920, h=1080, audio='aac', duration=2):
    path.parent.mkdir(parents=True, exist_ok=True)
    ff('-f', 'lavfi', '-i', f'testsrc2=size={w}x{h}:rate=25,drawgrid=w=60:h=60:t=2:c=white@0.8', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
       '-t', str(duration), '-c:v', 'libx264', '-preset', 'ultrafast', '-c:a', audio, '-metadata', 'title=PRIVATE_METADATA', str(path))

def probe(path):
    return json.loads(run([FFPROBE, '-v', 'error', '-show_streams', '-show_format', '-of', 'json', path]).stdout)

def process(source, destination, *args, expected=0):
    destination.mkdir(parents=True, exist_ok=True)
    return run([a.cli, '--input', source, '--output', destination, '--encoder', 'cpu', '--overwrite', *args], expected)

def frame(path, vf=None):
    args = ['-i', str(path)]
    if vf:
        args += ['-vf', vf]
    return ff(*args, '-frames:v', '1', '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-').stdout

def hash_audio(path):
    return ff('-i', str(path), '-map', '0:a:0', '-c:a', 'copy', '-f', 'hash', '-').stdout

def mae_region(out, ref, W, x, y, w, h):
    total = count = 0
    # Deterministic subsampling is sufficient to detect stretched/cropped/off-center foregrounds.
    for row in range(4, h-4, 7):
        for col in range(4, w-4, 7):
            p1 = ((row+y)*W + col+x)*3
            p2 = (row*w+col)*3
            for channel in range(3):
                total += abs(out[p1+channel]-ref[p2+channel]); count += 1
    return total/count

def edge_strength(data, W, H, box):
    x,y,w,h=box
    values=[]
    for row in range(y, y+h-1, 5):
        for col in range(x, x+w-1):
            j=(row*W+col)*3
            values.append(sum((data[j+k]-data[j+3+k])**2 for k in range(3))/3)
    return statistics.mean(values)

cases=[(1920,1080,720,1080),(1080,1920,720,1080),(1080,1080,720,1080),(1920,1080,1080,1920)]
for w,h,W,H in cases:
    source=a.work/'中文 输入 & spaces'/f'{w}x{h}.mp4'
    make(source,w,h)
    output=a.work/f'blur_{w}_{h}_{W}_{H}'
    process(source,output,'--width',W,'--height',H)
    result=output/source.name
    info=probe(result); streams=info['streams'];v=next(s for s in streams if s['codec_type']=='video');audio=next(s for s in streams if s['codec_type']=='audio')
    assert (v['width'],v['height'])==(W,H)
    assert v['sample_aspect_ratio']=='1:1'
    assert v['r_frame_rate']=='25/1'
    assert abs(float(v['duration'])-float(audio['duration']))<0.08
    assert abs(float(v.get('start_time',0))-float(audio.get('start_time',0)))<0.08
    assert hash_audio(source)==hash_audio(result), 'Original audio packets changed'
    assert 'PRIVATE_METADATA' not in json.dumps(info)
    # Probe the independently scaled reference dimensions rather than assuming a rounding rule.
    ref_file=output/'reference.png'
    fit=f'scale={W}:{H}:force_original_aspect_ratio=decrease:force_divisible_by=2:flags=lanczos,setsar=1'
    ff('-i',source,'-vf',fit,'-frames:v','1',ref_file)
    ri=probe(ref_file)['streams'][0];fw,fh=ri['width'],ri['height']
    x,y=(W-fw)//2,(H-fh)//2
    # yuv420 overlay rounds to even chroma offsets.
    x=x//2*2;y=y//2*2
    raw=frame(result);ref=frame(ref_file)
    error=mae_region(raw,ref,W,x,y,fw,fh)
    assert error<9, f'Foreground mismatch: {error}'
    band=(0,0,W,max(10,y-12)) if y>0 else (0,0,max(10,x-12),H)
    sharp=frame(source,f'scale={W}:{H}:force_original_aspect_ratio=increase:force_divisible_by=2,crop={W}:{H},setsar=1')
    blurred_edges=edge_strength(raw,W,H,band);sharp_edges=edge_strength(sharp,W,H,band)
    assert blurred_edges<sharp_edges*0.8, (blurred_edges,sharp_edges)
    corners=[raw[((yy*W+xx)*3):((yy*W+xx)*3+3)] for yy in (0,H-1) for xx in (0,W-1)]
    assert all(sum(pixel)>25 for pixel in corners), 'Black padding found'
    ff('-i',result,'-frames:v','1',output/'preview.png')
    report.append({'test':f'{w}x{h} -> {W}x{H} blur','foreground_mae':round(error,3),'blur_edge_ratio':round(blurred_edges/sharp_edges,3),'result':'PASS'})
    print(report[-1],flush=True)

source=a.work/'中文 输入 & spaces'/'1920x1080.mp4'
for mode in ('stretch','black','crop'):
    dst=a.work/mode;process(source,dst,'--mode',mode,'--fps','30','--no-audio')
    info=probe(dst/source.name);v=info['streams'][0]
    assert (v['width'],v['height'])==(720,1080) and v['r_frame_rate']=='30/1'
    assert not any(s['codec_type']=='audio' for s in info['streams'])
    if mode=='black':
        pixels=frame(dst/source.name);assert sum(pixels[:3])<10
    report.append({'test':mode+' / FPS 30 / audio off','result':'PASS'})

logo=a.work/'透明 Logo.png'
# Generate a transparent PNG with a visible center, independent of production video processing.
ff('-f','lavfi','-i','color=c=black@0:s=160x100,format=rgba,drawbox=x=40:y=25:w=80:h=50:color=red@1:t=fill:replace=1','-frames:v','1',logo)
font=Path('C:/Windows/Fonts/msyh.ttc')
assert font.exists()
dst=a.work/'watermarks'
process(source,dst,'--image',logo,'--image-alpha','0.5','--image-scale','20','--image-rotate','15','--image-position','tl',
        '--text',"中文水印: 50% '引号' [x]; \\ 测试\n第二行",'--font',font,'--text-position','custom','--text-x','15','--text-y','500',
        '--font-color','#FFCC88','--mirror','--flip','--brightness','0.01','--contrast','1.01','--saturation','1.03','--noise','2')
assert float(probe(dst/source.name)['streams'][0]['duration'])>1.9, 'Logo truncated video'
ff('-i',dst/source.name,'-frames:v','1',dst/'preview.png')
report.append({'test':'Chinese text / PNG alpha / rotation / picture adjustments','result':'PASS'})

# Explicit transparency checks at alpha 0 and 1.
base=frame(a.work/'blur_1920_1080_720_1080'/source.name)
for alpha in ('0','1'):
    dst=a.work/f'alpha_{alpha}'
    process(source,dst,'--image',logo,'--image-alpha',alpha,'--image-scale','20','--image-position','custom','--image-x','100','--image-y','100')
    pixels=frame(dst/source.name)
    assert sum(abs(pixels[((105*720+105)*3)+k]-base[((105*720+105)*3)+k]) for k in range(3))<20, 'Transparent corner lost'
    if alpha=='1':
        point=(145*720+170)*3
        assert pixels[point]>150 and pixels[point+1]<80 and pixels[point+2]<80, 'Opaque logo not visible'
    else:
        assert mae_region(pixels,base,720,0,0,720,1080)<2
report.append({'test':'PNG original transparency and alpha 0 / 1','result':'PASS'})

# G.711 mu-law is unsupported in MP4; copy must fall back to AAC.
pcm=a.work/'pcm.mkv';make(pcm,320,180,'pcm_mulaw')
dst=a.work/'aac_fallback';r=process(pcm,dst)
assert 'AAC' in r.stdout.decode('utf8')
assert next(s for s in probe(dst/'pcm.mp4')['streams'] if s['codec_type']=='audio')['codec_name']=='aac'
report.append({'test':'Audio copy failure -> AAC fallback','result':'PASS'})

# A corrupt video and matching stems must not stop/overwrite the queue; output subtree excluded.
batch=a.work/'batch';batch.mkdir(exist_ok=True)
shutil.copyfile(source,batch/'same.mp4');shutil.copyfile(pcm,batch/'same.mkv')
(batch/'broken.mp4').write_bytes(b'corrupt video')
(batch/'sub').mkdir(exist_ok=True);shutil.copyfile(source,batch/'sub'/'good.mp4')
dst=batch/'output';r=process(batch,dst,'--recursive',expected=1)
assert 'success=3 failed=1' in r.stdout.decode('utf8')
assert len(list(dst.rglob('*.mp4')))==3
r=process(batch,dst,'--recursive',expected=1)
assert 'success=3 failed=1' in r.stdout.decode('utf8'), 'Output subtree re-queued'
report.append({'test':'Batch corrupt input / recursion / collision / output exclusion','result':'PASS'})

# In-place replacement succeeds only after encoding; stopping must preserve the original.
inplace=a.work/'inplace';inplace.mkdir(exist_ok=True);copy=inplace/'original.mp4';shutil.copyfile(source,copy)
process(copy,inplace);assert probe(copy)['streams'][0]['width']==720
shutil.copyfile(source,copy);before=hashlib.sha256(copy.read_bytes()).hexdigest()
start=time.monotonic();r=process(copy,inplace,'--cancel-after-ms','200',expected=130)
assert time.monotonic()-start<10
assert hashlib.sha256(copy.read_bytes()).hexdigest()==before
assert not list(inplace.glob('.svh_*'))
report.append({'test':'Safe in-place overwrite / cancellation / temporary cleanup','result':'PASS'})

for args in (['--width','721'],['--width','-2'],['--fps','nan'],['--preset','p4']):
    process(source,a.work/'invalid',*args,expected=2)
report.append({'test':'Invalid dimensions / NaN / encoder preset rejected','result':'PASS'})

# HEVC CPU must remain available for systems with no GPU.
dst=a.work/'hevc';process(pcm,dst,'--codec','hevc','--width','320','--height','480')
assert probe(dst/'pcm.mp4')['streams'][0]['codec_name']=='hevc'
report.append({'test':'H.265 CPU and AAC fallback','result':'PASS'})

# Preserve relative audio delay and the source VFR frame timestamps.
delayed=a.work/'delayed_audio.mp4'
ff('-f','lavfi','-i','testsrc2=size=320x180:rate=25:duration=2','-itsoffset','0.35','-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=1.5',
   '-c:v','libx264','-preset','ultrafast','-c:a','aac',delayed)
dst=a.work/'delayed_output';process(delayed,dst,'--width','320','--height','480')
def relative_delay(path):
    streams=probe(path)['streams']
    video=next(x for x in streams if x['codec_type']=='video')
    audio=next(x for x in streams if x['codec_type']=='audio')
    return float(audio['start_time'])-float(video['start_time'])
assert abs(relative_delay(delayed)-relative_delay(dst/delayed.name))<0.025
assert hash_audio(delayed)==hash_audio(dst/delayed.name)
report.append({'test':'Original delayed audio timeline retained','result':'PASS'})
vfr=a.work/'vfr.mp4'
ff('-f','lavfi','-i','testsrc2=size=320x180:rate=25:duration=2','-vf',"select='not(mod(n,3))+not(mod(n,5))'",'-fps_mode','vfr','-c:v','libx264','-preset','ultrafast',vfr)
dst=a.work/'vfr_output';process(vfr,dst,'--width','320','--height','480')
def frame_times(path):
    j=json.loads(run([FFPROBE,'-v','error','-select_streams','v:0','-show_frames','-show_entries','frame=best_effort_timestamp_time','-of','json',path]).stdout)
    return [float(f['best_effort_timestamp_time']) for f in j['frames']]
t1=frame_times(vfr);t2=frame_times(dst/vfr.name)
assert len(t1)==len(t2) and all(abs(x-y)<0.002 for x,y in zip(t1,t2))
report.append({'test':'Original VFR frame timestamps retained','result':'PASS'})
# Square-pixel output with an anamorphic source, not forced into its encoded pixel ratio.
sar=a.work/'sar.mp4'
ff('-f','lavfi','-i','testsrc2=size=320x240:rate=25:duration=1','-vf','setsar=2','-c:v','libx264','-preset','ultrafast',sar)
dst=a.work/'sar_output';process(sar,dst,'--width','720','--height','1080')
assert probe(dst/sar.name)['streams'][0]['sample_aspect_ratio']=='1:1'
report.append({'test':'Anamorphic SAR normalization','result':'PASS'})

if a.gpu:
    dst=a.work/'nvenc';dst.mkdir(exist_ok=True)
    r=run([a.cli,'--input',source,'--output',dst,'--encoder','nvenc','--overwrite'])
    assert 'h264_nvenc' in r.stdout.decode('utf8')
    assert probe(dst/source.name)['streams'][0]['width']==720
    report.append({'test':'NVIDIA NVENC real 1080P transcode','result':'PASS'})
    dst=a.work/'nvenc_hevc';dst.mkdir(exist_ok=True)
    r=run([a.cli,'--input',source,'--output',dst,'--encoder','nvenc','--codec','hevc','--overwrite'])
    assert '\u7f16\u7801\u5668 hevc_nvenc' in r.stdout.decode('utf8')
    assert probe(dst/source.name)['streams'][0]['codec_name']=='hevc'
    report.append({'test':'NVIDIA HEVC NVENC real 1080P transcode','result':'PASS'})
    dst=a.work/'auto_gpu';dst.mkdir(exist_ok=True)
    r=run([a.cli,'--input',source,'--output',dst,'--encoder','auto','--overwrite'])
    assert '\u7f16\u7801\u5668 h264_nvenc' in r.stdout.decode('utf8')
    report.append({'test':'Auto encoder selects working NVENC','result':'PASS'})

(a.work/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
print(f'ALL {len(report)} integration checks passed. Report: {a.work / "report.json"}',flush=True)
