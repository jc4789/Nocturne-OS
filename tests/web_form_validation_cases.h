/* Native/scripting-disabled regressions included by webtest.c. GUI input and
   submission use exactly these native predicates, not JavaScript mock state. */
#include "form_validation.h"
#include "form_value.h"

static node_t *fv_find(node_t *n, const char *id) {
    if (n->type == N_ELEM && n->id && !strcmp(n->id, id)) return n;
    for (node_t *c = n->first; c; c = c->next) { node_t *found = fv_find(c, id); if (found) return found; }
    return NULL;
}
static void fv_check(int *checks, int *errors, const char *name, bool value) {
    (*checks)++;
    if (!value) { printf("FAIL native-form-validation-%s\n", name); (*errors)++; }
}
static void run_native_form_validation_cases(int *checks, int *errors) {
#define FV(name, expression) fv_check(checks, errors, name, (expression))
    const char *html = "<form id=f action=/send><input id=required name=q required><input id=email type=email>"
        "<input id=url type=url><input id=pattern pattern='[A-Z]{2}[0-9]{3}'>"
        "<input id=number type=number min=2 max=10 step=2><input id=date type=date min=2024-02-28 max=2024-03-03 step=2>"
        "<input id=time type=time min=22:00 max=02:00><input id=length minlength=4 maxlength=8>"
        "<input id=checkbox type=checkbox required><input id=r1 type=radio name=r required disabled><input id=r2 type=radio name=r>"
        "<select id=select required><option value=''>choose</option><option value=x>x</option></select>"
        "<fieldset id=fs disabled><span></span><legend><input id=legend required></legend><legend><input id=legend2 required></legend><input id=blocked required></fieldset>"
        "<textarea id=readonly readonly required></textarea><input id=hidden type=hidden required>"
        "<button id=submit>send</button><button id=bypass formnovalidate>skip</button></form>"
        "<input id=outside form=f required><datalist><input id=datalist required></datalist>";
    web_doc *d = web_parse(html, strlen(html), "http://native.fixture/forms", NULL);
    FV("parse", d && d->root);
    if (!d || !d->root) { web_free(d); return; }
    node_t *form = fv_find(d->root, "f"), *required = fv_find(d->root, "required"),
        *email = fv_find(d->root, "email"), *url = fv_find(d->root, "url"), *pattern = fv_find(d->root, "pattern"),
        *number = fv_find(d->root, "number"), *date = fv_find(d->root, "date"), *time = fv_find(d->root, "time"),
        *length = fv_find(d->root, "length"), *checkbox = fv_find(d->root, "checkbox"),
        *r1 = fv_find(d->root, "r1"), *r2 = fv_find(d->root, "r2"), *select = fv_find(d->root, "select"),
        *legend = fv_find(d->root, "legend"), *blocked = fv_find(d->root, "blocked"),
        *outside = fv_find(d->root, "outside"), *submit = fv_find(d->root, "submit");
    FV("native-required", web_control_validity(d, required) == WEB_VALIDITY_VALUE_MISSING);
    FV("static-check-invalid", !web_form_check_validity(d, form, false));
    FV("static-check-no-report", !d->validation_report_pending);
    FV("native-report-invalid", !web_control_check_validity(d, required, true));
    FV("report-target", d->validation_target == required && d->validation_report_pending);
    FV("report-message", d->validation_message && d->validation_message_length > 0);
    FV("report-focus", web_focused(d) == required);
    d->validation_report_pending = false;
    FV("form-aggregate-invalid", !web_form_constraints_valid(d, form));
    FV("legend-exemption", !web_control_disabled(legend) && web_control_will_validate(legend));
    FV("fieldset-descendant-disabled", web_control_disabled(blocked) && !web_control_will_validate(blocked));
    FV("second-legend-disabled", web_control_disabled(fv_find(d->root, "legend2")));
    FV("readonly-barred", !web_control_will_validate(fv_find(d->root, "readonly")));
    FV("readonly-not-missing", !web_control_validity(d, fv_find(d->root, "readonly")));
    FV("hidden-barred", !web_control_will_validate(fv_find(d->root, "hidden")));
    FV("hidden-required-nonapplicable", !web_control_required_applicable(fv_find(d->root, "hidden")));
    FV("datalist-barred", !web_control_will_validate(fv_find(d->root, "datalist")));
    FV("native-submit-gate", !web_form_submission_validate(d, submit));
    FV("native-submitter-skip", web_form_submission_validate(d, fv_find(d->root, "bypass")));
    doc_node_attr(d, required, "formnovalidate", "");
    FV("nonbutton-formnovalidate-no-bypass", !web_form_submission_validate(d, required));
    doc_node_attr(d, required, "formnovalidate", NULL);
    doc_node_attr(d, form, "novalidate", "");
    FV("native-form-skip", web_form_submission_validate(d, submit));
    doc_node_attr(d, form, "novalidate", NULL);
    const char custom[] = {'\0', 'e', '\0'};
    FV("custom-NUL-store", web_control_set_custom_validity(d, required, custom, sizeof custom));
    FV("custom-NUL-invalid", (web_control_validity(d, required) & WEB_VALIDITY_CUSTOM_ERROR) != 0);
    FV("custom-NUL-length", web_control_validation_message_length(d, required) == sizeof custom);
    FV("custom-NUL-roundtrip", !memcmp(web_control_validation_message(d, required), custom, sizeof custom));
    FV("clone-custom-initial-empty", doc_node_clone(d, required, false)->custom_validity_length == 0);
    web_control_set_custom_validity(d, required, "", 0);
    FV("custom-clear", !(web_control_validity(d, required) & WEB_VALIDITY_CUSTOM_ERROR));
    FV("radio-required-disabled-member", (web_control_validity(d, r2) & WEB_VALIDITY_VALUE_MISSING) != 0);
    FV("radio-disabled-still-state", (web_control_validity(d, r1) & WEB_VALIDITY_VALUE_MISSING) != 0);
    doc_control_checked(d, r2, true);
    FV("radio-group-selected", !web_control_validity(d, r1) && !web_control_validity(d, r2));
    FV("checkbox-required", (web_control_validity(d, checkbox) & WEB_VALIDITY_VALUE_MISSING) != 0);
    doc_control_checked(d, checkbox, true);
    FV("checkbox-selected", !web_control_validity(d, checkbox));
    FV("select-placeholder", (web_control_validity(d, select) & WEB_VALIDITY_VALUE_MISSING) != 0);
    web_select_set(d, select, 1);
    FV("select-value", !web_control_validity(d, select));
    node_t *multi = doc_node_create(d, N_ELEM, "select", NULL, 0);
    if (multi) {
        doc_node_attr(d, multi, "multiple", "");doc_node_attr(d, multi, "required", "");
        const char *options = "<option value=''>empty</option><option value=x>x</option>";
        doc_node_html(d, multi, options, strlen(options));doc_node_move(d, d->body, multi, NULL);doc_control_init(d, multi);
        node_t *one = doc_select_option(multi, 0), *two = doc_select_option(multi, 1);
        FV("multiple-no-auto-selection", multi->selected == -1);
        FV("multiple-empty-missing", (web_control_validity(d, multi) & WEB_VALIDITY_VALUE_MISSING) != 0);
        web_option_set_selected(d, one, true, true);
        FV("multiple-empty-option-valid", !web_control_validity(d, multi));
        web_option_set_selected(d, two, true, true);
        FV("multiple-two-selected", doc_option_selected(one) && doc_option_selected(two));
        web_option_set_selected(d, one, false, true);web_option_set_selected(d, two, false, true);
        FV("multiple-deselection-missing", (web_control_validity(d, multi) & WEB_VALIDITY_VALUE_MISSING) != 0);
        web_select_set_index(d, multi, 1);FV("multiple-native-set-index-valid", !web_control_validity(d, multi));
        web_select_set_index(d, multi, -1);FV("multiple-native-index-clear", (web_control_validity(d, multi) & WEB_VALIDITY_VALUE_MISSING) != 0);
        doc_node_remove(d, multi);
    }
    doc_node_value(d, email, "a@b", 3);
    FV("email-single-valid", !web_control_validity(d, email));
    doc_node_value(d, email, "a@-b", 4);
    FV("email-invalid-domain", (web_control_validity(d, email) & WEB_VALIDITY_TYPE_MISMATCH) != 0);
    doc_node_attr(d, email, "multiple", ""); doc_node_value(d, email, "a@b, c@d", 8);
    FV("email-list-valid", !web_control_validity(d, email));
    doc_node_value(d, email, "a@b,", 4);
    FV("email-list-empty-token", (web_control_validity(d, email) & WEB_VALIDITY_TYPE_MISMATCH) != 0);
    doc_node_value(d, email, "", 0);
    static const char *const good_urls[] = {"https://example.test/a", "mailto:a@example.test", "urn:example:one", "data:text/plain,hello", "file:///data/example", "https://xn--r8jz45g.example/", "http://[::1]/"};
    static const char *const bad_urls[] = {"example.test", "/relative", "https://", "http://[broken]/", "http://host:70000/"};
    for (unsigned i = 0; i < sizeof good_urls / sizeof *good_urls; i++) {
        doc_node_value(d, url, good_urls[i], strlen(good_urls[i]));
        FV("URL-WHATWG-valid", !web_control_validity(d, url));
    }
    for (unsigned i = 0; i < sizeof bad_urls / sizeof *bad_urls; i++) {
        doc_node_value(d, url, bad_urls[i], strlen(bad_urls[i]));
        FV("URL-WHATWG-invalid", (web_control_validity(d, url) & WEB_VALIDITY_TYPE_MISMATCH) != 0);
    }
    doc_node_value(d, url, "", 0);
    doc_node_value(d, pattern, "AB123", 5);
    FV("pattern-valid", !web_control_validity(d, pattern));
    doc_node_value(d, pattern, "prefixAB123", 11);
    FV("pattern-full-match", (web_control_validity(d, pattern) & WEB_VALIDITY_PATTERN_MISMATCH) != 0);
    doc_node_attr(d, pattern, "pattern", "[");
    FV("pattern-invalid-regex-ignored", !web_control_validity(d, pattern));
    doc_node_attr(d, pattern, "pattern", "[\\p{Letter}&&\\p{ASCII}]+");doc_node_value(d, pattern, "Ascii", 5);
    FV("pattern-v-intersection", !web_control_validity(d, pattern));
    doc_node_value(d, pattern, "\xe6\x97\xa5\xe6\x9c\xac", 6);
    FV("pattern-v-unicode-mismatch", (web_control_validity(d, pattern) & WEB_VALIDITY_PATTERN_MISMATCH) != 0);
    doc_node_attr(d, pattern, "pattern", ".");doc_node_value(d, pattern, "\xf0\x9f\x98\xba", 4);
    FV("pattern-UTF16-scalar", !web_control_validity(d, pattern));
    char *huge = malloc(65538);
    if (huge) {
        memset(huge, 'a', 65537); huge[65537] = 0;doc_node_attr(d, pattern, "pattern", huge);doc_node_value(d, pattern, "a", 1);
        FV("pattern-resource-limit-not-valid", (web_control_validity(d, pattern) & WEB_VALIDITY_PATTERN_MISMATCH) != 0);free(huge);
    }
    doc_node_value(d, pattern, "", 0);
    doc_node_value(d, number, "1", 1);
    FV("number-underflow", (web_control_validity(d, number) & WEB_VALIDITY_RANGE_UNDERFLOW) != 0);
    FV("number-step-mismatch", (web_control_validity(d, number) & WEB_VALIDITY_STEP_MISMATCH) != 0);
    FV("number-out-of-range-pseudo", web_control_in_range(d, number) == 0);
    doc_node_value(d, number, "4", 1);
    FV("number-valid", !web_control_validity(d, number));FV("number-in-range-pseudo", web_control_in_range(d, number) == 1);
    doc_node_value(d, number, "11", 2);
    FV("number-overflow", (web_control_validity(d, number) & WEB_VALIDITY_RANGE_OVERFLOW) != 0);
    FV("native-user-bad-input-stored", web_input_user_value(d, number, "-", 1));
    FV("native-user-bad-input", (web_control_validity(d, number) & WEB_VALIDITY_BAD_INPUT) != 0);
    FV("native-user-canonical-empty", number->value && !*number->value);
    FV("native-user-raw-edit-retained", !strcmp(web_input_edit_text(number), "-"));
    doc_node_value(d, number, "broken", 6);
    FV("script-value-clears-bad-input", !web_control_validity(d, number));
    doc_node_value(d, date, "2024-02-29", 10);
    FV("date-step-mismatch", (web_control_validity(d, date) & WEB_VALIDITY_STEP_MISMATCH) != 0);
    doc_node_value(d, date, "2024-03-01", 10);
    FV("date-valid", !web_control_validity(d, date));
    doc_node_value(d, time, "23:00", 5);FV("time-reversed-late-valid", !web_control_validity(d, time));
    doc_node_value(d, time, "01:00", 5);FV("time-reversed-early-valid", !web_control_validity(d, time));
    doc_node_value(d, time, "12:00", 5);FV("time-reversed-invalid", (web_control_validity(d, time) & (WEB_VALIDITY_RANGE_UNDERFLOW | WEB_VALIDITY_RANGE_OVERFLOW)) == (WEB_VALIDITY_RANGE_UNDERFLOW | WEB_VALIDITY_RANGE_OVERFLOW));
    doc_node_value(d, time, "", 0);
    doc_node_value(d, length, "x", 1);FV("length-script-not-short", !(web_control_validity(d, length) & WEB_VALIDITY_TOO_SHORT));
    doc_node_value(d, length, "", 0);web_focus(d, length);
    struct gui_event key = {.type = EV_KEY, .key = 0x1f63a, .pressed = 1};
    FV("static-document-rejects-physical-edit", web_key(d,&key)==0);
    const char *gui_html="<input id=length minlength=3 maxlength=6>";
    web_doc *gui=web_live(gui_html,strlen(gui_html),"http://native.fixture/gui",NULL,NULL);
    if(gui)web_tick(gui,uptime_ms());
    node_t *gui_length=gui?fv_find(gui->root,"length"):NULL;
    web_focus(gui,gui_length);
    FV("native-GUI-unicode-edit", gui_length && web_key(gui, &key) == 1);
    FV("native-GUI-user-flag", gui_length && gui_length->control_user_edited && gui_length->value_dirty);
    FV("native-GUI-UTF16-too-short", gui_length && (web_control_validity(gui, gui_length) & WEB_VALIDITY_TOO_SHORT) != 0);
    FV("native-GUI-UTF16-length", gui_length && doc_utf16_length(gui_length->value) == 2);
    web_free(gui);
    web_input_user_value(d, length, "long text", 9);doc_node_attr(d, length, "maxlength", "6");
    FV("native-user-too-long", (web_control_validity(d, length) & WEB_VALIDITY_TOO_LONG) != 0);
    doc_node_value(d, length, "long text", 9);FV("script-not-too-long", !(web_control_validity(d, length) & WEB_VALIDITY_TOO_LONG));
    doc_node_value(d, required, "filled", 6);doc_node_value(d, legend, "filled", 6);doc_node_value(d, outside, "filled", 6);
    FV("outside-form-owner", web_form_owner(d, outside) == form);
    FV("form-valid-after-native-edits", web_form_check_validity(d, form, false));
    doc_node_value(d, outside, "", 0);FV("outside-required-gate", !web_form_submission_validate(d, submit));
    web_free(d);
#undef FV
}
