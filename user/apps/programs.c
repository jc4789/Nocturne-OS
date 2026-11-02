/* サービス programs：起こせるプログラムの台帳と、文書の種類ごとの既定。
   システムのプログラムはこの表にあり、ほかのプログラムは管理者が登録する。
   シェルの開始メニューと印、ファイルの窓の「開く」、設定の窓の「アプリ」の頁はここを読む。
   名前もパスも一覧も長さを決めない。文は common/services_abi.h の形で、一度の応答に全部を載せる。 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include "nocturne.h"
#include "nocturne_services.h"
#include "nocturne_settings.h"

#define SYSTEM N_FOLDER_SYSTEM "\\"
#define PROGRAMS N_FOLDER_PROGRAMS "\\"
#define TYPES(...) (const char *const[]){__VA_ARGS__, NULL}
#define SHOWN (N_PROGRAM_MENU | N_PROGRAM_DESKTOP)

static const struct {
    const char *name[N_LANGUAGE_COUNT]; /* enum n_language の順 */
    const char *path, *argument;
    uint32_t icon, flags;
    const char *const *types;
} system_programs[] = {
    {{"端末", "Terminal"}, SYSTEM "term", "", N_ICON_TERMINAL, SHOWN, NULL},
    {{"ファイル", "Files"}, SYSTEM "files", "", N_ICON_FILES, SHOWN, NULL},
    {{"ブラウザー", "Web Browser"}, PROGRAMS "Browser\\browser", "", N_ICON_NET, SHOWN, TYPES("html", "htm")},
    {{"メモ帳", "Text Editor"}, PROGRAMS "Notepad\\notepad", "", N_ICON_EDITOR, SHOWN, TYPES(N_PROGRAM_ANY_TYPE, "txt", "sh")},
    {{"ペイント", "Paint"}, PROGRAMS "Paint\\paint", "", N_ICON_PAINT, SHOWN, TYPES("ppm")},
    {{"電卓", "Calculator"}, PROGRAMS "Calculator\\calc", "", N_ICON_CALC, N_PROGRAM_MENU, NULL},
    {{"時計", "Clock"}, PROGRAMS "Clock\\clock", "", N_ICON_CLOCK, N_PROGRAM_MENU, NULL},
    {{"状況", "System Monitor"}, SYSTEM "sysmon", "", N_ICON_MONITOR, N_PROGRAM_MENU, NULL},
    {{"ピアノ", "Piano"}, PROGRAMS "Piano\\piano", "", N_ICON_PIANO, SHOWN, NULL},
    {{"音楽", "Sound Player"}, PROGRAMS "Player\\player", "", N_ICON_SOUND, SHOWN, TYPES("wav")},
    {{"メディアプレーヤー", "Media Player"}, PROGRAMS "Media Player\\mediaplayer", "", N_ICON_SOUND, 0,
     TYPES("wav", "mp3", "flac", "aac", "m4a", "ogg", "oga", "opus", "mp4", "avi", "mov")},
    {{"スネーク", "Snake"}, PROGRAMS "Snake\\snake", "", N_ICON_SNAKE, SHOWN, NULL},
    {{"テトリス", "Tetris"}, PROGRAMS "Tetris\\tetris", "", N_ICON_TETRIS, N_PROGRAM_MENU, NULL},
    {{"地雷", "Minesweeper"}, PROGRAMS "Minesweeper\\mines", "", N_ICON_MINES, N_PROGRAM_MENU, NULL},
    {{"フラクタル", "Mandelbrot"}, PROGRAMS "Mandelbrot\\mandel", "", N_ICON_FRACTAL, SHOWN, NULL},
    {{"立方体", "3D Cube"}, PROGRAMS "Cube\\cube", "", N_ICON_CUBE, SHOWN, NULL},
    {{"ライフ", "Game of Life"}, PROGRAMS "Life\\life", "", N_ICON_LIFE, N_PROGRAM_MENU, NULL},
    {{"設定", "Settings"}, SYSTEM "settings", "", N_ICON_SETTINGS, SHOWN, NULL},
    {{"情報", "About"}, SYSTEM "js", SYSTEM "about.js", N_ICON_ABOUT, SHOWN, NULL},
};
#define SYSTEM_PROGRAMS (sizeof system_programs / sizeof *system_programs)
#define REGISTERED_FIRST ((uint32_t)SYSTEM_PROGRAMS + 1) /* 登録の番号は続きから。消しても使い回さない。 */

/* 保存の一通：この頭に bytes の中身が続く。
   RECORD_PROGRAM は n_program_head の一通、RECORD_CLAIM は種類、RECORD_CHOICE は種類とパス（どちらも nul 終わり）。 */
#define RECORD_MAGIC 0x4e50524du
enum { RECORD_PROGRAM = 1, RECORD_CLAIM, RECORD_CHOICE };
struct record {
    uint32_t magic, kind, user, id, bytes;
};

/* 番号の順。 */
struct program {
    uint32_t id, flags, bytes;
    struct n_program_head *message; /* そのまま送る一通。name と path はこの中を指す。 */
    const char *path;
};
/* 種類の順、同じ種類の中は名乗った順。 */
struct claim {
    uint32_t id;
    char *type;
};
/* 利用者、種類の順。path のプログラムが媒体に無くても選択は残る。 */
struct choice {
    uint32_t user;
    char *type, *path;
};

static mtx_t lock;
static n_handle region;
static struct program *programs;
static size_t program_count, program_room;
static struct claim *claims;
static size_t claim_count, claim_room;
static struct choice *choices;
static size_t choice_count, choice_room;
static uint32_t next_id = REGISTERED_FIRST;

static bool grow(void *list, size_t *room, size_t count, size_t size) {
    if (count < *room) return true;
    size_t wanted = *room ? *room * 2 : 32;
    void *grown = realloc(*(void **)list, wanted * size);
    if (!grown) return false;
    *(void **)list = grown;
    *room = wanted;
    return true;
}

static bool type_valid(const char *type) {
    if (!*type) return false;
    if (!strcmp(type, N_PROGRAM_ANY_TYPE)) return true;
    for (const char *c = type; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'))) return false;
    return true;
}

static bool on_medium(const char *path) {
    struct n_entry e;
    return n_entry_info(path, &e) >= 0 && e.kind == N_KIND_DOCUMENT;
}

/* ---- 表 ---- */
static struct program *program_of(uint32_t id) {
    size_t low = 0, high = program_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (programs[middle].id < id) low = middle + 1;
        else high = middle;
    }
    return low < program_count && programs[low].id == id ? &programs[low] : NULL;
}

static struct program *program_at_path(const char *path) {
    for (size_t i = 0; i < program_count; i++)
        if (!strcasecmp(programs[i].path, path)) return &programs[i];
    return NULL;
}

/* bytes の一通を写して表へ入れる。形が合わなければ入れない。 */
static n_result program_add(const void *message, size_t bytes) {
    if (bytes < sizeof(struct n_program_head) || bytes > UINT32_MAX) return -N_ERR_INVALID;
    if (!grow(&programs, &program_room, program_count, sizeof *programs)) return -N_ERR_NO_MEMORY;
    struct n_program_head *copy = malloc(bytes);
    if (!copy) return -N_ERR_NO_MEMORY;
    memcpy(copy, message, bytes);
    const char *text = (const char *)(copy + 1);
    size_t n = copy->name_bytes, p = copy->path_bytes, a = copy->argument_bytes;
    if (!n || !p || !a || bytes != sizeof *copy + n + p + a || text[n - 1] || text[n + p - 1] || text[n + p + a - 1]) {
        free(copy);
        return -N_ERR_INVALID;
    }
    size_t at = program_count;
    while (at && programs[at - 1].id > copy->id) at--;
    memmove(&programs[at + 1], &programs[at], (program_count - at) * sizeof *programs);
    programs[at] = (struct program){.id = copy->id, .flags = copy->flags, .bytes = (uint32_t)bytes, .message = copy, .path = text + n};
    program_count++;
    return 0;
}

static void program_drop(struct program *p) {
    free(p->message);
    program_count--;
    memmove(p, p + 1, (size_t)(&programs[program_count] - p) * sizeof *programs);
}

/* 種類の最初の組の位置。 */
static size_t claim_first(const char *type) {
    size_t low = 0, high = claim_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (strcmp(claims[middle].type, type) < 0) low = middle + 1;
        else high = middle;
    }
    return low;
}

static n_result claim_add(uint32_t id, const char *type) {
    if (!grow(&claims, &claim_room, claim_count, sizeof *claims)) return -N_ERR_NO_MEMORY;
    char *copy = strdup(type);
    if (!copy) return -N_ERR_NO_MEMORY;
    size_t at = claim_first(type);
    while (at < claim_count && !strcmp(claims[at].type, type)) at++;
    memmove(&claims[at + 1], &claims[at], (claim_count - at) * sizeof *claims);
    claims[at] = (struct claim){.id = id, .type = copy};
    claim_count++;
    return 0;
}

static size_t choice_position(uint32_t user, const char *type, bool *found) {
    size_t low = 0, high = choice_count;
    *found = false;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = choices[middle].user != user ? (choices[middle].user < user ? -1 : 1) : strcmp(choices[middle].type, type);
        if (!order) *found = true;
        if (order < 0) low = middle + 1;
        else high = middle;
    }
    return low;
}

static n_result choice_set(uint32_t user, const char *type, const char *path) {
    char *new_path = strdup(path);
    if (!new_path) return -N_ERR_NO_MEMORY;
    bool found;
    size_t at = choice_position(user, type, &found);
    if (found) {
        free(choices[at].path);
        choices[at].path = new_path;
        return 0;
    }
    char *new_type = strdup(type);
    if (!new_type || !grow(&choices, &choice_room, choice_count, sizeof *choices)) {
        free(new_path);
        free(new_type);
        return -N_ERR_NO_MEMORY;
    }
    memmove(&choices[at + 1], &choices[at], (choice_count - at) * sizeof *choices);
    choices[at] = (struct choice){.user = user, .type = new_type, .path = new_path};
    choice_count++;
    return 0;
}

/* ---- 保存：一度に書き直す。変わるのは人が操作したときだけ。 ---- */
static char *record_put(char *at, uint32_t kind, uint32_t user, uint32_t id, const void *a, size_t a_bytes, const void *b, size_t b_bytes) {
    struct record r = {RECORD_MAGIC, kind, user, id, (uint32_t)(a_bytes + b_bytes)};
    memcpy(at, &r, sizeof r);
    at += sizeof r;
    memcpy(at, a, a_bytes);
    at += a_bytes;
    if (b_bytes) memcpy(at, b, b_bytes);
    return at + b_bytes;
}

static n_result table_save(void) {
    size_t total = 0;
    for (size_t i = 0; i < program_count; i++)
        if (!(programs[i].flags & N_PROGRAM_SYSTEM)) total += sizeof(struct record) + programs[i].bytes;
    for (size_t i = 0; i < claim_count; i++)
        if (claims[i].id >= REGISTERED_FIRST) total += sizeof(struct record) + strlen(claims[i].type) + 1;
    for (size_t i = 0; i < choice_count; i++)
        total += sizeof(struct record) + strlen(choices[i].type) + 1 + strlen(choices[i].path) + 1;
    char *all = malloc(total ? total : 1), *at = all;
    if (!all) return -N_ERR_NO_MEMORY;
    for (size_t i = 0; i < program_count; i++)
        if (!(programs[i].flags & N_PROGRAM_SYSTEM))
            at = record_put(at, RECORD_PROGRAM, 0, programs[i].id, programs[i].message, programs[i].bytes, NULL, 0);
    for (size_t i = 0; i < claim_count; i++)
        if (claims[i].id >= REGISTERED_FIRST)
            at = record_put(at, RECORD_CLAIM, 0, claims[i].id, claims[i].type, strlen(claims[i].type) + 1, NULL, 0);
    for (size_t i = 0; i < choice_count; i++)
        at = record_put(at, RECORD_CHOICE, choices[i].user, 0, choices[i].type, strlen(choices[i].type) + 1,
                        choices[i].path, strlen(choices[i].path) + 1);
    n_result r = total ? n_document_write_at(region, 0, all, total) : 0;
    free(all);
    if (r >= 0) r = n_document_set_size(region, total);
    return r < 0 ? r : n_document_commit(region, N_COMMIT_WAIT);
}

static struct n_program_head *message_make(uint32_t id, uint32_t icon, uint32_t flags, const char *name, const char *path,
                                           const char *argument, size_t *bytes) {
    size_t n = strlen(name) + 1, p = strlen(path) + 1, a = strlen(argument) + 1;
    *bytes = sizeof(struct n_program_head) + n + p + a;
    struct n_program_head *m = malloc(*bytes);
    if (!m) return NULL;
    *m = (struct n_program_head){id, icon, flags, (uint32_t)n, (uint32_t)p, (uint32_t)a};
    char *text = (char *)(m + 1);
    memcpy(text, name, n);
    memcpy(text + n, path, p);
    memcpy(text + n + p, argument, a);
    return m;
}

static n_result system_load(void) {
    for (size_t i = 0; i < SYSTEM_PROGRAMS; i++) {
        if (!on_medium(system_programs[i].path)) continue;
        size_t bytes;
        struct n_program_head *m = message_make((uint32_t)i + 1, system_programs[i].icon, system_programs[i].flags | N_PROGRAM_SYSTEM,
                                                system_programs[i].name[0], system_programs[i].path, system_programs[i].argument, &bytes);
        if (!m) return -N_ERR_NO_MEMORY;
        n_result r = program_add(m, bytes);
        free(m);
        if (r < 0) return r;
        for (const char *const *t = system_programs[i].types; t && *t; t++) {
            r = claim_add((uint32_t)i + 1, *t);
            if (r < 0) return r;
        }
    }
    return 0;
}

static n_result table_load(void) {
    n_result r = system_load();
    if (r < 0) return r;
    n_result size = n_document_size(region);
    if (size <= 0) return size;
    char *all = malloc((size_t)size);
    if (!all) return -N_ERR_NO_MEMORY;
    n_result got = n_document_read_at(region, 0, all, (size_t)size);
    for (size_t at = 0; got >= 0 && r >= 0 && at + sizeof(struct record) <= (size_t)got;) {
        struct record h;
        memcpy(&h, all + at, sizeof h);
        char *body = all + at + sizeof h;
        if (h.magic != RECORD_MAGIC || h.bytes > (size_t)got - at - sizeof h) break;
        if (h.kind == RECORD_PROGRAM) {
            if (h.id >= next_id) next_id = h.id + 1;
            r = program_add(body, h.bytes);
        } else if (h.bytes && !body[h.bytes - 1]) {
            if (h.kind == RECORD_CLAIM) {
                r = claim_add(h.id, body);
            } else if (h.kind == RECORD_CHOICE) {
                size_t type_bytes = strlen(body) + 1;
                if (type_bytes < h.bytes) r = choice_set(h.user, body, body + type_bytes);
            }
        }
        at += sizeof h + h.bytes;
    }
    free(all);
    return got < 0 ? got : r;
}

/* ---- 要求 ---- */
struct request_text {
    const char *type, *name, *path, *argument;
};

static bool request_read(const struct n_programs_request *q, size_t bytes, struct request_text *t) {
    size_t a = q->type_bytes, b = q->name_bytes, c = q->path_bytes, d = q->argument_bytes;
    if (!a || !b || !c || !d || bytes != sizeof *q + a + b + c + d) return false;
    const char *text = (const char *)(q + 1);
    if (text[a - 1] || text[a + b - 1] || text[a + b + c - 1] || text[a + b + c + d - 1]) return false;
    *t = (struct request_text){text, text + a, text + a + b, text + a + b + c};
    return true;
}

static n_result program_register(const struct n_programs_request *q, const struct request_text *t, char **body, size_t *body_bytes) {
    if (!*t->name) return -N_ERR_INVALID;
    if (strncasecmp(t->path, PROGRAMS, sizeof PROGRAMS - 1)) return -N_ERR_DENIED;
    if (!on_medium(t->path)) return -N_ERR_NOT_FOUND;
    if (program_at_path(t->path)) return -N_ERR_EXISTS;
    uint32_t id = next_id;
    size_t bytes;
    struct n_program_head *m = message_make(id, q->icon < N_ICON_COUNT ? q->icon : N_ICON_GENERIC, q->flags & SHOWN,
                                            t->name, t->path, t->argument, &bytes);
    if (!m) return -N_ERR_NO_MEMORY;
    n_result r = program_add(m, bytes);
    if (r >= 0) {
        next_id++;
        r = table_save();
        if (r < 0) program_drop(program_of(id));
    }
    if (r < 0) {
        free(m);
        return r;
    }
    *body = (char *)m;
    *body_bytes = bytes;
    return 0;
}

static n_result program_claim(uint32_t id, const char *type) {
    if (!type_valid(type)) return -N_ERR_INVALID;
    if (!program_of(id)) return -N_ERR_NOT_FOUND;
    for (size_t i = claim_first(type); i < claim_count && !strcmp(claims[i].type, type); i++)
        if (claims[i].id == id) return 0;
    n_result r = claim_add(id, type);
    return r < 0 ? r : table_save();
}

static n_result program_remove(uint32_t id) {
    struct program *p = program_of(id);
    if (!p) return -N_ERR_NOT_FOUND;
    if (p->flags & N_PROGRAM_SYSTEM) return -N_ERR_DENIED;
    size_t kept = 0;
    for (size_t i = 0; i < choice_count; i++) {
        if (strcasecmp(choices[i].path, p->path)) {
            choices[kept++] = choices[i];
        } else {
            free(choices[i].type);
            free(choices[i].path);
        }
    }
    choice_count = kept;
    kept = 0;
    for (size_t i = 0; i < claim_count; i++) {
        if (claims[i].id != id) claims[kept++] = claims[i];
        else free(claims[i].type);
    }
    claim_count = kept;
    program_drop(p);
    return table_save();
}

/* 種類を名乗る最初のもの。 */
static struct program *first_claimer(const char *type) {
    size_t i = claim_first(type);
    return i < claim_count && !strcmp(claims[i].type, type) ? program_of(claims[i].id) : NULL;
}

/* その人が選んだもの、無ければ種類を名乗る最初のもの、無ければ何でも開くもの。 */
static struct program *program_for(uint32_t user, const char *type) {
    bool found;
    size_t at = choice_position(user, type, &found);
    struct program *p = found ? program_at_path(choices[at].path) : NULL;
    if (!p) p = first_claimer(type);
    return p ? p : first_claimer(N_PROGRAM_ANY_TYPE);
}

static bool claims_type(uint32_t id, const char *type) {
    const char *const names[] = {type, N_PROGRAM_ANY_TYPE};
    for (int n = 0; n < 2; n++)
        for (size_t i = claim_first(names[n]); i < claim_count && !strcmp(claims[i].type, names[n]); i++)
            if (claims[i].id == id) return true;
    return false;
}

static n_result program_choose(uint32_t user, uint32_t id, const char *type) {
    if (!user) return -N_ERR_DENIED;
    if (!type_valid(type) || !strcmp(type, N_PROGRAM_ANY_TYPE)) return -N_ERR_INVALID;
    struct program *p = program_of(id);
    if (!p || !claims_type(id, type)) return -N_ERR_NOT_FOUND;
    n_result r = choice_set(user, type, p->path);
    return r < 0 ? r : table_save();
}

/* 応答の中身：全部のプログラムの一通を並べる。 */
/* その言語の名前。システムのプログラムは表に言語ごとの名前があり、登録したものは登録した名前のまま。 */
static const char *program_name(const struct program *p, uint32_t language) {
    if (p->id >= 1 && p->id <= SYSTEM_PROGRAMS && language < N_LANGUAGE_COUNT) return system_programs[p->id - 1].name[language];
    return (const char *)(p->message + 1);
}

static n_result list_pack(uint32_t language, char **out, size_t *out_bytes, uint32_t *count) {
    size_t total = 0;
    for (size_t i = 0; i < program_count; i++)
        total += programs[i].bytes - programs[i].message->name_bytes + strlen(program_name(&programs[i], language)) + 1;
    char *all = malloc(total ? total : 1), *at = all;
    if (!all) return -N_ERR_NO_MEMORY;
    for (size_t i = 0; i < program_count; i++) {
        struct n_program_head head = *programs[i].message;
        const char *name = program_name(&programs[i], language), *rest = (const char *)(programs[i].message + 1) + head.name_bytes;
        size_t rest_bytes = head.path_bytes + head.argument_bytes;
        head.name_bytes = (uint32_t)strlen(name) + 1;
        memcpy(at, &head, sizeof head);
        at += sizeof head;
        memcpy(at, name, head.name_bytes);
        at += head.name_bytes;
        memcpy(at, rest, rest_bytes);
        at += rest_bytes;
    }
    *out = all;
    *out_bytes = total;
    *count = (uint32_t)program_count;
    return 0;
}

/* 応答の中身：全部の組。設定の窓は種類の数だけ尋ね直さない。 */
static n_result claims_pack(uint32_t user, char **out, size_t *out_bytes, uint32_t *count) {
    size_t total = 0;
    for (size_t i = 0; i < claim_count; i++) total += sizeof(struct n_program_claim_head) + strlen(claims[i].type) + 1;
    char *all = malloc(total ? total : 1), *at = all;
    if (!all) return -N_ERR_NO_MEMORY;
    uint32_t current = 0;
    for (size_t i = 0; i < claim_count; i++) {
        if (!i || strcmp(claims[i].type, claims[i - 1].type)) {
            struct program *now = strcmp(claims[i].type, N_PROGRAM_ANY_TYPE) ? program_for(user, claims[i].type) : NULL;
            current = now ? now->id : 0;
        }
        size_t type_bytes = strlen(claims[i].type) + 1;
        struct n_program_claim_head h = {claims[i].id, current, (uint32_t)type_bytes};
        memcpy(at, &h, sizeof h);
        memcpy(at + sizeof h, claims[i].type, type_bytes);
        at += sizeof h + type_bytes;
    }
    *out = all;
    *out_bytes = total;
    *count = (uint32_t)claim_count;
    return 0;
}

static void request_serve(const struct n_programs_request *q, const struct request_text *t, const struct n_identity *peer,
                          struct n_programs_reply *reply, char **body, size_t *body_bytes) {
    bool admin = peer->cls >= N_CLASS_ADMIN;
    switch (q->op) {
    case N_PROGRAMS_LIST: reply->result = list_pack(q->language, body, body_bytes, &reply->count); break;
    case N_PROGRAMS_CLAIMS: reply->result = claims_pack(peer->user, body, body_bytes, &reply->count); break;
    case N_PROGRAMS_FOR_TYPE: {
        struct program *p = type_valid(t->type) ? program_for(peer->user, t->type) : NULL;
        reply->result = !p ? -N_ERR_NOT_FOUND : (*body = malloc(p->bytes)) ? 0 : -N_ERR_NO_MEMORY;
        if (reply->result >= 0) {
            memcpy(*body, p->message, p->bytes);
            *body_bytes = p->bytes;
            reply->count = 1;
        }
        break;
    }
    case N_PROGRAMS_CHOOSE: reply->result = program_choose(peer->user, q->id, t->type); break;
    case N_PROGRAMS_REGISTER:
        reply->result = admin ? program_register(q, t, body, body_bytes) : -N_ERR_DENIED;
        if (reply->result >= 0) reply->count = 1;
        break;
    case N_PROGRAMS_CLAIM: reply->result = admin ? program_claim(q->id, t->type) : -N_ERR_DENIED; break;
    case N_PROGRAMS_REMOVE: reply->result = admin ? program_remove(q->id) : -N_ERR_DENIED; break;
    default: break;
    }
}

static void client_serve(void *arg) {
    n_handle channel = (n_handle)(intptr_t)arg;
    struct n_identity peer;
    if (n_channel_peer(channel, &peer) < 0) {
        n_handle_close(channel);
        return;
    }
    for (;;) {
        n_result size = n_channel_receive(channel, NULL, 0, N_CHANNEL_WAIT | N_CHANNEL_PEEK, NULL);
        if (size < (n_result)sizeof(struct n_programs_request)) break;
        struct n_programs_request *q = malloc((size_t)size);
        if (!q) break;
        n_result got = n_channel_receive(channel, q, (size_t)size, N_CHANNEL_WAIT, NULL);
        struct request_text t;
        struct n_programs_reply reply = {.result = -N_ERR_INVALID};
        char *body = NULL;
        size_t body_bytes = 0;
        if (got == size && request_read(q, (size_t)size, &t)) {
            mtx_lock(&lock);
            request_serve(q, &t, &peer, &reply, &body, &body_bytes);
            mtx_unlock(&lock);
        }
        free(q);
        char *whole = malloc(sizeof reply + body_bytes);
        bool sent = false;
        if (whole) {
            memcpy(whole, &reply, sizeof reply);
            if (body_bytes) memcpy(whole + sizeof reply, body, body_bytes);
            sent = n_channel_send(channel, whole, sizeof reply + body_bytes, -1) >= 0;
        }
        free(whole);
        free(body);
        if (!sent) break;
    }
    n_handle_close(channel);
}

int main(void) {
    n_result r = mtx_init(&lock, mtx_plain) == thrd_success ? 0 : -N_ERR_NO_MEMORY;
    if (r >= 0) r = region = n_store_private_open(n_cap_find(N_CAP_STORE_PRIVATE), N_REGION_PROGRAMS, 0, N_DOCUMENT_EDIT);
    if (r >= 0) r = table_load();
    if (r < 0) {
        printf("programs: the table cannot be read (%s)\n", n_result_text(r));
        return 1;
    }
    n_service_ready();
    for (;;) {
        n_handle channel = n_inlet_accept(N_SERVICE_HANDLE_CONTROL, N_CHANNEL_WAIT);
        if (channel < 0) return 0;
        n_result thread = thread_create(client_serve, (void *)(intptr_t)channel);
        if (thread < 0) n_handle_close(channel);
        else thread_detach((int)thread);
    }
}
