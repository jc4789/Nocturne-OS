#ifndef NOCTURNE_HTML_PI_H
#define NOCTURNE_HTML_PI_H
#include <stddef.h>
#include <stdbool.h>
struct html_pi_attribute { char *name, *value; size_t value_length; };
enum html_pi_parse_status { HTML_PI_OK, HTML_PI_INVALID, HTML_PI_OOM };
enum html_pi_parse_status html_pi_parse_ex(const char *data,size_t length,
                   struct html_pi_attribute **attributes,size_t *count);
/* XML pseudo-attributes, case-sensitive and all-or-nothing. Caller owns map. */
bool html_pi_parse(const char *data, size_t length,
                   struct html_pi_attribute **attributes, size_t *count);
void html_pi_free(struct html_pi_attribute *attributes, size_t count);
char *html_pi_get(const char *data, size_t length, const char *name);
#endif
