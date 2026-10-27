/* nsh - the Nocturne shell: line editing, history, pipes, redirection, globbing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "nocturne.h"

#define MAX_LINE 1024 /* a line typed at the prompt; scripts and "sh -c" have no such limit */
#define MAX_HIST 64
#define WELCOME  "C:\\Nocturne\\Welcome.txt" /* shown when the shell starts at a terminal */

static char *history[MAX_HIST];
static int nhist;
static int last_status;

/* The current folder: names that are not complete paths are inside it, and the
   apps the shell starts get it as their FOLDER setting. */
static char folder[4096] = "C:\\";
/* A channel on which the terminal sends a message for each Ctrl+C (setting INTERRUPTS). */
static n_handle interrupts = -1;
/* Which of the shell's own input, output and log exist. */
static bool std_open[3];

/* ---- terminal output helpers ---- */
static void out(const char *s) {
    fputs(s, stdout);
    fflush(stdout);
}

/* name typed by the user as a complete path */
static bool complete_name(const char *name, char *path, size_t n) { return n_path_join(folder, name, path, n) >= 0; }

/* The last folder separator in a typed name ('\', or '/' accepted as one), or NULL. */
static const char *last_separator(const char *name) {
    const char *b = strrchr(name, '\\'), *s = strrchr(name, '/');
    return !b ? s : !s ? b : b > s ? b : s;
}

static void prompt_str(char *buf, size_t n) {
    snprintf(buf, n, "\x1b[1;35mnocturne\x1b[0m:\x1b[1;34m%s\x1b[0m%s ", folder, last_status ? "\x1b[31m$\x1b[0m" : "$");
}

/* ---- tab completion ---- */
struct matches {
    char common[sizeof ((struct n_entry *)0)->name]; /* the start every match shares */
    bool folder;                                     /* the first match is a folder */
    int count;
    char list[1024];
};

/* Add the names in the folder at path that start with prefix; letter case aside, as names compare. */
static void add_matches(const char *path, const char *prefix, struct matches *m) {
    struct n_entry *ents;
    n_result count = n_folder_entries(path, &ents);
    size_t pl = strlen(prefix);
    for (n_result i = 0; i < count; i++) {
        const struct n_entry *d = &ents[i];
        if (strncasecmp(d->name, prefix, pl)) continue;
        if (m->count++ == 0) {
            strcpy(m->common, d->name);
            m->folder = d->kind == N_KIND_FOLDER;
        } else {
            int k = 0;
            while (m->common[k] && tolower((unsigned char)m->common[k]) == tolower((unsigned char)d->name[k])) k++;
            m->common[k] = 0;
        }
        if (strlen(m->list) + strlen(d->name) + 3 < sizeof m->list) {
            strcat(m->list, d->name);
            strcat(m->list, "  ");
        }
    }
    if (count >= 0) free(ents);
}

static void complete(char *buf, int *len, int *cur) {
    int start = *cur;
    while (start > 0 && buf[start - 1] != ' ') start--;
    char word[MAX_LINE];
    int wl = *cur - start;
    memcpy(word, buf + start, wl);
    word[wl] = 0;
    bool first = true;
    for (int i = 0; i < start; i++)
        if (buf[i] != ' ') first = false;

    /* the folder typed so far keeps its separator, so that "\" alone is the root */
    char dir[MAX_LINE] = ".", path[4096];
    const char *prefix = word, *sep = last_separator(word);
    if (sep) {
        memcpy(dir, word, sep - word + 1);
        dir[sep - word + 1] = 0;
        prefix = sep + 1;
    }
    struct matches m = {0};
    if (!sep && first) {
        /* a program: the system's, then those in each app's folder (as find_program looks) */
        add_matches(N_FOLDER_SYSTEM, prefix, &m);
        struct n_entry *apps;
        n_result count = n_folder_entries(N_FOLDER_PROGRAMS, &apps);
        for (n_result i = 0; i < count; i++)
            if (apps[i].kind == N_KIND_FOLDER && n_path_join(N_FOLDER_PROGRAMS, apps[i].name, path, sizeof path) >= 0)
                add_matches(path, prefix, &m);
        if (count >= 0) free(apps);
    } else if (complete_name(dir, path, sizeof path)) {
        add_matches(path, prefix, &m);
    }
    if (!m.count) return;
    const char *add = m.common + strlen(prefix);
    int al = (int)strlen(add);
    if (m.count == 1) {
        static char tmp[sizeof m.common + 1];
        snprintf(tmp, sizeof tmp, "%s%s", add, m.folder ? "\\" : " ");
        add = tmp;
        al = (int)strlen(tmp);
    }
    if (al == 0 && m.count > 1) {
        out("\n");
        out(m.list);
        out("\n");
        return;
    }
    if (*len + al >= MAX_LINE - 1) return;
    memmove(buf + *cur + al, buf + *cur, *len - *cur);
    memcpy(buf + *cur, add, al);
    *len += al;
    *cur += al;
    buf[*len] = 0;
}

/* ---- line editor ---- */
enum { TOKEN_INPUT = 1, TOKEN_INTERRUPT = 2 };
static n_handle prompt_events = -1;

/* Take every waiting Ctrl+C message; true if there was one. */
static bool take_interrupts(void) {
    bool any = false;
    char msg[64];
    while (interrupts >= 0 && n_channel_receive(interrupts, msg, sizeof msg, 0, NULL) >= 0) any = true;
    return any;
}

/* The next key byte typed at the terminal; 3 (Ctrl+C) for an interrupt
   message; -1 when the input has ended. */
static int read_byte(void) {
    static char *in;
    static size_t cap, pos, len;
    while (pos == len) {
        if (prompt_events >= 0) {
            struct n_event ev[2];
            if (n_events_wait(prompt_events, ev, 2, -1) < 0) return -1;
            if (take_interrupts()) return 3;
        }
        if (!cap && !(in = malloc(cap = 4096))) return -1;
        n_result r = n_channel_receive(0, in, cap, prompt_events >= 0 ? 0 : N_CHANNEL_WAIT, NULL);
        if (r == -N_ERR_TOO_LARGE) {
            /* a long paste arrives as one message */
            n_result need = n_channel_receive(0, NULL, 0, N_CHANNEL_PEEK, NULL);
            char *grown = need > 0 ? realloc(in, (size_t)need) : NULL;
            if (!grown) return -1;
            in = grown;
            cap = (size_t)need;
            continue;
        }
        if (r == -N_ERR_NOT_READY) continue;
        if (r < 0) return -1;
        pos = 0;
        len = (size_t)r;
    }
    return (unsigned char)in[pos++];
}

static void redraw(const char *prompt, const char *buf, int len, int cur) {
    char tmp[MAX_LINE + sizeof folder + 128];
    int n = snprintf(tmp, sizeof tmp, "\r%s%.*s\x1b[K", prompt, len, buf);
    if (len - cur > 0) n += snprintf(tmp + n, sizeof tmp - n, "\x1b[%dD", len - cur);
    fwrite(tmp, 1, (size_t)n, stdout);
    fflush(stdout);
}

/* returns false on EOF */
static bool read_line(char *buf) {
    char prompt[sizeof folder + 64];
    prompt_str(prompt, sizeof prompt);
    out(prompt);
    int len = 0, cur = 0, hpos = nhist;
    buf[0] = 0;
    for (;;) {
        int c = read_byte();
        if (c < 0) return false;
        if (c == '\r' || c == '\n') {
            out("\r\n");
            buf[len] = 0;
            return true;
        }
        if (c == 3) { /* Ctrl+C */
            out("^C\r\n");
            buf[0] = 0;
            return true;
        }
        if (c == 4) { /* Ctrl+D */
            if (len == 0) return false;
            continue;
        }
        if (c == 12) { /* Ctrl+L */
            out("\x1b[2J\x1b[H");
            redraw(prompt, buf, len, cur);
            continue;
        }
        if (c == 1) cur = 0;
        else if (c == 5) cur = len;
        else if (c == 21) { /* Ctrl+U */
            memmove(buf, buf + cur, len - cur);
            len -= cur;
            cur = 0;
        } else if (c == 11) { /* Ctrl+K */
            len = cur;
        } else if (c == '\t') {
            buf[len] = 0;
            complete(buf, &len, &cur);
            redraw(prompt, buf, len, cur);
            continue;
        } else if (c == 127 || c == 8) {
            if (cur > 0) {
                memmove(buf + cur - 1, buf + cur, len - cur);
                cur--;
                len--;
            }
        } else if (c == 0x1b) {
            int c1 = read_byte();
            if (c1 != '[') continue;
            int c2 = read_byte();
            int c3 = 0;
            if (c2 >= '0' && c2 <= '9') c3 = read_byte();
            if (c2 == 'A' || c2 == 'B') {
                int np = hpos + (c2 == 'A' ? -1 : 1);
                if (np >= 0 && np <= nhist) {
                    hpos = np;
                    const char *h = hpos < nhist ? history[hpos] : "";
                    strlcpy(buf, h, MAX_LINE);
                    len = cur = (int)strlen(buf);
                }
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
            }
        } else if (c >= 32 && len < MAX_LINE - 1) {
            memmove(buf + cur + 1, buf + cur, len - cur);
            buf[cur++] = (char)c;
            len++;
            if (cur == len) {
                putchar(c);
                fflush(stdout);
                continue;
            }
        } else {
            continue;
        }
        redraw(prompt, buf, len, cur);
    }
}

static void add_history(const char *line) {
    if (!*line) return;
    if (nhist && !strcmp(history[nhist - 1], line)) return;
    if (nhist == MAX_HIST) {
        free(history[0]);
        memmove(history, history + 1, sizeof(char *) * (MAX_HIST - 1));
        nhist--;
    }
    history[nhist++] = strdup(line);
}

/* ---- parsing ---- */
enum { T_WORD, T_PIPE, T_LT, T_GT, T_GTGT, T_AMP, T_SEMI, T_AND, T_OR, T_2GT, T_END };

struct token {
    int type;
    char *text;
    bool quoted;
};

/* The tokens of a line, however many it has, ending with T_END; NULL without memory. */
static struct token *tokenize(const char *s, int *count) {
    int n = 0, cap = 16;
    struct token *toks = malloc(sizeof *toks * (size_t)cap);
    if (!toks) return NULL;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') break;
        if (n + 1 >= cap) {
            struct token *more = realloc(toks, sizeof *toks * (size_t)(cap *= 2));
            if (!more) goto failed;
            toks = more;
        }
        struct token *t = &toks[n++];
        t->text = NULL;
        t->quoted = false;
        if (s[0] == '|' && s[1] == '|') { t->type = T_OR; s += 2; continue; }
        if (s[0] == '&' && s[1] == '&') { t->type = T_AND; s += 2; continue; }
        if (s[0] == '>' && s[1] == '>') { t->type = T_GTGT; s += 2; continue; }
        if (s[0] == '2' && s[1] == '>' && (s[2] != '>')) { t->type = T_2GT; s += 2; continue; }
        if (*s == '|') { t->type = T_PIPE; s++; continue; }
        if (*s == '<') { t->type = T_LT; s++; continue; }
        if (*s == '>') { t->type = T_GT; s++; continue; }
        if (*s == '&') { t->type = T_AMP; s++; continue; }
        if (*s == ';') { t->type = T_SEMI; s++; continue; }
        t->type = T_WORD;
        char *buf = malloc(strlen(s) + 1); /* a word is never longer than the rest of its line */
        if (!buf) goto failed;
        size_t bl = 0;
        while (*s && !strchr(" \t|<>&;", *s)) {
            if (*s == '"' || *s == '\'') {
                char q = *s++;
                t->quoted = true;
                while (*s && *s != q) {
                    if (q == '"' && *s == '$' && s[1] == '?') {
                        buf[bl++] = '\x01'; /* $?, expanded at run time */
                        s += 2;
                        continue;
                    }
                    buf[bl++] = *s++;
                }
                if (*s) s++;
            } else if (*s == '$' && s[1] == '?') {
                buf[bl++] = '\x01'; /* $?, expanded at run time */
                s += 2;
            } else {
                buf[bl++] = *s++;
            }
        }
        buf[bl] = 0;
        t->text = buf;
    }
    toks[n].type = T_END;
    toks[n].text = NULL;
    *count = n;
    return toks;
failed:
    for (int i = 0; i < n; i++) free(toks[i].text);
    free(toks);
    return NULL;
}

/* glob matching with * and ?, letter case aside as names compare */
static bool match(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') {
        for (;; s++) {
            if (match(p + 1, s)) return true;
            if (!*s) return false;
        }
    }
    if (!*s) return false;
    if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) return match(p + 1, s + 1);
    return false;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

/* One command of a pipeline: argv holds argc words, then NULL. */
struct cmd {
    char **argv;
    int argc, cap;
    char *in, *out, *err;
    bool append;
};

/* Add a word (malloc'd, or NULL without memory) to the command's arguments, as many as it has. */
static bool add_arg(struct cmd *c, char *word) {
    if (word && c->argc + 1 >= c->cap) {
        int cap = c->cap ? c->cap * 2 : 8;
        char **more = realloc(c->argv, sizeof *more * (size_t)cap);
        if (more) c->argv = more, c->cap = cap;
        else free(word), word = NULL;
    }
    if (!word) {
        fprintf(stderr, "sh: %s\n", n_result_text(-N_ERR_NO_MEMORY));
        return false;
    }
    c->argv[c->argc++] = word;
    c->argv[c->argc] = NULL;
    return true;
}

/* The word, or the names it matches with * and ?, sorted. */
static bool expand_glob(const char *pat, struct cmd *c) {
    if (!strpbrk(pat, "*?")) return add_arg(c, strdup(pat));
    /* the folder as typed, with its separator: it starts every match */
    const char *file = pat, *sep = last_separator(pat);
    size_t dl = sep ? (size_t)(sep - pat + 1) : 0;
    char *dir = malloc(dl + 1);
    if (!dir) return add_arg(c, NULL);
    memcpy(dir, pat, dl);
    dir[dl] = 0;
    if (sep) file = sep + 1;
    int start = c->argc;
    bool ok = true;
    char path[4096];
    struct n_entry *ents;
    n_result count = complete_name(dir, path, sizeof path) ? n_folder_entries(path, &ents) : -1;
    if (count >= 0) {
        for (int i = 0; ok && i < (int)count; i++) {
            if (!match(file, ents[i].name)) continue;
            char *full = malloc(dl + strlen(ents[i].name) + 1);
            if (full) {
                memcpy(full, dir, dl);
                strcpy(full + dl, ents[i].name);
            }
            ok = add_arg(c, full);
        }
        free(ents);
    }
    free(dir);
    if (!ok) return false;
    if (c->argc == start) return add_arg(c, strdup(pat));
    qsort(c->argv + start, c->argc - start, sizeof(char *), cmp_str);
    return true;
}

/* ---- builtins ---- */
static void cmd_help(void) {
    out("\x1b[1mNocturne shell\x1b[0m - builtins: cd pwd exit history help clear\n"
        "Syntax: cmd args | cmd2 > file, >> append, < input, 2> errors, & background,\n"
        "        cmd1 ; cmd2, cmd1 && cmd2, cmd1 || cmd2, wildcards * and ?\n"
        "Keys:   Up/Down history, Tab completion, Ctrl+C cancel, Ctrl+L clear\n"
        "Try:    ls C:\\Nocturne\\System, neofetch, ps, free, fortune, moonsay hi,\n"
        "        ifconfig, ping example.com, fetch http://example.com\n");
}

static bool builtin(int argc, char **argv, int *status) {
    if (!strcmp(argv[0], "cd")) {
        const char *d = argc > 1 ? argv[1] : n_user_folder();
        char path[sizeof folder];
        struct n_entry e;
        n_result r = complete_name(d, path, sizeof path) ? n_entry_info(path, &e) : -N_ERR_TOO_LARGE;
        if (r >= 0 && e.kind != N_KIND_FOLDER) r = -N_ERR_NOT_FOLDER;
        if (r < 0) {
            fprintf(stderr, "cd: %s: %s\n", d, n_result_text(r));
            *status = 1;
        } else {
            memcpy(folder, path, sizeof folder);
            *status = 0;
        }
        return true;
    }
    if (!strcmp(argv[0], "pwd")) {
        printf("%s\n", folder);
        fflush(stdout);
        *status = 0;
        return true;
    }
    if (!strcmp(argv[0], "exit")) {
        exit(argc > 1 ? atoi(argv[1]) : last_status);
    }
    if (!strcmp(argv[0], "history")) {
        for (int i = 0; i < nhist; i++) printf("%4d  %s\n", i + 1, history[i]);
        fflush(stdout);
        *status = 0;
        return true;
    }
    if (!strcmp(argv[0], "help")) {
        cmd_help();
        *status = 0;
        return true;
    }
    return false;
}

/* ---- execution ---- */

static void close_handle(n_handle *h) {
    if (*h >= 0) n_handle_close(*h);
    *h = -1;
}

/* A redirection's document: closing it saves it (n_handle_close). */
struct output {
    n_handle h;
    const char *name;
};

/* Closed, so saved: true, with a message, when the save failed. */
static bool close_output(struct output *o) {
    n_result r = o->h >= 0 ? n_handle_close(o->h) : 0;
    o->h = -1;
    if (r < 0) fprintf(stderr, "sh: %s: not saved: %s\n", o->name, n_result_text(r));
    return r < 0;
}

/* A program name as a complete path: a name with a folder in it is the current
   folder's, a bare name is found by find_program. */
static bool find_command(const char *name, char *path, size_t n) {
    struct n_entry e;
    if (!strpbrk(name, "\\/:")) return find_program(name, path, n);
    return complete_name(name, path, n) && n_entry_info(path, &e) >= 0 && e.kind == N_KIND_DOCUMENT;
}

/* A redirection target: ">" empties the document (ISO C "w"), ">>" adds to its end. */
static n_handle open_output(const char *name, bool append) {
    char path[4096];
    if (!complete_name(name, path, sizeof path)) return -N_ERR_TOO_LARGE;
    n_handle h = n_document_open(path, N_DOCUMENT_EDIT);
    if (h == -N_ERR_NOT_FOUND) return n_document_open(path, N_DOCUMENT_CREATE);
    if (h >= 0 && !append) {
        n_result r = n_document_set_size(h, 0);
        if (r < 0) {
            n_handle_close(h);
            return (n_handle)r;
        }
    }
    return h;
}

static n_handle launch(const char *path, struct cmd *c, const int32_t handles[3]) {
    char setting_folder[sizeof folder + 8], setting_terminal[32] = "TERMINAL=";
    snprintf(setting_folder, sizeof setting_folder, "FOLDER=%s", folder);
    /* the child reaches the terminal through the handles it shares with the shell */
    bool terminal = false;
    for (int i = 0; i < 3; i++)
        if (handles[i] == i && n_terminal(i)) {
            char num[4];
            snprintf(num, sizeof num, "%s%d", terminal ? " " : "", i);
            strlcat(setting_terminal, num, sizeof setting_terminal);
            terminal = true;
        }
    const char *settings[2] = {setting_folder, setting_terminal};
    struct n_launch l = {.arguments = (const char *const *)c->argv, .settings = settings, .handles = handles,
                         .argument_count = (uint32_t)c->argc, .setting_count = terminal ? 2 : 1, .handle_count = 3};
    return n_app_launch(path, &l);
}

/* Wait for the apps of a foreground pipeline; a Ctrl+C stops them and what
   they started. Returns the result of the last one. */
static int wait_apps(n_handle *apps, int napps) {
    n_handle q = n_events_create();
    if (q < 0) return 1;
    for (int i = 0; i < napps; i++)
        if (apps[i] >= 0) n_events_watch(q, apps[i], N_EVENT_APP_ENDED, (uint64_t)i);
    if (interrupts >= 0) n_events_watch(q, interrupts, N_EVENT_RECEIVABLE, UINT64_MAX);
    int status = 0, running = 0;
    for (int i = 0; i < napps; i++) running += apps[i] >= 0;
    while (running > 0) {
        struct n_event ev[17];
        n_result got = n_events_wait(q, ev, 17, -1);
        if (got < 0) break;
        for (int k = 0; k < (int)got; k++) {
            if (ev[k].token == UINT64_MAX) {
                if (!take_interrupts()) continue;
                for (int i = 0; i < napps; i++)
                    if (apps[i] >= 0) n_app_stop(apps[i], N_APP_STOP_TREE);
                out("^C\n");
                continue;
            }
            int i = (int)ev[k].token, st = 0;
            if (i >= napps || apps[i] < 0 || n_app_result(apps[i], 0, &st) < 0) continue;
            if (i == napps - 1) status = st;
            close_handle(&apps[i]);
            running--;
        }
    }
    n_handle_close(q);
    for (int i = 0; i < napps; i++) close_handle(&apps[i]);
    return status;
}

/* run a pipeline; returns exit status of the last command */
static int run_pipeline(struct cmd *cmds, int n, bool background) {
    if (n == 1 && cmds[0].argc && !cmds[0].in && !cmds[0].out) {
        int st;
        if (builtin(cmds[0].argc, cmds[0].argv, &st)) return st;
    }
    n_handle apps[16];
    int napps = 0;
    n_handle prev = -1; /* the receiving end of the previous command's output */
    /* Redirections of the commands waited for stay open until they end, so that closing them saves
       what they wrote: it is on the medium when the pipeline is done. */
    struct output outs[32];
    int nouts = 0;
    int status = 0;
    for (int i = 0; i < n; i++) {
        struct cmd *c = &cmds[i];
        bool waited = false;
        int32_t handles[3];
        for (int k = 0; k < 3; k++) handles[k] = std_open[k] ? k : -1;
        n_handle pipe_ends[2] = {-1, -1}, in = -1, outh = -1, err = -1;
        if (prev >= 0) handles[0] = prev;
        if (i < n - 1) {
            n_result r = n_channel_pair(pipe_ends);
            if (r < 0) {
                fprintf(stderr, "sh: %s\n", n_result_text(r));
                break;
            }
            handles[1] = pipe_ends[1];
        }
        if (c->in) {
            char path[4096];
            in = complete_name(c->in, path, sizeof path) ? n_document_open(path, N_DOCUMENT_READ) : -N_ERR_TOO_LARGE;
            if (in < 0) {
                fprintf(stderr, "sh: %s: %s\n", c->in, n_result_text(in));
                status = 1;
                goto next;
            }
            handles[0] = in;
        }
        if (c->out) {
            outh = open_output(c->out, c->append);
            if (outh < 0) {
                fprintf(stderr, "sh: %s: %s\n", c->out, n_result_text(outh));
                status = 1;
                goto next;
            }
            handles[1] = outh;
        }
        if (c->err) {
            err = open_output(c->err, false);
            if (err >= 0) handles[2] = err;
        }
        if (c->argc) {
            char path[4096];
            int st;
            if (builtin(c->argc, c->argv, &st)) {
                status = st;
            } else if (!find_command(c->argv[0], path, sizeof path)) {
                fprintf(stderr, "sh: %s: command not found\n", c->argv[0]);
                status = 127;
            } else {
                n_handle app = launch(path, c, handles);
                if (app < 0) {
                    fprintf(stderr, "sh: %s: %s\n", c->argv[0], n_result_text(app));
                    status = 126;
                } else if (background) {
                    /* it keeps running on its own; there is nothing to collect */
                    printf("[%d]\n", (int)n_app_id(app));
                    n_handle_close(app);
                } else if (napps < 16) {
                    apps[napps++] = app;
                    waited = true;
                } else {
                    n_handle_close(app);
                }
            }
        }
    next:
        close_handle(&prev);
        close_handle(&pipe_ends[1]);
        prev = pipe_ends[0];
        close_handle(&in);
        struct output o[2] = {{outh, c->out}, {err, c->err}};
        for (int k = 0; k < 2; k++) {
            if (waited && o[k].h >= 0) outs[nouts++] = o[k];
            else if (close_output(&o[k]) && !status) status = 1;
        }
    }
    close_handle(&prev);
    if (napps) status = wait_apps(apps, napps);
    for (int i = 0; i < nouts; i++)
        if (close_output(&outs[i]) && !status) status = 1;
    fflush(stdout);
    return status;
}

static void free_cmds(struct cmd *cmds, int n) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < cmds[i].argc; j++) free(cmds[i].argv[j]);
        free(cmds[i].argv);
    }
}

/* positional parameters of a script or "sh -c": $0..$9, $#, $@ and $* */
static int pargc = 1;
static char **pargv = (char *[]){"sh", NULL};

/* A string that grows as it is written; failed once memory ran out. */
struct text {
    char *p;
    size_t n, cap;
    bool failed;
};

static void put_text(struct text *t, const char *s, size_t n) {
    if (t->failed) return;
    if (t->n + n + 1 > t->cap) {
        size_t cap = (t->n + n + 1) * 2;
        char *more = realloc(t->p, cap);
        if (!more) {
            t->failed = true;
            return;
        }
        t->p = more;
        t->cap = cap;
    }
    memcpy(t->p + t->n, s, n);
    t->p[t->n += n] = 0;
}

/* The line with its positional parameters substituted (not inside single quotes), malloc'd;
   NULL without memory. Unquoted values are split into words by the tokenizer. */
static char *expand_params(const char *line) {
    struct text out = {0};
    bool sq = false, dq = false;
    put_text(&out, "", 0);
    for (const char *s = line; *s; s++) {
        if (*s == '\'' && !dq) sq = !sq;
        else if (*s == '"' && !sq) dq = !dq;
        if (*s != '$' || sq || !(isdigit((unsigned char)s[1]) || s[1] == '#' || s[1] == '@' || s[1] == '*')) {
            put_text(&out, s, 1);
            continue;
        }
        s++;
        if (isdigit((unsigned char)*s)) {
            int k = *s - '0';
            if (k < pargc) put_text(&out, pargv[k], strlen(pargv[k]));
        } else if (*s == '#') {
            char num[16];
            put_text(&out, num, (size_t)snprintf(num, sizeof num, "%d", pargc - 1));
        } else {
            for (int k = 1; k < pargc; k++) {
                if (k > 1) put_text(&out, " ", 1);
                put_text(&out, pargv[k], strlen(pargv[k]));
            }
        }
    }
    if (out.failed) free(out.p);
    return out.failed ? NULL : out.p;
}

static int execute(const char *raw) {
    char *line = expand_params(raw);
    int nt = 0;
    struct token *toks = line ? tokenize(line, &nt) : NULL;
    free(line);
    if (!toks) {
        fprintf(stderr, "sh: %s\n", n_result_text(-N_ERR_NO_MEMORY));
        return 1;
    }
    int pos = 0;
    int status = last_status;
    int skip_mode = 0; /* 0 run, T_AND / T_OR conditions */
    while (pos < nt) {
        struct cmd cmds[16];
        int nc = 0;
        memset(cmds, 0, sizeof cmds);
        bool bg = false;
        int sep = T_END;
        bool syntax_err = false, failed = false;
        while (pos < nt) {
            struct token *t = &toks[pos];
            struct cmd *c = &cmds[nc];
            if (t->type == T_WORD) {
                struct text word = {0};
                put_text(&word, "", 0);
                for (const char *q = t->text; *q; q++) {
                    if (*q != '\x01') {
                        put_text(&word, q, 1);
                        continue;
                    }
                    char num[16];
                    put_text(&word, num, (size_t)snprintf(num, sizeof num, "%d", status));
                }
                if (word.failed) add_arg(c, NULL);
                failed = word.failed || !expand_glob(word.p, c);
                free(word.p);
                if (failed) break;
                pos++;
            } else if (t->type == T_LT || t->type == T_GT || t->type == T_GTGT || t->type == T_2GT) {
                if (pos + 1 >= nt || toks[pos + 1].type != T_WORD) {
                    syntax_err = true;
                    break;
                }
                char *f = toks[pos + 1].text;
                if (t->type == T_LT) c->in = f;
                else if (t->type == T_2GT) c->err = f;
                else {
                    c->out = f;
                    c->append = t->type == T_GTGT;
                }
                pos += 2;
            } else if (t->type == T_PIPE) {
                if (nc == 15 || !c->argc) {
                    syntax_err = true;
                    break;
                }
                nc++;
                pos++;
            } else {
                sep = t->type;
                if (sep == T_AMP) bg = true;
                pos++;
                break;
            }
        }
        nc++;
        if (syntax_err || failed) {
            if (syntax_err) fprintf(stderr, "sh: syntax error\n");
            free_cmds(cmds, nc);
            status = syntax_err ? 2 : 1;
            break;
        }
        bool run = skip_mode == 0 || (skip_mode == T_AND && status == 0) || (skip_mode == T_OR && status != 0);
        if (run && (cmds[0].argc || nc > 1)) status = run_pipeline(cmds, nc, bg);
        free_cmds(cmds, nc);
        skip_mode = (sep == T_AND || sep == T_OR) ? sep : 0;
    }
    for (int i = 0; i < nt; i++) free(toks[i].text);
    free(toks);
    return status;
}

/* A script's lines are as long as they are written: one is never run in pieces. */
static int run_script(const char *name) {
    char path[4096];
    FILE *f = complete_name(name, path, sizeof path) ? fopen(path, "r") : NULL;
    if (!f) {
        fprintf(stderr, "sh: %s: %s\n", name, n_result_text(-errno));
        return 127;
    }
    for (int c = 0; c != EOF;) {
        struct text line = {0};
        put_text(&line, "", 0);
        while ((c = getc(f)) != EOF && c != '\n') {
            char ch = (char)c;
            put_text(&line, &ch, 1);
        }
        if (line.failed) {
            fprintf(stderr, "sh: %s: %s\n", name, n_result_text(-N_ERR_NO_MEMORY));
            last_status = 1;
        } else if (c != EOF || line.n) {
            line.p[strcspn(line.p, "\r")] = 0;
            last_status = execute(line.p);
        }
        free(line.p);
        if (line.failed) break;
    }
    fclose(f);
    return last_status;
}

int main(int argc, char **argv) {
    if (n_path_complete(".", folder, sizeof folder) < 0) strcpy(folder, "C:\\");
    for (int i = 0; i < 3; i++) std_open[i] = n_handle_kind(i) >= 0;
    if (argc > 2 && !strcmp(argv[1], "-c")) {
        /* sh -c CMD [NAME [ARGS...]]: NAME becomes $0 */
        if (argc > 3) pargc = argc - 3, pargv = argv + 3;
        return execute(argv[2]);
    }
    if (argc > 1) {
        pargc = argc - 1, pargv = argv + 1;
        return run_script(argv[1]);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ih = getenv("INTERRUPTS");
    if (ih && n_handle_kind(atoi(ih)) == N_KIND_CHANNEL) interrupts = atoi(ih);
    /* at the prompt, wait for a key or a Ctrl+C, whichever comes first */
    if (interrupts >= 0 && (prompt_events = n_events_create()) >= 0) {
        n_events_watch(prompt_events, 0, N_EVENT_RECEIVABLE | N_EVENT_CLOSED, TOKEN_INPUT);
        n_events_watch(prompt_events, interrupts, N_EVENT_RECEIVABLE, TOKEN_INTERRUPT);
    }
    struct n_entry welcome;
    if (n_entry_info(WELCOME, &welcome) >= 0) {
        char *a[] = {"cat", WELCOME, NULL};
        run_wait("cat", a);
    }
    char line[MAX_LINE];
    for (;;) {
        if (!read_line(line)) break;
        add_history(line);
        last_status = execute(line);
    }
    out("exit\n");
    return 0;
}
