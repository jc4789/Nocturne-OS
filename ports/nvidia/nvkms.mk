# NVIDIA's mode-setting driver (NVKMS, "nvidia-modeset") inside the nvrm driver app
#
# Included from ports/nvidia/nvrm.mk (it uses NVRM_VENDOR, NVRM_DIR, NVRM_OUT and the RM flags).
# On Linux NVKMS is a second kernel module (nvidia-modeset.ko) next to RM's nvidia.ko; here it
# is linked into nvrm, the one program that owns the GPU, and ports/nvidia/nvkms_os.c is its OS
# layer and its connection to RM (see ports/nvidia/README.md, "NVKMS").
#
#   make nvkms-objs          # compile NVKMS core + its OS layer into build/nvkms/
#   make nvkms-undefined     # what RM + NVKMS + both OS layers still need from outside
#
# The sources are open-gpu-kernel-modules 615.78.08 as vendored in third_party/nvidia-open:
# src/nvidia-modeset and the parts of src/common its srcs.mk names. The flags follow
# src/nvidia-modeset/Makefile and utils.mk, minus what only a kernel module needs
# (-mcmodel=kernel, -mno-sse/-msoft-float, retpoline and return thunks, -fcf-protection).

XZ ?= xz
CXX_NVKMS ?= clang++

NVKMS_DIR := $(NVRM_VENDOR)/src/nvidia-modeset
NVKMS_OUT := $(BUILD)/nvkms

# ---------------------------------------------------------------- source list
# srcs.mk adds to SRCS and SRCS_CXX; paths are relative to src/nvidia-modeset and may start
# with ../common (src/common).
SRCS :=
SRCS_CXX :=
include $(NVKMS_DIR)/srcs.mk
NVKMS_SRCS := $(SRCS)
NVKMS_SRCS_CXX := $(SRCS_CXX)
SRCS :=
SRCS_CXX :=

# object name: <src>.o (C) or <src>.cpp.o (C++) under build/nvkms/core; "../" becomes "up/".
nvkms_obj = $(addprefix $(NVKMS_OUT)/core/,$(subst ../,up/,$(1:.c=.o)))
nvkms_cxx_obj = $(addprefix $(NVKMS_OUT)/core/,$(subst ../,up/,$(1:.cpp=.cpp.o)))

# The headsurface shaders (src/shaders/g_<arch>_shaders) are embedded xz-compressed, as
# NVIDIA's Makefile does; nvidia-3d-init.c finds them as _binary_<arch>_shaders_xz_start/_end
# and xzminidec unpacks the one for the GPU.
NVKMS_SHADERS := turing ampere hopper blackwell gb20x
NVKMS_SHADER_OBJ := $(patsubst %,$(NVKMS_OUT)/shaders/%_shaders.xz.o,$(NVKMS_SHADERS))

# nvrm's own part of the core (nvkms_kapi_rm.c): it reads KAPI's private structures, so it is
# compiled like NVKMS's sources, and its entry points are kept global like NVIDIA's exports.
NVKMS_NOCTURNE_OBJ := $(NVKMS_OUT)/core/nocturne/nvkms_kapi_rm.o
NVKMS_NOCTURNE_EXPORTS := nvrm_nvkms_memory_handle

NVKMS_OBJ := $(call nvkms_obj,$(NVKMS_SRCS)) $(call nvkms_cxx_obj,$(NVKMS_SRCS_CXX)) \
    $(NVKMS_OUT)/core/g_nvid_string.o $(NVKMS_SHADER_OBJ) $(NVKMS_NOCTURNE_OBJ)

# ---------------------------------------------------------------- compile flags
NVKMS_COMMON := $(NVKMS_DIR)/../common
NVKMS_INC := $(addprefix $(NVKMS_COMMON)/,sdk/nvidia/inc sdk/nvidia/inc/hw shared/inc inc \
        softfloat/nvidia softfloat/source/include softfloat/source/8086-SSE \
        unix/common/utils/interface unix/common/inc modeset \
        displayport/inc displayport/inc/dptestutil inc/displayport \
        unix/nvidia-3d/interface unix/nvidia-push/interface unix/nvidia-3d/include \
        unix/nvidia-push/include unix/xzminidec/interface unix/nvidia-headsurface) \
    $(addprefix $(NVKMS_DIR)/,os-interface/include kapi/interface interface include kapi/include \
        src/shaders) \
    $(NVRM_DIR)/arch/nvalloc/unix/include

# NV_BSD and NV_X86_64 for the same reason RM has them (nvrm.mk): common/inc/nvUnixVersion.h
# gives NV_VERSION_STRING, which NVKMS checks against its clients, only to a "Unix build".
# NVKMS's own sources test neither. NV_KERNEL_INTERFACE_LAYER is not defined: that names the
# Linux glue (kernel-open), which is not built.
NVKMS_DEFS := -DNV_BSD -DNV_X86_64 -DNV_ARCH_BITS=64 -DNDEBUG -D_LANGUAGE_C -D__NO_CTYPE \
    -DNV_CPU_INTRINSICS_KERNEL -DNVHDMIPKT_RM_CALLS_INTERNAL=0 -DNVHDMIPKT_NVKMS \
    -DSOFTFLOAT_ROUND_ODD -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32 -DNVT_USE_NVKMS \
    -DNV_SMG_IN_NVKMS -DNV_PUSH_IN_KERNEL -DNV_XZ_CUSTOM_MEM_HOOKS -DNV_XZ_USE_NVTYPES -DXZ_DEC_SINGLE

# NVIDIA builds NVKMS with gcc. These diagnostics are clang's alone (or wider in clang) and
# fire on code gcc accepts silently: braces around nested initialisers (RM has the same switch,
# nvrm.mk), counters only incremented (nvt_edidext_861.c), "static inline __always_inline" in
# xzminidec (its xz_config.h asks for that spelling), and virtual overrides in the DisplayPort
# library that mark some members "override" but not others.
NVKMS_CLANG_ONLY := -Wno-missing-braces -Wno-unused-but-set-variable

# The program flags of nvrm (nvrm.mk): user space, SSE2 like the rest of Nocturne's userland,
# frame pointers kept so os_dump_stack (nvkms_dump_stack) can walk through NVKMS frames too.
# The warnings are NVIDIA's list for NVKMS.
NVKMS_BASE_FLAGS := --target=x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic \
    -fno-pie -mno-red-zone -msse2 -O2 -g -fno-strict-aliasing -fno-omit-frame-pointer -fno-common \
    -ffunction-sections -fdata-sections \
    -Wformat -Wreturn-type -Wswitch -Wunused-local-typedefs -Wchar-subscripts -Wparentheses \
    -Wpointer-arith -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter \
    -Wno-missing-field-initializers -Wno-format-zero-length -Wmissing-declarations -Wno-cast-qual \
    $(NVKMS_CLANG_ONLY)     -include $(NVKMS_COMMON)/sdk/nvidia/inc/cpuopsys.h $(addprefix -I,$(NVKMS_INC)) $(NVKMS_DEFS) -MMD -MP
NVKMS_CFLAGS := $(NVKMS_BASE_FLAGS) -std=gnu11 -Wimplicit -Wstrict-prototypes -Wmissing-prototypes     -Wno-duplicate-decl-specifier
# C++ (the DisplayPort library and src/dp): no exceptions, no RTTI, as upstream; no C++ library
# headers are used, so libc++ is not on the include path.
NVKMS_CXXFLAGS := $(NVKMS_BASE_FLAGS) -std=gnu++11 -fno-operator-names -fno-rtti -fno-exceptions \
    -fcheck-new -fno-asynchronous-unwind-tables -nostdinc++ -Wno-inconsistent-missing-override

$(NVKMS_OUT)/core/%.o: $(NVKMS_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  KMS  $<"
	@$(CC) $(NVKMS_CFLAGS) -c $< -o $@

$(NVKMS_OUT)/core/up/%.o: $(NVKMS_DIR)/../%.c
	@mkdir -p $(dir $@)
	@echo "  KMS  $<"
	@$(CC) $(NVKMS_CFLAGS) -c $< -o $@

$(NVKMS_OUT)/core/%.cpp.o: $(NVKMS_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  KMS  $<"
	@$(CXX_NVKMS) $(NVKMS_CXXFLAGS) -c $< -o $@

$(NVKMS_OUT)/core/up/%.cpp.o: $(NVKMS_DIR)/../%.cpp
	@mkdir -p $(dir $@)
	@echo "  KMS  $<"
	@$(CXX_NVKMS) $(NVKMS_CXXFLAGS) -c $< -o $@

# The identification string nvkms.c logs at load (pNV_KMS_ID); as for RM (nvrm.mk), without
# the build user, host and date that would make the output irreproducible.
$(NVKMS_OUT)/core/g_nvid_string.c: $(NVRM_VENDOR)/version.mk ports/nvidia/nvkms.mk
	@mkdir -p $(dir $@)
	@printf '%s\n' 'const char NV_KMS_ID[] = "nvidia id: NVIDIA Nocturne Kernel Mode Setting Driver for x86_64  $(NVRM_VERSION)  Release Build";' \
	    'const char *const pNV_KMS_ID = NV_KMS_ID + 11;' > $@
$(NVKMS_OUT)/core/g_nvid_string.o: $(NVKMS_OUT)/core/g_nvid_string.c
	@echo "  KMS  $<"
	@$(CC) $(NVKMS_CFLAGS) -Wno-missing-variable-declarations -c $< -o $@

$(NVKMS_OUT)/core/nocturne/%.o: ports/nvidia/%.c ports/nvidia/nvkms_os.h
	@mkdir -p $(dir $@)
	@echo "  KMS  $<"
	@$(CC) $(NVKMS_CFLAGS) -c $< -o $@

# xz -C none: xzminidec is built without integrity checks (XZ_DEC_SINGLE, no CRC64/SHA-256).
$(NVKMS_OUT)/shaders/%_shaders.xz: $(NVKMS_DIR)/src/shaders/g_%_shaders
	@mkdir -p $(dir $@)
	@echo "  XZ   $<"
	@$(XZ) -ce -C none < $< > $@
# The symbol names come from the file name as the linker is given it, hence the cd.
$(NVKMS_OUT)/shaders/%_shaders.xz.o: $(NVKMS_OUT)/shaders/%_shaders.xz
	@echo "  LD   $@"
	@cd $(dir $<) && $(LD) -m elf_x86_64 -r -z noexecstack --format=binary $(notdir $<) -o $(notdir $@)
	@$(OBJCOPY) --rename-section .data=.rodata,contents,alloc,load,data,readonly $@

# ---------------------------------------------------------------- the NVKMS core object
# One nv-modeset-kernel.o, as NVIDIA's Makefile links it. On Linux the module boundary keeps
# NVKMS's symbols apart from RM's (both carry, for example, their own nvstatus.c); here both
# are in one program, so every global symbol except the entry points NVIDIA exports to the
# kernel interface layer (exports_link_command.txt) and nvrm's own (NVKMS_NOCTURNE_EXPORTS) is
# made local. Undefined references (the nvkms_* OS interface, libc) stay as they are.
# NVIDIA's nvkms.ld is not used: it merges the per-function sections for GNU ld, and doing that
# with lld breaks the C++ COMDAT groups of the DisplayPort library (their member sections end up
# inside .text, which objcopy rejects). Kept apart, the sections also stay collectable by nvrm's
# --gc-sections. The groups keep their (now local) signatures; the names are DisplayPort:: and
# _nv_dplibtimer, which nothing else in nvrm defines, so no group of another object replaces them.
$(NVKMS_OUT)/exports.txt: $(NVKMS_DIR)/exports_link_command.txt ports/nvidia/nvkms.mk
	@mkdir -p $(dir $@)
	@{ tr ' ' '\n' < $< | sed -n 's/^--undefined=//p'; printf '%s\n' $(NVKMS_NOCTURNE_EXPORTS); } > $@

$(NVKMS_OUT)/nv-modeset-kernel.o: $(NVKMS_OBJ) $(NVKMS_OUT)/exports.txt ports/nvidia/nvkms.mk
	@echo "  LD   $@"
	@$(file >$@.rsp,$(NVKMS_OBJ))
	@$(LD) -m elf_x86_64 -r @$@.rsp -o $@
	@$(OBJCOPY) --keep-global-symbols=$(NVKMS_OUT)/exports.txt --remove-section=.note.gnu.property $@

# ---------------------------------------------------------------- the OS layer
# nvkms_os.c includes NVKMS's OS interface headers and RM's (through nvrm_os.h), and the
# interface between the two modules (kernel-open/common/inc/nv-modeset-interface.h).
NVKMS_OS_SRC := ports/nvidia/nvkms_os.c
NVKMS_OS_OBJ := $(NVKMS_OUT)/os/nvkms_os.o
NVKMS_OS_CFLAGS := $(NVRM_OS_CFLAGS) -I$(NVRM_VENDOR)/kernel-open/common/inc \
    $(addprefix -I$(NVKMS_DIR)/,os-interface/include kapi/interface interface) \
    -I$(NVKMS_COMMON)/unix/common/inc
$(NVKMS_OS_OBJ): $(NVKMS_OS_SRC) ports/nvidia/nvkms_os.h ports/nvidia/nvrm_os.h
	@mkdir -p $(dir $@)
	@echo "  NVOS $<"
	@$(CC) $(NVKMS_OS_CFLAGS) -c $< -o $@

# What nvrm links for NVKMS (the top-level Makefile's $(PROGRAMS)/nvrm rule).
NVKMS_LINK_OBJ := $(NVKMS_OUT)/nv-modeset-kernel.o $(NVKMS_OS_OBJ)

# ---------------------------------------------------------------- targets
.PHONY: nvkms-objs nvkms-undefined
nvkms-objs: $(NVKMS_LINK_OBJ)

# RM, NVKMS and both OS layers together, as nvrm links them: what is left undefined must come
# from libc (memcpy, malloc, thrd_*, n_*).
nvkms-undefined: $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ) $(NVKMS_LINK_OBJ)
	@$(LD) -m elf_x86_64 -r $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ) $(NVKMS_LINK_OBJ) -o $(NVKMS_OUT)/all.o && \
	$(NM) -u $(NVKMS_OUT)/all.o | awk '{print $$2}' | sort > $(NVKMS_OUT)/undefined.txt && \
	echo "undefined symbols: $$(wc -l < $(NVKMS_OUT)/undefined.txt)  ($(NVKMS_OUT)/undefined.txt)"

-include $(patsubst %.o,%.d,$(filter-out $(NVKMS_SHADER_OBJ),$(NVKMS_OBJ))) $(NVKMS_OS_OBJ:.o=.d)
