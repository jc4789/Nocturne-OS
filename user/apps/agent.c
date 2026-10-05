/* agent: an AI coding agent that lives inside Nocturne.

   It talks to an OpenAI-compatible chat completions endpoint over HTTPS (BearSSL, in-process),
   streams the reply, and lets the model drive the machine through a few tools: run shell commands,
   read/write/edit files, list directories, fetch URLs. Everything it builds can live on the
   persistent disk under /data.

   usage: agent [-r] [-m model] [-c config] [task...]
     no task  interactive session (type /help inside)
     task     run that one task, then exit
     -r       resume the previous session (/data/agent/session.json)

   Configuration lives in /data/etc/agent.conf (key=value lines):
     endpoint=https://hyper.charm.land/v1
     model=glm-5.3-flash
     api_key=...            (never shown; also asked for on first run)
     max_tokens=16384       reasoning=low|high|max   show_thinking=0|1   context_limit=200000 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "nocturne.h"
#include "http.h"
#include "json.h"

#define CONF_PATH    "/data/etc/agent.conf"
#define AGENT_DIR    "/data/agent"
#define SESSION_PATH "/data/agent/session.json"
#define NOTES_PATH   "/data/agent/NOTES.md"
#define SYSTEM_PATH  "/etc/agent/system.md"

#define TOOL_OUT_MAX  (24 * 1024) /* bytes of tool output the model sees */
#define MAX_STEPS     60          /* model calls per task before asking to continue */

/* colours */
#define C_RESET "\x1b[0m"
#define C_DIM   "\x1b[90m"
#define C_USER  "\x1b[1;35m"
#define C_TOOL  "\x1b[36m"
#define C_ERR   "\x1b[31m"
#define C_OK    "\x1b[32m"
#define C_BOLD  "\x1b[1m"

static struct {
    char endpoint[256];
    char model[96];
    char key[512];
    char reasoning[16];
    int max_tokens;
    long context_limit;
    bool show_thinking;
} cf = {"https://hyper.charm.land/v1", "glm-5.3-flash", "", "", 16384, 200000, false};

/* ---------------------------------------------------------------- small helpers */

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* never let the key reach the screen or the model: blank it out of any text we pass along */
static void redact(char *s) {
    size_t kl = strlen(cf.key);
    if (kl < 8 || !s) return;
    for (char *p = strstr(s, cf.key); p; p = strstr(p, cf.key)) memset(p, '*', kl);
}

static char *read_text(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct n_stat st;
    if (fstat(fd, &st) < 0 || st.type != N_FT_FILE) {
        close(fd);
        errno = EISDIR;
        return NULL;
    }
    char *buf = malloc(st.size + 1);
    if (!buf) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    size_t got = 0;
    while (got < st.size) {
        ssize_t n = read(fd, buf + got, st.size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

/* mkdir -p for the directory part of path */
static void make_parents(const char *path) {
    char tmp[512];
    strlcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(tmp);
        *p = '/';
    }
}

static int write_text(const char *path, const char *data, size_t len) {
    make_parents(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    close(fd);
    return 0;
}

/* strip ANSI escape sequences and carriage returns (terminal decoration means nothing to a model) */
static size_t strip_ansi(char *s, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == 0x1b) {
            if (i + 1 < n && s[i + 1] == '[') {
                i += 2;
                while (i < n && !((s[i] >= '@' && s[i] <= '~'))) i++;
            }
            continue;
        }
        if (s[i] == '\r') continue;
        s[o++] = s[i];
    }
    s[o] = 0;
    return o;
}

/* ---------------------------------------------------------------- configuration */

static void conf_load(const char *path) {
    char *text = read_text(path, NULL);
    if (!text) return;
    char *save;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || !*line) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        for (char *e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
        while (*v == ' ' || *v == '\t') v++;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) v[--vl] = 0;
        if (!strcmp(k, "endpoint")) strlcpy(cf.endpoint, v, sizeof cf.endpoint);
        else if (!strcmp(k, "model")) strlcpy(cf.model, v, sizeof cf.model);
        else if (!strcmp(k, "api_key")) strlcpy(cf.key, v, sizeof cf.key);
        else if (!strcmp(k, "reasoning")) strlcpy(cf.reasoning, v, sizeof cf.reasoning);
        else if (!strcmp(k, "max_tokens")) cf.max_tokens = atoi(v);
        else if (!strcmp(k, "context_limit")) cf.context_limit = atol(v);
        else if (!strcmp(k, "show_thinking")) cf.show_thinking = atoi(v) != 0;
    }
    memset(text, 0, strlen(text));
    free(text);
    size_t el = strlen(cf.endpoint);
    while (el && cf.endpoint[el - 1] == '/') cf.endpoint[--el] = 0;
}

static void conf_save(const char *path) {
    struct jbuf b;
    jb_init(&b);
    jb_printf(&b,
              "# Nocturne agent settings\n"
              "endpoint=%s\nmodel=%s\napi_key=%s\nmax_tokens=%d\ncontext_limit=%ld\nshow_thinking=%d\n",
              cf.endpoint, cf.model, cf.key, cf.max_tokens, cf.context_limit, cf.show_thinking ? 1 : 0);
    if (cf.reasoning[0]) jb_printf(&b, "reasoning=%s\n", cf.reasoning);
    if (!b.oom && write_text(path, b.s, b.len) == 0) say(C_DIM "saved settings to %s" C_RESET "\n", path);
    else say(C_ERR "could not write %s: %s" C_RESET "\n", path, strerror(errno));
    memset(b.s, 0, b.len);
    jb_free(&b);
}

/* ---------------------------------------------------------------- conversation */

/* each message is kept as its JSON text; the request body is assembled from them */
struct msg {
    char *json;
    bool tool_result;
};
static struct msg *msgs;
static int nmsgs, cap_msgs;
static long last_prompt_tokens, total_in, total_out;

static void add_msg(char *json, bool tool_result) {
    if (nmsgs == cap_msgs) {
        cap_msgs = cap_msgs ? cap_msgs * 2 : 32;
        msgs = realloc(msgs, sizeof *msgs * cap_msgs);
    }
    msgs[nmsgs].json = json;
    msgs[nmsgs].tool_result = tool_result;
    nmsgs++;
}

static void add_text_msg(const char *role, const char *text) {
    struct jbuf b;
    jb_init(&b);
    jb_puts(&b, "{\"role\":");
    jb_str(&b, role);
    jb_puts(&b, ",\"content\":");
    jb_str(&b, text);
    jb_puts(&b, "}");
    add_msg(jb_take(&b), false);
}

static void clear_msgs(void) {
    for (int i = 0; i < nmsgs; i++) free(msgs[i].json);
    nmsgs = 0;
}

static char *build_system_prompt(void) {
    struct jbuf b;
    jb_init(&b);
    char *sys = read_text(SYSTEM_PATH, NULL);
    jb_puts(&b, sys ? sys : "You are an AI agent running inside Nocturne OS. Use the tools to help the user.\n");
    free(sys);
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char cwd[256];
    getcwd(cwd, sizeof cwd);
    struct n_sysinfo si;
    sysinfo(&si);
    jb_printf(&b,
              "\n# Session\n- Date (UTC): %04d-%02d-%02d %02d:%02d\n- Working directory: %s\n- Model: %s\n"
              "- Free memory: %lu MiB\n",
              tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, cwd, cf.model,
              (unsigned long)(si.free_mem / 1048576));
    char *notes = read_text(NOTES_PATH, NULL);
    if (notes) {
        jb_puts(&b, "\n# Your notes from earlier sessions (" NOTES_PATH ")\n");
        jb_puts(&b, notes);
        free(notes);
    }
    return jb_take(&b);
}

static void save_session(void) {
    struct jbuf b;
    jb_init(&b);
    jb_puts(&b, "[\n");
    for (int i = 0; i < nmsgs; i++) {
        jb_puts(&b, msgs[i].json);
        jb_puts(&b, i + 1 < nmsgs ? ",\n" : "\n");
    }
    jb_puts(&b, "]\n");
    mkdir(AGENT_DIR);
    if (!b.oom) write_text(SESSION_PATH, b.s, b.len);
    jb_free(&b);
}

static bool load_session(void) {
    size_t len;
    char *text = read_text(SESSION_PATH, &len);
    if (!text) return false;
    const char *err;
    struct json *arr = json_parse(text, len, &err);
    free(text);
    if (!arr || arr->type != J_ARR) {
        json_free(arr);
        return false;
    }
    clear_msgs();
    for (int i = 0; i < arr->n; i++) {
        struct jbuf b;
        jb_init(&b);
        json_write(&b, arr->items[i]);
        add_msg(jb_take(&b), json_getstr(arr->items[i], "role") && !strcmp(json_getstr(arr->items[i], "role"), "tool"));
    }
    json_free(arr);
    return true;
}

/* keep the context bounded: replace the content of old tool results with a stub */
static void compact(void) {
    if (last_prompt_tokens < cf.context_limit) return;
    int keep = 8, seen = 0, elided = 0;
    for (int i = nmsgs - 1; i >= 0; i--) {
        if (!msgs[i].tool_result) continue;
        if (++seen <= keep) continue;
        struct json *j = json_parse(msgs[i].json, strlen(msgs[i].json), NULL);
        if (!j) continue;
        const char *id = json_getstr(j, "tool_call_id");
        const char *content = json_getstr(j, "content");
        if (content && strncmp(content, "[elided", 7)) {
            struct jbuf b;
            jb_init(&b);
            jb_puts(&b, "{\"role\":\"tool\",\"tool_call_id\":");
            jb_str(&b, id ? id : "");
            jb_puts(&b, ",\"content\":\"[elided to save context; run the tool again if you need it]\"}");
            free(msgs[i].json);
            msgs[i].json = jb_take(&b);
            elided++;
        }
        json_free(j);
    }
    if (elided) say(C_DIM "(context at %ld tokens: elided %d old tool outputs)" C_RESET "\n", last_prompt_tokens, elided);
}

/* ---------------------------------------------------------------- tools */

static const char *TOOLS_JSON =
    "["
    "{\"type\":\"function\",\"function\":{\"name\":\"run_command\",\"description\":"
    "\"Run a command line with the Nocturne shell (sh -c) and return its exit status and combined stdout/stderr. "
    "Supports pipes, redirection, ;, &&, ||, globs. Each call starts in the agent's working directory, so use "
    "'cd dir && cmd'. Stdin is empty. The command is killed after `timeout` seconds (default 60, max 600); GUI "
    "programs keep running until killed, so give them a short timeout.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\"},"
    "\"timeout\":{\"type\":\"integer\",\"description\":\"seconds\"}},\"required\":[\"command\"]}}},"

    "{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":"
    "\"Read a text file. Lines are prefixed with their line number and a tab (the prefix is not part of the file). "
    "Use offset/limit (in lines) for big files.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},"
    "\"offset\":{\"type\":\"integer\",\"description\":\"first line, 1-based\"},"
    "\"limit\":{\"type\":\"integer\",\"description\":\"number of lines\"}},\"required\":[\"path\"]}}},"

    "{\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"description\":"
    "\"Create or overwrite a file with the given content. Parent directories are created.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}},"

    "{\"type\":\"function\",\"function\":{\"name\":\"edit_file\",\"description\":"
    "\"Replace exact text in a file. old_text must match the file exactly (including whitespace) and be unique, "
    "unless replace_all is true. Prefer this over rewriting a whole file.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"},"
    "\"replace_all\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"old_text\",\"new_text\"]}}},"

    "{\"type\":\"function\",\"function\":{\"name\":\"list_dir\",\"description\":"
    "\"List a directory: one entry per line with its type and size.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"

    "{\"type\":\"function\",\"function\":{\"name\":\"fetch_url\",\"description\":"
    "\"HTTP(S) GET a URL from the internet and return the status and (text) body, truncated to 24 KB.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}}}"
    "]";

/* output collector: keeps the head and the tail of long output */
struct outcap {
    char head[TOOL_OUT_MAX / 3 + 1];
    char tail[TOOL_OUT_MAX - TOOL_OUT_MAX / 3];
    size_t hl, tl, total;
};

static void cap_add(struct outcap *c, const char *s, size_t n) {
    c->total += n;
    size_t room = sizeof c->head - 1 - c->hl;
    size_t take = MIN(room, n);
    memcpy(c->head + c->hl, s, take);
    c->hl += take;
    s += take;
    n -= take;
    if (!n) return;
    if (n >= sizeof c->tail) {
        memcpy(c->tail, s + n - sizeof c->tail, sizeof c->tail);
        c->tl = sizeof c->tail;
        return;
    }
    if (c->tl + n > sizeof c->tail) {
        size_t drop = c->tl + n - sizeof c->tail;
        memmove(c->tail, c->tail + drop, c->tl - drop);
        c->tl -= drop;
    }
    memcpy(c->tail + c->tl, s, n);
    c->tl += n;
}

static void cap_finish(struct outcap *c, struct jbuf *out) {
    c->head[c->hl] = 0;
    jb_raw(out, c->head, c->hl);
    if (c->total > c->hl + c->tl) jb_printf(out, "\n[... %zu bytes omitted ...]\n", c->total - c->hl - c->tl);
    jb_raw(out, c->tail, c->tl);
}

static void tool_run(struct json *args, struct jbuf *out) {
    const char *cmd = json_getstr(args, "command");
    if (!cmd || !*cmd) {
        jb_puts(out, "error: missing command");
        return;
    }
    int timeout = (int)json_getnum(args, "timeout", 60);
    if (timeout < 1) timeout = 1;
    if (timeout > 600) timeout = 600;
    int in_p[2], out_p[2];
    if (pipe(in_p) < 0 || pipe(out_p) < 0) {
        jb_printf(out, "error: pipe: %s", strerror(errno));
        return;
    }
    close(in_p[1]); /* the command sees end-of-file on stdin */
    int fdmap[3] = {in_p[0], out_p[1], out_p[1]};
    char *argv[] = {"sh", "-c", (char *)cmd, NULL};
    int pid = spawn("/bin/sh", argv, fdmap, 0);
    close(in_p[0]);
    close(out_p[1]);
    if (pid < 0) {
        close(out_p[0]);
        jb_printf(out, "error: cannot start the shell: %s", strerror(errno));
        return;
    }
    struct outcap *c = calloc(1, sizeof *c);
    uint64_t deadline = uptime_ms() + (uint64_t)timeout * 1000;
    bool timed_out = false;
    char buf[4096];
    for (;;) {
        int64_t left = (int64_t)(deadline - uptime_ms());
        if (left <= 0) {
            timed_out = true;
            break;
        }
        struct n_pollfd pf = {out_p[0], N_POLLIN, 0};
        int r = poll(&pf, 1, (int)MIN(left, 500));
        if (r < 0) break;
        if (r == 0) continue;
        ssize_t n = read(out_p[0], buf, sizeof buf);
        if (n <= 0) break; /* every writer closed: the command finished */
        cap_add(c, buf, (size_t)n);
    }
    if (timed_out) killtree(pid, 1);
    close(out_p[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    struct jbuf body;
    jb_init(&body);
    cap_finish(c, &body);
    free(c);
    body.len = strip_ansi(body.s ? body.s : (char *)"", body.len);
    if (timed_out) jb_printf(out, "[killed after %d s timeout]\n", timeout);
    else jb_printf(out, "exit status %d\n", status);
    if (body.len) jb_raw(out, body.s, body.len);
    else jb_puts(out, "(no output)");
    jb_free(&body);
}

static void tool_read(struct json *args, struct jbuf *out) {
    const char *path = json_getstr(args, "path");
    if (!path) {
        jb_puts(out, "error: missing path");
        return;
    }
    size_t len;
    char *text = read_text(path, &len);
    if (!text) {
        jb_printf(out, "error: %s: %s", path, strerror(errno));
        return;
    }
    if (memchr(text, 0, len)) {
        jb_printf(out, "%s is a binary file (%zu bytes)", path, len);
        free(text);
        return;
    }
    long offset = (long)json_getnum(args, "offset", 1), limit = (long)json_getnum(args, "limit", 2000);
    if (offset < 1) offset = 1;
    long line = 1, shown = 0;
    const char *p = text;
    while (*p && line < offset) {
        if (*p++ == '\n') line++;
    }
    while (*p && shown < limit && out->len < TOOL_OUT_MAX * 2) {
        const char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        jb_printf(out, "%ld\t", line);
        jb_raw(out, p, l);
        jb_raw(out, "\n", 1);
        p += l + (nl ? 1 : 0);
        line++;
        shown++;
    }
    if (*p) {
        long total = line;
        for (const char *q = p; *q; q++)
            if (*q == '\n') total++;
        jb_printf(out, "[... more lines follow, %ld in total; use offset=%ld to continue]\n", total, line);
    } else if (!shown) {
        jb_puts(out, len ? "(no lines in that range)" : "(empty file)");
    }
    free(text);
}

static void tool_write(struct json *args, struct jbuf *out) {
    const char *path = json_getstr(args, "path");
    struct json *content = json_get(args, "content");
    if (!path || !content || content->type != J_STR) {
        jb_puts(out, "error: needs path and content");
        return;
    }
    if (write_text(path, content->str, content->len) < 0) jb_printf(out, "error: %s: %s", path, strerror(errno));
    else jb_printf(out, "wrote %zu bytes to %s", content->len, path);
}

static void tool_edit(struct json *args, struct jbuf *out) {
    const char *path = json_getstr(args, "path");
    struct json *oldj = json_get(args, "old_text"), *newj = json_get(args, "new_text");
    struct json *all = json_get(args, "replace_all");
    if (!path || !oldj || !newj || oldj->type != J_STR || newj->type != J_STR || !oldj->len) {
        jb_puts(out, "error: needs path, old_text (non-empty) and new_text");
        return;
    }
    size_t len;
    char *text = read_text(path, &len);
    if (!text) {
        jb_printf(out, "error: %s: %s", path, strerror(errno));
        return;
    }
    int count = 0;
    for (char *p = strstr(text, oldj->str); p; p = strstr(p + oldj->len, oldj->str)) count++;
    bool replace_all = all && all->type == J_BOOL && all->b;
    if (count == 0) {
        jb_printf(out, "error: old_text not found in %s (it must match exactly, including indentation)", path);
    } else if (count > 1 && !replace_all) {
        jb_printf(out, "error: old_text occurs %d times in %s; add surrounding lines to make it unique, or set replace_all",
                  count, path);
    } else {
        struct jbuf nb;
        jb_init(&nb);
        char *p = text;
        for (char *hit = strstr(p, oldj->str); hit; hit = strstr(p, oldj->str)) {
            jb_raw(&nb, p, (size_t)(hit - p));
            jb_raw(&nb, newj->str, newj->len);
            p = hit + oldj->len;
            if (!replace_all) break;
        }
        jb_puts(&nb, p);
        if (nb.oom || write_text(path, nb.s, nb.len) < 0) jb_printf(out, "error: writing %s: %s", path, strerror(errno));
        else jb_printf(out, "replaced %d occurrence%s in %s", replace_all ? count : 1, (replace_all ? count : 1) == 1 ? "" : "s", path);
        jb_free(&nb);
    }
    free(text);
}

static void tool_list(struct json *args, struct jbuf *out) {
    const char *path = json_getstr(args, "path");
    if (!path) path = ".";
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        jb_printf(out, "error: %s: %s", path, strerror(errno));
        return;
    }
    struct n_dirent d;
    int i = 0;
    for (; readdir(fd, i, &d) > 0; i++) {
        if (d.type == N_FT_DIR) jb_printf(out, "dir   %s/\n", d.name);
        else jb_printf(out, "file  %s  (%lu bytes)\n", d.name, (unsigned long)d.size);
        if (out->len > TOOL_OUT_MAX) {
            jb_puts(out, "[... truncated]\n");
            break;
        }
    }
    close(fd);
    if (!i) jb_puts(out, "(empty directory)");
}

static void tool_fetch(struct json *args, struct jbuf *out) {
    const char *url = json_getstr(args, "url");
    if (!url) {
        jb_puts(out, "error: missing url");
        return;
    }
    struct http_req rq = {.url = url, .timeout_ms = 30000, .headers = "User-Agent: Nocturne-agent/1.0\r\n"};
    struct http_resp rs;
    if (http_request(&rq, &rs) < 0) {
        jb_printf(out, "error: %s", rs.error);
        return;
    }
    jb_printf(out, "HTTP %d, %zu bytes\n", rs.status, rs.body_len);
    size_t n = MIN(rs.body_len, (size_t)TOOL_OUT_MAX);
    jb_raw(out, rs.body, n);
    if (n < rs.body_len) jb_printf(out, "\n[... %zu more bytes]", rs.body_len - n);
    http_resp_free(&rs);
}

/* a one-line description of a tool call for the screen */
static void show_call(const char *name, struct json *args) {
    const char *arg = json_getstr(args, "command");
    if (!arg) arg = json_getstr(args, "path");
    if (!arg) arg = json_getstr(args, "url");
    char line[200];
    snprintf(line, sizeof line, "%s", arg ? arg : "");
    for (char *p = line; *p; p++)
        if (*p == '\n') *p = ' ';
    say(C_TOOL "* %s" C_RESET " %s\n", name, line);
}

/* print the first few lines of a tool result, dimmed */
static void show_result(const char *text) {
    int lines = 0;
    const char *p = text;
    say(C_DIM);
    while (*p && lines < 6) {
        const char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        printf("  %.*s\n", (int)MIN(l, (size_t)110), p);
        lines++;
        p += l + (nl ? 1 : 0);
    }
    if (*p) {
        int more = 0;
        for (; *p; p++)
            if (*p == '\n') more++;
        printf("  ... (%d more lines)\n", more + 1);
    }
    say(C_RESET);
}

static char *run_tool(const char *name, const char *argtext) {
    struct jbuf out;
    jb_init(&out);
    const char *err = NULL;
    struct json *args = json_parse(argtext, strlen(argtext), &err);
    if (!args || args->type != J_OBJ) {
        say(C_TOOL "* %s" C_RESET " (bad arguments)\n", name);
        jb_printf(&out, "error: the arguments are not valid JSON (%s); send a JSON object", err ? err : "not an object");
        json_free(args);
        return jb_take(&out);
    }
    show_call(name, args);
    if (!strcmp(name, "run_command")) tool_run(args, &out);
    else if (!strcmp(name, "read_file")) tool_read(args, &out);
    else if (!strcmp(name, "write_file")) tool_write(args, &out);
    else if (!strcmp(name, "edit_file")) tool_edit(args, &out);
    else if (!strcmp(name, "list_dir")) tool_list(args, &out);
    else if (!strcmp(name, "fetch_url")) tool_fetch(args, &out);
    else jb_printf(&out, "error: there is no tool called %s", name);
    json_free(args);
    char *s = jb_take(&out);
    if (!s) s = strdup("error: out of memory");
    redact(s);
    if (strcmp(name, "read_file")) show_result(s);
    else {
        int n = 0;
        for (char *p = s; *p; p++)
            if (*p == '\n') n++;
        say(C_DIM "  (%d lines)" C_RESET "\n", n);
    }
    return s;
}

/* ---------------------------------------------------------------- streaming the reply */

/* ---- streamed markdown: **bold**, `code`, ``` fences and # headings get colours instead of
   showing their markup. Text arrives in arbitrary pieces, so this is a byte-at-a-time machine. */
struct md {
    bool sol;        /* at the start of a line */
    int stars;       /* '*' seen but not yet decided */
    int ticks;       /* '`' seen but not yet decided */
    int hashes;      /* '#' at line start, not yet decided */
    bool bold, code, fence, head;
    bool skip_line;  /* rest of a ``` line (the language name) */
};
static struct md md;

static void md_style(void) {
    fputs(C_RESET, stdout);
    if (md.fence) fputs("\x1b[33m", stdout);
    else if (md.head) fputs("\x1b[1;95m", stdout);
    else if (md.code) fputs("\x1b[36m", stdout);
    else if (md.bold) fputs("\x1b[1;97m", stdout);
}

static void md_flush_pending(void) {
    for (; md.hashes; md.hashes--) putchar('#');
    if (md.ticks) {
        if (md.ticks >= 3 && md.sol) {
            md.fence = !md.fence;
            md.skip_line = true;
        } else if (!md.fence) {
            md.code = !md.code;
        } else {
            for (int i = 0; i < md.ticks; i++) putchar('`');
        }
        md.ticks = 0;
        md_style();
    }
    if (md.stars) {
        if (md.stars == 2 && !md.fence && !md.code) {
            md.bold = !md.bold;
            md_style();
        } else {
            for (int i = 0; i < md.stars; i++) putchar('*');
        }
        md.stars = 0;
    }
}

static void md_char(char c) {
    if (md.skip_line) {
        if (c != '\n') return;
        md.skip_line = false;
    }
    if (c == '`') {
        if (md.stars || md.hashes) md_flush_pending();
        md.ticks++;
        return;
    }
    if (md.ticks) md_flush_pending();
    if (md.skip_line) {
        if (c != '\n') return;
        md.skip_line = false;
    }
    if (md.fence) {
        putchar(c);
        md.sol = c == '\n';
        return;
    }
    if (c == '*' && !md.code && md.stars < 2) {
        if (md.hashes) md_flush_pending();
        md.stars++;
        return;
    }
    if (c == '#' && md.sol && md.hashes < 6 && !md.stars) {
        md.hashes++;
        return;
    }
    if (md.hashes) {
        if (c == ' ') {
            md.hashes = 0;
            md.head = true;
            md_style();
            return;
        }
        md_flush_pending();
    }
    if (md.stars) md_flush_pending();
    if (c == '\n') {
        if (md.head || md.code || md.bold) { /* styles never run past a line */
            md.head = md.code = md.bold = false;
            md_style();
        }
        putchar('\n');
        md.sol = true;
        return;
    }
    putchar(c);
    md.sol = false;
}

static void md_put(const char *s) {
    for (; *s; s++) md_char(*s);
    fflush(stdout);
}

static void md_reset(void) {
    md_flush_pending();
    if (md.bold || md.code || md.fence || md.head) fputs(C_RESET, stdout);
    memset(&md, 0, sizeof md);
    md.sol = true;
    fflush(stdout);
}

#define MAX_CALLS 16
struct call {
    char id[128];
    char name[64];
    struct jbuf args;
};

struct stream {
    char line[65536];
    size_t ll;
    bool overflow;
    struct jbuf content;
    struct call calls[MAX_CALLS];
    int ncalls;
    char finish[32];
    bool in_thinking, said_thinking, printed_text, done;
    struct jbuf raw; /* first bytes of the body, for error messages */
};

static void on_event(struct stream *st, const char *data) {
    if (!strcmp(data, "[DONE]")) {
        st->done = true;
        return;
    }
    struct json *j = json_parse(data, strlen(data), NULL);
    if (!j) return;
    struct json *usage = json_get(j, "usage");
    if (usage && usage->type == J_OBJ) {
        last_prompt_tokens = (long)json_getnum(usage, "prompt_tokens", 0);
        total_in += last_prompt_tokens;
        total_out += (long)json_getnum(usage, "completion_tokens", 0);
    }
    struct json *choice = json_at(json_get(j, "choices"), 0);
    struct json *delta = json_get(choice, "delta");
    const char *fin = json_getstr(choice, "finish_reason");
    if (fin) strlcpy(st->finish, fin, sizeof st->finish);
    const char *think = json_getstr(delta, "reasoning_content");
    if (!think) think = json_getstr(delta, "reasoning");
    if (think && *think) {
        if (cf.show_thinking) {
            if (!st->in_thinking) say(C_DIM);
            st->in_thinking = true;
            fputs(think, stdout);
            fflush(stdout);
        } else if (!st->said_thinking) {
            say(C_DIM "thinking..." C_RESET "\r");
            st->said_thinking = true;
        }
    }
    const char *text = json_getstr(delta, "content");
    if (text && *text) {
        if (st->in_thinking) {
            say(C_RESET "\n");
            st->in_thinking = false;
        }
        if (st->said_thinking && !st->printed_text) say("\x1b[K");
        st->printed_text = true;
        jb_puts(&st->content, text);
        md_put(text);
    }
    struct json *tcs = json_get(delta, "tool_calls");
    for (int i = 0; tcs && i < tcs->n; i++) {
        struct json *tc = tcs->items[i];
        int idx = (int)json_getnum(tc, "index", i);
        if (idx < 0 || idx >= MAX_CALLS) continue;
        if (idx >= st->ncalls) {
            for (int k = st->ncalls; k <= idx; k++) jb_init(&st->calls[k].args);
            st->ncalls = idx + 1;
        }
        struct call *c = &st->calls[idx];
        const char *id = json_getstr(tc, "id");
        if (id && *id) strlcpy(c->id, id, sizeof c->id);
        struct json *fn = json_get(tc, "function");
        const char *name = json_getstr(fn, "name");
        if (name && *name) strlcpy(c->name, name, sizeof c->name);
        const char *a = json_getstr(fn, "arguments");
        if (a) jb_puts(&c->args, a);
    }
    json_free(j);
}

static int on_body(void *ctx, const char *data, size_t n) {
    struct stream *st = ctx;
    if (st->raw.len < 2048) jb_raw(&st->raw, data, MIN(n, 2048 - st->raw.len));
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n') {
            st->line[st->ll] = 0;
            if (st->ll && st->line[st->ll - 1] == '\r') st->line[--st->ll] = 0;
            if (!st->overflow && !strncmp(st->line, "data:", 5)) {
                const char *d = st->line + 5;
                while (*d == ' ') d++;
                on_event(st, d);
            }
            st->ll = 0;
            st->overflow = false;
        } else if (st->ll < sizeof st->line - 1) {
            st->line[st->ll++] = c;
        } else {
            st->overflow = true;
        }
    }
    return 0;
}

static void stream_free(struct stream *st) {
    jb_free(&st->content);
    jb_free(&st->raw);
    for (int i = 0; i < st->ncalls; i++) jb_free(&st->calls[i].args);
}

/* one model call; returns 1 if the model asked for tools, 0 if it finished, -1 on error */
static int model_step(const char *system) {
    struct jbuf body;
    jb_init(&body);
    jb_puts(&body, "{\"model\":");
    jb_str(&body, cf.model);
    jb_printf(&body, ",\"stream\":true,\"stream_options\":{\"include_usage\":true},\"max_tokens\":%d", cf.max_tokens);
    if (cf.reasoning[0]) {
        jb_puts(&body, ",\"reasoning_effort\":");
        jb_str(&body, cf.reasoning);
    }
    jb_puts(&body, ",\"tools\":");
    jb_puts(&body, TOOLS_JSON);
    jb_puts(&body, ",\"messages\":[{\"role\":\"system\",\"content\":");
    jb_str(&body, system);
    jb_puts(&body, "}");
    for (int i = 0; i < nmsgs; i++) {
        jb_raw(&body, ",", 1);
        jb_puts(&body, msgs[i].json);
    }
    jb_puts(&body, "]}");
    if (body.oom) {
        say(C_ERR "out of memory building the request" C_RESET "\n");
        jb_free(&body);
        return -1;
    }

    char url[300], headers[700];
    snprintf(url, sizeof url, "%s/chat/completions", cf.endpoint);
    snprintf(headers, sizeof headers,
             "Authorization: Bearer %s\r\nContent-Type: application/json\r\nAccept: text/event-stream\r\n", cf.key);

    struct stream *st = NULL;
    int result = -1;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt) {
            int wait = attempt * attempt * 3;
            say(C_DIM "retrying in %d s..." C_RESET "\n", wait);
            sleep((unsigned)wait);
        }
        if (st) stream_free(st);
        free(st);
        st = calloc(1, sizeof *st);
        struct http_req rq = {.method = "POST",
                              .url = url,
                              .headers = headers,
                              .body = body.s,
                              .body_len = body.len,
                              .timeout_ms = 300000,
                              .on_body = on_body,
                              .ctx = st};
        struct http_resp rs;
        md_reset();
        int r = http_request(&rq, &rs);
        md_reset();
        if (st->in_thinking || st->printed_text) say(C_RESET "\n");
        else if (st->said_thinking) say("\x1b[K");
        if (r < 0) {
            say(C_ERR "network error: %s" C_RESET "\n", rs.error);
            if (st->printed_text || st->ncalls) break; /* a partial reply: do not repeat it */
            continue;
        }
        if (rs.status != 200) {
            char *raw = jb_take(&st->raw);
            if (raw) redact(raw);
            say(C_ERR "the API answered HTTP %d: %.400s" C_RESET "\n", rs.status, raw ? raw : "");
            free(raw);
            if (rs.status == 429 || rs.status >= 500) continue;
            break;
        }
        result = 0;
        break;
    }
    memset(headers, 0, sizeof headers);
    jb_free(&body);
    if (result < 0) {
        stream_free(st);
        free(st);
        return -1;
    }

    /* record the assistant turn */
    struct jbuf m;
    jb_init(&m);
    jb_puts(&m, "{\"role\":\"assistant\",\"content\":");
    if (st->content.len) jb_strn(&m, st->content.s, st->content.len);
    else jb_puts(&m, st->ncalls ? "null" : "\"\"");
    int ncalls = 0;
    for (int i = 0; i < st->ncalls; i++) {
        struct call *c = &st->calls[i];
        if (!c->name[0]) continue;
        if (!c->id[0]) snprintf(c->id, sizeof c->id, "call_%d_%lu", i, (unsigned long)uptime_ms());
        jb_puts(&m, ncalls ? "," : ",\"tool_calls\":[");
        jb_puts(&m, "{\"id\":");
        jb_str(&m, c->id);
        jb_puts(&m, ",\"type\":\"function\",\"function\":{\"name\":");
        jb_str(&m, c->name);
        jb_puts(&m, ",\"arguments\":");
        jb_str(&m, c->args.s ? c->args.s : "{}");
        jb_puts(&m, "}}");
        ncalls++;
    }
    if (ncalls) jb_puts(&m, "]");
    jb_puts(&m, "}");
    add_msg(jb_take(&m), false);

    if (!strcmp(st->finish, "length")) say(C_ERR "(the reply hit max_tokens=%d)" C_RESET "\n", cf.max_tokens);

    /* run the tools and add their results */
    for (int i = 0; i < st->ncalls; i++) {
        struct call *c = &st->calls[i];
        if (!c->name[0]) continue;
        char *res = run_tool(c->name, c->args.s ? c->args.s : "{}");
        struct jbuf t;
        jb_init(&t);
        jb_puts(&t, "{\"role\":\"tool\",\"tool_call_id\":");
        jb_str(&t, c->id);
        jb_puts(&t, ",\"content\":");
        jb_str(&t, res);
        jb_puts(&t, "}");
        add_msg(jb_take(&t), true);
        free(res);
    }
    stream_free(st);
    free(st);
    save_session();
    return ncalls ? 1 : 0;
}

/* work on the current conversation until the model stops calling tools */
static void run_task(bool interactive) {
    char *system = build_system_prompt();
    for (int step = 1;; step++) {
        compact();
        int r = model_step(system);
        if (r <= 0) break;
        if (step % MAX_STEPS == 0) {
            if (!interactive) {
                say(C_ERR "stopping after %d steps" C_RESET "\n", step);
                break;
            }
            char ans[16];
            say(C_BOLD "%d steps so far." C_RESET, step);
            if (readline(" Continue? [Y/n] ", ans, sizeof ans, 0) < 0 || ans[0] == 'n' || ans[0] == 'N') break;
        }
    }
    free(system);
    say(C_DIM "[%s | context %ld tokens | session %ld in / %ld out]" C_RESET "\n", cf.model, last_prompt_tokens,
        total_in, total_out);
}

/* ---------------------------------------------------------------- main */

static void ask_key(void) {
    say("No API key configured. The agent talks to an OpenAI-compatible endpoint:\n  %s (model %s)\n"
        "Paste the key with Ctrl+Shift+V. It is stored in " CONF_PATH " and never shown.\n",
        cf.endpoint, cf.model);
    char key[512];
    if (readline("API key: ", key, sizeof key, RL_SECRET) <= 0) return;
    char *k = key;
    while (*k == ' ') k++;
    strlcpy(cf.key, k, sizeof cf.key);
    for (char *p = cf.key + strlen(cf.key); p > cf.key && p[-1] == ' ';) *--p = 0;
    memset(key, 0, sizeof key);
    conf_save(CONF_PATH);
}

static void help(void) {
    say("Type a task and press Enter; the agent works until it is done. Commands:\n"
        "  /new         start a fresh conversation\n"
        "  /model NAME  switch model (saved)       /models   list the provider's models\n"
        "  /think       show or hide the model's reasoning\n"
        "  /key         enter a new API key\n"
        "  /notes       show the agent's notes (" NOTES_PATH ")\n"
        "  /exit        quit (Ctrl+D works too; Ctrl+C stops the agent at once)\n"
        "The conversation is saved after every step; 'agent -r' resumes it.\n");
}

static void list_models(void) {
    char url[300], headers[700];
    snprintf(url, sizeof url, "%s/models", cf.endpoint);
    snprintf(headers, sizeof headers, "Authorization: Bearer %s\r\n", cf.key);
    struct http_req rq = {.url = url, .headers = headers, .timeout_ms = 30000};
    struct http_resp rs;
    int r = http_request(&rq, &rs);
    memset(headers, 0, sizeof headers);
    if (r < 0 || rs.status != 200) {
        say(C_ERR "could not list models: %s (HTTP %d)" C_RESET "\n", r < 0 ? rs.error : "", rs.status);
        if (r == 0) http_resp_free(&rs);
        return;
    }
    struct json *j = json_parse(rs.body, rs.body_len, NULL);
    struct json *data = json_get(j, "data");
    for (int i = 0; data && i < data->n; i++) {
        struct json *m = data->items[i];
        say("  %-28s %8.0fk context\n", json_getstr(m, "id") ? json_getstr(m, "id") : "?",
            json_getnum(m, "context_window", 0) / 1000);
    }
    json_free(j);
    http_resp_free(&rs);
}

static bool command(const char *line) {
    if (!strcmp(line, "/exit") || !strcmp(line, "/quit")) exit(0);
    if (!strcmp(line, "/help")) help();
    else if (!strcmp(line, "/new")) {
        clear_msgs();
        last_prompt_tokens = 0;
        say(C_DIM "new conversation" C_RESET "\n");
    } else if (!strncmp(line, "/model", 6)) {
        const char *m = line + 6;
        while (*m == ' ') m++;
        if (*m) {
            strlcpy(cf.model, m, sizeof cf.model);
            conf_save(CONF_PATH);
        }
        say("model: %s\n", cf.model);
    } else if (!strcmp(line, "/models")) list_models();
    else if (!strcmp(line, "/think")) {
        cf.show_thinking = !cf.show_thinking;
        say("reasoning is now %s\n", cf.show_thinking ? "shown" : "hidden");
    } else if (!strcmp(line, "/key")) {
        cf.key[0] = 0;
        ask_key();
    } else if (!strcmp(line, "/notes")) {
        char *n = read_text(NOTES_PATH, NULL);
        say("%s\n", n ? n : "(no notes yet)");
        free(n);
    } else {
        say("unknown command; try /help\n");
    }
    return true;
}

int main(int argc, char **argv) {
    const char *conf_path = CONF_PATH, *model = NULL;
    bool resume = false;
    static char task[8192];
    size_t tl = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r")) resume = true;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) conf_path = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            say("usage: agent [-r] [-m model] [-c config] [task...]\n");
            return 0;
        } else {
            int n = snprintf(task + tl, sizeof task - tl, "%s%s", tl ? " " : "", argv[i]);
            if (n > 0 && tl + n < sizeof task) tl += n;
        }
    }
    conf_load(conf_path);
    if (model) strlcpy(cf.model, model, sizeof cf.model);

    struct n_stat st;
    if (stat("/data", &st) < 0) mkdir("/data");
    mkdir(AGENT_DIR);
    if (!cf.key[0]) {
        if (!isatty(0)) {
            fprintf(stderr, "agent: no api_key in %s\n", conf_path);
            return 1;
        }
        ask_key();
        if (!cf.key[0]) return 1;
    }
    if (!net_wait_up(10000)) say(C_ERR "warning: the network is not up (no DHCP lease yet)" C_RESET "\n");

    if (resume) {
        if (load_session()) say(C_DIM "resumed %d messages from " SESSION_PATH C_RESET "\n", nmsgs);
        else say(C_DIM "no saved session to resume" C_RESET "\n");
    }

    if (tl) {
        add_text_msg("user", task);
        run_task(false);
        return 0;
    }

    say(C_BOLD "Nocturne agent" C_RESET " - model %s. Type a task, /help for commands.\n", cf.model);
    static char line[8192];
    for (;;) {
        int n = readline(C_USER "> " C_RESET, line, sizeof line, 0);
        if (n < 0) break;
        if (!n) continue;
        if (line[0] == '/') {
            command(line);
            continue;
        }
        add_text_msg("user", line);
        run_task(true);
    }
    return 0;
}
