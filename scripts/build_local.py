"""Local build wrapper: normalizes Windows environment key casing for MSBuild subprocesses."""
import argparse
import os
import subprocess
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--build',default='build')
p.add_argument('--sdk',help='Optional installed SDK version, for example 10.0.19041.0')
p.add_argument('--mingw',action='store_true')
p.add_argument('--package',action='store_true')
p.add_argument('--ffmpeg-dir',type=Path)
a=p.parse_args()
repo=Path(__file__).resolve().parent.parent
configure=['cmake','-S',str(repo),'-B',a.build]
if a.mingw:configure+=['-G','Ninja','-DCMAKE_BUILD_TYPE=Release']
else:configure+=['-G','Visual Studio 17 2022','-A','x64']
if a.sdk:configure+=['-DCMAKE_SYSTEM_VERSION='+a.sdk]
# Python on Windows collapses duplicate environment names such as PATH and Path.
environment=dict(os.environ)
for cmd in (configure,['cmake','--build',a.build,'--config','Release','--parallel'],['ctest','--test-dir',a.build,'-C','Release','--output-on-failure']):
    subprocess.run(cmd,cwd=repo,env=environment,check=True)

if a.package:
    cmd=['python',str(repo/'scripts/package.py'),'--build',a.build,'--dest','bin']
    if a.ffmpeg_dir:cmd+=['--ffmpeg-dir',str(a.ffmpeg_dir)]
    subprocess.run(cmd,cwd=repo,env=environment,check=True)
