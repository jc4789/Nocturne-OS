#pragma once
#include "../../third_party/freetype/include/freetype/config/ftoption.h"
/* Memory-only web outline decoder. No host files, platform font discovery,
 * Type1/PFR/BDF drivers, SVG injection, PNG codec or bytecode interpreter. */
#define FT_CONFIG_OPTION_USE_BROTLI
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_MAC_FONTS
#undef FT_CONFIG_OPTION_SVG
/* TT_CONFIG_OPTION_COLOR_LAYERS stays on: COLR/CPAL parsing for color glyphs
 * (font.c rasterizes the paint graph itself). */
#undef TT_CONFIG_OPTION_BYTECODE_INTERPRETER
