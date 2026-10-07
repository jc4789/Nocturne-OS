/* Runs the public web_live wrapper and the actual native key/value path.
   Date/time picker UX is intentionally outside this regression's assertions. */
#define main nocturne_jstest_main
#include "jstest.c"
#undef main
#include <math.h>
#include "form_value.h"

static void native_value_cases(void) {
    const char html[] = "<!doctype html><html><head></head><body><form></form></body></html>";
    web_doc *d = web_parse(html, sizeof html - 1, BASE, "utf-8");
    test_check("control-native-document", d != NULL); if (!d) return;
    node_t *n = doc_node_create(d, N_ELEM, "input", NULL, 0);
    node_t *form = d->body->first;
    test_check("control-native-create", n && form && doc_node_move(d, form, n, NULL));
    if (!n) { web_free(d); return; }
    test_check("control-native-date-type", doc_node_attr(d, n, "type", "date"));
    test_check("control-native-invalid-calendar", doc_node_value(d, n, "1900-02-29", 10) && !*n->value);
    test_check("control-native-calendar", doc_node_value(d, n, "2000-02-29", 10) && !strcmp(n->value, "2000-02-29"));
    test_check("control-native-date-number", web_input_value_number(d, n) == 951782400000.0);
    test_check("control-native-date-step", web_input_step(d, n, 1, false) == WEB_INPUT_OK && !strcmp(n->value, "2000-03-01"));
    test_check("control-native-type-resanitize", doc_node_attr(d, n, "type", "number") && !*n->value);
    test_check("control-native-number", doc_node_value(d, n, "0.3", 3));
    test_check("control-native-step-attribute", doc_node_attr(d, n, "step", "0.1"));
    test_check("control-native-decimal-no-mismatch", !web_input_step_mismatch(n, 0.3));
    test_check("control-native-decimal-mismatch", web_input_step_mismatch(n, 0.35));
    test_check("control-native-decimal-step", web_input_step(d, n, 1, false) == WEB_INPUT_OK && !strcmp(n->value, "0.4"));
    test_check("control-native-clear-default", doc_node_attr(d, n, "value", ""));
    size_t baseline = d->control_bytes;
    for (int i = 0; i < 1000; i++) {
        if (!web_input_user_value(d, n, "1e", 2)) { test_check("control-native-edit-loop", false); break; }
    }
    test_check("control-native-partial-preserved", !strcmp(web_input_edit_text(n), "1e") && !*n->value && n->input_bad_input && n->control_user_edited);
    test_check("control-native-edit-memory-bounded", d->control_bytes == baseline + 3);
    test_check("control-native-partial-number-nan", isnan(web_input_value_number(d, n)));
    test_check("control-native-edit-valid", web_input_user_value(d, n, "1e2", 3) && !strcmp(n->value, "1e2") && !n->input_bad_input);
    test_check("control-native-edit-number", web_input_value_number(d, n) == 100);
    test_check("control-native-api-replaces-edit", doc_node_value(d, n, "5", 1) && !n->input_edit && !n->input_bad_input && !n->control_user_edited && d->control_bytes == baseline);
    test_check("control-native-reset-edit-start", web_input_user_value(d, n, "-", 1));
    test_check("control-native-reset-edit-cleared", doc_form_reset(d, form) && !n->input_edit && !n->input_bad_input && !n->control_user_edited && !n->value_dirty && !*n->value);
    test_check("control-native-date-edit-type", doc_node_attr(d, n, "type", "date"));
    test_check("control-native-date-partial", web_input_user_value(d, n, "2026-", 5) && !strcmp(web_input_edit_text(n), "2026-") && !*n->value && n->input_bad_input);
    test_check("control-native-date-completed", web_input_user_value(d, n, "2026-10-07", 10) && !strcmp(n->value, "2026-10-07") && !n->input_bad_input);
    test_check("control-native-clone", doc_node_clone(d, n, false) != NULL);
    double value;
    test_check("control-native-week-53", web_input_parse_number(WEB_INPUT_WEEK, "2015-W53", &value) && value == 1451260800000.0);
    test_check("control-native-week-invalid-53", !web_input_parse_number(WEB_INPUT_WEEK, "2014-W53", &value));
    test_check("control-native-week-base", web_input_parse_number(WEB_INPUT_WEEK, "1970-W01", &value) && value == -259200000.0);
    test_check("control-native-time-ms", web_input_parse_number(WEB_INPUT_TIME, "12:34:56.123", &value) && value == 45296123.0);
    test_check("control-native-month-negative", web_input_parse_number(WEB_INPUT_MONTH, "1969-12", &value) && value == -1);
    test_check("control-native-attribute-numeric-prefix", web_input_parse_attribute(WEB_INPUT_NUMBER, " \t+2.5tail", &value) && value == 2.5);
    test_check("control-native-value-strict", !web_input_parse_number(WEB_INPUT_NUMBER, " \t+2.5tail", &value));
    web_free(d);
}

static void native_key_case(void) {
    const char *html = START "</head><body><input id='edit' type='number' maxlength='1'><script>"
        "globalThis.edit=document.getElementById('edit');globalThis.probe=0;"
        "const expected=['','-1','','-1e2',''];const bad=[true,false,true,false,true];"
        "edit.addEventListener('number-probe',()=>{check('control-key-js-value-'+probe,edit.value===expected[probe]);"
        "check('control-key-js-bad-input-'+probe,edit.validity.badInput===bad[probe]);mark('control-key-step-'+probe++);});"
        "edit.addEventListener('clear-api',()=>{edit.value='5';check('control-key-api-clear',edit.value==='5'&&!edit.validity.badInput);});"
        "globalThis.arrowProbe=0;edit.addEventListener('arrow-probe',()=>{check('control-key-js-arrow-'+arrowProbe,"
        "edit.value===['6','5','51'][arrowProbe++]&&!edit.validity.badInput);});"
        "edit.focus();mark('control-key-ready');</script></body>";
    if (!open_case(html, false)) return;
    test_check("control-key-ready", pump("control-key-ready", 5000));
    node_t *n = web_focused(fixture.doc);
    test_check("control-key-native-focus", n != NULL);
    static const uint32_t keys[] = {'-','1','e','2',NKEY_BACKSPACE};
    static const char *const raw[] = {"-", "-1", "-1e", "-1e2", "-1e"};
    for (unsigned i = 0; n && i < sizeof keys / sizeof *keys; i++) {
        struct gui_event key = {.key = keys[i]};
        test_check("control-key-handled", web_key(fixture.doc, &key) == 1);
        test_check("control-key-raw-preserved", !strcmp(web_control_value(n), raw[i]));
        if (i == 3) test_check("control-key-number-maxlength-ignored", node_attr(n, "maxlength") && !strcmp(n->value, "-1e2"));
        struct web_event event = {.type = "number-probe"}; web_dispatch(fixture.doc, n, &event);
        char marker[48]; snprintf(marker, sizeof marker, "control-key-step-%u", i);
        test_check("control-key-probe", has_mark(&fixture, marker));
    }
    if (n) {
        struct web_event event = {.type = "clear-api"}; web_dispatch(fixture.doc, n, &event);
        test_check("control-key-native-api-clear", !n->input_edit && !n->input_bad_input && !strcmp(n->value, "5"));
        static const uint32_t arrows[] = {NKEY_UP, NKEY_DOWN};
        static const char *const arrow_values[] = {"6", "5"};
        for (unsigned i = 0; i < 2; i++) {
            struct gui_event key = {.key = arrows[i]};
            test_check("control-key-arrow-handled", web_key(fixture.doc, &key) == 1);
            test_check("control-key-arrow-native-value", !strcmp(n->value, arrow_values[i]));
            struct web_event probe = {.type = "arrow-probe"}; web_dispatch(fixture.doc, n, &probe);
        }
        test_check("control-key-range-type", doc_node_attr(fixture.doc, n, "type", "range") && doc_node_attr(fixture.doc, n, "readonly", "") && doc_node_value(fixture.doc, n, "50", 2));
        struct gui_event range_key = {.key = NKEY_UP};
        test_check("control-key-range-readonly-ignored", web_key(fixture.doc, &range_key) == 1 && !strcmp(n->value, "51"));
        struct web_event probe = {.type = "arrow-probe"}; web_dispatch(fixture.doc, n, &probe);
    }
    test_check("control-key-no-js-errors", fixture.errors == 0 && fixture.js_failures == 0);
    close_case();
}

static void native_select_cases(void) {
    const char html[] = "<!doctype html><html><head></head><body><form method='post'>"
        "<select name='pick' multiple><option value='a' selected>Alpha</option><option value='b' selected>Beta</option>"
        "<option value='blocked' disabled selected>Disabled</option><optgroup disabled><option value='groupblocked' selected>Group</option></optgroup>"
        "<option label='not the value'>  collapsed \n text </option></select>"
        "<select name='list' size='3'><option value='unused'>Unused</option></select></form></body></html>";
    web_doc *d = web_parse(html, sizeof html - 1, BASE, "utf-8");
    test_check("select-native-document", d != NULL); if (!d) return;
    node_t *form = d->body->first, *select = form ? form->first : NULL, *list = select ? select->next : NULL;
    test_check("select-native-tree", form && select && list); if (!select || !list) { web_free(d); return; }
    doc_control_init(d, select); doc_control_init(d, list);
    node_t *a = doc_select_option(select, 0), *b = doc_select_option(select, 1), *disabled = doc_select_option(select, 2),
        *groupdisabled = doc_select_option(select, 3), *text = doc_select_option(select, 4);
    test_check("select-native-option-count", a && b && disabled && groupdisabled && text && !doc_select_option(select, 5));
    test_check("select-native-multiple-selected", doc_option_selected(a) && doc_option_selected(b) && select->selected == 0);
    test_check("select-native-listbox-no-default", list->selected == -1 && !doc_option_selected(doc_select_option(list, 0)));
    test_check("select-native-disabled", web_option_disabled(disabled) && web_option_disabled(groupdisabled) && !web_option_disabled(a));
    char *url = NULL, *body = NULL;
    test_check("select-native-submit-multiple", web_submit(d, form, &url, &body) && body && !strcmp(body, "pick=a&pick=b"));
    free(url); free(body); url = body = NULL;
    web_select_set(d, select, 0);
    test_check("select-native-gui-toggle-one", !doc_option_selected(a) && doc_option_selected(b) && select->selected == 1);
    test_check("select-native-default-preserved", node_attr(a, "selected") != NULL && a->selected_set);
    web_select_set(d, select, 2);
    test_check("select-native-gui-disabled-no-toggle", doc_option_selected(disabled));
    web_select_set(d, select, 4);
    test_check("select-native-gui-toggle-text", doc_option_selected(text));
    test_check("select-native-submit-fallback-text", web_submit(d, form, &url, &body) && body && !strcmp(body, "pick=b&pick=collapsed+text"));
    free(url); free(body);
    test_check("select-native-reset", doc_form_reset(d, form) && doc_option_selected(a) && doc_option_selected(b) && !doc_option_selected(text) && !a->selected_set);
    web_select_set_index(d, select, -1);
    test_check("select-native-index-clears-all", select->selected == -1 && !doc_option_selected(a) && !doc_option_selected(b) && !doc_option_selected(disabled));
    web_option_set_selected(d, b, true, true);
    test_check("select-native-option-setter", doc_option_selected(b) && !doc_option_selected(a) && node_attr(b, "selected") != NULL);
    test_check("select-native-remove-multiple", doc_node_attr(d, select, "multiple", NULL) && select->selected == 1);
    web_option_set_selected(d, a, true, true);
    test_check("select-native-single-exclusive", doc_option_selected(a) && !doc_option_selected(b));
    web_select_set_index(d, select, -1);
    test_check("select-native-single-index-empty", !doc_option_selected(a) && select->selected == -1);
    test_check("select-native-text-empty-preserved", doc_node_text(d, a, "changed", 7) && select->selected == -1 && !doc_option_selected(a));
    test_check("select-native-container-move-empty-preserved", doc_node_move(d, d->body, form, NULL) && select->selected == -1 && !doc_option_selected(a));
    test_check("select-native-empty-submit-preserved", web_submit(d, form, &url, &body) && body && !*body);
    free(url); free(body); url = body = NULL;
    doc_node_remove(d, a);
    test_check("select-native-remove-resets-selection", select->selected == 0 && doc_option_selected(b));
    web_free(d);
}

static void native_parser_select_case(void) {
    const char *html = START "</head><body><form id='parse-form' method='post'><select id='parse-select' name='pick'>"
        "<option value='a' selected>A</option><option value='b'>B</option></select></form><script>"
        "globalThis.parseSelect=document.getElementById('parse-select');globalThis.parseForm=document.getElementById('parse-form');"
        "parseSelect.selectedIndex=-1;parseSelect.options[0].text='updated';document.body.append(parseForm);"
        "mark('control-select-parser-paused');</script><p>parser resumes</p><script>"
        "check('control-select-parser-empty',parseSelect.selectedIndex===-1&&parseSelect.value===''&&parseSelect.selectedOptions.length===0);"
        "check('control-select-parser-default-retained',parseSelect.options[0].defaultSelected===true);"
        "mark('control-select-parser-done');</script></body>";
    if (!open_case(html, false)) return;
    test_check("control-select-parser-done", pump("control-select-parser-done", 5000));
    node_t *form = NULL;
    for (node_t *n = fixture.doc->body->first; n; n = n->next) if (n->tag == T_form) { form = n; break; }
    char *url = NULL, *body = NULL;
    test_check("control-select-parser-native-empty-submit", form && web_submit(fixture.doc, form, &url, &body) && body && !*body);
    free(url); free(body);
    test_check("control-select-parser-no-js-errors", fixture.errors == 0 && fixture.js_failures == 0);
    close_case();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    external_case("js_form_controls_cases.js", ";try{const n=runFormControlCases();console.log('Input value checks '+n);check('input-value-count',n>300);}catch(e){console.log('FAIL input-value '+e+' '+e.stack);}mark('api-done');", BASE);
    native_value_cases(); native_key_case(); native_select_cases(); native_parser_select_case();
    printf("js_form_controls_native: %d checks, %d failed\n", total, failed);
    return failed != 0;
}
