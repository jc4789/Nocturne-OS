# NVIDIA's Unified Virtual Memory driver (UVM, "nvidia-uvm") inside the nvrm driver app
#
# Included from ports/nvidia/nvrm.mk (it uses NVRM_VENDOR, NVRM_OUT, NVRM_OS_CFLAGS and the RM
# flags). On Linux UVM is a third kernel module (nvidia-uvm.ko) next to RM's nvidia.ko; here it
# is linked into nvrm, the one program that owns the GPU. ports/nvidia/uvm_os.h is the OS layer
# its sources are written against (it takes the place of uvm_linux.h), and uvm_os.c, uvm_mm.c,
# uvm_rm.c and uvm_serve.c implement it (see ports/nvidia/README.md, "UVM").
#
#   make uvm-objs          # compile UVM + its OS layer into build/uvm/
#   make uvm-undefined     # what RM + NVKMS + UVM + the OS layers still need from outside
#
# The sources are open-gpu-kernel-modules 615.78.08 as vendored in
# third_party/nvidia-open/kernel-open/nvidia-uvm, listed by its nvidia-uvm-sources.Kbuild. Left
# out: uvm_common.c (GPLv2-or-later, not vendored; uvm_os.c implements what uvm_common.h
# declares), uvm_linux.c (Linux's memory cgroups and scatterlist iteration; nothing here uses
# them) and nv-kthread-q.c (Linux kernel threads; uvm_os.c implements nv-kthread-q.h on C11
# threads).

UVM_DIR := $(NVRM_VENDOR)/kernel-open/nvidia-uvm
UVM_OUT := $(BUILD)/uvm

# ---------------------------------------------------------------- source list
NVIDIA_UVM_SOURCES :=
NVIDIA_UVM_SOURCES_CXX :=
include $(UVM_DIR)/nvidia-uvm-sources.Kbuild
UVM_SRCS := $(filter-out uvm_common.c uvm_linux.c nv-kthread-q.c,$(patsubst nvidia-uvm/%,%,$(NVIDIA_UVM_SOURCES)))
NVIDIA_UVM_SOURCES :=
NVIDIA_UVM_SOURCES_CXX :=
UVM_OBJ := $(addprefix $(UVM_OUT)/core/,$(UVM_SRCS:.c=.o))

# ---------------------------------------------------------------- compile flags
# The include order matters: ports/nvidia comes first so that uvm_os.h and the OS layer's
# headers are found, then UVM's own directory, the interface headers shared with RM
# (kernel-open/common/inc for nv_uvm_interface.h and nv-kthread-q.h, src/nvidia/interface for
# nv_uvm_types.h) and the SDK. The definitions are NVIDIA's for this module (nvidia-uvm.Kbuild,
# kernel-open/Kbuild) minus Linux's: no -DLinux/-D__linux__/-D__KERNEL__, no conftest.h.
# NV_KERNEL_INTERFACE_LAYER stays, as on Linux: uvm_os.h includes RM's nv.h as nv-linux.h does,
# and that is what keeps nv.h to the interface-layer view (no nvport, stdint types). NVRM_NOCTURNE
# selects uvm_os.h in uvm_linux.h and the few vendored files that include Linux headers of their
# own.
UVM_INC := ports/nvidia ports/nvidia/include $(UVM_DIR) $(NVRM_VENDOR)/kernel-open/common/inc \
    $(addprefix $(NVRM_VENDOR)/src/,nvidia/interface common/sdk/nvidia/inc common/sdk/nvidia/inc/hw \
        common/inc common/uproc/os/common/include nvidia/arch/nvalloc/unix/include \
        nvidia/arch/nvalloc/common/inc nvidia/inc/kernel) \
    user/include common
UVM_DEFS := -DNVRM_NOCTURNE -DNVIDIA_UVM_ENABLED -DNVIDIA_UNDEF_LEGACY_BIT_MACROS -DNV_UVM_ENABLE \
    -DNV_KERNEL_INTERFACE_LAYER -DNVRM -DNV_VERSION_STRING=\"$(NVRM_VERSION)\"
# The program flags of nvrm (nvrm.mk). The warnings are NVIDIA's for kernel-open (-Wall,
# -Wno-unused-function, -Wno-cast-qual, -Wno-format-extra-args, and -Wno-format-security, as
# the kernel builds it: UVM passes push names through as formats) and clang's wording of what
# gcc accepts in this code (braces around nested initialisers, a variable only written).
UVM_CFLAGS := --target=x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
    -mno-red-zone -msse2 -O2 -g -std=gnu11 -fno-strict-aliasing -fno-omit-frame-pointer -fno-common \
    -ffunction-sections -fdata-sections \
    -Wall -Wno-unused-function -Wno-cast-qual -Wno-format-extra-args -Wno-format-security -Wno-missing-braces \
    -Wno-unused-but-set-variable \
    $(addprefix -I,$(UVM_INC)) $(UVM_DEFS) -MMD -MP

$(UVM_OUT)/core/%.o: $(UVM_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  UVM  $<"
	@$(CC) $(UVM_CFLAGS) -c $< -o $@

# ---------------------------------------------------------------- the UVM object
# One nvidia-uvm.o, as Linux links the module. Its globals are made local except the entry
# points the OS layer calls (module_init/module_exit in uvm_os.h name them); what it needs
# from outside (the OS layer, nvUvmInterface* in uvm_rm.c, libc) stays undefined. UVM carries
# its own nvstatus.c and nvCpuUuid.c, as RM and NVKMS do; locality keeps the three apart.
UVM_EXPORTS := nvrm_uvm_module_init nvrm_uvm_module_exit
$(UVM_OUT)/nvidia-uvm.o: $(UVM_OBJ) ports/nvidia/uvm.mk
	@echo "  LD   $@"
	@$(file >$@.rsp,$(UVM_OBJ))
	@$(LD) -m elf_x86_64 -r @$@.rsp -o $@
	@$(OBJCOPY) $(addprefix --keep-global-symbol=,$(UVM_EXPORTS)) --remove-section=.note.gnu.property $@

# ---------------------------------------------------------------- the OS layer
# Two sides (README "UVM"): uvm_os.c, uvm_mm.c and uvm_serve.c are written against uvm_os.h
# like UVM itself; uvm_rm.c and RM's half of the interface, nvidia.ko's nv_uvm_interface.c
# (vendored in kernel-open/nvidia), against RM's OS layer (nvrm_os.h, uvm_rm.h) with RM's
# flags. Only uvm_nvrm.h and uvm_shared.h are seen by both.
UVM_OS_SRC := ports/nvidia/uvm_os.c ports/nvidia/uvm_mm.c ports/nvidia/uvm_serve.c
UVM_OS_OBJ := $(patsubst ports/nvidia/%.c,$(UVM_OUT)/os/%.o,$(UVM_OS_SRC))
$(UVM_OS_OBJ): $(UVM_OUT)/os/%.o: ports/nvidia/%.c
	@mkdir -p $(dir $@)
	@echo "  NVOS $<"
	@$(CC) $(UVM_CFLAGS) -c $< -o $@
UVM_RM_CFLAGS = $(NVRM_OS_CFLAGS) -I$(NVRM_VENDOR)/kernel-open/common/inc -I$(NVRM_DIR)/inc/kernel/rmapi -DNV_UVM_ENABLE
UVM_RM_OBJ := $(UVM_OUT)/os/uvm_rm.o $(UVM_OUT)/rm/nv_uvm_interface.o
$(UVM_OUT)/os/uvm_rm.o: ports/nvidia/uvm_rm.c
	@mkdir -p $(dir $@)
	@echo "  NVOS $<"
	@$(CC) $(UVM_RM_CFLAGS) -c $< -o $@
$(UVM_OUT)/rm/nv_uvm_interface.o: $(NVRM_VENDOR)/kernel-open/nvidia/nv_uvm_interface.c
	@mkdir -p $(dir $@)
	@echo "  RM   $<"
	@$(CC) $(UVM_RM_CFLAGS) -c $< -o $@

# What nvrm links for UVM (the top-level Makefile's $(PROGRAMS)/nvrm rule).
UVM_LINK_OBJ := $(UVM_OUT)/nvidia-uvm.o $(UVM_OS_OBJ) $(UVM_RM_OBJ)

# ---------------------------------------------------------------- targets
.PHONY: uvm-objs uvm-undefined
uvm-objs: $(UVM_LINK_OBJ)

uvm-undefined: $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ) $(NVKMS_LINK_OBJ) $(UVM_LINK_OBJ)
	@$(LD) -m elf_x86_64 -r $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ) $(NVKMS_LINK_OBJ) $(UVM_LINK_OBJ) -o $(UVM_OUT)/all.o && \
	$(NM) -u $(UVM_OUT)/all.o | awk '{print $$2}' | sort > $(UVM_OUT)/undefined.txt && \
	echo "undefined symbols: $$(wc -l < $(UVM_OUT)/undefined.txt)  ($(UVM_OUT)/undefined.txt)"

-include $(UVM_OBJ:.o=.d) $(UVM_OS_OBJ:.o=.d) $(UVM_RM_OBJ:.o=.d)
