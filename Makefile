# Nocturne OS build. Run inside the MSYS2 UCRT64 shell (see build.ps1).
.DEFAULT_GOAL := all
CC   := clang
LD   := ld.lld
AR   := llvm-ar
NASM := nasm
PY   := python

BUILD := build
QEMU_MEMORY ?= 2048M
QEMU_CPUS ?= 4
QEMU_ACCEL ?= tcg
QEMU_GPU ?= none
QEMU ?= qemu-system-x86_64
comma := ,
QEMU_GRAPHICS := $(if $(filter virgl,$(QEMU_GPU)),-device virtio-gpu-gl-pci -display sdl$(comma)gl=on,)

# ---------------------------------------------------------------- kernel
KCFLAGS := --target=x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-stack-check \
           -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone -mgeneral-regs-only -mno-sse -mno-mmx \
           -mno-80387 -O2 -g -std=gnu11 -fno-omit-frame-pointer -fno-strict-aliasing \
           -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Ikernel/src -Icommon -MMD -MP
KLDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T kernel/linker.ld --no-dynamic-linker

KSRC_C   := $(shell find kernel/src -name '*.c') common/gfx.c
KSRC_ASM := $(shell find kernel/src -name '*.asm') common/unifont.asm
KOBJ     := $(patsubst %.c,$(BUILD)/k/%.o,$(KSRC_C)) $(patsubst %.asm,$(BUILD)/k/%.asm.o,$(KSRC_ASM))

$(BUILD)/k/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/k/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  NASM $<"
	@$(NASM) -f elf64 -g $< -o $@

$(BUILD)/kernel.elf: $(KOBJ) kernel/linker.ld
	@echo "  LD   $@"
	@$(LD) $(KLDFLAGS) $(KOBJ) -o $@

# ---------------------------------------------------------------- userland
UCFLAGS := --target=x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -O2 -g -std=gnu11 -fno-strict-aliasing -msse2 \
           -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Iuser/include -Icommon -MMD -MP
ULDFLAGS := -m elf_x86_64 -s -nostdlib -static -z max-page-size=0x1000 -T user/user.ld --no-dynamic-linker

LIBC_C   := $(shell find user/libc -name '*.c') common/gfx.c
LIBC_ASM := $(shell find user/libc -name '*.asm') common/unifont.asm
LIBC_OBJ := $(patsubst %.c,$(BUILD)/u/%.o,$(LIBC_C)) $(patsubst %.asm,$(BUILD)/u/%.asm.o,$(LIBC_ASM))

# BearSSL (TLS) is linked like libc: programs only pull in the parts they use
BR_C     := $(shell find third_party/bearssl/src -name '*.c')
BR_OBJ   := $(patsubst %.c,$(BUILD)/br/%.o,$(BR_C)) $(BUILD)/br/tls_roots.o
BRFLAGS  := $(filter-out -W% -MMD -MP,$(UCFLAGS)) -w -Ithird_party/bearssl/inc -Ithird_party/bearssl/src \
            -DBR_USE_URANDOM=1 -DBR_USE_GETENTROPY=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_UNIX_TIME=1 -DBR_USE_WIN32_TIME=0
UCFLAGS  += -Ithird_party/bearssl/inc -Ithird_party/img
UCFLAGS  += -Ithird_party/quickjs
UCFLAGS  += -Ithird_party/lexbor/source -Iports/lexbor/include

# Lexbor's parsing/DOM modules use only native libc. Do not build upstream
# POSIX/Windows filesystem or performance ports, CLI, or unfinished layout.
include third_party/lexbor/sources.mk
LXB_C := $(LXB_VENDOR_C) ports/lexbor/memory.c
LXB_OBJ := $(patsubst %.c,$(BUILD)/lexbor/%.o,$(LXB_C))
LXBFLAGS := $(filter-out -W%,$(UCFLAGS)) -w -fno-addrsig -DLEXBOR_STATIC
$(BUILD)/lexbor/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  LXB  $<"
	@$(CC) $(LXBFLAGS) -c $< -o $@

# Nocturne IOを用いる限定decoder。FFmpegのCLI・OS・network層は取り込まない。
include third_party/ffmpeg/sources.mk
FF_OBJ := $(patsubst %.c,$(BUILD)/ffmpeg/%.o,$(FFMPEG_C))
$(FF_OBJ): third_party/ffmpeg/config.h third_party/ffmpeg/config_components.h third_party/ffmpeg/libavutil/avconfig.h
FFFLAGS := -Iports/ffmpeg/include -Ithird_party/ffmpeg -Ithird_party/ffmpeg/compat/stdbit \
           $(filter-out -W%,$(UCFLAGS)) -w -DHAVE_AV_CONFIG_H -D_ISOC11_SOURCE -D_FILE_OFFSET_BITS=64 \
           -D_LARGEFILE_SOURCE -std=c17 -O3 -fno-math-errno -fno-signed-zeros -mstack-alignment=16
$(BUILD)/ffmpeg/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  FF   $<"
	@$(CC) $(FFFLAGS) -c $< -o $@
$(BUILD)/u/user/libc/media.o: UCFLAGS := -Iports/ffmpeg/include -Ithird_party/ffmpeg $(UCFLAGS)
$(BUILD)/u/user/libc/media_adaptive.o: UCFLAGS := -Iports/ffmpeg/include -Ithird_party/ffmpeg $(UCFLAGS)
$(BUILD)/u/user/libc/media_mse.o: UCFLAGS := -Iports/ffmpeg/include -Ithird_party/ffmpeg $(UCFLAGS)

# プロセス単位の生存メディア割当。既定は固定quotaなし、実OSmalloc失敗で判定。
# FF原文は維持し、allocator/entropyの3接点だけをprivate familyへ接続。
# native decoderと表示frameも同じ課金統計を使う。有限quotaは明示useroverrideのみ。
MEDIA_QUOTA_BYTES ?= SIZE_MAX
.PHONY: media-quota-config
$(BUILD)/media-quota.cfg: media-quota-config
	@mkdir -p $(dir $@)
	@if [ ! -f "$@" ] || [ "$$(cat "$@")" != '$(MEDIA_QUOTA_BYTES)' ]; then \
	    printf '%s\n' '$(MEDIA_QUOTA_BYTES)' > "$@"; \
	    if [ '$(MEDIA_QUOTA_BYTES)' = SIZE_MAX ]; then printf '%s\n' 'media heap: 固定quotaなし（実OS割当、size_t overflow/OOM判定）'; \
	    else printf '%s\n' 'media heap: 明示user quota $(MEDIA_QUOTA_BYTES) bytes'; fi; \
	fi
$(BUILD)/u/user/libc/media_alloc.o: UCFLAGS += -DNMEDIA_ALLOC_LIMIT_BYTES=$(MEDIA_QUOTA_BYTES)
$(BUILD)/u/user/libc/media_alloc.o: $(BUILD)/media-quota.cfg Makefile
$(BUILD)/ffmpeg/third_party/ffmpeg/libavutil/mem.o: FFFLAGS += -DMALLOC_PREFIX=nmedia_ff_
$(BUILD)/ffmpeg/third_party/ffmpeg/libavutil/file_open.o: FFFLAGS += -Dfdopen=nmedia_ff_fdopen
$(BUILD)/ffmpeg/third_party/ffmpeg/libavutil/random_seed.o: FFFLAGS += -Dsetvbuf=nmedia_ff_setvbuf -Dfread=nmedia_ff_fread -Dfclose=nmedia_ff_fclose
$(BUILD)/ffmpeg/third_party/ffmpeg/libavutil/mem.o $(BUILD)/ffmpeg/third_party/ffmpeg/libavutil/file_open.o $(BUILD)/ffmpeg/third_party/ffmpeg/libavutil/random_seed.o: Makefile user/include/media_alloc_private.h user/include/media_stdio_private.h

# QuickJS core only. No OS helper library, CLI or native-module loader.
QJS_C := $(addprefix third_party/quickjs/,quickjs.c dtoa.c libregexp.c libunicode.c cutils.c)
QJS_OBJ := $(patsubst %.c,$(BUILD)/qjs/%.o,$(QJS_C))
QJSFLAGS := $(filter-out -W%,$(UCFLAGS)) -w -fwrapv -funsigned-char -fno-addrsig \
            -DCONFIG_NOCTURNE -DCONFIG_VERSION=\"2026-06-04\"
$(BUILD)/qjs/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  QJS  $<"
	@$(CC) $(QJSFLAGS) -c $< -o $@

# stb_truetype, stb_image, jebp and nanosvg are compiled as they are, without warnings
$(BUILD)/u/user/libc/third_party_%.o: UCFLAGS += -w

# Application chrome uses the bundled font; the kernel compositor and early
# console keep their allocation-free bitmap renderer and unchanged ABI.
$(BUILD)/u/common/gfx.o: UCFLAGS += -DNOCTURNE_USER_FONT

# Checked-in full Unifont bitmap data can be regenerated without external paths.
.PHONY: bitmap-fonts
bitmap-fonts:
	@$(PY) scripts/unifont2bin.py third_party/unifont/unifont_jp-18.0.01.bdf.gz common/unifont.bin

$(BUILD)/k/common/unifont.asm.o $(BUILD)/u/common/unifont.asm.o: common/unifont.bin

$(BUILD)/br/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(BRFLAGS) -c $< -o $@

$(BUILD)/br/tls_roots.c: third_party/ca/ca-bundle.crt scripts/mkroots.py
	@mkdir -p $(dir $@)
	@$(PY) scripts/mkroots.py $< $@

$(BUILD)/br/tls_roots.o: $(BUILD)/br/tls_roots.c
	@$(CC) $(BRFLAGS) -c $< -o $@

APPS     := $(notdir $(basename $(wildcard user/apps/*.c)))
APP_BINS := $(addprefix $(BUILD)/root/bin/,$(APPS))

$(BUILD)/u/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(CC) $(UCFLAGS) -c $< -o $@

# The checked-in copy permits in-OS linking without Python; regenerate on host edits.
user/libc/web/js_bootstrap.inc user/libc/web/js_form_url.inc &: $(wildcard user/libc/web/js_*.js) user/libc/web/js_embed.py third_party/web_streams/ponyfill.mjs third_party/web_streams/upstream.json
	@$(PY) user/libc/web/js_embed.py
$(BUILD)/u/user/libc/web/js.o: user/libc/web/js_bootstrap.inc
$(BUILD)/u/user/libc/web/form_validation.o: user/libc/web/js_form_url.inc
user/libc/web/js_worker_runtime.inc: user/libc/web/js_worker_runtime.js user/libc/web/js_navigator.js user/libc/web/js_dom_exception.js user/libc/web/js_clone.js user/libc/web/js_encoding.js user/libc/web/js_url.js user/libc/web/js_worker_messaging.js user/libc/web/js_worker_embed.py
	@$(PY) user/libc/web/js_worker_embed.py
$(BUILD)/u/user/apps/browserjsworker.o: user/libc/web/js_worker_runtime.inc user/libc/web/js_worker_transfer.h user/libc/web/js_worker_port_lifetime.h
$(BUILD)/u/user/libc/web/js_worker.o: user/libc/web/js_worker.h user/include/js_worker_wire.h user/libc/web/js_worker_port_lifetime.h

$(BUILD)/u/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  NASM $<"
	@$(NASM) -f elf64 -g $< -o $@

# no archiver in the toolchain: lld's --start-lib gives archive semantics to plain objects
LIBC_LINK := $(filter-out %crt0.asm.o,$(LIBC_OBJ)) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(FF_OBJ)
LIBC_RSP := $(BUILD)/libc-objects.list

# Windowsのコマンド長上限を超えないよう、lldとsysrootに同じobject一覧を渡す。
$(LIBC_RSP): $(LIBC_LINK) Makefile
	@$(file >$@,$(LIBC_LINK))

$(BUILD)/root/bin/%: $(BUILD)/u/user/apps/%.o $(LIBC_OBJ) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(FF_OBJ) $(LIBC_RSP) user/user.ld
	@mkdir -p $(dir $@)
	@echo "  LD   $@"
	@$(LD) $(ULDFLAGS) $(BUILD)/u/user/libc/crt0.asm.o $< --start-lib @$(LIBC_RSP) --end-lib -o $@

# ---------------------------------------------------------------- TinyCC (the in-OS C compiler)
TCC_DIR   := third_party/tinycc
TCC_CFLAGS := $(filter-out -W% -MMD -MP -g,$(UCFLAGS)) -w -Iports/tcc -I$(BUILD)/tcc -I$(TCC_DIR)
# runtime objects that TinyCC links into the programs it builds
TCCRT_FLAGS := --target=x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie                -mno-red-zone -O2 -fno-addrsig -fno-asynchronous-unwind-tables -fno-unwind-tables -w                -I$(TCC_DIR)/include -Iuser/include
TCCRT_SRC := $(TCC_DIR)/lib/libtcc1.c $(TCC_DIR)/lib/va_list.c $(TCC_DIR)/lib/alloca.S              $(TCC_DIR)/lib/stdatomic.c $(TCC_DIR)/lib/atomic.S $(TCC_DIR)/lib/builtin.c ports/tcc/runmain.c
TCCRT_OBJ := $(patsubst %,$(BUILD)/tccrt/%.o,$(notdir $(TCCRT_SRC)))

# c2str turns tccdefs.h into C strings compiled into tcc (it runs on the build machine)
$(BUILD)/tcc/c2str.exe: $(TCC_DIR)/conftest.c
	@mkdir -p $(dir $@)
	@$(CC) -DC2STR $< -o $@

$(BUILD)/tcc/tccdefs_.h: $(TCC_DIR)/include/tccdefs.h $(BUILD)/tcc/c2str.exe
	@$(BUILD)/tcc/c2str.exe $< $@

$(BUILD)/tcc/tcc.o: $(wildcard $(TCC_DIR)/*.c $(TCC_DIR)/*.h) ports/tcc/config.h $(BUILD)/tcc/tccdefs_.h
	@echo "  CC   $(TCC_DIR)/tcc.c"
	@$(CC) $(TCC_CFLAGS) -c $(TCC_DIR)/tcc.c -o $@

$(BUILD)/root/bin/tcc: $(BUILD)/tcc/tcc.o $(LIBC_OBJ) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(FF_OBJ) $(LIBC_RSP) user/user.ld
	@mkdir -p $(dir $@)
	@echo "  LD   $@"
	@$(LD) $(ULDFLAGS) $(BUILD)/u/user/libc/crt0.asm.o $< --start-lib @$(LIBC_RSP) --end-lib -o $@

$(BUILD)/tccrt/%.c.o: $(TCC_DIR)/lib/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(TCCRT_FLAGS) -c $< -o $@
$(BUILD)/tccrt/%.S.o: $(TCC_DIR)/lib/%.S
	@mkdir -p $(dir $@)
	@$(CC) $(TCCRT_FLAGS) -c $< -o $@
$(BUILD)/tccrt/runmain.c.o: ports/tcc/runmain.c
	@mkdir -p $(dir $@)
	@$(CC) $(TCCRT_FLAGS) -c $< -o $@

# headers and libraries for compiling inside Nocturne (an extra tree in the initrd)
$(BUILD)/sysroot.stamp: $(LIBC_OBJ) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(FF_OBJ) $(LIBC_RSP) $(TCCRT_OBJ) scripts/mksysroot.sh \
		$(shell find user/include ports/tcc/include third_party/bearssl/inc $(TCC_DIR)/include -name '*.h') \
		$(LXB_HEADERS) ports/lexbor/include/memory.h third_party/lexbor/headers.list \
		third_party/lexbor/LICENSE third_party/lexbor/NOTICE third_party/lexbor/UPSTREAM.json \
		third_party/ffmpeg/LICENSE.md third_party/ffmpeg/COPYING.LGPLv2.1 third_party/ffmpeg/manifest.json third_party/ffmpeg/README.nocturne.md \
		third_party/unifont/OFL-1.1.txt third_party/unifont/NOTICE.txt third_party/unifont/manifest.json \
		common/abi.h common/gfx.h common/gpu_abi.h $(wildcard user/apps/*.c)
	@echo "  SYSROOT"
	@bash scripts/mksysroot.sh $(BUILD)/sysroot $(BUILD)/u/user/libc/crt0.asm.o @$(LIBC_RSP) -- $(TCCRT_OBJ)
	@touch $@

# ---------------------------------------------------------------- images
.PHONY: all kernel user lexbor image run clean test test-quick test-full
.SECONDARY:
all: image

kernel: $(BUILD)/kernel.elf
user: $(APP_BINS) $(BUILD)/root/bin/tcc
lexbor: $(LXB_OBJ)

$(BUILD)/initrd.tar: $(APP_BINS) $(BUILD)/root/bin/tcc $(BUILD)/sysroot.stamp $(BUILD)/sounds.stamp $(shell find rootfs -type f) scripts/mkinitrd.py
	@echo "  TAR  $@"
	@$(PY) scripts/mkinitrd.py $@ rootfs $(BUILD)/sounds $(BUILD)/root $(BUILD)/sysroot

# the music in /home/Music, made by a script
$(BUILD)/sounds.stamp: scripts/mksounds.py
	@echo "  GEN  sounds"
	@rm -rf $(BUILD)/sounds
	@$(PY) scripts/mksounds.py $(BUILD)/sounds
	@touch $@

image: $(BUILD)/kernel.elf $(BUILD)/initrd.tar $(BUILD)/data-blank.vhdx
	@bash scripts/mkimage.sh
	@bash scripts/mkdata.sh

# an empty 2 GiB data disk for Hyper-V (hyperv.ps1 copies it once; it is dynamic, so tiny on disk)
$(BUILD)/data-blank.vhdx: scripts/mkdata.sh
	@rm -f $(BUILD)/data-blank.img
	@bash scripts/mkdata.sh $(BUILD)/data-blank.img 2048
	@qemu-img convert -f raw -O vhdx -o subformat=dynamic $(BUILD)/data-blank.img $@
	@rm -f $(BUILD)/data-blank.img

run: image
	@$(QEMU) -M pc -m $(QEMU_MEMORY) -smp $(QEMU_CPUS) -accel $(QEMU_ACCEL) $(QEMU_GRAPHICS) -drive file=$(BUILD)/nocturne.img,format=raw,if=ide,index=0 \
		-drive file=$(BUILD)/data.img,format=raw,if=ide,index=1 -serial stdio -vga std \
		-audiodev dsound,id=snd -device AC97,audiodev=snd

# the in-OS test suite on a scratch data disk (see scripts/test.py; your data.img is not touched)
test: image
	@$(PY) scripts/test.py
test-quick: image
	@$(PY) scripts/test.py --quick
test-full: image
	@$(PY) scripts/test.py --full

# keeps build/data.img: it is the persistent /data disk (your files, and the agent's key)
clean:
	find $(BUILD) -mindepth 1 -maxdepth 1 ! -name data.img -exec rm -rf {} +

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
