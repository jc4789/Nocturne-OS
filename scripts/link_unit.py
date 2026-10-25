"""Link objects into one relocatable unit, and refuse a unit whose weak references could read NULL.

usage: python link_unit.py out.o members.list others.list NM LD [linker options]

A unit is a set of objects that is linked whole, as a shared library would be. Mesa's Vulkan
dispatch tables name every entrypoint weakly, so that an entrypoint the build leaves out reads NULL.
In the libc link every object is a lazy member, and a weak reference does not pull a member in:
linked as separate members, an implementation nothing names strongly (vk_common_CmdCopyBuffer,
the properties' vk_common_GetPhysicalDeviceProperties2) stays out and its table entry reads NULL.
As one relocatable object the unit is still lazy, so a program that does not use it links none of
it, and a program that does gets all of it; --gc-sections keeps what the tables reach.

A weak reference that leaves the unit is not followed either. If another member of the link
(others.list) defines it, whether a program gets the definition or NULL depends on what else that
program links, so such references are refused here by name."""
import os
import shutil
import subprocess
import sys


def nm(tool, *args):
    """(type, name) of each symbol llvm-nm lists"""
    out = subprocess.run([tool, *args], check=True, capture_output=True, text=True).stdout
    for line in out.splitlines():
        words = line.split()
        if len(words) >= 2 and not line.endswith(":"):
            yield words[-2], words[-1]


def main():
    out, members, others, tool, *ld = sys.argv[1:]
    # CreateProcess would not find ld.lld.exe by itself: "ld.lld" already has an extension
    subprocess.run([*ld, "-r", "@" + members, "-o", out], executable=shutil.which(ld[0]), check=True)
    weak = {name for kind, name in nm(tool, "--undefined-only", out) if kind in ("w", "v")}
    defined = {name for _, name in nm(tool, "--defined-only", "--extern-only", "@" + others)}
    stray = sorted(weak & defined)
    if stray:
        sys.stderr.write(f"{out}: weak references that another member of the link defines, which a program "
                         f"would get or not depending on what else it links:\n  " + "\n  ".join(stray) + "\n")
        os.remove(out)
        sys.exit(1)


main()
