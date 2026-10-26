# llvmpipe (MIT): Gallium's rasterizer that compiles each shader to machine code with LLVM
# (gallivm) and draws with every CPU, for WebGL where no GPU draws it. It is built when the LLVM
# port's build for Nocturne (ports/llvm, `build.py nocturne`) has made the JIT's libraries;
# without them softpipe draws alone and this file adds nothing. The source lists follow
# src/gallium/drivers/llvmpipe/meson.build and the draw_with_llvm part of
# src/gallium/auxiliary/meson.build (MCJIT, not ORC: lp_bld_init.c).
#
# Included by mesa.mk before its flags, so DRAW_LLVM_AVAILABLE, which changes the layout of
# draw's structures, is the same in every Mesa object. Sets LLVMPIPE (empty when not built),
# LLVMPIPE_SRC, LLVMPIPE_DEFS, LLVMPIPE_INC and LLVM_JIT_LIBS, the LLVM libraries a program
# that draws with OpenGL is linked against after the libc's objects (Makefile).
LLVM_BUILD ?= tools/llvm/build-nocturne
LLVM_SRC ?= tools/llvm/src
LLVM_HOST ?= tools/llvm/host
LLVM_SYSROOT ?= tools/llvm/sysroot
LLVM_RUNTIMES ?= tools/llvm/runtimes
LLVMPIPE := $(if $(wildcard $(LLVM_BUILD)/lib/libLLVMMCJIT.a),yes)
ifneq ($(LLVMPIPE),)
LLVMPIPE_SRC := $(wildcard $(MESA)/src/gallium/drivers/llvmpipe/*.c) \
    $(filter-out %/lp_bld_init_orc.cpp %/f.cpp,$(wildcard $(MESA)/src/gallium/auxiliary/gallivm/*.c \
      $(MESA)/src/gallium/auxiliary/gallivm/*.cpp)) \
    $(addprefix $(MESA)/src/gallium/auxiliary/,draw/draw_llvm.c draw/draw_pt_fetch_shade_pipeline_llvm.c \
      draw/draw_vs_llvm.c nir/nir_to_tgsi_info.c) \
    $(MESA_GEN)/src/gallium/auxiliary/lp_bld_nir_no_integer_algebraic.c
LLVMPIPE_DEFS := -DLLVM_AVAILABLE -DMESA_LLVM_VERSION_STRING=\"$(shell sed -n \
    's/^.define LLVM_VERSION_STRING "\(.*\)"/\1/p' $(LLVM_BUILD)/include/llvm/Config/llvm-config.h)\" \
    -DLLVM_IS_SHARED=0 -DGALLIUM_LLVMPIPE
# LLVM's headers know Nocturne by the macro the port's clang defines (ports/llvm); the host clang
# that builds the rest of Mesa for Nocturne names another target and does not define it.
LLVMPIPE_INC := -I$(LLVM_SRC)/llvm/include -I$(LLVM_BUILD)/include -D__nocturne__
# Every library of the build (the linker takes only what llvmpipe reaches) and the builtins of the
# port's compiler-rt, which LLVM's code and the code it makes call (lp_bld_init.c).
LLVM_JIT_LIBS := $(wildcard $(LLVM_BUILD)/lib/libLLVM*.a) \
    $(wildcard $(LLVM_HOST)/lib/clang/*/lib/x86_64-unknown-nocturne/libclang_rt.builtins.a)

# gallivm's C++ uses LLVM's C++ API, so it is compiled as LLVM was (ports/llvm/cmake/nocturne.cmake):
# with the port's clang for Nocturne and its libc++, against the SDK that build.py staged.
LLVMPIPE_CXX := $(LLVM_HOST)/bin/clang++.exe --target=x86_64-unknown-nocturne --sysroot=$(LLVM_SYSROOT) \
    -std=c++17 -fno-exceptions -fno-rtti -fno-pic -O2 -g0 -fno-strict-aliasing -fno-math-errno \
    -ffunction-sections -fdata-sections -fno-addrsig -w -MMD -MP
LLVMPIPE_CXX_OBJ := $(patsubst %,$(BUILD)/mesa/o/%.o,$(filter %.cpp,$(LLVMPIPE_SRC)))
$(LLVMPIPE_CXX_OBJ): $(BUILD)/mesa/o/%.cpp.o: %.cpp $(MESA_FLAGS_RECORD) | $(MESA_GEN_STAMP)
	@mkdir -p $(dir $@)
	@echo "  MESA $<"
	@$(LLVMPIPE_CXX) $(MESA_DEFS) $(MESA_INC) -c $< -o $@
endif
