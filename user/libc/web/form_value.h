#pragma once
#include "webi.h"

/* The native input owns the value. JS and the renderer use these conversions;
   there is no second JavaScript form state or host time-zone dependency. */
enum web_input_kind {
    WEB_INPUT_TEXT, WEB_INPUT_SEARCH, WEB_INPUT_TEL, WEB_INPUT_URL,
    WEB_INPUT_EMAIL, WEB_INPUT_PASSWORD, WEB_INPUT_HIDDEN,
    WEB_INPUT_DATE, WEB_INPUT_MONTH, WEB_INPUT_WEEK, WEB_INPUT_TIME,
    WEB_INPUT_DATETIME_LOCAL, WEB_INPUT_NUMBER, WEB_INPUT_RANGE, WEB_INPUT_COLOR,
    WEB_INPUT_CHECKBOX, WEB_INPUT_RADIO, WEB_INPUT_FILE, WEB_INPUT_SUBMIT,
    WEB_INPUT_IMAGE, WEB_INPUT_RESET, WEB_INPUT_BUTTON
};
enum web_input_mode { WEB_INPUT_VALUE, WEB_INPUT_DEFAULT, WEB_INPUT_DEFAULT_ON, WEB_INPUT_FILENAME };
struct web_input_limits {
    double min, max, step, base;
    bool has_min, has_max, any, numeric;
};
enum web_input_result { WEB_INPUT_OK, WEB_INPUT_INVALID_STATE, WEB_INPUT_INVALID_NUMBER, WEB_INPUT_OOM };

enum web_input_kind web_input_type(const node_t *n);
const char *web_input_type_name(enum web_input_kind type);
enum web_input_mode web_input_value_mode(enum web_input_kind type);
bool web_input_numeric(enum web_input_kind type);
bool web_input_date_supported(enum web_input_kind type);
bool web_input_parse_number(enum web_input_kind type, const char *text, double *out);
bool web_input_parse_attribute(enum web_input_kind type, const char *text, double *out);
bool web_input_format_number(enum web_input_kind type, double value, char *out, size_t capacity);
void web_input_constraints(const node_t *n, struct web_input_limits *out);
bool web_input_step_mismatch(const node_t *n, double value);
/* out has at least max(len + 1, 128) bytes; sanitization can supply defaults. */
size_t web_input_sanitize(const node_t *n, const char *text, size_t len, char *out);
double web_input_value_number(web_doc *d, node_t *n);
double web_input_value_date(web_doc *d, node_t *n);
enum web_input_result web_input_set_number(web_doc *d, node_t *n, double value, bool as_date);
enum web_input_result web_input_step(web_doc *d, node_t *n, int32_t count, bool down);

const char *web_input_edit_text(const node_t *n);
void web_input_clear_edit(web_doc *d, node_t *n);
bool web_input_user_value(web_doc *d, node_t *n, const char *text, size_t len);

/* Option selectedness is per native option, not a single-index stand-in. */
node_t *web_select_next_option(node_t *select, node_t *after);
node_t *web_option_select(node_t *option);
bool web_option_disabled(const node_t *option);
void web_select_sync(web_doc *d, node_t *select, bool reset);
void web_option_set_selected(web_doc *d, node_t *option, bool selected, bool dirty);
void web_select_set_index(web_doc *d, node_t *select, int index);
void web_select_attribute_changed(web_doc *d, node_t *node, const char *name, bool present);
void web_select_inserted(web_doc *d, node_t *subtree, node_t *old_parent);
void web_option_value(node_t *option, sbuf *out);
void web_option_text(node_t *option, sbuf *out);
