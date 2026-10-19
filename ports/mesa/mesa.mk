# Mesa (MIT): the GLSL/NIR compiler, the OpenGL ES 3 state tracker and the
# Gallium softpipe rasterizer, used by WebGL. No LLVM, GLX/EGL, DRM or
# platform window systems. Source lists follow Mesa's meson.build files for
# a softpipe-only build; ports/mesa/README.md records the local changes.
MESA := third_party/mesa
MESA_GEN := $(BUILD)/mesa/gen
MESA_PREGEN := ports/mesa/generated
MESA_GEN_STAMP := $(BUILD)/mesa/gen.stamp

MESA_UTIL_SRC := $(addprefix $(MESA)/src/util/,bitscan.c blob.c build_id.c cnd_monotonic.c \
    thread_sched.c crc32.c dag.c double.c fast_idiv_by_const.c float8.c futex.c half_float.c hash_table.c \
    helpers.c u_idalloc.c log.c lut.c memstream.c mesa-blake3.c os_time.c os_misc.c pb_slab.c \
    streaming-load-memcpy.c \
    perf/u_trace.c u_process.c u_qsort.cpp rwlock.c ralloc.c rand_xor.c range_minimum_query.c rb_tree.c \
    register_allocate.c rgtc.c set.c simple_mtx.c slab.c softfloat.c sparse_array.c string_buffer.c strndup.c \
    strtod.c texcompress_astc_luts.cpp texcompress_astc_luts_wrap.cpp texcompress_astc.cpp u_atomic.c \
    u_call_once.c u_dl.c u_dynarray.c u_hash_table.c u_queue.c u_range_remap.c u_shader_variant_cache.c u_string.c \
    u_thread.c u_vector.c u_math.c u_mm.c u_debug.c u_debug_memory.c u_cpu_detect.c u_printf.c u_worklist.c \
    vl_zscan_data.c vma.c cache_ops_null.c xmlconfig.c \
    format/u_format.c format/u_format_bptc.c format/u_format_etc.c format/u_format_fxt1.c \
    format/u_format_latc.c format/u_format_other.c format/u_format_rgtc.c format/u_format_s3tc.c \
    format/u_format_yuv.c format/u_format_zs.c \
    blake3/blake3.c blake3/blake3_dispatch.c blake3/blake3_portable.c)

MESA_COMPILER_SRC := $(addprefix $(MESA)/src/compiler/,glsl_types.c shader_enums.c) \
    $(wildcard $(MESA)/src/compiler/nir/*.c) \
    $(addprefix $(MESA)/src/compiler/spirv/,gl_spirv.c spirv_to_nir.c vtn_alu.c vtn_amd.c vtn_cfg.c \
    vtn_cmat.c vtn_debug.c vtn_glsl450.c vtn_opencl.c vtn_structured_cfg.c vtn_subgroup.c vtn_variables.c) \
    $(filter-out %/main.cpp %/standalone.cpp %/standalone_scaffolding.cpp,\
      $(wildcard $(MESA)/src/compiler/glsl/*.c $(MESA)/src/compiler/glsl/*.cpp)) \
    $(MESA)/src/compiler/glsl/glcpp/pp.c
MESA_COMPILER_SRC := $(filter-out %/nir_stub.c,$(MESA_COMPILER_SRC))

MESA_GL_SRC := $(filter-out %/sse_minmax.c %/dummy_errors.c,$(wildcard \
    $(MESA)/src/mesa/main/*.c $(MESA)/src/mesa/main/*.cpp $(MESA)/src/mesa/math/*.c \
    $(MESA)/src/mesa/program/*.c $(MESA)/src/mesa/state_tracker/*.c $(MESA)/src/mesa/state_tracker/*.cpp \
    $(MESA)/src/mesa/vbo/*.c)) \
    $(MESA)/src/mesa/glapi/shared-glapi/core.c $(MESA)/src/mesa/glapi/es2api/libgles2_public.c

MESA_GALLIUM_SRC := $(addprefix $(MESA)/src/gallium/auxiliary/,\
    pipebuffer/pb_buffer_fenced.c pipebuffer/pb_bufmgr_cache.c pipebuffer/pb_bufmgr_debug.c \
    pipebuffer/pb_bufmgr_mm.c pipebuffer/pb_bufmgr_slab.c pipebuffer/pb_cache.c pipebuffer/pb_validate.c \
    util/u_async_debug.c util/u_bitmask.c util/u_blitter.c util/u_debug_describe.c util/u_debug_flush.c \
    util/u_debug_image.c util/u_debug_refcnt.c util/u_draw.c util/u_draw_quad.c util/u_driconf.c \
    util/u_dump_defines.c util/u_dump_state.c util/u_framebuffer.c util/u_gen_mipmap.c util/u_handle_table.c \
    util/u_helpers.c util/u_index_modify.c util/u_live_shader_cache.c util/u_log.c util/u_prim.c \
    util/u_prim_restart.c util/u_pstipple.c util/u_resource.c util/u_sample_positions.c util/u_sampler.c \
    util/u_screen.c util/u_split_draw.c util/u_suballoc.c util/u_surface.c util/u_texture.c util/u_tile.c \
    util/u_transfer.c util/u_transfer_helper.c util/u_threaded_context.c util/u_trace_gallium.c \
    util/u_upload_mgr.c util/u_vbuf.c util/u_vertex_state_cache.c util/u_simple_shaders.c util/u_tests.c \
    cso_cache/cso_cache.c cso_cache/cso_context.c cso_cache/cso_hash.c \
    indices/u_primconvert.c rtasm/rtasm_execmem.c rtasm/rtasm_x86sse.c \
    translate/translate.c translate/translate_cache.c translate/translate_generic.c translate/translate_sse.c \
    nir/tgsi_to_nir.c nir/nir_to_tgsi.c nir/nir_draw_helpers.c) \
    $(wildcard $(MESA)/src/gallium/auxiliary/draw/*.c $(MESA)/src/gallium/auxiliary/tgsi/*.c) \
    $(wildcard $(MESA)/src/gallium/auxiliary/driver_trace/*.c $(MESA)/src/gallium/auxiliary/tessellator/*.cpp) \
    $(wildcard $(MESA)/src/gallium/drivers/softpipe/*.c)
MESA_GALLIUM_SRC := $(filter-out %/draw_llvm.c %/draw_llvm_sample.c %/draw_pt_fetch_shade_pipeline_llvm.c \
    %/draw_vs_llvm.c %/draw_gs_llvm.c %/draw_tess_llvm.c,$(MESA_GALLIUM_SRC))

# Code made by the build (ports/mesa/generate.py) and by bison/flex (checked in).
MESA_GEN_SRC := $(addprefix $(MESA_GEN)/src/,util/format_srgb.c util/format/u_format_table.c \
    compiler/builtin_types.c compiler/nir/nir_constant_expressions.c compiler/nir/nir_opcodes.c \
    compiler/nir/nir_opt_algebraic.c compiler/nir/nir_intrinsics.c compiler/spirv/vtn_gather_types.c \
    gallium/auxiliary/draw_nir_lower_opcodes.c gallium/auxiliary/u_tracepoints.c gallium/auxiliary/tr_util.c \
    gallium/auxiliary/u_indices_gen.c gallium/auxiliary/u_unfilled_gen.c mesa/format_fallback.c \
    mesa/glapi/glapi/gen/enums.c mesa/glapi/glapi/gen/api_exec_init.c mesa/glapi/glapi/gen/unmarshal_table.c \
    mesa/glapi/glapi/gen/api_trace.c $(foreach n,0 1 2 3 4 5 6 7,mesa/glapi/glapi/gen/marshal_generated$(n).c))
MESA_SPIRV_INFO := $(MESA_GEN)/src/compiler/spirv/spirv_info.c
MESA_GEN_SRC += $(MESA_SPIRV_INFO)
MESA_PREGEN_SRC := $(addprefix $(MESA_PREGEN)/src/,compiler/glsl/glsl_parser.cpp compiler/glsl/glsl_lexer.cpp \
    compiler/glsl/glcpp/glcpp-parse.c compiler/glsl/glcpp/glcpp-lex.c mesa/program/program_parse.tab.c \
    mesa/program/lex.yy.c)

# Nocturne's own frontend: a softpipe screen on a memory winsys and GL contexts.
MESA_PORT_SRC := ports/mesa/nocturne_gl.c

MESA_SRC := $(MESA_UTIL_SRC) $(MESA_COMPILER_SRC) $(MESA_GL_SRC) $(MESA_GALLIUM_SRC) $(MESA_GEN_SRC) \
    $(MESA_PREGEN_SRC) $(MESA_PORT_SRC)
MESA_OBJ := $(patsubst %,$(BUILD)/mesa/o/%.o,$(MESA_SRC))

MESA_DIRS := include src src/mesa src/mesa/main src/mesa/glapi src/mesa/glapi/glapi src/mesa/glapi/glapi/gen \
    src/mesa/glapi/shared-glapi src/mesa/glapi/es2api src/gallium/include src/gallium/auxiliary src/gallium/drivers \
    src/gallium/winsys src/compiler src/compiler/glsl src/compiler/glsl/glcpp src/compiler/nir \
    src/compiler/spirv src/util src/util/format src/mesa/program src/gallium/auxiliary/util
MESA_INC := $(addprefix -I$(MESA)/,$(MESA_DIRS)) $(addprefix -I$(MESA_GEN)/,$(filter-out include,$(MESA_DIRS))) \
    $(addprefix -I$(MESA_PREGEN)/,src/compiler/glsl src/compiler/glsl/glcpp src/mesa src/mesa/program) -Iports/mesa
MESA_DEFS := -DHAVE_OPENGL=1 -DHAVE_OPENGL_ES_1=0 -DHAVE_OPENGL_ES_2=1 -DHAVE_SOFTPIPE -DHAVE_SWRAST \
    -DGALLIUM_SOFTPIPE -DGLAPI_EXPORT_PROTO_ENTRY_POINTS=0 -DUSE_LIBGLVND=0 -DMESA_DEBUG=0 -DNDEBUG \
    -DHAVE___BUILTIN_BSWAP32 -DHAVE___BUILTIN_BSWAP64 -DHAVE___BUILTIN_CLZ -DHAVE___BUILTIN_CLZLL \
    -DHAVE___BUILTIN_CTZ -DHAVE___BUILTIN_EXPECT -DHAVE___BUILTIN_FFS -DHAVE___BUILTIN_FFSLL \
    -DHAVE___BUILTIN_POPCOUNT -DHAVE___BUILTIN_POPCOUNTLL -DHAVE___BUILTIN_UNREACHABLE \
    -DHAVE___BUILTIN_TYPES_COMPATIBLE_P -DHAVE___BUILTIN_ADD_OVERFLOW -DHAVE___BUILTIN_IA32_CLFLUSHOPT=0 \
    -DHAVE_FUNC_ATTRIBUTE_CONST -DHAVE_FUNC_ATTRIBUTE_FLATTEN -DHAVE_FUNC_ATTRIBUTE_MALLOC \
    -DHAVE_FUNC_ATTRIBUTE_PURE -DHAVE_FUNC_ATTRIBUTE_UNUSED -DHAVE_FUNC_ATTRIBUTE_WARN_UNUSED_RESULT \
    -DHAVE_FUNC_ATTRIBUTE_WEAK -DHAVE_FUNC_ATTRIBUTE_FORMAT -DHAVE_FUNC_ATTRIBUTE_PACKED \
    -DHAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL -DHAVE_FUNC_ATTRIBUTE_ALIAS -DHAVE_FUNC_ATTRIBUTE_NORETURN \
    -DHAVE_FUNC_ATTRIBUTE_OPTIMIZE -DHAVE_FUNC_ATTRIBUTE_COLD -DHAVE_FUNC_ATTRIBUTE_VISIBILITY \
    -DHAVE_UINT128 -DUSE_GCC_ATOMIC_BUILTINS -DHAVE_STRUCT_TIMESPEC -DHAVE_THRD_CREATE -DHAVE_TIMESPEC_GET -DHAVE_QSORT_S=1 -DNO_REGEX \
    -DDRAW_LLVM_AVAILABLE=0 -DAMD_LLVM_AVAILABLE=0 -DGALLIVM_USE_ORCJIT=0 -DHAVE_RENDERDOC_INTEGRATION=0 \
    -DTHREAD_SANITIZER=0 -DPACKAGE_VERSION=\"26.3.0-devel\" -DPACKAGE_BUGREPORT=\"\" -DWITH_XMLCONFIG=0 \
    -DXXH_FORCE_ALIGN_CHECK=0 -DXXH_FORCE_MEMORY_ACCESS=0 -DYY_NO_UNISTD_H -DMAPI_MODE_SHARED_GLAPI \
    -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0 \
    -DENABLE_ST_OMX_BELLAGIO=0 -DENABLE_ST_OMX_TIZONIA=0 -DHAVE_COMPRESSION=0 -DNOCTURNE
MESA_COMMON := --target=x86_64-unknown-none-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
    -mno-red-zone -msse2 -O2 -g0 -fno-strict-aliasing -fno-math-errno -fno-trapping-math -fomit-frame-pointer \
    -ffunction-sections -fdata-sections -fno-addrsig -w -MMD -MP $(MESA_DEFS) $(MESA_INC)
MESA_CFLAGS := $(MESA_COMMON) -std=gnu11 -Iuser/include -Icommon
MESA_CXXFLAGS := $(MESA_COMMON) -std=c++17 -fno-exceptions -fno-rtti -nostdinc++ -Iuser/include/c++ \
    -I$(BUILD)/cxx/upstream/libcxx/include -idirafter user/include -Icommon

# The WebGL binding calls the public GLES entry points and Mesa's pixel
# format helpers; its constant tables come from the same Khronos headers.
WEBGL_OBJ := $(BUILD)/u/user/libc/web/js_webgl.o
$(WEBGL_OBJ): UCFLAGS += $(MESA_DEFS) -I$(MESA)/include -I$(MESA)/src -Iports/mesa
$(WEBGL_OBJ): user/libc/web/js_webgl_constants.inc
user/libc/web/js_webgl_constants.inc: user/libc/web/js_webgl_constants.py $(MESA)/include/GLES2/gl2.h $(MESA)/include/GLES3/gl3.h
	@$(PY) user/libc/web/js_webgl_constants.py
	@touch $@

MESA_GEN_INPUTS := ports/mesa/generate.py $(shell find $(MESA)/src -name '*.py' -o -name '*.xml' -o -name '*.yaml' \
    -o -name '*.csv' -o -name '*.json' -o -name '*.glsl' -o -name '*.conf' -o -name '*.rnc' -o -name '*.txt')
$(MESA_GEN_STAMP): $(MESA_GEN_INPUTS)
	@echo "  GEN  mesa"
	@$(PY) ports/mesa/generate.py
	@touch $@
$(filter $(MESA_GEN)/%,$(MESA_GEN_SRC)): $(MESA_GEN_STAMP)
	@:

$(BUILD)/mesa/o/%.c.o: %.c | $(MESA_GEN_STAMP)
	@mkdir -p $(dir $@)
	@echo "  MESA $<"
	@$(CC) $(MESA_CFLAGS) -c $< -o $@
$(BUILD)/mesa/o/%.cpp.o: %.cpp $(BUILD)/cxx/libc++.a | $(MESA_GEN_STAMP)
	@mkdir -p $(dir $@)
	@echo "  MESA $<"
	@$(CC) $(MESA_CXXFLAGS) -x c++ -c $< -o $@

.PHONY: mesa
mesa: $(MESA_OBJ)

include ports/mesa/zink.mk ports/mesa/nvk.mk
