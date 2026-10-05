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

# ---------------------------------------------------------------- images
.PHONY: all kernel user image run clean
.SECONDARY:
all: image

kernel: $(BUILD)/kernel.elf
user: $(APP_BINS)

$(BUILD)/initrd.tar: $(APP_BINS) $(shell find rootfs -type f) scripts/mkinitrd.py
	@echo "  TAR  $@"
	@$(PY) scripts/mkinitrd.py $@ rootfs $(BUILD)/root

image: $(BUILD)/kernel.elf $(BUILD)/initrd.tar
	@bash scripts/mkimage.sh

run: image
	@qemu-system-x86_64 -M pc -m 512M -drive file=$(BUILD)/nocturne.img,format=raw,if=ide \
		-serial stdio -vga std

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
