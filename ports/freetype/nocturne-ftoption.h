#pragma once
#include "../../third_party/freetype/include/freetype/config/ftoption.h"
/* Memory-only web outline decoder. No host files, platform font discovery,
 * Type1/PFR/BDF drivers, SVG injection, PNG codec or bytecode interpreter. */
#define FT_CONFIG_OPTION_USE_BROTLI
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_MAC_FONTS
#undef FT_CONFIG_OPTION_SVG
#undef TT_CONFIG_OPTION_COLOR_LAYERS
#undef TT_CONFIG_OPTION_BYTECODE_INTERPRETER
