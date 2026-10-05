"""Bundle EXEs and all non-system DLLs using PE import tables, with no third-party Python packages."""
import argparse
import hashlib
import json
import os
import shutil
import struct
from pathlib import Path


def pe_imports(path):
    data=path.read_bytes()
    if data[:2]!=b'MZ': raise RuntimeError(f'Not a Windows executable: {path}')
    pe=struct.unpack_from('<I',data,0x3c)[0]
    if data[pe:pe+4]!=b'PE\0\0': raise RuntimeError(f'Invalid PE header: {path}')
    machine, sections=struct.unpack_from('<HH',data,pe+4)
    if machine!=0x8664: raise RuntimeError(f'Expected Windows x64: {path}')
    opt=pe+24
    opt_size=struct.unpack_from('<H',data,pe+20)[0]
    if struct.unpack_from('<H',data,opt)[0]!=0x20b: raise RuntimeError(f'Expected PE32+: {path}')
    mappings=[]
    for i in range(sections):
        pos=opt+opt_size+i*40
        virtual_size,virtual_address,raw_size,raw_offset=struct.unpack_from('<IIII',data,pos+8)
        mappings.append((virtual_address,max(virtual_size,raw_size),raw_offset))
    def offset(rva):
        for va,size,raw in mappings:
            if va<=rva<va+size:return raw+rva-va
        raise RuntimeError(f'Invalid PE RVA in {path}')
    def name(rva):
        start=offset(rva);return data[start:data.index(b'\0',start)].decode('ascii')
    result=set()
    for index,size,name_offset in ((1,20,12),(13,32,4)):
        rva,total=struct.unpack_from('<II',data,opt+112+index*8)
        if not rva:continue
        pos=offset(rva)
        for _ in range(4096):
            entry=data[pos:pos+size]
            if len(entry)!=size or not any(entry):break
            # Modern PE32+ delay descriptors use RVAs, not VAs.
            result.add(name(struct.unpack_from('<I',entry,name_offset)[0]));pos+=size
    return sorted(result,key=str.lower)


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--build',type=Path,default=Path('build'))
    p.add_argument('--config',default='Release')
    p.add_argument('--dest',type=Path,default=Path('bin'))
    p.add_argument('--ffmpeg-dir',type=Path)
    p.add_argument('--test-report',type=Path)
    a=p.parse_args()
    repo=Path(__file__).resolve().parent.parent
    a.dest=a.dest.resolve();a.dest.mkdir(parents=True,exist_ok=True)
    release=a.build/a.config
    if not (release/'SimpleVideoHandle.exe').is_file():release=a.build
    search=[release.resolve()]
    for exe in ('SimpleVideoHandle.exe','SimpleVideoHandleCLI.exe'):
        src=release/exe
        if not src.is_file():raise RuntimeError(f'Missing built executable: {src}')
        shutil.copy2(src,a.dest/exe)
    ffdir=a.ffmpeg_dir
    if ffdir is None:
        candidate=shutil.which('ffmpeg.exe')
        if candidate:ffdir=Path(candidate).parent
    if ffdir is None:raise RuntimeError('Supply --ffmpeg-dir with ffmpeg.exe and ffprobe.exe')
    ffdir=ffdir.resolve();search+=[ffdir,ffdir.parent,repo/'deps',a.dest]
    for exe in ('ffmpeg.exe','ffprobe.exe'):
        source=ffdir/exe
        if not source.is_file():raise RuntimeError(f'Missing FFmpeg executable: {source}')
        if source.resolve()!=a.dest/exe:shutil.copy2(source,a.dest/exe)
    for item in ('README.md','THIRD_PARTY.md'):
        shutil.copy2(repo/item,a.dest/item)
    if a.test_report:
        shutil.copy2(a.test_report,a.dest/'test-report.json')
    system=Path(os.environ.get('SystemRoot','C:/Windows'))/'System32'
    report={};pending=list(a.dest.glob('*.exe'));visited=set();copied=[]
    while pending:
        exe=pending.pop();key=exe.name.lower()
        if key in visited:continue
        visited.add(key);imports=pe_imports(exe);report[exe.name]=imports
        for dll in imports:
            # API sets are provided by Windows; graphics driver DLLs remain the installed driver's responsibility.
            if dll.lower().startswith(('api-ms-','ext-ms-')) or (system/dll).is_file():continue
            candidates=[p/dll for p in search]
            found=next((p for p in candidates if p.is_file()),None)
            if found is None:raise RuntimeError(f'Missing non-system dependency {dll} required by {exe.name}')
            dest=a.dest/dll
            if found.resolve()!=dest:shutil.copy2(found,dest)
            pending.append(dest)
            if dll not in copied:copied.append(dll)
    license_dir=a.dest/'licenses'/'FFmpeg';license_dir.mkdir(parents=True,exist_ok=True)
    for directory in (ffdir.parent,ffdir):
        for pattern in ('LICENSE*','COPYING*','README*'):
            for file in directory.glob(pattern):
                if file.is_file():shutil.copy2(file,license_dir/file.name)
    report={'architecture':'Windows x64','imports':report,'copied_non_system_dlls':copied}
    (a.dest/'dependencies.json').write_text(json.dumps(report,indent=2),encoding='utf8')
    checksums={str(p.relative_to(a.dest)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(a.dest.rglob('*')) if p.is_file() and p.name!='SHA256.json'}
    (a.dest/'SHA256.json').write_text(json.dumps(checksums,indent=2),encoding='utf8')
    print(f'Bundle ready: {a.dest}\nCopied non-system DLLs: {copied or "none (static runtimes)"}')

if __name__=='__main__':main()
