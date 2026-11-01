"""kernel/src/arch/tsc_rate.h をホストで組み、各社の文書の書式の例と照らす。実機の確かめではない。"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CASES = r"""
#include <stdio.h>
#include "arch/tsc_rate.h"

static int failed;

static void expect(const char *name, const struct tsc_rate *r, uint64_t num, uint64_t den) {
    if (num ? r->num == num && r->den == den : !r->num) return;
    printf("FAIL %s: %llu / %llu (%s), want %llu / %llu\n", name, (unsigned long long)r->num,
           (unsigned long long)r->den, r->by, (unsigned long long)num, (unsigned long long)den);
    failed++;
}

#define SEL TSC_AMD_TSC_FREQ_SEL
#define EN TSC_AMD_PSTATE_EN

int main(void) {
    struct tsc_rate r;
    /* PPR 56713, CoreCOF = CpuFid[7:0] / CpuDfsId * 200 MHz */
    tsc_rate_amd(0x19, SEL, EN | 0x08BC, &r);
    expect("19h FID BCh DID 08h", &r, 200000000ull * 0xBC, 8);
    tsc_rate_amd(0x19, SEL, EN | 0x2C10, &r);
    expect("19h the last even DID", &r, 200000000ull * 0x10, 0x2C);
    tsc_rate_amd(0x19, SEL, EN | 0x1BBC, &r);
    expect("19h DID 1Bh is reserved", &r, 0, 0);
    tsc_rate_amd(0x19, SEL, EN | 0x1DBC, &r);
    expect("19h DID 1Dh is reserved", &r, 0, 0);
    tsc_rate_amd(0x19, SEL, EN | 0x070F, &r);
    expect("19h FID 0Fh is reserved", &r, 0, 0);
    /* PPR 57238, CoreCOF = CpuFid[11:0] * 5 MHz; these fields read as the other family's would not fit */
    tsc_rate_amd(0x1A, SEL, EN | 0x258, &r);
    expect("1Ah FID 258h", &r, 5000000ull * 0x258, 1);
    tsc_rate_amd(0x1A, SEL, EN | 0xFFF, &r);
    expect("1Ah FID FFFh", &r, 5000000ull * 0xFFF, 1);
    tsc_rate_amd(0x1A, SEL, EN | (0x7Full << 14) | 0x258, &r);
    expect("1Ah the VID above is not the FID", &r, 5000000ull * 0x258, 1);
    tsc_rate_amd(0x1A, SEL, EN | 0x00F, &r);
    expect("1Ah FID 00Fh is reserved", &r, 0, 0);
    tsc_rate_amd(0x1A, 0, EN | 0x258, &r);
    expect("TscFreqSel 0", &r, 0, 0);
    tsc_rate_amd(0x1A, SEL, 0x258, &r);
    expect("PstateEn 0", &r, 0, 0);
    if (!tsc_amd_family_known(0x19) || !tsc_amd_family_known(0x1A) || tsc_amd_family_known(0x18) || tsc_amd_family_known(0x1B)) {
        printf("FAIL the families the manuals cover\n");
        failed++;
    }
    /* SDM vol. 1 22.7.3: ECX * EBX / EAX */
    tsc_rate_intel(0x20, 2, 168, 25000000, &r);
    expect("15H whole", &r, 25000000ull * 168, 2);
    tsc_rate_intel(0x14, 2, 168, 25000000, &r);
    expect("no leaf 15H", &r, 0, 0);
    tsc_rate_intel(0x20, 0, 168, 25000000, &r);
    expect("15H EAX 0", &r, 0, 0);
    tsc_rate_intel(0x20, 2, 0, 25000000, &r);
    expect("15H EBX 0", &r, 0, 0);
    tsc_rate_intel(0x20, 2, 168, 0, &r);
    expect("15H ECX 0", &r, 0, 0);
    if (failed) return 1;
    printf("ok\n");
    return 0;
}
"""


def find_cc():
    for cc in (ROOT / "tools/msys64/ucrt64/bin/clang.exe", Path("C:/Program Files/LLVM/bin/clang.exe")):
        if cc.exists():
            return cc
    found = shutil.which("clang")
    if not found:
        raise SystemExit("clang がありません")
    return Path(found)


def main():
    cc = find_cc()
    env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
    with tempfile.TemporaryDirectory() as out:
        source = Path(out) / "cases.c"
        source.write_text(CASES, encoding="utf-8", newline="\n")
        exe = Path(out) / "cases.exe"
        subprocess.run([str(cc), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "kernel/src"),
                        str(source), "-o", str(exe)], env=env, check=True)
        result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=30)
    print(result.stdout, end="")
    if result.returncode:
        raise SystemExit(result.returncode)


main()
