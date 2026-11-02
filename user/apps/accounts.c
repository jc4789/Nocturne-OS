/* 利用者のサービス：利用者の一覧と合言葉の持ち主。
   記録は C: の名前の木の外の私的な領域（N_CAP_STORE_PRIVATE）に型のある slot として置く。
   slot は固定の大きさで、番号 n の利用者は slot n-1。一つの slot の末尾の SHA-256 が合わなければ、
   その利用者は無いものとして扱う（書きかけの電源断でも他の利用者は残る）。
   合言葉は salt つきの PBKDF2-HMAC-SHA256（RFC 8018 §5.2）で持つ。原文は持たない。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include "bearssl.h"
#include "nocturne.h"
#include "nocturne_services.h"
#include "settings_abi.h"

#define RECORD_MAGIC 0x5243434eu
#define SALT_BYTES 16u
#define HASH_BYTES 32u
#define PBKDF2_ROUNDS 200000u
#define USERS_FOLDER "C:\\Users"
#define NEW_USER_FILES "C:\\Nocturne\\NewUser" /* 新しい利用者のフォルダーの中身の元 */

struct account_record {
    uint32_t magic;
    uint32_t rounds;
    struct n_account account;
    uint8_t salt[SALT_BYTES];
    uint8_t hash[HASH_BYTES];
    uint8_t check[HASH_BYTES]; /* ここより前の SHA-256 */
};

static n_handle region = -1;
static mtx_t lock; /* 一覧と領域を守る。合言葉の計算の間は持たない。 */
static struct account_record *records;
static size_t record_count;

/* ---- 記録 ---- */
static void record_check(const struct account_record *r, uint8_t out[HASH_BYTES]) {
    br_sha256_context c;
    br_sha256_init(&c);
    br_sha256_update(&c, r, offsetof(struct account_record, check));
    br_sha256_out(&c, out);
}

static n_result records_load(void) {
    n_result size = n_document_size(region);
    if (size < 0) return size;
    size_t count = (size_t)size / sizeof(struct account_record);
    records = calloc(count ? count : 1, sizeof *records);
    if (!records) return -N_ERR_NO_MEMORY;
    if (count) {
        n_result got = n_document_read_at(region, 0, records, count * sizeof *records);
        if (got < 0) return got;
        count = (size_t)got / sizeof *records;
    }
    record_count = count;
    return 0;
}

static bool record_valid(const struct account_record *r) {
    uint8_t check[HASH_BYTES];
    record_check(r, check);
    return r->magic == RECORD_MAGIC && !memcmp(check, r->check, sizeof check);
}

static n_result record_save(size_t slot) {
    struct account_record *r = &records[slot];
    r->magic = RECORD_MAGIC;
    record_check(r, r->check);
    n_result w = n_document_write_at(region, slot * sizeof *r, r, sizeof *r);
    if (w < 0) return w;
    return n_document_commit(region, N_COMMIT_WAIT);
}

/* ---- 合言葉 ---- */
static void pbkdf2_sha256(const char *password, size_t password_bytes, const uint8_t *salt, size_t salt_bytes, uint32_t rounds,
                          uint8_t out[HASH_BYTES]) {
    br_hmac_key_context key;
    br_hmac_key_init(&key, &br_sha256_vtable, password, password_bytes);
    /* 導く鍵は一区画分（32 バイト）：INT(1) を salt の後に付ける。 */
    static const uint8_t block_one[4] = {0, 0, 0, 1};
    br_hmac_context h;
    uint8_t u[HASH_BYTES], t[HASH_BYTES];
    br_hmac_init(&h, &key, 0);
    br_hmac_update(&h, salt, salt_bytes);
    br_hmac_update(&h, block_one, sizeof block_one);
    br_hmac_out(&h, u);
    memcpy(t, u, sizeof t);
    for (uint32_t i = 1; i < rounds; i++) {
        br_hmac_init(&h, &key, 0);
        br_hmac_update(&h, u, sizeof u);
        br_hmac_out(&h, u);
        for (size_t k = 0; k < sizeof t; k++) t[k] ^= u[k];
    }
    memcpy(out, t, HASH_BYTES);
}

static bool equal_in_constant_time(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t difference = 0;
    for (size_t i = 0; i < n; i++) difference |= a[i] ^ b[i];
    return !difference;
}

/* ---- フォルダー ---- */
static bool name_valid(const char *name) {
    size_t n = strnlen(name, N_ACCOUNT_NAME_MAX);
    if (!n || n >= N_ACCOUNT_NAME_MAX || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f || strchr("\\/:*?\"<>|", c)) return false;
    }
    return name[n - 1] != ' ' && name[n - 1] != '.';
}

static n_result folder_ready(const char *path) {
    n_result r = n_folder_create(path);
    return r == -N_ERR_EXISTS ? 0 : r;
}

/* from の中身を to へ。to に同じ名前があれば残す。 */
static n_result copy_tree(const char *from, const char *to) {
    struct n_entry *entries;
    n_result count = n_folder_entries(from, &entries);
    if (count < 0) return count == -N_ERR_NOT_FOUND ? 0 : count;
    n_result result = 0;
    char *source = malloc(4096), *target = malloc(4096);
    if (!source || !target) result = -N_ERR_NO_MEMORY;
    for (n_result i = 0; i < count && result >= 0; i++) {
        result = n_path_join(from, entries[i].name, source, 4096);
        if (result >= 0) result = n_path_join(to, entries[i].name, target, 4096);
        if (result < 0) break;
        if (entries[i].kind == N_KIND_FOLDER) {
            result = folder_ready(target);
            if (result >= 0) result = copy_tree(source, target);
            continue;
        }
        struct n_entry there;
        if (n_entry_info(target, &there) >= 0) continue;
        void *data;
        size_t size;
        result = n_document_load(source, &data, &size);
        if (result >= 0) {
            result = n_document_save(target, data, size);
            n_memory_release(data, size);
        }
    }
    free(source);
    free(target);
    free(entries);
    return result < 0 ? result : 0;
}

static n_result remove_tree(const char *path) {
    struct n_entry *entries;
    n_result count = n_folder_entries(path, &entries);
    if (count < 0) return count;
    n_result result = 0;
    char *inner = malloc(4096);
    if (!inner) result = -N_ERR_NO_MEMORY;
    for (n_result i = 0; i < count && result >= 0; i++) {
        result = n_path_join(path, entries[i].name, inner, 4096);
        if (result < 0) break;
        if (entries[i].kind == N_KIND_FOLDER) {
            result = remove_tree(inner);
            if (result < 0) break;
        }
        result = n_entry_remove(inner);
    }
    free(inner);
    free(entries);
    return result;
}

/* 利用者のフォルダーを用意し、一時フォルダーを空にする。中身は持ち主を親から継ぐので、持ち主を先に決める。 */
static n_result folder_prepare(const struct n_account *a, bool fresh) {
    static const char *const standard[] = {"Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos", "AppData", "AppData\\Temp"};
    n_result r = folder_ready(USERS_FOLDER);
    if (r >= 0) r = folder_ready(a->folder);
    if (r >= 0) r = n_entry_set_owner(a->folder, a->user);
    char path[N_ACCOUNT_FOLDER_MAX + 32];
    for (size_t i = 0; r >= 0 && i < sizeof standard / sizeof *standard; i++) {
        r = n_path_join(a->folder, standard[i], path, sizeof path);
        if (r >= 0) r = folder_ready(path);
    }
    if (r >= 0 && fresh) r = copy_tree(NEW_USER_FILES, a->folder);
    if (r >= 0) {
        r = n_path_join(a->folder, "AppData\\Temp", path, sizeof path);
        if (r >= 0) r = remove_tree(path);
    }
    return r < 0 ? r : 0;
}

/* ---- 要求 ---- */
static struct account_record *record_named(const char *name) {
    for (size_t i = 0; i < record_count; i++)
        if (record_valid(&records[i]) && !strcasecmp(records[i].account.name, name)) return &records[i];
    return NULL;
}

static struct account_record *record_of(uint32_t user) {
    if (!user || user > record_count || !record_valid(&records[user - 1])) return NULL;
    return &records[user - 1];
}

static size_t valid_records(void) {
    size_t n = 0;
    for (size_t i = 0; i < record_count; i++) n += record_valid(&records[i]);
    return n;
}

static n_result account_create(const struct n_accounts_request *q, const struct n_identity *peer, struct n_account *out) {
    if (!name_valid(q->name) || q->cls > N_CLASS_ADMIN) return -N_ERR_INVALID;
    size_t password_bytes = strnlen(q->password, sizeof q->password);
    if (password_bytes == sizeof q->password) return -N_ERR_INVALID;
    struct account_record fresh = {.rounds = PBKDF2_ROUNDS};
    n_result r = n_random(fresh.salt, sizeof fresh.salt);
    if (r < 0) return r;
    if (password_bytes) pbkdf2_sha256(q->password, password_bytes, fresh.salt, sizeof fresh.salt, fresh.rounds, fresh.hash);
    mtx_lock(&lock);
    bool first = !valid_records();
    r = 0;
    if (!first && peer->cls < N_CLASS_ADMIN) r = -N_ERR_DENIED;
    else if (record_named(q->name)) r = -N_ERR_EXISTS;
    size_t slot = record_count;
    if (r >= 0) {
        struct account_record *grown = realloc(records, (record_count + 1) * sizeof *records);
        if (!grown) r = -N_ERR_NO_MEMORY;
        else records = grown;
    }
    if (r >= 0) {
        fresh.account = (struct n_account){.user = (uint32_t)slot + 1, .cls = first ? N_CLASS_ADMIN : q->cls,
                                           .auto_login = q->auto_login != 0, .has_password = password_bytes != 0};
        strlcpy(fresh.account.name, q->name, sizeof fresh.account.name);
        snprintf(fresh.account.folder, sizeof fresh.account.folder, USERS_FOLDER "\\%s", q->name);
        r = folder_prepare(&fresh.account, true);
    }
    if (r >= 0) {
        records[slot] = fresh;
        record_count = slot + 1;
        r = record_save(slot);
        if (r < 0) record_count = slot;
    }
    if (r >= 0) *out = fresh.account;
    mtx_unlock(&lock);
    return r < 0 ? r : 0;
}

static n_result account_verify(const struct n_accounts_request *q, struct n_account *out) {
    mtx_lock(&lock);
    struct account_record *found = record_named(q->name);
    struct account_record r;
    if (found) r = *found;
    mtx_unlock(&lock);
    /* 無い名前にも同じだけの計算をする：応答の時間で名前の有無が分からないように */
    static const uint8_t no_salt[SALT_BYTES];
    size_t bytes = strnlen(q->password, sizeof q->password);
    uint8_t hash[HASH_BYTES];
    if (!found) {
        pbkdf2_sha256(q->password, bytes, no_salt, sizeof no_salt, PBKDF2_ROUNDS, hash);
        return -N_ERR_DENIED;
    }
    if (!r.account.has_password) {
        *out = r.account;
        return bytes ? -N_ERR_DENIED : 0;
    }
    pbkdf2_sha256(q->password, bytes, r.salt, sizeof r.salt, r.rounds, hash);
    if (!equal_in_constant_time(hash, r.hash, sizeof hash)) return -N_ERR_DENIED;
    *out = r.account;
    return 0;
}

static void client_serve(void *arg) {
    n_handle channel = (n_handle)(intptr_t)arg;
    struct n_identity peer;
    if (n_channel_peer(channel, &peer) < 0) {
        n_handle_close(channel);
        return;
    }
    for (;;) {
        struct n_accounts_request q;
        n_result got = n_channel_receive(channel, &q, sizeof q, N_CHANNEL_WAIT, NULL);
        if (got < 0 || got != sizeof q) break;
        q.name[sizeof q.name - 1] = 0;
        q.password[sizeof q.password - 1] = 0;
        struct n_accounts_reply reply = {0};
        struct n_account *list = NULL;
        switch (q.op) {
        case N_ACCOUNTS_LIST:
            mtx_lock(&lock);
            list = calloc(record_count ? record_count : 1, sizeof *list);
            if (!list) reply.result = -N_ERR_NO_MEMORY;
            else
                for (size_t i = 0; i < record_count; i++)
                    if (record_valid(&records[i])) list[reply.count++] = records[i].account;
            mtx_unlock(&lock);
            break;
        case N_ACCOUNTS_SELF: {
            mtx_lock(&lock);
            struct account_record *r = record_of(peer.user);
            if (r) reply.account = r->account;
            else reply.result = -N_ERR_NOT_FOUND;
            mtx_unlock(&lock);
            break;
        }
        case N_ACCOUNTS_CREATE:
            reply.result = account_create(&q, &peer, &reply.account);
            break;
        case N_ACCOUNTS_VERIFY:
            reply.result = peer.cls >= N_CLASS_SYSTEM ? account_verify(&q, &reply.account) : -N_ERR_DENIED;
            break;
        case N_ACCOUNTS_PREPARE: {
            struct n_account a;
            mtx_lock(&lock);
            struct account_record *r = peer.cls >= N_CLASS_SYSTEM ? record_of(q.user) : NULL;
            if (r) a = r->account;
            mtx_unlock(&lock);
            reply.result = r ? folder_prepare(&a, false) : -N_ERR_NOT_FOUND;
            break;
        }
        default:
            reply.result = -N_ERR_INVALID;
            break;
        }
        memset(&q, 0, sizeof q); /* 合言葉を残さない */
        n_result sent = n_channel_send(channel, &reply, sizeof reply, -1);
        for (uint32_t i = 0; sent >= 0 && i < reply.count; i++) sent = n_channel_send(channel, &list[i], sizeof list[i], -1);
        free(list);
        if (sent < 0) break;
    }
    n_handle_close(channel);
}

int main(void) {
    n_result r = mtx_init(&lock, mtx_plain) == thrd_success ? 0 : -N_ERR_NO_MEMORY;
    if (r >= 0) r = region = n_store_private_open(n_cap_find(N_CAP_STORE_PRIVATE), N_REGION_USERS, 0, N_DOCUMENT_EDIT);
    if (r >= 0) r = records_load();
    if (r < 0) {
        printf("accounts: the records cannot be read (%s)\n", n_result_text(r));
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
