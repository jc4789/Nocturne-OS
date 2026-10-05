# Nocturne OS build. Run inside the MSYS2 UCRT64 shell (see build.ps1).
CC   := clang
LD   := ld.lld
AR   := llvm-ar
NASM := nasm
PY   := python

BUILD := build

# ---------------------------------------------------------------- kernel
KCFLAGS := --target=x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-stack-check \
           -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone -mgeneral-regs-only -mno-sse -mno-mmx \
           -mno-80387 -O2 -g -std=gnu11 -fno-omit-frame-pointer -fno-strict-aliasing \
           -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Ikernel/src -Icommon -MMD -MP
KLDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T kernel/linker.ld --no-dynamic-linker

KSRC_C   := $(shell find kernel/src -name '*.c') common/gfx.c
KSRC_ASM := $(shell find kernel/src -name '*.asm')
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
LIBC_ASM := $(shell find user/libc -name '*.asm')
LIBC_OBJ := $(patsubst %.c,$(BUILD)/u/%.o,$(LIBC_C)) $(patsubst %.asm,$(BUILD)/u/%.asm.o,$(LIBC_ASM))

# BearSSL (TLS) is linked like libc: programs only pull in the parts they use
BR_C     := $(shell find third_party/bearssl/src -name '*.c')
BR_OBJ   := $(patsubst %.c,$(BUILD)/br/%.o,$(BR_C)) $(BUILD)/br/tls_roots.o
BRFLAGS  := $(filter-out -W% -MMD -MP,$(UCFLAGS)) -w -Ithird_party/bearssl/inc -Ithird_party/bearssl/src \
            -DBR_USE_URANDOM=1 -DBR_USE_GETENTROPY=0 -DBR_USE_WIN32_RAND=0 -DBR_USE_UNIX_TIME=1 -DBR_USE_WIN32_TIME=0
UCFLAGS  += -Ithird_party/bearssl/inc

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

$(BUILD)/u/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	@echo "  NASM $<"
	@$(NASM) -f elf64 -g $< -o $@

# no archiver in the toolchain: lld's --start-lib gives archive semantics to plain objects
LIBC_LINK := $(filter-out %crt0.asm.o,$(LIBC_OBJ)) $(BR_OBJ)

$(BUILD)/root/bin/%: $(BUILD)/u/user/apps/%.o $(LIBC_OBJ) $(BR_OBJ) user/user.ld
	@mkdir -p $(dir $@)
	@echo "  LD   $@"
	@$(LD) $(ULDFLAGS) $(BUILD)/u/user/libc/crt0.asm.o $< --start-lib $(LIBC_LINK) --end-lib -o $@

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

$(BUILD)/root/bin/tcc: $(BUILD)/tcc/tcc.o $(LIBC_OBJ) user/user.ld
	@mkdir -p $(dir $@)
	@echo "  LD   $@"
	@$(LD) $(ULDFLAGS) $(BUILD)/u/user/libc/crt0.asm.o $< --start-lib $(LIBC_LINK) --end-lib -o $@

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
$(BUILD)/sysroot.stamp: $(LIBC_OBJ) $(BR_OBJ) $(TCCRT_OBJ) scripts/mksysroot.sh \
		$(shell find user/include ports/tcc/include third_party/bearssl/inc $(TCC_DIR)/include -name '*.h') \
		common/abi.h common/gfx.h $(wildcard user/apps/*.c)
	@echo "  SYSROOT"
	@bash scripts/mksysroot.sh $(BUILD)/sysroot $(LIBC_OBJ) $(BR_OBJ) -- $(TCCRT_OBJ)
	@touch $@

# ---------------------------------------------------------------- images
.PHONY: all kernel user image run clean
.SECONDARY:
all: image

kernel: $(BUILD)/kernel.elf
user: $(APP_BINS) $(BUILD)/root/bin/tcc

$(BUILD)/initrd.tar: $(APP_BINS) $(BUILD)/root/bin/tcc $(BUILD)/sysroot.stamp $(shell find rootfs -type f) scripts/mkinitrd.py
	@echo "  TAR  $@"
	@$(PY) scripts/mkinitrd.py $@ rootfs $(BUILD)/root $(BUILD)/sysroot

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
	@qemu-system-x86_64 -M pc -m 512M -drive file=$(BUILD)/nocturne.img,format=raw,if=ide,index=0 \
		-drive file=$(BUILD)/data.img,format=raw,if=ide,index=1 -serial stdio -vga std

# keeps build/data.img: it is the persistent /data disk (your files, and the agent's key)
clean:
	find $(BUILD) -mindepth 1 -maxdepth 1 ! -name data.img -exec rm -rf {} +

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
