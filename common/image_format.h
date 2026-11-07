/* Nocturne の像の形式。プログラム、利用者の空間の像（C:\Nocturne\Drivers の GPU の像）、カーネルの実装の像は
   どれもこの形式で、頭の kind で分ける。組みは ELF64 を中間物として作り、変換（common/image_write.c）がこの形式へ
   変える。読むのはカーネル（sys/proc.c のプログラムと利用者の像、io/load.c の実装の像）だけ。

   ファイルの並び：頭、表（区画、基底の再配置、輸入、注記、名の文字列）、区画の中身。区画の中身はファイルの中で
   頁の境から始まり、ファイルの位置と番地が頁の中で同じずれを持つ（読み取り専用の区画は文書の頁をそのまま共有して
   写せる）。表の位置は 8 の倍数。数はどれも 32 bit で、表の中身はファイルの中に収まる。

   読み手が確かめること（どれかが外れれば像を読まない）：印と形式の版と頭の大きさ、kind と machine、表と区画が
   ファイルに収まる、区画が番地の昇順で頁を分け合わず span に収まる、書けて走る区画が無い、入口が走る区画の中、
   再配置と輸入の slot が区画のファイルの中身の中で 8 に揃う、注記の ABI の版、kind ごとの制約（下の enum）。
   slot を持つ区画は共有せず、読み手が値を書いてから権限を付けて写す。 */
#ifndef NOCTURNE_IMAGE_FORMAT_H
#define NOCTURNE_IMAGE_FORMAT_H

#include <stdint.h>

#define N_IMAGE_MAGIC "NOCTURNE" /* 8 byte、nul なし */
#define N_IMAGE_FORMAT 1u
#define N_IMAGE_X86_64 0x8664u

enum n_image_kind {
    N_IMAGE_PROGRAM = 1, /* プロセスの初めの像。base に写す。再配置も輸入も無い。注記は N_IMAGE_NOTE_PROGRAM */
    N_IMAGE_LIBRARY = 2, /* 走るプロセスへ写す像（SYS_IMAGE_MAP）。base は 0 で読み手が番地を選び、基底の再配置を
                            当てる。輸入は無い。注記は N_IMAGE_NOTE_PROGRAM */
    N_IMAGE_DRIVER = 3,  /* カーネルの実装の像。base は 0 でカーネルの番地へ写し、基底の再配置と輸入（DDK の輸出の
                            表で名を引く）を当てる。TLS を持たない。注記は N_IMAGE_NOTE_DDK と N_IMAGE_NOTE_DRIVER と
                            一つ以上の N_IMAGE_NOTE_RULE */
};

enum n_image_access {
    N_IMAGE_READ = 1u,
    N_IMAGE_WRITE = 2u,
    N_IMAGE_RUN = 4u, /* N_IMAGE_WRITE と一緒には持てない */
};

struct n_image_table {
    uint32_t offset; /* ファイルの頭から */
    uint32_t count;  /* 記録の数。strings は byte の数 */
};

/* 像の thread-local の雛形（x86-64 の variant II）。memory_bytes が 0 なら無い。 */
struct n_image_tls {
    uint64_t address; /* base からの距離。file_bytes の雛形のあとは memory_bytes まで 0 */
    uint64_t file_bytes;
    uint64_t memory_bytes;
    uint64_t align;
};

struct n_image_head {
    char magic[8];
    uint32_t format;     /* N_IMAGE_FORMAT */
    uint32_t head_bytes; /* sizeof(struct n_image_head) */
    uint32_t kind;       /* enum n_image_kind */
    uint32_t machine;    /* N_IMAGE_X86_64 */
    uint64_t base;       /* N_IMAGE_PROGRAM：写す番地（頁の倍数）。ほかは 0 */
    uint64_t span;       /* 番地の幅（頁の倍数）。区画は全部 [0, span) の中 */
    uint64_t entry;      /* base からの距離 */
    uint64_t file_bytes; /* ファイルの大きさ */
    struct n_image_table sections;    /* struct n_image_section */
    struct n_image_table relocations; /* uint64_t：基底を足す slot の番地（base から） */
    struct n_image_table imports;     /* struct n_image_import */
    struct n_image_table notes;       /* struct n_image_note の並び。count は記録の数 */
    struct n_image_table strings;     /* nul で終わる名の並び */
    struct n_image_tls tls;
};

struct n_image_section {
    uint64_t address;      /* base からの距離（頁の倍数） */
    uint64_t memory_bytes; /* file_bytes のあとは 0 */
    uint64_t file_offset;  /* address と頁の中で同じずれ。file_bytes が 0 なら 0 */
    uint64_t file_bytes;
    uint32_t access; /* enum n_image_access。N_IMAGE_READ は必ず持つ */
    uint32_t reserved;
};

/* 基底の再配置：slot の 64 bit には番地 0 から数えた値が入っていて、読み手は写した番地を足す。 */

/* 輸入：slot へ「名の番地 + addend」を書く。名は strings の中の位置。 */
struct n_image_import {
    uint64_t slot;
    int64_t addend;
    uint32_t name;
    uint32_t reserved;
};

/* 注記：struct n_image_note の頭と bytes の中身。次の記録は 8 に揃えた位置から。 */
struct n_image_note {
    uint32_t type; /* enum n_image_note_type */
    uint32_t bytes;
};

enum n_image_note_type {
    N_IMAGE_NOTE_PROGRAM = 1, /* struct n_image_note_program */
    N_IMAGE_NOTE_DDK = 2,     /* struct n_image_note_ddk */
    N_IMAGE_NOTE_DRIVER = 3,  /* struct n_image_note_driver */
    N_IMAGE_NOTE_RULE = 4,    /* struct n_image_rule_term の並び。全部の項が合えば規則が合う */
};

/* 組んだときのネイティブ ABI（common/abi.h）。読み手は generation が N_ABI_GENERATION と同じで slots が
   N_ENTRY_SLOTS 以下の像だけを読む。 */
struct n_image_note_program {
    uint32_t generation;
    uint32_t slots;
};

/* 組んだときの DDK の版（ddk/ddk.h）。読み手は major が同じで minor が自分の minor 以下の像だけを読む。 */
struct n_image_note_ddk {
    uint32_t major;
    uint32_t minor;
};

#define N_IMAGE_DRIVER_BOOT 1u /* 起動の媒体を読むまでの道に要る：起動の束（N_IMAGE_BUNDLE_MAGIC）に入る */
struct n_image_note_driver {
    uint32_t flags; /* N_IMAGE_DRIVER_* */
    uint32_t name;  /* 実装の名。strings の中の位置 */
};

/* 規則の項。size が 0 なら数の項（性質の値 & mask == value）、そうでなければ bytes の項（性質の bytes の頭が、
   この記録のあとに続く size の bytes と同じ）。次の項は 8 に揃えた位置から。鍵と性質は common/hal_abi.h。 */
struct n_image_rule_term {
    uint32_t key;
    uint32_t size;
    uint64_t value;
    uint64_t mask;
};

/* 起動の束：Limine の module（module_string: drivers）で来る、起動に要る実装の像の並び。頭のあとに
   struct n_image_bundle_entry が count 個、そのあとに像の中身（8 に揃える）。 */
#define N_IMAGE_BUNDLE_MAGIC "NOCTDRVS"
struct n_image_bundle_head {
    char magic[8];
    uint32_t count;
    uint32_t reserved;
};
struct n_image_bundle_entry {
    uint64_t offset; /* 束の頭から */
    uint64_t bytes;
};

#endif
