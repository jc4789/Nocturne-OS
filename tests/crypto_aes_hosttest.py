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
worker_body = "--worker-body" in sys.argv[1:]
source = ["tests/crypto_aes_hosttest.c"]
if worker_body:
    source += ["tests/worker_stream_hosttest.c", "tests/sharedworker_hosttest.c", "user/libc/web/js_encoding.c", "user/libc/web/util.c"]
source += [f"third_party/quickjs/{s}.c" for s in ("quickjs", "dtoa", "libregexp", "libunicode", "cutils")]
source += [f"third_party/bearssl/src/hash/{s}.c" for s in ("sha1", "sha2small", "sha2big")]
source += [f"third_party/bearssl/src/codec/{s}.c" for s in ("enc32be", "dec32be", "enc32le", "dec32le", "enc64be", "dec64be")]
source += [f"third_party/bearssl/src/symcipher/{s}.c" for s in ("aes_ct64", "aes_ct64_enc", "aes_ct64_dec", "aes_ct64_cbcenc", "aes_ct64_cbcdec", "aes_ct64_ctr")]
source += ["third_party/bearssl/src/aead/gcm.c", "third_party/bearssl/src/hash/ghash_ctmul64.c"]
name = "worker-body-native-20261012" if worker_body else "crypto-aes-20261012"
exe = root / ("build/" + name + ".exe")
options = ["-DWORKER_BODY_NATIVE", "-DWORKER_BODY_PARENT", "-DCONFIG_NOCTURNE", "-ffunction-sections", "-fdata-sections", "-include", "tests/worker_host_include/platform.h", "-Itests/worker_host_include", "-Ibuild/nocturne-audit/url-host-include", "-idirafter", "user/include", "-Icommon", "-Wl,--gc-sections", "-Wl,--stack,8388608"] if worker_body else []
command = [str(cc), "-O2", "-std=gnu11", "-fwrapv", "-funsigned-char", "-D_GNU_SOURCE", '-DCONFIG_VERSION="Nocturne-test"', "-Ithird_party/quickjs", "-Ithird_party/bearssl/inc", "-Ithird_party/bearssl/src", *options, *source, "-lbcrypt", "-lm", "-o", str(exe)]
result = subprocess.run(command, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
output = result.stdout + result.stderr
output += "COMPILE_EXIT " + str(result.returncode) + "\n"
if result.returncode == 0:
    arguments = ["--fetch-body"] if worker_body else sys.argv[1:]
    result = subprocess.run([str(exe), *arguments], env=env, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
    output += result.stdout + result.stderr
    output += "RUNTIME_EXIT " + str(result.returncode) + "\n"
Path("build/" + name + ".log").write_text(output, encoding="utf-8")
print(output)
sys.exit(result.returncode)
