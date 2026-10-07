#pragma once
#include "webi.h"

/* Shared by the native GUI, CSS selectors and JavaScript's live ValidityState.
   These bits are private bridge data, not author-controlled DOM properties. */
enum web_validity_flag {
    WEB_VALIDITY_VALUE_MISSING = 1u << 0,
    WEB_VALIDITY_TYPE_MISMATCH = 1u << 1,
    WEB_VALIDITY_PATTERN_MISMATCH = 1u << 2,
    WEB_VALIDITY_TOO_LONG = 1u << 3,
    WEB_VALIDITY_TOO_SHORT = 1u << 4,
    WEB_VALIDITY_RANGE_UNDERFLOW = 1u << 5,
    WEB_VALIDITY_RANGE_OVERFLOW = 1u << 6,
    WEB_VALIDITY_STEP_MISMATCH = 1u << 7,
    WEB_VALIDITY_BAD_INPUT = 1u << 8,
    WEB_VALIDITY_CUSTOM_ERROR = 1u << 9
};

bool web_control_validation_interface(const node_t *n);
bool web_control_submit_button(const node_t *n);
bool web_control_disabled(const node_t *n);
bool web_control_required_applicable(const node_t *n);
bool web_control_required(const node_t *n);
bool web_control_read_write(const node_t *n);
bool web_control_will_validate(const node_t *n);
uint32_t web_control_validity(web_doc *d, node_t *n);
/* -1: no range constraints apply; 0: out of range; 1: in range. */
int web_control_in_range(web_doc *d, node_t *n);
bool web_control_set_custom_validity(web_doc *d, node_t *n, const char *text, size_t length);
const char *web_control_validation_message(web_doc *d, node_t *n);
size_t web_control_validation_message_length(web_doc *d, node_t *n);
bool web_control_check_validity(web_doc *d, node_t *n, bool report);
/* Event-free :valid/:invalid aggregate for a form or fieldset. */
bool web_form_constraints_valid(web_doc *d, node_t *form_or_fieldset);
bool web_form_check_validity(web_doc *d, node_t *form, bool report);
/* Interactive validation only. HTMLFormElement.submit deliberately bypasses it. */
bool web_form_submission_validate(web_doc *d, node_t *submitter);
void web_form_validation_free(web_doc *d);
