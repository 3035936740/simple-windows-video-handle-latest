"""Pixel-level tiled watermark checks against actual C++/FFmpeg outputs."""
from pathlib import Path
import shutil

def check_tiles(work, process, ff, probe, frame, hash_audio, report):
    source=work/'平铺 中文输入.mp4'
    ff('-f','lavfi','-i','color=c=gray:s=720x1080:r=25:d=2',
       '-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=2',
       '-c:v','libx264','-preset','ultrafast','-c:a','aac',source)
    font=Path('C:/Windows/Fonts/msyh.ttc')
    common=['--text','中文水印','--font',font,'--font-size','36','--text-alpha','1']
    def render(name,*args):
        dst=work/name
        result=process(source,dst,*common,*args)
        output=dst/source.name
        streams=probe(output)['streams']; video=next(s for s in streams if s['codec_type']=='video')
        assert (video['width'],video['height'])==(720,1080)
        assert float(video['duration'])>=1.96, 'Static watermark truncated the video'
        assert hash_audio(source)==hash_audio(output), 'Tiling changed original audio'
        return frame(output),result,output
    def rows_visible(data,lo,hi,white=True):
        count=0
        for y in range(lo,hi):
            for x in range(720):
                i=(y*720+x)*3
                if (min(data[i:i+3])>180 if white else max(data[i:i+3])<60):count+=1
        return count
    full,r,out=render('tile_full','--text-position','tile-full','--text-tile-rotation','0','--text-tile-spacing','20')
    assert all(rows_visible(full,lo,hi)>200 for lo,hi in [(0,300),(400,700),(800,1080)])
    # Text appears in multiple distant columns, not just one centred label.
    assert all(any(min(full[(y*720+x)*3:(y*720+x)*3+3])>180 for y in range(300) for x in range(lo,hi)) for lo,hi in [(0,200),(260,460),(520,720)])
    ff('-i',out,'-frames:v','1',work/'tile_full_preview.png')
    report.append({'test':'Full-screen Chinese tiled text / repetition / audio / full duration','result':'PASS'})
    bottom,r,out=render('tile_bottom','--text-position','tile-bottom','--text-tile-rotation','0','--text-tile-spacing','20')
    assert rows_visible(bottom,0,900)==0 and rows_visible(bottom,900,1080)>200
    assert rows_visible(bottom,1060,1080)==0, 'Bottom margin missing'
    ff('-i',out,'-frames:v','1',work/'tile_bottom_preview.png')
    report.append({'test':'Bottom tiled text limited to one bottom row / margin','result':'PASS'})
    sparse,_,_=render('tile_spacing','--text-position','tile-full','--text-tile-rotation','0','--text-tile-spacing','100')
    assert rows_visible(sparse,0,1080)<rows_visible(full,0,1080)*0.6
    rotated,r,out=render('tile_rotated','--text-position','tile-full','--text-tile-rotation','-30','--text-tile-spacing','20')
    assert sum(abs(x-y)>40 for x,y in zip(rotated,full))>10000
    assert '角度 -30' in r.stdout.decode('utf8')
    ff('-i',out,'-frames:v','1',work/'tile_rotated_preview.png')
    report.append({'test':'Tiled rotation and pixel spacing affect the actual frame','result':'PASS'})
    zero,_,_=render('tile_alpha_zero','--text-position','tile-full','--text-alpha','0')
    base=frame(source)
    assert sum(abs(x-y) for x,y in zip(zero,base))/len(base)<2
    half,_,_=render('tile_alpha_half','--text-position','tile-full','--text-tile-rotation','0','--text-tile-spacing','20','--text-alpha','0.5')
    point=next(i for i in range(0,len(full),3) if min(full[i:i+3])>230)
    assert 165<sum(half[point:point+3])/3<210
    dark,_,_=render('tile_black','--text-position','tile-full','--font-color','black')
    assert rows_visible(dark,0,1080,False)>500, 'Black text bounds measured incorrectly'
    report.append({'test':'Transparent tile layer / alpha 0 and 0.5 / black glyph measurement','result':'PASS'})
    logo=work/'tile_logo.png'
    ff('-f','lavfi','-i','color=c=red:s=160x100,format=rgba','-frames:v','1',logo)
    combined,_,_=render('tile_and_logo','--text-position','tile-full','--image',logo,'--image-alpha','1','--image-position','tl')
    i=(50*720+50)*3
    assert combined[i]>170 and combined[i+1]<80 and rows_visible(combined,400,700)>200
    render('tile_multiline','--text-position','tile-full','--text',"中文 50% '引号' \\ 测试\n第二行",'--font-color','#FFCC88')
    report.append({'test':'Tiled Chinese multiline / special characters / image watermark coexist','result':'PASS'})
    batch=work/'tile_batch';batch.mkdir(exist_ok=True)
    shutil.copyfile(source,batch/'one.mp4');shutil.copyfile(source,batch/'two.mp4')
    result=process(batch,work/'tile_batch_out',*common,'--text-position','tile-full')
    log=result.stdout.decode('utf8')
    assert 'success=2 failed=0' in log and log.count('已生成文字平铺层')==1
    report.append({'test':'Tiled layer prepared once and reused for entire batch','result':'PASS'})
    for option,value,field in [('--text-tile-rotation','361','平铺旋转角度'),('--text-tile-spacing','-1','平铺间距'),('--text-tile-spacing','1001','平铺间距')]:
        r=process(source,work/'tile_invalid',*common,'--text-position','tile-full',option,value,expected=2)
        assert field in r.stderr.decode('utf8') and '允许范围' in r.stderr.decode('utf8')
    render('unused_tile_options','--text-position','center','--text-tile-rotation','9999','--text-tile-spacing','-5')
    process(source,work/'unused_blur_options','--mode','stretch','--blur','-1','--zoom','0')
    report.append({'test':'Field-specific tile ranges / non-tile and unused blur parameters ignored','result':'PASS'})
    result=process(source,work/'tile_cancel',*common,'--text-position','tile-full','--cancel-after-ms','100',expected=130)
    assert not list((work/'tile_cancel').glob('.svh_*')) and not list((work/'tile_cancel').glob('*.mp4'))
    report.append({'test':'Cancel tiled layer preparation / no incomplete output','result':'PASS'})
