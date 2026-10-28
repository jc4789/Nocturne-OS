#pragma once
#include "webi.h"
/* Native 1x screen policy, using actual picture/source media/type and srcset.
 * The caller owns returned text. Unsupported sizes expressions fall back to
 * viewport width, not a fabricated CSS result. */
char *web_image_source_pick(web_doc *, node_t *, const char *fallback);
/* Whether the image decoders take this MIME type essence (n bytes, ASCII
   case-insensitive): a <source type> they can show, and a type Resource
   Timing calls supported (minimize a supported MIME type, js.c). */
bool web_image_type_supported(const char *type, size_t n);
