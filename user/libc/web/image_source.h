#pragma once
#include "webi.h"
/* Native 1x screen policy, using actual picture/source media/type and srcset.
 * The caller owns returned text. Unsupported sizes expressions fall back to
 * viewport width, not a fabricated CSS result. */
char *web_image_source_pick(web_doc *, node_t *, const char *fallback);
