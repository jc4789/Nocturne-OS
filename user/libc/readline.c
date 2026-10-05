/* readline(): a small line editor for terminal programs. Terminals hand programs raw key bytes, so
   echo and editing happen here. Keys: arrows, Home/End, Backspace/Delete, Ctrl+A/E/U/K/W,
   Up/Down history. Lines longer than the terminal wrap, so redrawing only works on the last row;
   typing at the end of the line (the usual case) is always fine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

#define HIST 64
static char *hist[HIST];
static int nhist;

static int byte_in(void) {
    unsigned char c;
    return read(0, &c, 1) == 1 ? c : -1;
}

static void out(const char *s, size_t n) {
    while (n) {
        ssize_t w = write(1, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void redraw(const char *prompt, const char *buf, size_t len, size_t cur, bool secret) {
    char tmp[256];
    out("\r", 1);
    out(prompt, strlen(prompt));
    if (secret) {
        for (size_t i = 0; i < len; i++) out("*", 1);
    } else {
        out(buf, len);
    }
    out("\x1b[K", 3);
    if (len > cur) {
        int n = snprintf(tmp, sizeof tmp, "\x1b[%zuD", len - cur);
        out(tmp, (size_t)n);
    }
}

static void add_hist(const char *line) {
    if (!*line || (nhist && !strcmp(hist[nhist - 1], line))) return;
    if (nhist == HIST) {
        free(hist[0]);
        memmove(hist, hist + 1, sizeof *hist * (HIST - 1));
        nhist--;
    }
    hist[nhist++] = strdup(line);
}

int readline(const char *prompt, char *buf, size_t size, int flags) {
    bool secret = flags & RL_SECRET;
    if (size == 0) return -1;
    if (!isatty(0)) {
        /* piped input: no echo or editing, just read up to a newline */
        size_t len = 0;
        out(prompt, strlen(prompt));
        for (;;) {
            int c = byte_in();
            if (c < 0) {
                if (!len) return -1;
                break;
            }
            if (c == '\n') break;
            if (c != '\r' && len + 1 < size) buf[len++] = (char)c;
        }
        buf[len] = 0;
        return (int)len;
    }
    size_t len = 0, cur = 0;
    int hpos = nhist;
    buf[0] = 0;
    out(prompt, strlen(prompt));
    for (;;) {
        int c = byte_in();
        if (c < 0) return -1;
        if (c == '\t') c = ' '; /* pasted tabs */
        if (c == '\r' || c == '\n') {
            out("\r\n", 2);
            buf[len] = 0;
            if (!secret) add_hist(buf);
            return (int)len;
        }
        if (c == 4) { /* Ctrl+D on an empty line: end of input */
            if (len == 0) {
                out("\r\n", 2);
                return -1;
            }
            continue;
        }
        if (c == 3) { /* Ctrl+C: abandon the line */
            out("^C\r\n", 4);
            buf[0] = 0;
            return 0;
        }
        if (c == 1) cur = 0;
        else if (c == 5) cur = len;
        else if (c == 21) { /* Ctrl+U */
            memmove(buf, buf + cur, len - cur);
            len -= cur;
            cur = 0;
        } else if (c == 11) { /* Ctrl+K */
            len = cur;
        } else if (c == 23) { /* Ctrl+W: delete the previous word */
            size_t s = cur;
            while (s > 0 && buf[s - 1] == ' ') s--;
            while (s > 0 && buf[s - 1] != ' ') s--;
            memmove(buf + s, buf + cur, len - cur);
            len -= cur - s;
            cur = s;
        } else if (c == 127 || c == 8) {
            if (cur > 0) {
                memmove(buf + cur - 1, buf + cur, len - cur);
                cur--;
                len--;
            }
        } else if (c == 0x1b) {
            if (byte_in() != '[') continue;
            int c2 = byte_in(), c3 = 0;
            if (c2 >= '0' && c2 <= '9') c3 = byte_in();
            if ((c2 == 'A' || c2 == 'B') && !secret) {
                int np = hpos + (c2 == 'A' ? -1 : 1);
                if (np < 0 || np > nhist) continue;
                hpos = np;
                strlcpy(buf, hpos < nhist ? hist[hpos] : "", size);
                len = cur = strlen(buf);
            } else if (c2 == 'C') {
                if (cur < len) cur++;
            } else if (c2 == 'D') {
                if (cur > 0) cur--;
            } else if (c2 == 'H') {
                cur = 0;
            } else if (c2 == 'F') {
                cur = len;
            } else if (c2 == '3' && c3 == '~') {
                if (cur < len) {
                    memmove(buf + cur, buf + cur + 1, len - cur - 1);
                    len--;
                }
            } else {
                continue;
            }
        } else if (c >= 32 && len + 1 < size) {
            memmove(buf + cur + 1, buf + cur, len - cur);
            buf[cur++] = (char)c;
            len++;
            if (cur == len) { /* appending: just echo it */
                out(secret ? "*" : (char *)&buf[cur - 1], 1);
                continue;
            }
        } else {
            continue;
        }
        buf[len] = 0;
        redraw(prompt, buf, len, cur, secret);
    }
}
