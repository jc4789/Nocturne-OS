# Zink (MIT): Gallium's OpenGL on Vulkan, here on the Vulkan driver linked into the same program
# (NVK, ports/mesa/README.md). The source list follows src/gallium/drivers/zink/meson.build and
# its nir_to_spirv/; the three generated files come from ports/mesa/generate.py.
ZINK := $(MESA)/src/gallium/drivers/zink
ZINK_GEN := $(MESA_GEN)/src/gallium/drivers/zink
ZINK_SRC := $(addprefix $(ZINK)/,zink_lower_cubemap_to_array.c zink_batch.c zink_blit.c zink_bo.c \
    zink_clear.c zink_compiler.c zink_context.c zink_kopper.c zink_descriptors.c zink_draw.cpp \
    zink_fence.c zink_format.c zink_pipeline.c zink_program.c zink_query.c zink_render_pass.c \
    zink_resource.c zink_screen.c zink_state.c zink_surface.c zink_synchronization.cpp \
    nir_to_spirv/nir_to_spirv.c nir_to_spirv/spirv_builder.c) \
    $(addprefix $(ZINK_GEN)/,zink_device_info.c zink_instance.c zink_nir_algebraic.c)
ZINK_OBJ := $(patsubst %,$(BUILD)/mesa/o/%.o,$(ZINK_SRC))
ZINK_FLAGS := -DGALLIUM_ZINK -I$(ZINK) -I$(ZINK_GEN) -I$(MESA)/src/vulkan/util -I$(MESA_GEN)/src/vulkan/util
$(ZINK_OBJ): MESA_CFLAGS += $(ZINK_FLAGS)
$(ZINK_OBJ): MESA_CXXFLAGS += $(ZINK_FLAGS)
# the GL frontend tries Zink before softpipe
$(BUILD)/mesa/o/ports/mesa/nocturne_gl.c.o: MESA_CFLAGS += -DGALLIUM_ZINK
$(filter $(MESA_GEN)/%,$(ZINK_SRC)): $(MESA_GEN_STAMP)
	@:
