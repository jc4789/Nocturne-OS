# NVIDIA Resource Manager (RM) as a user-space driver app: /bin/nvrm
#
# Include this from the top-level Makefile (after UCFLAGS/BUILD/CC are defined). It only
# defines variables and rules; the default goals of the top-level Makefile do not depend on
# it until the link rule is added (see ports/nvidia/README.md).
#
#   make -f Makefile nvrm-objs        # compile RM + the Nocturne OS layer into build/nvrm/
#   make nvrm-undefined               # list the symbols RM needs that nobody defines yet
#
# The RM sources are NVIDIA's open-gpu-kernel-modules 615.78.08 (MIT), vendored in
# third_party/nvidia-open (src/nvidia and part of src/common; nothing from kernel-open).
# The flags follow src/nvidia/Makefile and utils.mk of that tree, minus what only a kernel
# module needs (-mcmodel=kernel, -mno-sse, retpoline, ftrace).

# Binutils (GNU nm/objcopy) are what the toolchain ships; there is no llvm-nm/llvm-objcopy.
NM      ?= nm
OBJCOPY ?= objcopy

NVRM_VENDOR := third_party/nvidia-open
NVRM_DIR    := $(NVRM_VENDOR)/src/nvidia
NVRM_OUT    := $(BUILD)/nvrm
NVRM_VERSION := 615.78.08

# ---------------------------------------------------------------- RM source list
# srcs.mk adds to SRCS ("?=" then "+="); paths are relative to src/nvidia and may start
# with ../common.
SRCS :=
SRCS_CXX :=
include $(NVRM_DIR)/srcs.mk
NVRM_SRCS := $(SRCS)
NVRM_SRCS_CXX := $(SRCS_CXX)
SRCS :=
SRCS_CXX :=

# libspdm (DMTF, BSD-3-Clause) and the files that depend on its headers need extra include
# paths and a configuration header; openspdm.mk (NVIDIA) lists them.
include $(NVRM_DIR)/src/libraries/libspdm/nvidia/openspdm.mk
NVRM_SPDM_SRCS := $(LIBSPDM_SOURCES)
NVRM_SPDM_INCLUDES := $(LIBSPDM_INCLUDES)
NVRM_SPDM_DEFINES := $(LIBSPDM_DEFINES)

# object name: <src>.o under build/nvrm/rm; "../" (the sibling src/common) becomes "up/".
nvrm_obj = $(addprefix $(NVRM_OUT)/rm/,$(subst ../,up/,$(1:.c=.o)))

NVRM_OBJ := $(call nvrm_obj,$(NVRM_SRCS)) $(NVRM_OUT)/rm/g_nvid_string.o
NVRM_SPDM_OBJ := $(call nvrm_obj,$(NVRM_SPDM_SRCS))

# ---------------------------------------------------------------- RM compile flags
NVRM_INC := kernel/inc interface ../common/sdk/nvidia/inc ../common/sdk/nvidia/inc/hw \
    arch/nvalloc/common/inc arch/nvalloc/common/inc/gsp arch/nvalloc/common/inc/deprecated \
    arch/nvalloc/unix/include inc inc/os ../common/shared/inc ../common/inc \
    ../common/uproc/os/libos-v2.0.0/include ../common/uproc/os/common/include \
    ../common/inc/swref ../common/inc/swref/published generated \
    ../common/nvswitch/kernel/inc ../common/nvswitch/interface ../common/nvswitch/common/inc/ \
    ../common/inc/displayport ../common/nvlink/interface/ ../common/nvlink/inband/interface \
    src/mm/uvm/interface inc/libraries src/libraries inc/kernel

# RM is built as NVIDIA's OS-neutral "unix" flavour: NV_UNIX selects the shape of the OS
# interface (src/nvidia/arch/nvalloc/unix); it is a switch inside RM only. NV_BSD is there
# because common/inc/nvVer.h, nvBldVer.h and nvUnixVersion.h only take the "Unix build"
# branch (version strings) for NV_LINUX/NV_BSD/NV_SUNOS/...; in the sources compiled here
# NV_BSD changes nothing else (it matters only in the kernel interface layer, which is not
# part of this build). NV_LINUX is deliberately *not* defined: it would switch on the
# cgroup/dmem hooks. Nothing here asks Nocturne for POSIX.
NVRM_DEFS := -DNV_UNIX -DNV_BSD -DNV_X86_64 -DNV_ARCH_BITS=64 -D_LANGUAGE_C -D__NO_CTYPE -DNVRM \
    -DLOCK_VAL_ENABLED=0 -DPORT_ATOMIC_64_BIT_SUPPORTED=1 -DPORT_IS_KERNEL_BUILD=1 \
    -DPORT_IS_CHECKED_BUILD=0 -DPORT_MODULE_atomic=1 -DPORT_MODULE_core=1 -DPORT_MODULE_cpu=1 \
    -DPORT_MODULE_crypto=1 -DPORT_MODULE_debug=1 -DPORT_MODULE_memory=1 -DPORT_MODULE_safe=1 \
    -DPORT_MODULE_string=1 -DPORT_MODULE_sync=1 -DPORT_MODULE_thread=1 -DPORT_MODULE_time=1 \
    -DPORT_MODULE_util=1 -DPORT_MODULE_example=0 -DPORT_MODULE_mmio=0 -DRS_STANDALONE=0 \
    -DRS_STANDALONE_TEST=0 -DRS_COMPATABILITY_MODE=1 -DRS_PROVIDES_API_STATE=0 \
    -DNV_CONTAINERS_NO_TEMPLATES -DINCLUDE_NVLINK_LIB -DINCLUDE_NVSWITCH_LIB \
    -DNV_PRINTF_STRINGS_ALLOWED=1 -DNV_ASSERT_FAILED_USES_STRINGS=1 \
    -DPORT_ASSERT_FAILED_USES_STRINGS=1

# User-space program flags (cf. UCFLAGS): small code model, red zone off (RM also runs from
# the interrupt thread, which is an ordinary thread here, but keep the kernel habit), SSE2
# on like the rest of Nocturne's userland. Frame pointers stay (RM's utils.mk asks for them).
NVRM_CFLAGS := --target=x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic \
    -fno-pie -mno-red-zone -msse2 -O2 -g -std=gnu11 -fno-strict-aliasing -fno-omit-frame-pointer \
    -fno-common -ffunction-sections -fdata-sections -fno-jump-tables \
    -Wall -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare -Wno-format \
    -Werror-implicit-function-declaration -Wwrite-strings -Wno-undef -Wno-missing-braces \
    -include $(NVRM_DIR)/../common/sdk/nvidia/inc/cpuopsys.h \
    $(addprefix -I$(NVRM_DIR)/,$(NVRM_INC)) -I$(NVRM_OUT)/gen $(NVRM_DEFS) -MMD -MP

$(NVRM_SPDM_OBJ): NVRM_EXTRA := $(addprefix -I$(NVRM_DIR)/,$(NVRM_SPDM_INCLUDES)) \
    $(addprefix -D,$(NVRM_SPDM_DEFINES))

$(NVRM_OUT)/rm/%.o: $(NVRM_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  RM   $<"
	@$(CC) $(NVRM_CFLAGS) $(NVRM_EXTRA) -c $< -o $@

# "../common/..." sources live in src/common; the pattern above sees them as up/common/...
$(NVRM_OUT)/rm/up/%.o: $(NVRM_DIR)/../%.c
	@mkdir -p $(dir $@)
	@echo "  RM   $<"
	@$(CC) $(NVRM_CFLAGS) $(NVRM_EXTRA) -c $< -o $@

# The identification string nv.h declares (pNVRM_ID); NVIDIA's utils.mk generates the same
# two lines with the build user and date, which make the output irreproducible; leave them out.
$(NVRM_OUT)/rm/g_nvid_string.c: $(NVRM_VENDOR)/version.mk ports/nvidia/nvrm.mk
	@mkdir -p $(dir $@)
	@printf '%s\n' 'const char NVRM_ID[] = "nvidia id: NVIDIA Nocturne user-space RM for x86_64  $(NVRM_VERSION)  Release Build";' \
	    'const char *const pNVRM_ID = NVRM_ID + 11;' > $@
$(NVRM_OUT)/rm/g_nvid_string.o: $(NVRM_OUT)/rm/g_nvid_string.c
	@echo "  RM   $<"
	@$(CC) $(NVRM_CFLAGS) -c $< -o $@

# ---------------------------------------------------------------- Nocturne OS layer
# The os_*/nv_* callbacks RM expects (see ports/nvidia/README.md). They include both RM's
# headers (nv.h, os-interface.h, ...) and Nocturne's (nocturne.h, <threads.h>), so they take
# the RM flags plus the libc include path.
NVRM_OS_SRC := ports/nvidia/os.c ports/nvidia/os_pci.c ports/nvidia/os_stubs.c ports/nvidia/rm_api.c \
    ports/nvidia/nvlink_os.c ports/nvidia/spdm_stubs.c ports/nvidia/nvrm.c ports/nvidia/nvrm_serve.c
NVRM_OS_OBJ := $(patsubst ports/nvidia/%.c,$(NVRM_OUT)/os/%.o,$(NVRM_OS_SRC))
NVRM_OS_CFLAGS := $(filter-out -Wno-undef,$(NVRM_CFLAGS)) -Iports/nvidia -Iports/nvidia/include -Iuser/include -Icommon -DNVRM_NOCTURNE \
    -Wno-incompatible-pointer-types-discards-qualifiers
$(NVRM_OUT)/os/%.o: ports/nvidia/%.c ports/nvidia/nvrm_os.h ports/nvidia/nvrm_serve.h
	@mkdir -p $(dir $@)
	@echo "  NVOS $<"
	@$(CC) $(NVRM_OS_CFLAGS) -c $< -o $@

# ---------------------------------------------------------------- targets
.PHONY: nvrm-objs nvrm-rm nvrm-os nvrm-undefined
nvrm-rm: $(NVRM_OBJ)
nvrm-os: $(NVRM_OS_OBJ)
nvrm-objs: nvrm-rm nvrm-os

# Partial link of all RM objects: one nv-kernel.o like NVIDIA's Makefile makes. memcpy/memset
# defined by RM's gcc_helper.c are made local so they cannot clash with libc's.
$(NVRM_OUT)/nv-kernel.o: $(NVRM_OBJ)
	@echo "  LD   $@"
	@$(file >$@.rsp,$(NVRM_OBJ))
	@$(LD) -m elf_x86_64 -r @$@.rsp -o $@
	@$(OBJCOPY) --localize-symbol=memset --localize-symbol=memcpy --remove-section=.note.gnu.property $@

# Everything RM (and the OS layer) references that nothing in the objects defines. libc
# symbols (memcpy, malloc, thrd_*, n_*) show up here too; ports/nvidia/README.md splits them.
nvrm-undefined: $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ)
	@$(LD) -m elf_x86_64 -r $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ) -o $(NVRM_OUT)/all.o && \
	$(NM) -u $(NVRM_OUT)/all.o | awk '{print $$2}' | sort > $(NVRM_OUT)/undefined.txt && \
	echo "undefined symbols: $$(wc -l < $(NVRM_OUT)/undefined.txt)  ($(NVRM_OUT)/undefined.txt)"

-include $(NVRM_OBJ:.o=.d) $(NVRM_OS_OBJ:.o=.d)
