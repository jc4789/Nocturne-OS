# NVK (MIT): Mesa's Vulkan driver for NVIDIA GPUs, on Nocturne's RM (ports/nvidia) through
# nvkmd/nocturne instead of the nouveau kernel driver; Zink (zink.mk) draws OpenGL on it.
# Source lists follow src/nouveau/**/meson.build and src/vulkan/**/meson.build; generated code
# comes from ports/mesa/generate.py, bindgen and cbindgen. ports/mesa/README.md has the details.
NOUVEAU := $(MESA)/src/nouveau
NV_GEN := $(MESA_GEN)/src/nouveau
NVK_RS := $(BUILD)/mesa/rust
NVK_DIRS := src/nouveau src/nouveau/headers src/nouveau/headers/nvidia/classes src/nouveau/nil src/nouveau/compiler \
    src/nouveau/mme src/nouveau/vulkan src/vulkan/util src/vulkan/runtime src/vulkan/wsi
NVK_INC := $(addprefix -I$(MESA)/,$(NVK_DIRS)) $(addprefix -I$(MESA_GEN)/,$(NVK_DIRS)) \
    -I$(MESA_PREGEN)/src/nouveau/vulkan
# Mesa's -Dvideo-codecs: none here (Nocturne's RM backend has no video decode channel yet);
# MESA_VK_LOG is meson's -Ddebug, which logs every Vulkan error
NVK_DEFS := -DVIDEO_CODEC_H264DEC=0 -DMESA_VK_LOG=0

# C headers of the Rust crates (cbindgen), as nil/meson.build and vulkan/meson.build make them
$(NV_GEN)/nil/nil.h: $(NOUVEAU)/nil/cbindgen.toml $(wildcard $(NOUVEAU)/nil/*.rs)
	@echo "  CBINDGEN $@"
	@mkdir -p $(@D)
	@cbindgen -q --config $< --lang c --output $@ -- $(NOUVEAU)/nil/lib.rs
$(NV_GEN)/vulkan/nvk_rust.h: $(NOUVEAU)/vulkan/rust/cbindgen.toml $(shell find $(NOUVEAU)/vulkan/rust -name '*.rs')
	@echo "  CBINDGEN $@"
	@mkdir -p $(@D)
	@cbindgen -q --config $< --lang c --output $@ -- $(NOUVEAU)/vulkan/rust/lib.rs
NVK_RS_H := $(NV_GEN)/nil/nil.h $(NV_GEN)/vulkan/nvk_rust.h

# ---- Rust: NAK (the shader compiler), nil (image layouts) and NVK's video code -------------
# Mesa's crates are compiled for x86_64-unknown-nocturne against the std of ports/rust and
# bundled into one static library; the procedural macros (paste, NAK's derives) are host DLLs
# from ports/mesa/rust-host, built offline with cargo.
RUST_HOST := $(BUILD)/mesa/rust-host/release
NVK_RUSTC := RUSTC_BOOTSTRAP=1 rustc --edition 2021 -Zunstable-options --target ports/rust/x86_64-unknown-nocturne.json \
    --sysroot tools/rust/sysroot -C opt-level=2 -C panic=abort -C debuginfo=0 -C codegen-units=1 --cap-lints allow \
    -L dependency=$(NVK_RS)
nvk_rlib = $(NVK_RS)/lib$(1).rlib
nvk_extern = $(foreach c,$(1),--extern $(c)=$(call nvk_rlib,$(c)))
NVK_PROC := $(NVK_RS)/host.stamp
NVK_PROC_EXTERN := --extern paste=$(NVK_RS)/host/paste.dll --extern nak_ir_proc=$(NVK_RS)/host/nak_ir_proc.dll -L dependency=$(NVK_RS)/host

$(NVK_PROC): ports/mesa/rust-host/Cargo.toml ports/mesa/rust-host/Cargo.lock $(MESA)/src/compiler/rust/proc/lib.rs \
    $(NOUVEAU)/compiler/nak/ir_proc.rs
	@echo "  CARGO mesa host macros"
	@cd ports/mesa/rust-host && cargo build --release --offline --workspace --quiet \
	    --target-dir ../../../$(BUILD)/mesa/rust-host
	@mkdir -p $(NVK_RS)/host
	@cp $(RUST_HOST)/nak_ir_proc.dll $(NVK_RS)/host/nak_ir_proc.dll
	@cp $$(ls -t $(RUST_HOST)/deps/paste-*.dll | head -n 1) $(NVK_RS)/host/paste.dll
	@touch $@

# bindgen parses Mesa's headers with the same target and macros as the C code.
NVK_BINDGEN := bindgen --rust-edition 2021 --wrap-unsafe-ops --raw-line '\#![allow(clippy::all)]' \
    --raw-line '\#![allow(warnings)]' --raw-line '\#![allow(unsafe_op_in_unsafe_fn)]'
NVK_BINDGEN_CLANG = --target=x86_64-unknown-none-elf -ffreestanding -std=gnu11 $(MESA_DEFS) $(NVK_DEFS) \
    $(MESA_INC) $(NVK_INC) -Iuser/include -Icommon

# compiler: NIR for Rust (src/compiler/rust). Its lib.rs names a bindings module, so the crate
# is assembled next to the generated bindings.rs.
COMPILER_RS_TYPES := exec_list exec_node float_controls gc_ctx gl_access_qualifier gl_frag_result gl_interp_mode \
    mesa_shader_stage gl_subgroup_size gl_system_value gl_tess_spacing gl_varying_slot gl_vert_attrib \
    glsl_sampler_dim glsl_type glsl_matrix_layout 'nir_.*' mesa_scope mesa_prim shader_info \
    tess_primitive_mode u_printf_info util_dynarray
COMPILER_RS_BINDGEN := --allowlist-var 'NIR_.*' --allowlist-var 'nir_.*_infos' --allowlist-var 'rust_.*' \
    --allowlist-function 'glsl_.*' --allowlist-function _mesa_shader_stage_to_string \
    --allowlist-function '_mesa_.*half.*' --allowlist-function '_mesa_.*float16.*' --allowlist-function 'nir_.*' \
    --allowlist-function 'compiler_rs.*' --allowlist-function 'u_memstream.*' \
    --allowlist-function 'util_dynarray.*' --allowlist-type u_memstream --no-prepend-enum-name \
    --with-derive-default $(foreach t,$(COMPILER_RS_TYPES),--allowlist-type $(t))
COMPILER_RS_BLOCKLIST := $(foreach t,$(COMPILER_RS_TYPES),--blocklist-type $(t))
COMPILER_RS := $(MESA)/src/compiler/rust
COMPILER_RS_GEN := $(MESA_GEN)/src/compiler/rust
COMPILER_RS_SRC := $(filter-out %/bindings.rs,$(wildcard $(COMPILER_RS)/*.rs))
$(COMPILER_RS_GEN)/bindings.rs: $(COMPILER_RS)/bindings.h $(COMPILER_RS)/rust_helpers.h | $(MESA_GEN_STAMP)
	@echo "  BINDGEN $@"
	@mkdir -p $(@D)
	@$(NVK_BINDGEN) $(COMPILER_RS_BINDGEN) --experimental --wrap-static-fns-suffix _compiler_rs_extern \
	    --wrap-static-fns --wrap-static-fns-path $(COMPILER_RS_GEN)/bindings.c $< -o $@ -- $(NVK_BINDGEN_CLANG) || { rm -f $@; false; }
$(COMPILER_RS_GEN)/bindings.c: $(COMPILER_RS_GEN)/bindings.rs
	@:
$(NVK_RS)/compiler/lib.rs: $(COMPILER_RS_SRC) $(COMPILER_RS_GEN)/bindings.rs
	@mkdir -p $(@D)
	@cp $(COMPILER_RS_SRC) $(COMPILER_RS_GEN)/bindings.rs $(@D)/
$(call nvk_rlib,compiler): $(NVK_RS)/compiler/lib.rs
	@echo "  RUSTC compiler"
	@$(NVK_RUSTC) --crate-type rlib --crate-name compiler $< --emit=link=$@,dep-info=$(@:.rlib=.d)

$(call nvk_rlib,rustc_hash): third_party/rustc-hash/src/lib.rs
	@echo "  RUSTC rustc_hash"
	@mkdir -p $(@D)
	@$(NVK_RUSTC) --crate-type rlib --crate-name rustc_hash --cfg 'feature="std"' $< \
	    --emit=link=$@,dep-info=$(@:.rlib=.d)

$(call nvk_rlib,mesa_util): $(MESA)/src/util/rust/lib.rs
	@echo "  RUSTC mesa_util"
	@mkdir -p $(@D)
	@$(NVK_RUSTC) --crate-type rlib --crate-name mesa_util $< --emit=link=$@,dep-info=$(@:.rlib=.d)

# nvidia_headers: the classes as Rust (generate.py) and the MMU layouts of dev_mmu.h (bindgen).
NV_HWREF := maxwell/gm107 pascal/gp100 hopper/gh100 turing/tu102
NV_HWREF_RS := $(foreach g,$(NV_HWREF),$(NV_GEN)/headers/nvh_hwref_$(notdir $(g))_mmu.rs)
define nv_hwref_rule
$(NV_GEN)/headers/nvh_hwref_$(notdir $(1))_mmu.rs: $(NOUVEAU)/headers/nvidia/hwref/$(1)/dev_mmu.h | $(MESA_GEN_STAMP)
	@echo "  BINDGEN $$@"
	@$$(NVK_BINDGEN) --allowlist-var 'NV_MMU_.*' $$< -o $$@ -- $$(NVK_BINDGEN_CLANG) || { rm -f $$@; false; }
endef
$(foreach g,$(NV_HWREF),$(eval $(call nv_hwref_rule,$(g))))
$(call nvk_rlib,nvidia_headers): $(NV_HWREF_RS) $(MESA_GEN_STAMP)
	@echo "  RUSTC nvidia_headers"
	@mkdir -p $(@D)
	@$(NVK_RUSTC) --crate-type rlib --crate-name nvidia_headers $(NV_GEN)/headers/lib.rs \
	    --emit=link=$@,dep-info=$(@:.rlib=.d)

$(call nvk_rlib,nv_push_rs): $(NOUVEAU)/headers/nv_push_rs/lib.rs $(call nvk_rlib,nvidia_headers)
	@echo "  RUSTC nv_push_rs"
	@$(NVK_RUSTC) --crate-type rlib --crate-name nv_push_rs $< $(call nvk_extern,nvidia_headers) \
	    --emit=link=$@,dep-info=$(@:.rlib=.d)

$(call nvk_rlib,nak_latencies): $(MESA_GEN_STAMP)
	@echo "  RUSTC nak_latencies"
	@mkdir -p $(@D)
	@$(NVK_RUSTC) --crate-type rlib --crate-name nak_latencies $(NV_GEN)/compiler/latencies/lib.rs \
	    --emit=link=$@,dep-info=$(@:.rlib=.d)

# nak_bindings: NAK's C interface. The DRM and winsys parts of nak_bindings.h only serve the
# hardware tests, which are not built.
$(NV_GEN)/compiler/nak_bindings.rs: $(NOUVEAU)/compiler/nak_bindings.h | $(MESA_GEN_STAMP)
	@echo "  BINDGEN $@"
	@$(NVK_BINDGEN) $(COMPILER_RS_BLOCKLIST) --raw-line 'use compiler::bindings::*;' \
	    --allowlist-type 'nak_.*' --allowlist-var 'NAK_.*' --allowlist-var NVIDIA_VENDOR_ID \
	    --allowlist-function 'nak_.*' --blocklist-type 'glsl_.*' --blocklist-type 'pipe_format.*' \
	    --no-prepend-enum-name --with-derive-default $< -o $@ -- $(NVK_BINDGEN_CLANG) || { rm -f $@; false; }
$(call nvk_rlib,nak_bindings): $(NV_GEN)/compiler/nak_bindings.rs $(call nvk_rlib,compiler)
	@echo "  RUSTC nak_bindings"
	@$(NVK_RUSTC) --crate-type rlib --crate-name nak_bindings $< $(call nvk_extern,compiler) \
	    --emit=link=$@,dep-info=$(@:.rlib=.d)

NAK_RS_DEPS := rustc_hash compiler mesa_util nvidia_headers nak_bindings nak_latencies
$(call nvk_rlib,nak): $(NOUVEAU)/compiler/nak/lib.rs $(foreach c,$(NAK_RS_DEPS),$(call nvk_rlib,$(c))) $(NVK_PROC)
	@echo "  RUSTC nak"
	@$(NVK_RUSTC) --crate-type rlib --crate-name nak $< $(call nvk_extern,$(NAK_RS_DEPS)) $(NVK_PROC_EXTERN) \
	    --emit=link=$@,dep-info=$(@:.rlib=.d)

# nil: image layouts.
NIL_BINDGEN := $(foreach f,util_format_description util_format_get_blocksize util_format_is_compressed \
    util_format_is_pure_integer util_format_is_srgb drm_format_mod_block_linear_2D drm_mod_is_nvidia,\
    --allowlist-function $(f)) $(foreach t,nil_format_support_flags nv_device_info nv_device_type \
    nv_zcull_device_info pipe_format pipe_swizzle,--allowlist-type $(t)) \
    $(foreach v,nil_format_table drm_format_mod_invalid drm_format_mod_linear,--allowlist-var $(v)) \
    --no-prepend-enum-name
$(NV_GEN)/nil/nil_bindings.rs: $(NOUVEAU)/nil/nil_bindings.h | $(MESA_GEN_STAMP)
	@echo "  BINDGEN $@"
	@$(NVK_BINDGEN) $(NIL_BINDGEN) $< -o $@ -- $(NVK_BINDGEN_CLANG) || { rm -f $@; false; }
$(call nvk_rlib,nil_rs_bindings): $(NV_GEN)/nil/nil_bindings.rs
	@echo "  RUSTC nil_rs_bindings"
	@mkdir -p $(@D)
	@$(NVK_RUSTC) --crate-type rlib --crate-name nil_rs_bindings $< --emit=link=$@,dep-info=$(@:.rlib=.d)
NIL_RS_DEPS := mesa_util nvidia_headers nil_rs_bindings
$(call nvk_rlib,nil): $(NOUVEAU)/nil/lib.rs $(foreach c,$(NIL_RS_DEPS),$(call nvk_rlib,$(c))) $(NVK_PROC)
	@echo "  RUSTC nil"
	@$(NVK_RUSTC) --crate-type rlib --crate-name nil $< $(call nvk_extern,$(NIL_RS_DEPS)) $(NVK_PROC_EXTERN) \
	    --emit=link=$@,dep-info=$(@:.rlib=.d)

# nvk_rs: NVK's Rust (video decode command building).
NVK_VK := $(NOUVEAU)/vulkan
NVK_VK_GEN := $(NV_GEN)/vulkan
$(NVK_VK_GEN)/nvk_bindings.rs: $(NVK_VK)/rust/bindings.h $(NVK_RS_H) | $(MESA_GEN_STAMP)
	@echo "  BINDGEN $@"
	@$(NVK_BINDGEN) --no-prepend-enum-name --with-derive-default \
	    --allowlist-type 'Std.*|vk_video.*|.*nv.*|Vk.*|vk.*' --allowlist-function '.*vk_find.*|.*vk_video.*|nvk.*' \
	    --allowlist-function vk_buffer_address --allowlist-var '.*SUBC.*' --experimental --wrap-static-fns \
	    --wrap-static-fns-path $(NVK_VK_GEN)/nvk_inline_wrappers.c $< -o $@ -- $(NVK_BINDGEN_CLANG) || { rm -f $@; false; }
$(NVK_VK_GEN)/nvk_inline_wrappers.c: $(NVK_VK_GEN)/nvk_bindings.rs
	@:
$(call nvk_rlib,nvk_bindings_rs): $(NVK_VK_GEN)/nvk_bindings.rs
	@echo "  RUSTC nvk_bindings_rs"
	@mkdir -p $(@D)
	@$(NVK_RUSTC) --crate-type rlib --crate-name nvk_bindings_rs $< --emit=link=$@,dep-info=$(@:.rlib=.d)
NVK_RS_DEPS := rustc_hash nv_push_rs nvidia_headers nvk_bindings_rs
$(call nvk_rlib,nvk_rs): $(NVK_VK)/rust/lib.rs $(foreach c,$(NVK_RS_DEPS),$(call nvk_rlib,$(c))) $(NVK_PROC)
	@echo "  RUSTC nvk_rs"
	@$(NVK_RUSTC) --crate-type rlib --crate-name nvk_rs $< $(call nvk_extern,$(NVK_RS_DEPS)) $(NVK_PROC_EXTERN) \
	    --emit=link=$@,dep-info=$(@:.rlib=.d)

# One static library with the three crates NVK's C calls and the Rust std they share.
NVK_RS_LIB := $(NVK_RS)/libnvk_mesa_rs.a
$(NVK_RS_LIB): ports/mesa/nvk_rs.rs $(foreach c,nak nil nvk_rs,$(call nvk_rlib,$(c)))
	@echo "  RUSTC $@"
	@$(NVK_RUSTC) --crate-type staticlib --crate-name nvk_mesa_rs $< $(call nvk_extern,nak nil nvk_rs) $(NVK_PROC_EXTERN) \
	    --emit=link=$@,dep-info=$(@:.a=.d)

-include $(wildcard $(NVK_RS)/*.d)

# ---- C: Mesa's Vulkan runtime and NVK --------------------------------------------------------
# The runtime is the full one (meta, pipelines, NIR) without the parts that need glslang (ASTC,
# acceleration-structure BVH builds) or DRM (vk_drm_syncobj.c); WSI is the headless platform only,
# since Zink draws into its own images. vk_instance.c is compiled once, as the full runtime's.
VK := $(MESA)/src/vulkan
VK_GEN := $(MESA_GEN)/src/vulkan
VK_SRC := $(addprefix $(VK)/util/,vk_alloc.c vk_format.c vk_util.c) \
    $(addprefix $(VK_GEN)/util/,vk_dispatch_table.c vk_enum_to_str.c vk_extensions.c) \
    $(addprefix $(VK)/runtime/,rmv/vk_rmv_common.c rmv/vk_rmv_exporter.c vk_blend.c vk_buffer.c \
    vk_buffer_view.c vk_cmd_copy.c vk_cmd_enqueue.c vk_command_buffer.c vk_command_pool.c vk_debug_report.c \
    vk_debug_utils.c vk_deferred_operation.c vk_descriptor_set_layout.c vk_descriptors.c \
    vk_descriptor_update_template.c vk_device.c vk_device_generated_commands.c vk_device_memory.c vk_fence.c \
    vk_framebuffer.c vk_frame_pacer.c vk_graphics_state.c vk_image.c vk_instance.c vk_log.c \
    vk_meta_object_list.c vk_object.c vk_physical_device.c vk_pipeline_layout.c vk_query_pool.c vk_queue.c \
    vk_render_pass.c vk_sampler.c vk_semaphore.c vk_standard_sample_locations.c vk_sync.c vk_sync_binary.c \
    vk_sync_dummy.c vk_sync_timeline.c vk_synchronization.c vk_video.c vk_ycbcr_conversion.c \
    vk_meta.c vk_meta_blit_resolve.c vk_meta_clear.c vk_meta_copy_fill_update.c vk_meta_draw_rects.c \
    vk_nir.c vk_nir_convert_ycbcr.c vk_nir_lower_descriptor_heaps.c vk_pipeline.c vk_pipeline_cache.c \
    vk_shader.c vk_shader_module.c vk_texcompress_etc2.c) \
    $(addprefix $(VK_GEN)/runtime/,vk_cmd_enqueue_entrypoints.c vk_cmd_queue.c vk_common_entrypoints.c \
    vk_dispatch_trampolines.c vk_format_info.c vk_physical_device_features.c vk_physical_device_properties.c \
    vk_physical_device_spirv_caps.c vk_synchronization_helpers.c) \
    $(VK)/wsi/wsi_common.c $(VK)/wsi/wsi_common_headless.c $(VK_GEN)/wsi/wsi_common_entrypoints.c

# NVK with the nvkmd/nocturne backend in place of nvkmd/nouveau, and its helper libraries:
# the class push helpers, MME, nil's format table and NAK's C (NIR passes). nvk_cubin.c (ELF cubin
# import for VK_NVX_binary_import) needs libelf and is left out with that extension. nvkcl.cpp is
# NVK's OpenCL kernels as NIR builders, pregenerated with mesa_clc (ports/mesa/README.md).
NV_CLASSES := $(shell $(PY) ports/mesa/generate.py nv-classes)
NVK_SRC := $(NOUVEAU)/headers/nv_push.c $(patsubst %,$(NV_GEN)/headers/nv_push_%.c,$(NV_CLASSES)) \
    $(addprefix $(NOUVEAU)/mme/,mme_fermi.c mme_fermi_builder.c mme_fermi_sim.c mme_sim.c mme_tu104.c \
    mme_tu104_builder.c mme_tu104_sim.c) \
    $(NV_GEN)/nil/nil_format_table.c \
    $(addprefix $(NOUVEAU)/compiler/,nak_nir.c nak_nir_lower_cf.c nak_nir_lower_cmat.c nak_nir_lower_fs_inputs.c \
    nak_nir_lower_gs_intrinsics.c nak_nir_lower_image_addrs.c nak_nir_lower_mesh_intrinsics.c \
    nak_nir_lower_non_uniform_ldcx.c nak_nir_lower_scan_reduce.c nak_nir_lower_shared_atomics.c \
    nak_nir_lower_tex.c nak_nir_lower_vtg_io.c nak_nir_mark_lcssa_invariants.c \
    nak_nir_rematerialize_load_const.c) $(NV_GEN)/compiler/nak_nir_algebraic.c \
    $(filter-out %/nvk_cubin.c,$(wildcard $(NVK_VK)/*.c)) $(NVK_VK)/layers/nvk_app_workarounds.c \
    $(NVK_VK)/nvkmd/nvkmd.c $(NVK_VK)/nvkmd/nvkmd_device_info.c $(wildcard $(NVK_VK)/nvkmd/nocturne/*.c) \
    $(NVK_VK_GEN)/nvk_entrypoints.c $(NVK_VK_GEN)/nvk_drirc.c $(MESA_PREGEN)/src/nouveau/vulkan/nvkcl.cpp \
    $(COMPILER_RS_GEN)/bindings.c $(COMPILER_RS)/rust_helpers.c $(NVK_VK_GEN)/nvk_inline_wrappers.c
NVK_C_OBJ := $(patsubst %,$(BUILD)/mesa/o/%.o,$(VK_SRC) $(NVK_SRC))
# -I. for the bindgen wrappers, which name their header from the repository root
NVK_FLAGS := $(NVK_INC) $(NVK_DEFS) -I.
$(NVK_C_OBJ): MESA_CFLAGS += $(NVK_FLAGS)
$(NVK_C_OBJ): MESA_CXXFLAGS += $(NVK_FLAGS)
$(NVK_C_OBJ): $(NVK_RS_H)
# nvkmd_nocturne_rm.c talks to /bin/nvrm (ports/nvidia) with the RM's own SDK headers. They go first:
# the RM headers include <nvtypes.h> and must get the RM's, not the one in nouveau/headers.
$(BUILD)/mesa/o/$(NVK_VK)/nvkmd/nocturne/nvkmd_nocturne_rm.c.o: MESA_CFLAGS := -Iports/nvidia/include \
    $(addprefix -Ithird_party/nvidia-open/src/,common/sdk/nvidia/inc nvidia/arch/nvalloc/unix/include) \
    $(MESA_CFLAGS) $(NVK_FLAGS)
# (the bindgen wrappers have their own rules above)
$(filter-out $(COMPILER_RS_GEN)/bindings.c $(NVK_VK_GEN)/nvk_inline_wrappers.c,\
    $(filter $(MESA_GEN)/%,$(VK_SRC) $(NVK_SRC))): $(MESA_GEN_STAMP)
	@:

# Everything a program that uses NVK links: the C objects (lazy members of the libc link) and the
# Rust archive.
NVK_OBJ := $(NVK_C_OBJ) $(NVK_RS_LIB)

.PHONY: nvk
nvk: $(NVK_OBJ) $(NVK_RS_LIB) $(ZINK_OBJ)
