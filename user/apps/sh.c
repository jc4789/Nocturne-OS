/* nsh - the Nocturne shell: line editing, history, pipes, redirection, globbing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "nocturne.h"

#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_HIST 64

static char *history[MAX_HIST];
static int nhist;
static int last_status;
static bool interactive;

/* ---- terminal output helpers ---- */
static void out(const char *s) { write(1, s, strlen(s)); }

static void prompt_str(char *buf, size_t n) {
    char cwd[256];
    getcwd(cwd, sizeof cwd);
    const char *shown = cwd;
    char tmp[260];
    if (!strncmp(cwd, "/home", 5) && (cwd[5] == 0 || cwd[5] == '/')) {
        snprintf(tmp, sizeof tmp, "~%s", cwd + 5);
        shown = tmp;
    }
    snprintf(buf, n, "\x1b[1;35mnocturne\x1b[0m:\x1b[1;34m%s\x1b[0m%s ", shown, last_status ? "\x1b[31m$\x1b[0m" : "$");
}

/* ---- tab completion ---- */
static void complete(char *buf, int *len, int *cur) {
    int start = *cur;
    while (start > 0 && buf[start - 1] != ' ') start--;
    char word[256];
    int wl = *cur - start;
    memcpy(word, buf + start, wl);
    word[wl] = 0;
    bool first = true;
    for (int i = 0; i < start; i++)
        if (buf[i] != ' ') first = false;

    char dir[256], prefix[256];
    char *slash = strrchr(word, '/');
    if (slash) {
        int dl = (int)(slash - word);
        if (dl == 0) strcpy(dir, "/");
        else {
            memcpy(dir, word, dl);
            dir[dl] = 0;
        }
        strcpy(prefix, slash + 1);
    } else {
        strcpy(dir, first ? "/bin" : ".");
        strcpy(prefix, word);
    }
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return;
    char match[64] = {0};
    int nmatch = 0;
    bool match_dir = false;
    char list[1024] = {0};
    struct n_dirent d;
    for (int i = 0; readdir(fd, i, &d) > 0; i++) {
        if (strncmp(d.name, prefix, strlen(prefix))) continue;
        if (nmatch == 0) {
            strcpy(match, d.name);
            match_dir = d.type == N_FT_DIR;
        } else {
            /* keep the common prefix */
            int k = 0;
            while (match[k] && match[k] == d.name[k]) k++;
            match[k] = 0;
        }
        if (strlen(list) + strlen(d.name) + 3 < sizeof list) {
            strcat(list, d.name);
            strcat(list, "  ");
        }
        nmatch++;
    }
    close(fd);
    if (!nmatch) return;
    const char *add = match + strlen(prefix);
    int al = (int)strlen(add);
    if (nmatch == 1) {
        static char tmp[66];
        snprintf(tmp, sizeof tmp, "%s%s", add, match_dir ? "/" : " ");
        add = tmp;
        al = (int)strlen(tmp);
    }
    if (al == 0 && nmatch > 1) {
        out("\n");
        out(list);
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
static int read_byte(void) {
    char c;
    ssize_t n = read(0, &c, 1);
    if (n <= 0) return -1;
    return (unsigned char)c;
}

static void redraw(const char *prompt, const char *buf, int len, int cur) {
    char tmp[MAX_LINE + 512];
    int n = snprintf(tmp, sizeof tmp, "\r%s%.*s\x1b[K", prompt, len, buf);
    if (len - cur > 0) n += snprintf(tmp + n, sizeof tmp - n, "\x1b[%dD", len - cur);
    write(1, tmp, n);
}

/* returns false on EOF */
static bool read_line(char *buf) {
    char prompt[400];
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
                char ch = (char)c;
                write(1, &ch, 1);
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

static int tokenize(const char *s, struct token *toks, int max) {
    int n = 0;
    while (n < max - 1) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') break;
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
        char buf[MAX_LINE];
        int bl = 0;
        while (*s && !strchr(" \t|<>&;", *s)) {
            if (*s == '"' || *s == '\'') {
                char q = *s++;
                t->quoted = true;
                while (*s && *s != q) {
                    if (q == '"' && *s == '\\' && s[1]) s++;
                    if (q == '"' && *s == '$' && s[1] == '?') {
                        if (bl < MAX_LINE - 1) buf[bl++] = '\x01'; /* $?, expanded at run time */
                        s += 2;
                        continue;
                    }
                    if (bl < MAX_LINE - 1) buf[bl++] = *s;
                    s++;
                }
                if (*s) s++;
            } else if (*s == '\\' && s[1]) {
                s++;
                if (bl < MAX_LINE - 1) buf[bl++] = *s++;
            } else if (*s == '$' && s[1] == '?') {
                if (bl < MAX_LINE - 1) buf[bl++] = '\x01'; /* $?, expanded at run time */
                s += 2;
            } else if (*s == '~' && bl == 0 && (s[1] == 0 || s[1] == '/' || s[1] == ' ')) {
                bl += snprintf(buf + bl, sizeof buf - bl, "/home");
                s++;
            } else {
                if (bl < MAX_LINE - 1) buf[bl++] = *s;
                s++;
            }
        }
        buf[bl] = 0;
        t->text = strdup(buf);
    }
    toks[n].type = T_END;
    return n;
}

/* glob matching with * and ? */
static bool match(const char *p, const char *s) {
    if (!*p) return !*s;
    if (*p == '*') {
        for (;; s++) {
            if (match(p + 1, s)) return true;
            if (!*s) return false;
        }
    }
    if (!*s) return false;
    if (*p == '?' || *p == *s) return match(p + 1, s + 1);
    return false;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

static int expand_glob(const char *pat, char **argv, int argc, int max) {
    if (!strpbrk(pat, "*?")) {
        argv[argc++] = strdup(pat);
        return argc;
    }
    char dir[256] = ".", prefix[256] = "";
    const char *file = pat;
    const char *slash = strrchr(pat, '/');
    if (slash) {
        int dl = (int)(slash - pat);
        memcpy(dir, pat, dl);
        dir[dl] = 0;
        if (!dl) strcpy(dir, "/");
        memcpy(prefix, pat, dl + 1);
        prefix[dl + 1] = 0;
        file = slash + 1;
    }
    int start = argc;
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        struct n_dirent d;
        for (int i = 0; readdir(fd, i, &d) > 0 && argc < max; i++) {
            if (d.name[0] == '.' && file[0] != '.') continue;
            if (!match(file, d.name)) continue;
            char full[320];
            snprintf(full, sizeof full, "%s%s", prefix, d.name);
            argv[argc++] = strdup(full);
        }
        close(fd);
    }
    if (argc == start) argv[argc++] = strdup(pat);
    else qsort(argv + start, argc - start, sizeof(char *), cmp_str);
    return argc;
}

/* ---- builtins ---- */
static void cmd_help(void) {
    out("\x1b[1mNocturne shell\x1b[0m - builtins: cd pwd exit history help clear\n"
        "Syntax: cmd args | cmd2 > file, >> append, < input, 2> errors, & background,\n"
        "        cmd1 ; cmd2, cmd1 && cmd2, cmd1 || cmd2, wildcards * and ?\n"
        "Keys:   Up/Down history, Tab completion, Ctrl+C cancel, Ctrl+L clear\n"
        "Try:    ls /bin, neofetch, ps, free, fortune, moonsay hi,\n"
        "        ifconfig, ping example.com, fetch http://example.com\n");
}

static bool builtin(int argc, char **argv, int *status) {
    if (!strcmp(argv[0], "cd")) {
        const char *d = argc > 1 ? argv[1] : "/home";
        if (chdir(d) < 0) {
            fprintf(stderr, "cd: %s: %s\n", d, strerror(errno));
            *status = 1;
        } else {
            *status = 0;
        }
        return true;
    }
    if (!strcmp(argv[0], "pwd")) {
        char cwd[256];
        printf("%s\n", getcwd(cwd, sizeof cwd));
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
struct cmd {
    char *argv[MAX_ARGS];
    int argc;
    char *in, *out, *err;
    bool append;
};

/* run a pipeline; returns exit status of the last command */
static int run_pipeline(struct cmd *cmds, int n, bool background) {
    if (n == 1 && cmds[0].argc && !cmds[0].in && !cmds[0].out) {
        int st;
        if (builtin(cmds[0].argc, cmds[0].argv, &st)) return st;
    }
    int pids[16];
    int npids = 0;
    int prev_rd = -1;
    int status = 0;
    for (int i = 0; i < n; i++) {
        struct cmd *c = &cmds[i];
        int fdmap[3] = {0, 1, 2};
        int pfd[2] = {-1, -1};
        int infd = -1, outfd = -1, errfd = -1;
        if (prev_rd >= 0) fdmap[0] = prev_rd;
        if (i < n - 1) {
            if (pipe(pfd) < 0) {
                perror("pipe");
                break;
            }
            fdmap[1] = pfd[1];
        }
        if (c->in) {
            infd = open(c->in, O_RDONLY);
            if (infd < 0) {
                fprintf(stderr, "sh: %s: %s\n", c->in, strerror(errno));
                status = 1;
                goto next;
            }
            fdmap[0] = infd;
        }
        if (c->out) {
            outfd = open(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC));
            if (outfd < 0) {
                fprintf(stderr, "sh: %s: %s\n", c->out, strerror(errno));
                status = 1;
                goto next;
            }
            fdmap[1] = outfd;
        }
        if (c->err) {
            errfd = open(c->err, O_WRONLY | O_CREAT | O_TRUNC);
            if (errfd >= 0) fdmap[2] = errfd;
        }
        if (c->argc) {
            char path[256];
            int st;
            if (builtin(c->argc, c->argv, &st)) {
                status = st;
            } else if (!find_program(c->argv[0], path, sizeof path)) {
                fprintf(stderr, "sh: %s: command not found\n", c->argv[0]);
                status = 127;
            } else {
                c->argv[c->argc] = NULL;
                int pid = spawn(path, c->argv, fdmap, background ? SPAWN_DETACH : 0);
                if (pid < 0) {
                    fprintf(stderr, "sh: %s: %s\n", c->argv[0], strerror(errno));
                    status = 126;
                } else if (npids < 16) {
                    pids[npids++] = pid;
                    if (background) printf("[%d]\n", pid);
                }
            }
        }
    next:
        if (prev_rd >= 0) close(prev_rd);
        if (pfd[1] >= 0) close(pfd[1]);
        prev_rd = pfd[0];
        if (infd >= 0) close(infd);
        if (outfd >= 0) close(outfd);
        if (errfd >= 0) close(errfd);
    }
    if (prev_rd >= 0) close(prev_rd);
    if (!background) {
        for (int i = 0; i < npids; i++) {
            int st = 0;
            if (waitpid(pids[i], &st, 0) > 0 && i == npids - 1) status = st;
        }
    }
    fflush(stdout);
    return status;
}

static void free_cmds(struct cmd *cmds, int n) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < cmds[i].argc; j++) free(cmds[i].argv[j]);
}

static int execute(const char *line) {
    static struct token toks[256];
    int nt = tokenize(line, toks, 256);
    int pos = 0;
    int status = last_status;
    int skip_mode = 0; /* 0 run, T_AND / T_OR conditions */
    while (pos < nt) {
        struct cmd cmds[16];
        int nc = 0;
        memset(cmds, 0, sizeof cmds);
        bool bg = false;
        int sep = T_END;
        bool syntax_err = false;
        while (pos < nt) {
            struct token *t = &toks[pos];
            struct cmd *c = &cmds[nc];
            if (t->type == T_WORD) {
                char word[MAX_LINE];
                int wl = 0;
                for (const char *q = t->text; *q && wl < MAX_LINE - 12; q++) {
                    if (*q == '\x01') wl += snprintf(word + wl, sizeof word - wl, "%d", status);
                    else word[wl++] = *q;
                }
                word[wl] = 0;
                c->argc = expand_glob(word, c->argv, c->argc, MAX_ARGS - 1);
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
        if (syntax_err) {
            fprintf(stderr, "sh: syntax error\n");
            free_cmds(cmds, nc);
            status = 2;
            break;
        }
        bool run = skip_mode == 0 || (skip_mode == T_AND && status == 0) || (skip_mode == T_OR && status != 0);
        if (run && (cmds[0].argc || nc > 1)) status = run_pipeline(cmds, nc, bg);
        free_cmds(cmds, nc);
        skip_mode = (sep == T_AND || sep == T_OR) ? sep : 0;
    }
    for (int i = 0; i < nt; i++) free(toks[i].text);
    return status;
}

static int run_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "sh: %s: %s\n", path, strerror(errno));
        return 127;
    }
    char line[MAX_LINE];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        last_status = execute(line);
    }
    fclose(f);
    return last_status;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "-c")) return execute(argv[2]);
    if (argc > 1) return run_script(argv[1]);
    interactive = true;
    setvbuf(stdout, NULL, _IONBF, 0);
    struct n_stat st;
    if (stat("/etc/motd", &st) == 0) {
        char *a[] = {"cat", "/etc/motd", NULL};
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
