"""修理後の通信admission/fairness境界だけを実製品関数で調べる。"""
from pathlib import Path
import hashlib
import os
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / "build/browser-performance-layout"
out.mkdir(parents=True, exist_ok=True)
source_path = root / "user/libc/webnet.c"
source = source_path.read_text(encoding="utf-8")

def function(signature):
    at = source.index(signature)
    return source[at:source.index("\n}", at) + 2] + "\n"

prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "webnet.h"
#include "webnet_wire.h"
#include "abi.h"
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
static struct n_cpuinfo test_cpu;
static struct n_sysinfo test_memory;
static bool fail_cpu, fail_memory;
int cpu_info(struct n_cpuinfo *info){*info=test_cpu;return fail_cpu?-1:0;}
int sysinfo(struct n_sysinfo *info){*info=test_memory;return fail_memory?-1:0;}
'''
types = source[source.index("#define WORKERS"):source.index("static void request_free(")]
signatures = ["static unsigned worker_budget(", "static bool origin_available(",
              "static unsigned live_workers(", "static struct request **available_class(",
              "static struct request *take_request("]
extracted = types + "".join(function(s) for s in signatures)
cases_path = root / "tests/webnet_scheduler_perf_cases.c"
generated = out / "webnet-scheduler-perf-generated.c"
generated.write_text(prefix + extracted + cases_path.read_text(encoding="utf-8"), encoding="utf-8")
cc = root / "tools/msys64/ucrt64/bin/clang.exe"
exe = out / "webnet-scheduler-perf.exe"
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
command = [str(cc), "-std=gnu11", "-O1", "-Iuser/include", "-Icommon", str(generated), "-o", str(exe)]
with (out / "webnet-scheduler-perf-result.log").open("w", encoding="utf-8") as log:
    for path in [source_path, root / "user/include/webnet.h", root / "user/include/webnet_wire.h", cases_path, Path(__file__)]:
        log.write(str(path.relative_to(root)) + " SHA256 " + hashlib.sha256(path.read_bytes()).hexdigest() + "\n")
    log.write("EXTRACTED_UNMODIFIED_SHA256 " + hashlib.sha256(extracted.encode("utf-8")).hexdigest() + "\n")
    log.write("FUNCTIONS " + repr(signatures) + "\n")
    log.write("COMMAND " + repr(command) + "\n")
    log.flush()
    built = subprocess.run(command, cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    diagnostic = built.stdout.decode("utf-8", errors="replace")
    print(diagnostic, end="")
    log.write(diagnostic + "COMPILE_EXIT " + str(built.returncode) + "\n")
    log.flush()
    if built.returncode:
        raise SystemExit(built.returncode)
    result = subprocess.run([str(exe)], cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    output = result.stdout.decode("utf-8", errors="replace")
    print(output, end="")
    log.write(output + "RUNTIME_EXIT " + str(result.returncode) + "\n")
    raise SystemExit(result.returncode)
