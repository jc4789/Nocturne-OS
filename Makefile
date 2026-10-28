# Nocturne OS build. Run inside the MSYS2 UCRT64 shell (see build.ps1).
.DEFAULT_GOAL := all
CC   := clang
LD   := ld.lld
AR   := llvm-ar
NM   := llvm-nm
NASM := nasm
PY   := python

BUILD := build
# Where the system's parts are made before scripts/layout.py places them on C: every program, the
# licences that go to C:\Nocturne\Licenses, and C:\Nocturne\SDK (scripts/mksdk.sh)
PROGRAMS := $(BUILD)/programs
LICENSES := $(BUILD)/licenses
SDK      := $(BUILD)/sdk
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
# uACPI runs the firmware's AML inside the kernel (third_party/uacpi/README.nocturne.md)
UACPI_C  := $(wildcard third_party/uacpi/source/*.c)
KCFLAGS  += -Ithird_party/uacpi/include
KOBJ     := $(patsubst %.c,$(BUILD)/k/%.o,$(KSRC_C) $(UACPI_C)) $(patsubst %.asm,$(BUILD)/k/%.asm.o,$(KSRC_ASM))

$(BUILD)/k/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(CC) $(KCFLAGS) -c $< -o $@

$(BUILD)/k/third_party/uacpi/%.o: third_party/uacpi/%.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(CC) $(filter-out -W%,$(KCFLAGS)) -w -c $< -o $@

# The build host's tool for the volume (host/store/store.c): it makes the boot disk and puts the
# system on drive C: with the kernel's own store code, host/store/shim standing in for the kernel.
STORE_C := host/store/store.c $(addprefix kernel/src/fs/,names.c volume.c space.c catalog.c namespace.c) \
           kernel/src/lib/crc32c.c
$(BUILD)/host/store.exe: $(STORE_C) $(wildcard host/store/shim/*.h host/store/shim/*/*.h kernel/src/fs/*.h) \
                         common/store_format.h common/abi.h
	@mkdir -p $(dir $@)
	@echo "  CC   $@"
	@$(CC) -O2 -g -std=gnu11 -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -Ihost/store/shim -Ikernel/src \
	    -Icommon $(STORE_C) -o $@

# Limine's installer, built from the source in its release (tools/limine): the release's limine.exe
# cannot reach past 2 GiB of the boot disk (host/limine/seek64.h)
$(BUILD)/host/limine.exe: tools/limine/limine.c tools/limine/limine-bios-hdd.h host/limine/seek64.h
	@mkdir -p $(dir $@)
	@echo "  CC   $@"
	@$(CC) -O2 -std=gnu99 -w -include host/limine/seek64.h -Itools/limine $< -o $@

$(BUILD)/k/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  NASM $<"
	@$(NASM) -f elf64 -g $< -o $@

$(BUILD)/kernel.elf: $(KOBJ) kernel/linker.ld
	@echo "  LD   $@"
	@$(LD) $(KLDFLAGS) $(KOBJ) -o $@

# ---------------------------------------------------------------- userland
# The system's programs never unwind their stacks, so they carry no unwind tables (user.ld keeps
# the tables for the C++ exceptions of programs that clang builds in Nocturne). Each function and
# variable has a section of its own, so that --gc-sections leaves out of a program whatever of the
# libc and the libraries it does not use, not only the objects it does not use.
UCFLAGS := --target=x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -O2 -g -std=gnu11 -fno-strict-aliasing -msse2 -fomit-frame-pointer \
           -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections \
           -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Iuser/include -Icommon -MMD -MP
ULDFLAGS := -m elf_x86_64 -s -nostdlib -static -z max-page-size=0x1000 -T user/user.ld --no-dynamic-linker

LIBC_C   := $(shell find user/libc -name '*.c') common/gfx.c
LIBC_ASM := $(shell find user/libc -name '*.asm') common/unifont.asm
LIBC_OBJ := $(patsubst %.c,$(BUILD)/u/%.o,$(LIBC_C)) $(patsubst %.asm,$(BUILD)/u/%.asm.o,$(LIBC_ASM))

# BearSSL (TLS) is linked like libc: programs only pull in the parts they use
BR_C     := $(shell find third_party/bearssl/src -name '*.c')
BR_OBJ   := $(patsubst %.c,$(BUILD)/br/%.o,$(BR_C)) $(BUILD)/br/tls_roots.o
BRFLAGS  := $(filter-out -W% -MMD -MP,$(UCFLAGS)) -w -Ithird_party/bearssl/inc -Ithird_party/bearssl/src \
            -DBR_USE_URANDOM=0 -DBR_USE_NOCTURNE_RAND=1 -DBR_USE_GETENTROPY=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_UNIX_TIME=1 -DBR_USE_WIN32_TIME=0
UCFLAGS  += -Ithird_party/bearssl/inc -Ithird_party/img
UCFLAGS  += -Ithird_party/quickjs
UCFLAGS  += -Ithird_party/lexbor/source -Iports/lexbor/include
UCFLAGS  += -Ithird_party/freetype/include

# Memory-only WOFF/WOFF2/OpenType decoder. Upstream filesystem ports, CLI,
# encoders and unrelated font drivers are deliberately not linked.
FT_C := $(addprefix third_party/freetype/src/,base/ftbase.c base/ftinit.c base/ftdebug.c base/ftmm.c base/ftbitmap.c \
        truetype/truetype.c cff/cff.c psaux/psaux.c psnames/psnames.c \
        pshinter/pshinter.c sfnt/sfnt.c smooth/smooth.c gzip/ftgzip.c)
BROTLI_C := $(wildcard third_party/brotli/c/common/*.c third_party/brotli/c/dec/*.c)
FT_OBJ := $(patsubst %.c,$(BUILD)/font/%.o,$(FT_C) $(BROTLI_C))
FTFLAGS := $(filter-out -W%,$(UCFLAGS)) -w -Iports/freetype -Ithird_party/brotli/c/include \
           -DFT2_BUILD_LIBRARY -DFT_CONFIG_OPTIONS_H='"nocturne-ftoption.h"' \
           -DFT_CONFIG_MODULES_H='"nocturne-ftmodule.h"' -DBROTLI_BUILD_PORTABLE
$(BUILD)/u/user/libc/third_party_ttf.o: UCFLAGS += $(filter -Iports/freetype -DFT%,$(FTFLAGS))
# HTTP's br content coding shares the same brotli decoder objects.
$(BUILD)/u/user/libc/third_party_brotli.o: UCFLAGS += -Ithird_party/brotli/c/include
.PHONY: font-decoders
font-decoders: $(FT_OBJ) $(BUILD)/u/user/libc/font.o $(BUILD)/u/user/libc/third_party_ttf.o
FONT_VENDOR_INPUTS := $(wildcard third_party/freetype/src/*/*.c third_party/freetype/src/*/*.h \
    third_party/freetype/src/*/*/*.c third_party/freetype/src/*/*/*.h \
    third_party/freetype/include/freetype/*.h third_party/freetype/include/freetype/*/*.h \
    third_party/brotli/c/common/*.h third_party/brotli/c/dec/*.h third_party/brotli/c/include/brotli/*.h)
$(BUILD)/font/%.o: %.c ports/freetype/nocturne-ftoption.h ports/freetype/nocturne-ftmodule.h $(FONT_VENDOR_INPUTS)
	@mkdir -p $(dir $@)
	@echo "  FONT $<"
	@$(CC) $(FTFLAGS) -c $< -o $@
$(BUILD)/font-container-native.exe: tests/font_container_native.c tests/font_container_native_host.h user/libc/font.c user/libc/font_sfnt.h user/libc/third_party_ttf.c third_party/img/stb_truetype.h $(FT_C) $(BROTLI_C) $(FONT_VENDOR_INPUTS) ports/freetype/nocturne-ftoption.h ports/freetype/nocturne-ftmodule.h
	@$(CC) -O2 -std=gnu11 -w -D_SSIZE_T_DEFINED=1 -idirafter user/include -Icommon -Ithird_party/img -Ithird_party/freetype/include \
	    -Iports/freetype -Ithird_party/brotli/c/include -DFT2_BUILD_LIBRARY -include tests/font_container_native_host.h \
	    -DFT_CONFIG_OPTIONS_H='"nocturne-ftoption.h"' -DFT_CONFIG_MODULES_H='"nocturne-ftmodule.h"' \
	    -DBROTLI_BUILD_PORTABLE tests/font_container_native.c user/libc/font.c user/libc/third_party_ttf.c $(FT_C) $(BROTLI_C) -o $@

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
FF_C_OBJ := $(patsubst %.c,$(BUILD)/ffmpeg/%.o,$(FFMPEG_C))
FF_ASM_OBJ := $(patsubst %.asm,$(BUILD)/ffmpeg/%.asm.o,$(FFMPEG_ASM))
FF_OBJ := $(FF_C_OBJ) $(FF_ASM_OBJ)
$(FF_C_OBJ): third_party/ffmpeg/config.h third_party/ffmpeg/config_components.h third_party/ffmpeg/libavutil/avconfig.h
# FFmpeg x86-64 SIMD (NASM, ELF64, non-PIC: CONFIG_PIC 0). 実行時にlibavutil/x86/cpu.cがcpuid/xgetbvで選択する。
# config.asm は -P で先頭取込、-I はNASMの仕様で末尾 / が必要。AVX-512は構成で無効。
FFASMFLAGS := -f elf64 -I third_party/ffmpeg/ -Pthird_party/ffmpeg/config.asm
$(FF_ASM_OBJ): third_party/ffmpeg/config.asm third_party/ffmpeg/config_components.asm
$(BUILD)/ffmpeg/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  FFAS $<"
	@$(NASM) $(FFASMFLAGS) -I $(dir $<) -MD $(@:.o=.d) -o $@ $<
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
$(BUILD)/u/user/libc/media_rate.o: UCFLAGS := -Iports/ffmpeg/include -Ithird_party/ffmpeg $(UCFLAGS)

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
$(BUILD)/ffmpeg/third_party/ffmpeg/libavutil/mem.o: Makefile user/include/media_alloc_private.h

# QuickJS core only. No OS helper library, CLI or native-module loader.
QJS_C := $(addprefix third_party/quickjs/,quickjs.c dtoa.c libregexp.c libunicode.c cutils.c)
QJS_OBJ := $(patsubst %.c,$(BUILD)/qjs/%.o,$(QJS_C))
QJSFLAGS := $(filter-out -W%,$(UCFLAGS)) -w -fwrapv -funsigned-char -fno-addrsig \
            -DCONFIG_NOCTURNE -DCONFIG_VERSION=\"2026-06-04\"
$(BUILD)/qjs/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  QJS  $<"
	@$(CC) $(QJSFLAGS) -c $< -o $@

# Expat: XML tokens, the internal DTD subset and entity replacement only
# (third_party/expat/README.nocturne.md). Namespaces, the DOM and the
# parsererror document belong to user/libc/web/xml.c.
EXPAT_C := $(addprefix third_party/expat/lib/,xmlparse.c xmlrole.c xmltok.c)
EXPAT_OBJ := $(patsubst %.c,$(BUILD)/expat/%.o,$(EXPAT_C))
EXPATFLAGS := $(filter-out -W%,$(UCFLAGS)) -w -fno-addrsig -Iports/expat
$(BUILD)/expat/%.o: %.c ports/expat/expat_config.h
	@mkdir -p $(dir $@)
	@echo "  XML  $<"
	@$(CC) $(EXPATFLAGS) -c $< -o $@
$(BUILD)/u/user/libc/web/xml.o: UCFLAGS += -Ithird_party/expat/lib

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
CPP_APPS := $(notdir $(basename $(wildcard user/apps/*.cpp)))
CPP_BINS := $(addprefix $(PROGRAMS)/,$(CPP_APPS))
APP_BINS := $(addprefix $(PROGRAMS)/,$(APPS)) $(CPP_BINS)

$(BUILD)/u/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(CC) $(UCFLAGS) -c $< -o $@

# The checked-in copy permits in-OS linking without Python; regenerate on host edits.
user/libc/web/js_bootstrap.inc: $(wildcard user/libc/web/js_*.js) user/libc/web/js_embed.py third_party/web_streams/ponyfill.mjs third_party/web_streams/upstream.json
	@$(PY) user/libc/web/js_embed.py
$(BUILD)/u/user/libc/web/js.o: user/libc/web/js_bootstrap.inc
# 原UCD／generator変更時に表を正規再生成。guest内linkは保全された表を使う。
user/libc/web/unicode_grapheme_data.h: scripts/unicode_grapheme.py scripts/ucd.py third_party/unicode/18.0.0/GraphemeBreakProperty.txt third_party/unicode/18.0.0/DerivedCoreProperties.txt third_party/unicode/18.0.0/emoji-data.txt
	@$(PY) scripts/unicode_grapheme.py
$(BUILD)/u/user/libc/web/layout.o: user/libc/web/unicode_grapheme_data.h
user/libc/wctype_data.h: scripts/unicode_wctype.py scripts/ucd.py third_party/unicode/18.0.0/UnicodeData.txt third_party/unicode/18.0.0/PropList.txt third_party/unicode/18.0.0/DerivedCoreProperties.txt
	@$(PY) scripts/unicode_wctype.py
$(BUILD)/u/user/libc/wctype.o: user/libc/wctype_data.h
third_party/lexbor/source/lexbor/unicode/idna_validity_res.h: scripts/unicode_idna.py scripts/ucd.py third_party/unicode/18.0.0/UnicodeData.txt
	@$(PY) scripts/unicode_idna.py
$(BUILD)/lexbor/third_party/lexbor/source/lexbor/unicode/idna.o: third_party/lexbor/source/lexbor/unicode/idna_validity_res.h
user/libc/web/js_worker_runtime.inc: user/libc/web/js_worker_runtime.js user/libc/web/js_console.js user/libc/web/js_navigator.js user/libc/web/js_dom_exception.js user/libc/web/js_atob.js user/libc/web/js_error_event.js user/libc/web/js_promise_rejection.js user/libc/web/js_performance.js user/libc/web/js_clone.js user/libc/web/js_offscreen_canvas.js user/libc/web/js_opfs.js user/libc/web/js_encoding.js user/libc/web/js_url.js user/libc/web/js_worker_messaging.js user/libc/web/js_html_safety.js user/libc/web/js_sanitizer_constants.js user/libc/web/js_streams.js user/libc/web/js_blob.js user/libc/web/js_crypto.js user/libc/web/js_formdata.js user/libc/web/js_wasm.js user/libc/web/js_wasm.h third_party/web_streams/ponyfill.mjs third_party/web_streams/upstream.json user/libc/web/js_worker_embed.py
	@$(PY) user/libc/web/js_worker_embed.py
$(BUILD)/u/user/apps/browserjsworker.o: user/libc/web/js_worker_runtime.inc user/libc/web/js_worker_transfer.h user/libc/web/js_worker_port_lifetime.h
$(BUILD)/u/user/libc/web/js_worker.o: user/libc/web/js_worker.h user/include/js_worker_wire.h user/libc/web/js_worker_port_lifetime.h

$(BUILD)/u/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  NASM $<"
	@$(NASM) -f elf64 -g $< -o $@

# no archiver in the toolchain: lld's --start-lib gives archive semantics to plain objects
# Keep C++/WABT as individual lazy archive members, including in the in-OS
# SDK. A C application or TinyCC itself does not pull the interpreter or
# C++ ABI unless a reachable browser/JS object references it.
WABT_SOURCES := $(shell $(PY) scripts/build_cxx.py wabt-sources)
WABT_SRC := $(addprefix third_party/wabt/src/,$(WABT_SOURCES))
WABT_OBJ := $(addprefix $(BUILD)/wabt/,$(addsuffix .o,$(WABT_SOURCES)))
# Nocturne's own interpreter parts (the x86-64 tier) live under ports/wabt.
WABT_PORT_SOURCES := $(shell $(PY) scripts/build_cxx.py wabt-port-sources)
WABT_PORT_SRC := $(addprefix ports/wabt/,$(WABT_PORT_SOURCES))
WABT_OBJ += $(addprefix $(BUILD)/wabt/,$(addsuffix .o,$(WABT_PORT_SOURCES)))
WASM2C_SOURCES := $(shell $(PY) scripts/build_cxx.py wasm2c-sources)
WASM2C_SRC := $(addprefix third_party/wabt/src/,$(WASM2C_SOURCES))
WASM2C_OBJ := $(addprefix $(BUILD)/wabt/,$(addsuffix .o,$(WASM2C_SOURCES)))
WASM_RT_SRC := third_party/wabt/wasm2c/wasm-rt-impl.c third_party/wabt/wasm2c/wasm-rt-mem-impl.c ports/wabt/wasm_rt_nocturne.c
WASM_RT_OBJ := $(patsubst %.c,$(BUILD)/u/%.o,$(WASM_RT_SRC))
WASM_RT_INPUTS := $(wildcard third_party/wabt/wasm2c/* ports/wabt/include/*.h)
CXX_RUNTIME_OBJ := $(addprefix $(BUILD)/cxx/libcxx/,algorithm.o hash.o memory.o string.o vector.o verbose_abort.o)
WASM_BRIDGE := $(BUILD)/u/user/libc/web/js_wasm.cpp.o
WASM_LINK := $(WASM_BRIDGE) $(WABT_OBJ) $(WASM2C_OBJ) $(BUILD)/tcc/libtcc.o $(WASM_RT_OBJ) $(BUILD)/u/ports/wabt/wasm_rt_symbols.o
# nvrm: NVIDIA's RM as a driver app (ports/nvidia/README.md). init starts it only when an
# NVIDIA display function is present, so other machines just carry the file.
include ports/nvidia/nvrm.mk
include ports/mesa/mesa.mk
# The C++ runtime of every program. LLVM's libraries, which llvmpipe compiles shaders with
# (ports/mesa/llvmpipe.mk), are built against the LLVM port's libc++ 23 with its libc++abi and
# libunwind, so when they are linked every program has that runtime; otherwise the libc++ 18
# subset of scripts/build_cxx.py with Nocturne's own ABI (user/libc/cxxabi.cpp). A program never
# has both: the C++ compiled here with libc++ 18's headers uses only what libc++ 23 still provides.
CXX_RUNTIME_LINK := $(if $(LLVMPIPE),$(LLVM_RUNTIMES)/Lib/libc++.a $(LLVM_RUNTIMES)/Lib/libunwind.a,\
    $(BUILD)/u/user/libc/cxxabi.cpp.o $(CXX_RUNTIME_OBJ))
# Zink and NVK (with its Rust archive and libnvrm) are lazy members like the rest: only a program
# that draws with OpenGL pulls them in.
LIBC_LINK := $(filter-out %crt0.asm.o,$(LIBC_OBJ)) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(EXPAT_OBJ) $(FF_OBJ) $(FT_OBJ) $(WASM_LINK) \
    $(CXX_RUNTIME_LINK) $(MESA_OBJ) $(ZINK_OBJ) $(NVK_OBJ) $(NVRM_CLIENT_OBJ)
# Programs are linked from the libc's members and, with llvmpipe, LLVM's libraries. The SDK's
# libc.a is made from the same list: a program linked inside Nocturne reaches llvmpipe through
# the same members and needs what they need.
LIBC_RSP := $(BUILD)/libc-objects.list

# Windowsのコマンド長上限を超えないよう、lldとSDKにobject一覧をファイルで渡す。
$(LIBC_RSP): $(LIBC_LINK) $(LLVM_JIT_LIBS) Makefile
	@$(file >$@,$(LIBC_LINK) $(LLVM_JIT_LIBS))

# A unit's check (scripts/link_unit.py) reads the symbols of every other member.
$(NVK_UNIT): | $(filter-out $(NVK_UNIT),$(LIBC_LINK))

$(PROGRAMS)/%: $(BUILD)/u/user/apps/%.o $(LIBC_OBJ) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(FF_OBJ) $(LIBC_RSP) user/user.ld
	@mkdir -p $(dir $@)
	@echo "  LD   $@"
	@$(LD) $(ULDFLAGS) --gc-sections $(BUILD)/u/user/libc/crt0.asm.o $< --start-lib @$(LIBC_RSP) --end-lib -o $@

# C++ is an additional host cross-build path; C/TinyCC keep their own flags.
$(BUILD)/u/tests/media_cenc_test.o: UCFLAGS := -Iports/ffmpeg/include -Ithird_party/ffmpeg $(UCFLAGS)
$(BUILD)/u/tests/media_cenc_test.o: user/libc/media_mse.c
$(BUILD)/u/tests/nvk_contracttest.o: UCFLAGS += -Ithird_party/mesa/include
$(BUILD)/tests/nvk_contracttest: private ULDFLAGS += --gc-sections
# nvrm's request server exactly as nvrm links it, against the test's fake RM OS layer.
$(BUILD)/u/tests/nvrm_servetest.o: UCFLAGS += $(NVRM_CLIENT_INC)
$(BUILD)/tests/nvrm_servetest: private TEST_OBJ := $(NVRM_OUT)/os/nvrm_serve.o
$(BUILD)/tests/nvrm_servetest: $(NVRM_OUT)/os/nvrm_serve.o
$(BUILD)/tests/media_cenc_test $(BUILD)/tests/nvk_contracttest $(BUILD)/tests/nvrm_servetest: $(BUILD)/tests/%: $(BUILD)/u/tests/%.o $(LIBC_OBJ) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(FF_OBJ) $(LIBC_RSP) user/user.ld
	@mkdir -p $(dir $@)
	@$(LD) $(ULDFLAGS) $(BUILD)/u/user/libc/crt0.asm.o $< $(TEST_OBJ) --start-lib @$(LIBC_RSP) --end-lib -o $@

CXX_HEADERS := $(shell find user/include -type f) common/abi.h common/gfx.h
# One producer owns the archive and all generated members. A missing member
# re-enters the incremental builder once even when its archive still exists.
$(BUILD)/cxx/libc++.a $(CXX_RUNTIME_OBJ) &: scripts/build_cxx.py $(CXX_HEADERS)
	@$(PY) scripts/build_cxx.py library

WABT_INPUTS := $(WABT_SRC) $(WABT_PORT_SRC) ports/wabt/include/wasm-rt-nocturne.h $(wildcard third_party/wabt/include/wabt/* third_party/wabt/include/wabt/interp/* ports/wabt/include/wabt/*) third_party/wabt/UPSTREAM.json
$(BUILD)/wabt/libwabt.a $(WABT_OBJ) &: scripts/build_cxx.py $(WABT_INPUTS) $(BUILD)/cxx/libc++.a
	@$(PY) scripts/build_cxx.py wabt

$(BUILD)/wabt/libwasm2c.a $(WASM2C_OBJ) &: scripts/build_cxx.py $(WASM2C_SRC) $(wildcard third_party/wabt/src/template/*) third_party/wabt/scripts/gen-wasm2c-templates.cmake $(WABT_INPUTS) $(BUILD)/cxx/libc++.a
	@$(PY) scripts/build_cxx.py wasm2c

$(WASM_RT_OBJ) $(BUILD)/u/ports/wabt/wasm_rt_symbols.o: UCFLAGS += -Iports/wabt/include -Ithird_party/wabt/wasm2c -Ithird_party/tinycc
$(WASM_RT_OBJ) $(BUILD)/u/ports/wabt/wasm_rt_symbols.o: $(WASM_RT_INPUTS)
$(BUILD)/wabt/libwasm-rt.a: $(WASM_RT_OBJ)
	@$(PY) scripts/build_cxx.py archive $@ $^

$(WASM_BRIDGE): user/libc/web/js_wasm.cpp $(BUILD)/wabt/libwabt.a $(CXX_HEADERS) third_party/quickjs/quickjs.h
	@echo "  CXX  $<"
	@$(PY) scripts/build_cxx.py compile $< -o $@ -I ports/wabt/include -I third_party/wabt/include -I third_party/quickjs -I third_party/tinycc -I third_party/wabt/wasm2c

$(BUILD)/u/third_party/wabt/src/tools/wasm2c.cc.o: third_party/wabt/src/tools/wasm2c.cc $(BUILD)/wabt/libwasm2c.a $(BUILD)/wabt/libwabt.a
	@$(PY) scripts/build_cxx.py compile $< -o $@ -I ports/wabt/include -I third_party/wabt/include

APP_BINS += $(PROGRAMS)/wasm2c
$(PROGRAMS)/wasm2c: $(BUILD)/u/third_party/wabt/src/tools/wasm2c.cc.o $(LIBC_RSP) $(BUILD)/u/user/libc/crt0.asm.o user/cxx.ld user/user.ld
	@mkdir -p $(dir $@)
	@echo "  LD++ $@"
	@$(LD) $(patsubst user/user.ld,user/cxx.ld,$(ULDFLAGS)) --gc-sections $(BUILD)/u/user/libc/crt0.asm.o $< --start-lib @$(LIBC_RSP) --end-lib -o $@

# Go programs (ports/go/README.md), built when GO names a Go toolchain with the
# Nocturne port: make GO=/path/to/go/bin/go.exe. Without GO, a toolchain in
# tools/go (a copy or a link) is used; likewise TYPESCRIPT and tools/TypeScript.
# Desktop apps are written in Go and TypeScript, so a full system needs both.
GO ?= $(if $(wildcard tools/go/bin/go.exe),$(CURDIR)/tools/go/bin/go.exe)
TYPESCRIPT ?= $(if $(wildcard tools/TypeScript/tsc/go.mod),tools/TypeScript)
ifneq ($(GO),)
# the MSYS2 shell may not have the Windows folders Go keeps its cache in
GOCACHE ?= $(CURDIR)/$(BUILD)/go-cache
export GOCACHE
GO_APPS  := $(notdir $(wildcard ports/go/apps/*))
GO_BINS  := $(addprefix $(PROGRAMS)/,$(GO_APPS))
GO_FILES := $(GO_BINS) $(LICENSES)/go/LICENSE
$(GO_BINS): $(PROGRAMS)/%: ports/go/apps/%/main.go $(wildcard ports/go/ui/*.go) ports/go/go.mod ports/go/nocturne.patch
	@mkdir -p $(dir $@)
	@echo "  GO   $@"
	@cd ports/go/apps/$* && GOOS=nocturne GOARCH=amd64 CGO_ENABLED=0 "$(GO)" build -trimpath -o "$(CURDIR)/$@" main.go
$(LICENSES)/go/LICENSE:
	@mkdir -p $(dir $@)
	@cp "$$("$(GO)" env GOROOT)/LICENSE" $@
# TYPESCRIPT: Microsoft's TypeScript repository (a path below this folder, the
# clone left at its commit) -> tsc, TypeScript's compiler written in Go (tsc/).
# ports/typescript/nocturne.patch is applied to copies of the files it touches
# under $(BUILD)/typescript and given to Go as an overlay; the clone is not changed.
ifneq ($(TYPESCRIPT),)
TS_SRC := $(TYPESCRIPT)/tsc/go.mod $(TYPESCRIPT)/tsc/go.sum $(shell find $(TYPESCRIPT)/tsc/cmd/tsc \
          $(TYPESCRIPT)/tsc/internal -type f ! -name '*_test.go' ! -path '*/testdata/*')
TS_PATCHED := $(shell sed -n 's|^+++ b/||p' ports/typescript/nocturne.patch)
TS_OVERLAY := $(BUILD)/typescript/overlay.json
$(TS_OVERLAY): ports/typescript/nocturne.patch $(wildcard $(addprefix $(TYPESCRIPT)/,$(TS_PATCHED)))
	@rm -rf $(BUILD)/typescript/src
	@for f in $(TS_PATCHED); do mkdir -p $(BUILD)/typescript/src/$$(dirname $$f); \
	  if [ -f $(TYPESCRIPT)/$$f ]; then cp $(TYPESCRIPT)/$$f $(BUILD)/typescript/src/$$f; fi; done
	@patch -s -p1 -d $(BUILD)/typescript/src < $<
	@{ printf '{"Replace":{'; sep=; for f in $(TS_PATCHED); do \
	  printf '%s"%s":"%s"' "$$sep" "$$(cygpath -m "$(CURDIR)/$(TYPESCRIPT)/$$f")" "$$(cygpath -m "$(CURDIR)/$(BUILD)/typescript/src/$$f")"; \
	  sep=,; done; printf '}}\n'; } > $@
GO_FILES += $(PROGRAMS)/tsc $(LICENSES)/typescript/LICENSE.txt
$(PROGRAMS)/tsc: $(TS_SRC) $(TS_OVERLAY) ports/go/nocturne.patch
	@mkdir -p $(dir $@)
	@echo "  GO   $@"
	@cd $(TYPESCRIPT)/tsc && GOOS=nocturne GOARCH=amd64 CGO_ENABLED=0 "$(GO)" build -trimpath \
	    -overlay "$(CURDIR)/$(TS_OVERLAY)" -o "$(CURDIR)/$@" ./cmd/tsc
# TypeScript's licence and notice, and beside them those of the modules linked into tsc
# (go version -m lists them; the module cache escapes capitals as !c)
$(LICENSES)/typescript/LICENSE.txt: $(TYPESCRIPT)/LICENSE.txt $(TYPESCRIPT)/NOTICE.txt $(PROGRAMS)/tsc
	@rm -rf $(dir $@) && mkdir -p $(dir $@)
	@cp $(TYPESCRIPT)/LICENSE.txt $(TYPESCRIPT)/NOTICE.txt $(dir $@)
	@cache=$$(cygpath -u "$$("$(GO)" env GOMODCACHE)"); "$(GO)" version -m $(PROGRAMS)/tsc | \
	  while read kind path version sum; do [ "$$kind" = dep ] || continue; \
	    from="$$cache/$$(echo $$path | sed 's/[A-Z]/!\L&/g')@$$version"; mkdir -p $(dir $@)$$path; \
	    for f in "$$from"/LICENSE* "$$from"/NOTICE* "$$from"/COPYING*; do [ ! -f "$$f" ] || cp "$$f" $(dir $@)$$path/; done; \
	    ls $(dir $@)$$path | grep -q . || { echo "no licence for $$path" >&2; exit 1; }; done
# The desktop apps written in TypeScript (user/apps/*.ts, the toolkit in
# "nocturne/ui") are type-checked and compiled on the build host, so starting
# one does not wait for tsc: user/apps/settings.ts -> C:\Nocturne\System\settings.js,
# run as "js C:\Nocturne\System\settings.js".
TS_APPS := $(notdir $(basename $(wildcard user/apps/*.ts)))
TS_BINS := $(addprefix $(PROGRAMS)/,$(addsuffix .js,$(TS_APPS)))
GO_FILES += $(TS_BINS)
$(BUILD)/host/tsc.exe: $(TS_SRC)
	@mkdir -p $(dir $@)
	@echo "  GO   $@"
	@cd $(TYPESCRIPT)/tsc && GOOS= GOARCH= CGO_ENABLED=0 "$(GO)" build -trimpath -o "$(CURDIR)/$@" ./cmd/tsc
$(BUILD)/apps-ts.stamp: $(BUILD)/host/tsc.exe user/apps/tsconfig.json $(wildcard user/apps/*.ts) $(wildcard rootfs/Nocturne/SDK/Types/nocturne/*.d.ts)
	@echo "  TSC  user/apps"
	@$(BUILD)/host/tsc.exe -p user/apps/tsconfig.json --pretty false
	@touch $@
$(TS_BINS): $(PROGRAMS)/%.js: $(BUILD)/apps-ts.stamp
	@mkdir -p $(dir $@)
	@cp $(BUILD)/apps-ts/$*.js $@
endif
endif

include ports/rust/rust.mk
include ports/llvm/llvm.mk

NVRM_FILES := $(PROGRAMS)/nvrm $(LICENSES)/nvidia-open/COPYING $(LICENSES)/nvidia-open/NOTICES
# the licence texts of the third-party code in the system, as third_party has them
$(LICENSES)/%: third_party/%
	@mkdir -p $(dir $@)
	@cp $< $@
LICENSE_FILES := $(LICENSES)/libcxx/LICENSE.TXT $(addprefix $(LICENSES)/,uacpi/LICENSE wabt/LICENSE wabt/UPSTREAM.json \
    lexbor/LICENSE lexbor/NOTICE lexbor/UPSTREAM.json expat/COPYING expat/README.nocturne.md ffmpeg/LICENSE.md ffmpeg/COPYING.LGPLv2.1 ffmpeg/manifest.json \
    ffmpeg/README.nocturne.md unifont/OFL-1.1.txt unifont/NOTICE.txt unifont/manifest.json)
$(PROGRAMS)/nvrm: $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ) $(NVKMS_LINK_OBJ) $(UVM_LINK_OBJ) $(LIBC_OBJ) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(FF_OBJ) $(LIBC_RSP) user/user.ld
	@mkdir -p $(dir $@)
	@echo "  LD   $@"
	@$(LD) $(ULDFLAGS) --gc-sections $(BUILD)/u/user/libc/crt0.asm.o $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ) $(NVKMS_LINK_OBJ) $(UVM_LINK_OBJ) --start-lib @$(LIBC_RSP) --end-lib -o $@

$(LICENSES)/libcxx/LICENSE.TXT: $(BUILD)/cxx/libc++.a
	@mkdir -p $(dir $@)
	@cp $(BUILD)/cxx/LICENSE.libc++.txt $@

$(BUILD)/u/%.cpp.o: %.cpp $(BUILD)/cxx/libc++.a $(CXX_HEADERS)
	@echo "  CXX  $<"
	@$(PY) scripts/build_cxx.py compile $< -o $@

# The C++ runtime is among the libc's members (CXX_RUNTIME_LINK).
$(CPP_BINS): $(PROGRAMS)/%: $(BUILD)/u/user/apps/%.cpp.o $(LIBC_RSP) $(BUILD)/u/user/libc/crt0.asm.o user/user.ld user/cxx.ld
	@mkdir -p $(dir $@)
	@echo "  LD++ $@"
	@$(LD) $(patsubst user/user.ld,user/cxx.ld,$(ULDFLAGS)) --gc-sections $(BUILD)/u/user/libc/crt0.asm.o $< --start-lib @$(LIBC_RSP) --end-lib -o $@

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

$(BUILD)/tcc/libtcc.o: $(wildcard $(TCC_DIR)/*.c $(TCC_DIR)/*.h) ports/tcc/config.h $(BUILD)/tcc/tccdefs_.h
	@mkdir -p $(dir $@)
	@echo "  CC   $(TCC_DIR)/libtcc.c"
	@$(CC) $(TCC_CFLAGS) -c $(TCC_DIR)/libtcc.c -o $@

$(PROGRAMS)/tcc: $(BUILD)/tcc/tcc.o $(LIBC_OBJ) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(FF_OBJ) $(LIBC_RSP) user/user.ld
	@mkdir -p $(dir $@)
	@echo "  LD   $@"
	@$(LD) $(ULDFLAGS) --gc-sections $(BUILD)/u/user/libc/crt0.asm.o $< --start-lib @$(LIBC_RSP) --end-lib -o $@

$(BUILD)/tccrt/%.c.o: $(TCC_DIR)/lib/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(TCCRT_FLAGS) -c $< -o $@
$(BUILD)/tccrt/%.S.o: $(TCC_DIR)/lib/%.S
	@mkdir -p $(dir $@)
	@$(CC) $(TCCRT_FLAGS) -c $< -o $@
$(BUILD)/tccrt/runmain.c.o: ports/tcc/runmain.c
	@mkdir -p $(dir $@)
	@$(CC) $(TCCRT_FLAGS) -c $< -o $@

# the SDK: headers and libraries for compiling inside Nocturne (C:\Nocturne\SDK)
$(BUILD)/sdk.stamp: $(LIBC_OBJ) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(FF_OBJ) $(LIBC_RSP) $(TCCRT_OBJ) scripts/mksdk.sh \
		$(shell find user/include ports/tcc/include third_party/bearssl/inc ports/nvidia/include $(TCC_DIR)/include -name '*.h') \
		third_party/quickjs/quickjs.h $(shell find user/libc -name '*.h' -o -name '*.inc' -o -name '*.js') \
		$(LXB_HEADERS) ports/lexbor/include/memory.h third_party/lexbor/headers.list \
		common/abi.h common/gfx.h common/gpu_abi.h common/display_abi.h user/user.ld \
		$(wildcard user/apps/*.c user/apps/*.h user/apps/*.js user/apps/*.ts) $(LLVM_SDK_FILES)
	@echo "  SDK"
	@bash scripts/mksdk.sh $(SDK) $(if $(LLVM_SDK),--llvm $(LLVM_SDK)) $(BUILD)/u/user/libc/crt0.asm.o @$(LIBC_RSP) -- $(TCCRT_OBJ) --wasmrt $(WASM_RT_OBJ)
	@touch $@

# ---------------------------------------------------------------- images
.PHONY: all kernel user cxx lexbor image qemu-image run clean test test-quick test-full
.SECONDARY:
all: image

kernel: $(BUILD)/kernel.elf
user: $(APP_BINS) $(NVRM_FILES) $(PROGRAMS)/tcc
cxx: $(CPP_BINS) $(LICENSES)/libcxx/LICENSE.TXT
lexbor: $(LXB_OBJ)

# NVIDIA GSP firmware for nvrm (docs/nvidia/LEDGER.md D9), C:\Nocturne\Firmware\NVIDIA\<version>.
# It is never in git and never distributed: a copy unpacked into local/ (ignored) goes onto disks
# built on this machine, and disks get none without it. NVIDIA_FIRMWARE= leaves it out explicitly.
NVIDIA_FIRMWARE ?= local/firmware/nvidia/615.78.08
NVIDIA_FW_VERSION := 615.78.08
NVIDIA_FW_STATE := $(BUILD)/nvidia-firmware.json
# RM (osinit.c) needs both images of the GPU family: Ampere and Ada (the RTX 4070) use ga10x.
# Content/selection changes update the state; an unchanged selection leaves its mtime alone.
# scripts/layout.py puts exactly the files it names on C:, none when it names none.
.PHONY: nvidia-firmware-check
nvidia-firmware-check:
$(NVIDIA_FW_STATE): nvidia-firmware-check scripts/layout.py
	@$(PY) scripts/layout.py --firmware-state "$@" --firmware-source "$(NVIDIA_FIRMWARE)" --firmware-version "$(NVIDIA_FW_VERSION)"

# everything scripts/layout.py puts on C:
DISK_FILES := $(APP_BINS) $(GO_FILES) $(RUST_FILES) $(LLVM_FILES) $(NVRM_FILES) $(PROGRAMS)/tcc $(LICENSE_FILES) $(BUILD)/sdk.stamp \
    $(BUILD)/sounds.stamp $(NVIDIA_FW_STATE) $(shell find rootfs -type f) third_party/ca/ca-bundle.crt

# the music in the user's Music folder, made by a script
$(BUILD)/sounds.stamp: scripts/mksounds.py
	@echo "  GEN  sounds"
	@rm -rf $(BUILD)/sounds
	@$(PY) scripts/mksounds.py $(BUILD)/sounds
	@touch $@

# the boot disk, build/nocturne.img, its VHD and VHDX, and the live ISO (scripts/mkimage.sh)
image: $(BUILD)/kernel.elf $(BUILD)/host/store.exe $(BUILD)/host/limine.exe $(DISK_FILES)
	@bash scripts/mkimage.sh

# the raw QEMU boot disk only, for test iterations (no VHD/VHDX)
qemu-image: $(BUILD)/kernel.elf $(BUILD)/host/store.exe $(BUILD)/host/limine.exe $(DISK_FILES)
	@bash scripts/mkimage.sh --raw-only

# QEMU on build/run.img, a disk of its own whose C:\Users stays from one run to the next while its
# system is replaced by the new one (mkimage.sh --run)
run: $(BUILD)/kernel.elf $(BUILD)/host/store.exe $(BUILD)/host/limine.exe $(DISK_FILES)
	@bash scripts/mkimage.sh --run
	@$(QEMU) -M pc -m $(QEMU_MEMORY) -smp $(QEMU_CPUS) -accel $(QEMU_ACCEL) $(QEMU_GRAPHICS) \
		-device ahci,id=ahci -drive id=boot,file=$(BUILD)/run.img,format=raw,if=none \
		-device ide-hd,drive=boot,bus=ahci.0,bootindex=0 -serial stdio -vga std \
		-audiodev dsound,id=snd -device AC97,audiodev=snd

# the in-OS test suite on its own copy of build/nocturne.img (build/test.img, deleted after its store check;
# see scripts/test.py)
test: qemu-image
	@$(PY) scripts/test.py
test-quick: qemu-image
	@$(PY) scripts/test.py --quick
test-full: qemu-image
	@$(PY) scripts/test.py --full

# keeps build/run.img: `make run`'s disk, with C:\Users (your files, and the agent's key)
clean:
	find $(BUILD) -mindepth 1 -maxdepth 1 ! -name run.img -exec rm -rf {} +

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
