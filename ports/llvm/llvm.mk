# The LLVM toolchain (ports/llvm/README.md), on the disk when its Nocturne build
# exists in tools/llvm/nocturne (python ports/llvm/build.py nocturne). clang,
# lld and the binary tools are one program, llvm, in C:\Nocturne\System, with
# the launcher ports/llvm/tool.c under each tool's name beside it; their part of
# the SDK (clang's headers and runtime, libc++, libc++abi and libunwind) goes
# into C:\Nocturne\SDK with the rest of it (scripts/mksdk.sh --llvm). Without
# that build nothing here is built, so a normal `make` is unchanged.
#
# Needs PROGRAMS, LICENSES, LIBC_RSP and the crt0 object of the libc (Makefile),
# so the Makefile includes this file after the libc link rules, adds
# $(LLVM_FILES) to what goes on the disk (DISK_FILES) and gives mksdk.sh
# $(LLVM_SDK), whose files the SDK is made from ($(LLVM_SDK_FILES)).
LLVM_DIST ?= tools/llvm/nocturne
LLVM_SRC ?= tools/llvm/src
ifneq ($(wildcard $(LLVM_DIST)/System/llvm),)
# The names llvm answers to that a program is run by (llvm-driver.cpp).
LLVM_TOOLS := clang clang++ ld.lld llvm-ar llvm-ranlib llvm-nm llvm-objcopy llvm-strip llvm-objdump \
    llvm-readobj llvm-readelf llvm-size llvm-symbolizer llvm-addr2line llvm-cxxfilt
# The licence of every part of llvm-project in the programs and the SDK, and
# the notices of the code in them under licences of its own, by their place
# in llvm-project.
LLVM_NOTICES := llvm/LICENSE.TXT clang/LICENSE.TXT lld/LICENSE.TXT compiler-rt/LICENSE.TXT \
    libcxx/LICENSE.TXT libcxxabi/LICENSE.TXT libunwind/LICENSE.TXT \
    llvm/lib/Support/COPYRIGHT.regex llvm/lib/Support/BLAKE3/LICENSE
LLVM_SDK := $(LLVM_DIST)/SDK
LLVM_SDK_FILES := $(shell find $(LLVM_SDK) -type f)
LLVM_FILES := $(PROGRAMS)/llvm $(addprefix $(PROGRAMS)/,$(LLVM_TOOLS)) $(addprefix $(LICENSES)/llvm/,$(LLVM_NOTICES))
$(PROGRAMS)/llvm: $(LLVM_DIST)/System/llvm
	@mkdir -p $(dir $@)
	@cp $< $@
$(BUILD)/llvm/tool: $(BUILD)/u/ports/llvm/tool.o $(LIBC_OBJ) $(LIBC_RSP) user/user.ld
	@mkdir -p $(dir $@)
	@echo "  LD   $@"
	@$(LD) $(ULDFLAGS) --gc-sections $(BUILD)/u/user/libc/crt0.asm.o $< --start-lib @$(LIBC_RSP) --end-lib -o $@
$(addprefix $(PROGRAMS)/,$(LLVM_TOOLS)): $(BUILD)/llvm/tool
	@mkdir -p $(dir $@)
	@cp $< $@
$(LICENSES)/llvm/%: $(LLVM_SRC)/%
	@mkdir -p $(dir $@)
	@cp $< $@
endif
