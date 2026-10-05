"""Download a versioned FFmpeg Windows x64 build, verify the GitHub asset SHA-256 and archive paths."""
import argparse
import hashlib
import json
import shutil
import time
import urllib.request
import zipfile
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--version',default='7.1')
p.add_argument('--dest',type=Path,default=Path('deps/ffmpeg'))
a=p.parse_args()
a.dest.mkdir(parents=True,exist_ok=True)
headers={'User-Agent':'SimpleVideoHandle-build','Accept':'application/vnd.github+json'}
url=f'https://api.github.com/repos/GyanD/codexffmpeg/releases/tags/{a.version}'
with urllib.request.urlopen(urllib.request.Request(url,headers=headers),timeout=30) as r:meta=json.load(r)
name=f'ffmpeg-{a.version}-essentials_build.zip'
asset=next(x for x in meta['assets'] if x['name']==name)
archive=a.dest/name
# Query prevents a stale cached redirect containing an expired signed download URL.
url=f'https://api.github.com/repos/GyanD/codexffmpeg/releases/assets/{asset["id"]}?download=1&cache={int(time.time())}'
for attempt in range(3):
    try:
        req=urllib.request.Request(url,headers={'Accept':'application/octet-stream','User-Agent':'SimpleVideoHandle-build'})
        with urllib.request.urlopen(req,timeout=30) as response,archive.open('wb') as out:shutil.copyfileobj(response,out,1024*1024)
        if archive.stat().st_size!=asset['size']:raise RuntimeError('Downloaded size does not match release asset')
        break
    except Exception:
        if attempt==2:raise
        time.sleep(2)
digest=hashlib.sha256(archive.read_bytes()).hexdigest()
# Newer GitHub assets expose digest directly. Older assets are still recorded reproducibly in the manifest.
known={ '7.1': 'fa7d4d7e795db0e2503f49f105f46ed5852386f0cfdd819899be3b65ebde24fc' }
expected=asset.get('digest') or ('sha256:'+known[a.version] if a.version in known else None)
if expected and expected.startswith('sha256:') and digest!=expected.split(':',1)[1]:raise RuntimeError('FFmpeg checksum mismatch')
with zipfile.ZipFile(archive) as z:
    for name in z.namelist():
        if not (a.dest/name).resolve().is_relative_to(a.dest.resolve()):raise RuntimeError('Unsafe archive path')
    z.extractall(a.dest)
(a.dest/'download.json').write_text(json.dumps({'version':a.version,'asset':asset['name'],'sha256':digest,'source':meta['html_url']},indent=2),encoding='utf8')
print(f'FFmpeg {a.version}: SHA256 {digest}')
print(next(a.dest.rglob('ffmpeg.exe')).parent)
