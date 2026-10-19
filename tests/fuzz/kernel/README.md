# Kernel parser fuzzing (host side)

libFuzzer harnesses that compile the product `.c` files of `kernel/src` directly (they `#include` them, so
statics are reachable) with a few stubs, under AddressSanitizer and UndefinedBehaviorSanitizer. Nothing here is
linked into the OS and nothing touches `build/`.

| harness | product code | input |
|---|---|---|
| `fuzz_net.c` | `net/net.c`: Ethernet, ARP, IPv4, ICMP, UDP, DHCP, TCP (listen/accept, retransmit), DNS reply | records `[kind][len u16][body]`; IP and L4 checksums are rebuilt so inputs get past the checksum gates |
| `fuzz_dns.c` | `dns_name.h`, `dns_parse_reply` | raw reply bytes; encode/decode round trip |
| `fuzz_vfs.c` | `fs/vfs.c`, `ramfs.c`, `initrd.c` | even first byte: a tar archive for `initrd_load`; odd: a script of path/document operations |
| `fuzz_fat.c` | `fs/fat.c` on a memory disk | byte patches over a golden FAT32 image (BPB, FATs, directories), then an operation script |

`fz.h` is force-included: it replaces `current_task`, `need_resched`, `irq_save/restore` with host versions.
`stubs_*.c` supply the few kernel symbols the code under test calls. `kmalloc` maps to the host heap (limit 256 MB,
above that it returns NULL like an exhausted kernel heap), so ASan sees every product allocation.

## Build and run

Needs clang with the libFuzzer runtime (LLVM for Windows works). From the repository root, in a POSIX shell:

    export FUZZ_OUT=/path/to/scratch            # default: tests/fuzz/kernel/out
    sh tests/fuzz/kernel/build.sh fuzz_net  tests/fuzz/kernel/stubs_net.c
    sh tests/fuzz/kernel/build.sh fuzz_dns  tests/fuzz/kernel/stubs_net.c
    sh tests/fuzz/kernel/build.sh fuzz_vfs  tests/fuzz/kernel/stubs_vfs.c
    sh tests/fuzz/kernel/build.sh fuzz_fat  tests/fuzz/kernel/stubs_vfs.c

    export ASAN_OPTIONS=allocator_may_return_null=1:detect_leaks=0
    mkdir -p $FUZZ_OUT/corp_net
    $FUZZ_OUT/fuzz_net.exe $FUZZ_OUT/corp_net -max_total_time=180 -max_len=4096

Flags used: `-fsanitize=fuzzer,address,undefined -g -O1 -fno-sanitize-recover=undefined
-fno-sanitize=alignment` (the kernel reads packed headers through casts on purpose; x86 allows it). A crash is
written as `crash-*`; reproduce with `fuzz_x.exe crash-file`, minimize with `-minimize_crash=1`.
`FZ_TRACE=1` makes `fuzz_fat` print each operation. On Windows ASan prints "interception_win: unhandled
instruction" once at start; it is harmless.

Run them one at a time (single process); each already saturates a core.

## Not covered yet

ACPI table parsing, ELF validation in `proc_launch`, USB descriptors and RDP/VMBus. `acpi.c` was reviewed by hand
(bounded MADT/MCFG walks); the others depend on large parts of the scheduler/VMM and need extracting the pure
validation first.
