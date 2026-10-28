"""Actual shared storage, real isolated files: cursor/epoch cases, tabs, clears and usage;
then the hot path (getters without reads, one control read per unchanged get)."""
from pathlib import Path
import os
import subprocess
root=Path(__file__).resolve().parent.parent
out=root/'build/goal-20261009'
out.mkdir(parents=True,exist_ok=True)
cc=root/'tools/msys64/ucrt64/bin/clang.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ.get('PATH',''))
for case in ['shared', 'hotpath']:
    exe=out/f'storage-{case}-native.exe'
    subprocess.run([str(cc),'-std=gnu11','-O1','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-parameter','-Wno-sign-compare',
        '-idirafter','user/include','-idirafter','common','-Ithird_party/bearssl/inc','-Ithird_party/bearssl/src',
        f'tests/storage_{case}_native_cases.c','third_party/bearssl/src/hash/sha2small.c',
        'third_party/bearssl/src/codec/enc32be.c','third_party/bearssl/src/codec/dec32be.c','-o',str(exe)],cwd=root,env=env,check=True)
    subprocess.run([str(exe)],cwd=root,env=env,check=True,timeout=300)
