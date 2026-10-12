"""Single focused production AES/EME host pass; never executes QEMU."""
import os
from pathlib import Path
import subprocess
import sys
root = Path(__file__).resolve().parents[1]
os.chdir(root)
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
env = os.environ.copy()
env["PATH"] = str(cc.parent) + os.pathsep + str(root / "tools/msys64/usr/bin") + os.pathsep + env["PATH"]
source = ["tests/crypto_aes_hosttest.c"]
source += [f"third_party/quickjs/{s}.c" for s in ("quickjs", "dtoa", "libregexp", "libunicode", "cutils")]
source += [f"third_party/bearssl/src/hash/{s}.c" for s in ("sha1", "sha2small", "sha2big")]
source += [f"third_party/bearssl/src/codec/{s}.c" for s in ("enc32be", "dec32be", "enc32le", "dec32le", "enc64be", "dec64be")]
source += [f"third_party/bearssl/src/symcipher/{s}.c" for s in ("aes_ct64", "aes_ct64_enc", "aes_ct64_dec", "aes_ct64_cbcenc", "aes_ct64_cbcdec", "aes_ct64_ctr")]
source += ["third_party/bearssl/src/aead/gcm.c", "third_party/bearssl/src/hash/ghash_ctmul64.c"]
exe = root / "build/crypto-aes-20261012.exe"
command = [str(cc), "-O2", "-std=gnu11", "-fwrapv", "-funsigned-char", "-D_GNU_SOURCE", '-DCONFIG_VERSION="Nocturne-test"', "-Ithird_party/quickjs", "-Ithird_party/bearssl/inc", "-Ithird_party/bearssl/src", *source, "-lbcrypt", "-lm", "-o", str(exe)]
result = subprocess.run(command, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
output = result.stdout + result.stderr
if result.returncode == 0:
    result = subprocess.run([str(exe), *sys.argv[1:]], env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
    output += result.stdout + result.stderr
Path("build/crypto-aes-20261012.log").write_text(output, encoding="utf-8")
print(output)
sys.exit(result.returncode)
