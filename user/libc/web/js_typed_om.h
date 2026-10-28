#pragma once
#include "quickjs.h"

/* The native half of js_typed_om.js (CSS Typed OM Level 1), on host in the Window and worker realms.
   The numeric types, units, math trees and serialization are cssmath.c's, a property's grammar is
   cssprop.c's, colours are color.c's and the tokens are CSS Syntax's (Lexbor's tokenizer):
     typedUnits(): [[name ("" a number, "%" a percentage), base type (-1 none), relative, factor], ...]
     typedType(op, a, b): "add two types" (op 0) or "multiply two types" (1) of [7 exponents, hint],
       or null when they cannot be
     typedMatch(type, accept): the CSS_CALC_* type it matches in a context that accepts accept, or 0
     typedNumber(v): a <number> as CSS serializes it
     typedMath(text, accept): one numeric value: a leaf [value, unit] or a math tree ["sum"|"product"|
       "min"|"max", ...], ["negate"|"invert", x], ["clamp", a, b, c] of its simplified specified value
       (CSS Values 4 §10.10.1), parsed as CSS Typed OM §5.6 reifies it; null: not valid; false: a math
       function Typed OM has no object for
     typedProperty(name): [CSS_REIFY_* kind, shorthand, list-valued, numeric accept, accept is all the
       lone numeric types the grammar takes, a lone number must be an integer], or null
     typedValid(property, text): the declaration is valid (css_valid_declaration)
     typedSegments(text): CSS Typed OM §5.3: strings (source runs) and var() references
       [name, fallback segments or null]
     typedBoundary(left, right): CSS Syntax §9: left's last token and right's first would read as
       one, so a comment goes between them
     typedComponents(text): the top-level component values: ["ident", name], ["numeric", text],
       ["function", lowercased name, text, [arguments split at top-level commas]], ["url", text],
       [","], ["other", text]; whitespace and comments are left out
     typedColor(text): CSS Typed OM §5.7: null (not a <color>), ["keyword", name], ["other"], or
       [kind, space, ...4 channels: null absent, "none", [value, unit] (a unitless alpha is a
       fraction) or ["math", text]] */
void web_js_typed_om_init(JSContext *ctx, JSValue host);
