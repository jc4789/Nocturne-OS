/* block 装置：ディスクの実装（NVMe、AHCI、USB の大容量記憶、VM の ATA と Hyper-V の SCSI）がディスクをここに載せ、
   store がそれを読む。要求は 512 バイトの sector で数える。

   書くことと残すことは別の要求である。書込みは装置がデータを持てば終わり、データはまだ装置の揮発の cache にありうる。
   blk_persist は、その前に書いた全部が媒体に載ったときに終わる。store は書く順をこれで決め（データを先に、それを
   指す記録を後に）、保存ごとに一度だけ頼む。

   どの CPU、どの thread からでもよい。実装は一つの装置に来た要求を自分で並べる（一つずつ、または装置が受けるだけ
   並べて）。blk.c は装置の一覧、電源の門、書込みの数を持ち、実装に渡す要求を全部整える（下）。 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sys/wait.h"

struct hal_device;
struct blk_serial;

struct blkdev {
    int index;
    char name[24]; /* ログのため："nvme0.1"（controller、namespace）、"sata0.2"（adapter、port）、"usb0" */
    char model[41];
    uint64_t sectors;
    /* 装置の block のバイト。512 からの 2 の冪：block を丸ごと覆う書込みは実装の中で読み・変え・書きを要らない */
    uint32_t block;
    /* atomic：装置が去った（抜かれた、adapter が死んだ）。一覧に残り、要求は失敗する。電源の変化は持てないものを
       待たない */
    bool gone;
    uint32_t errors;    /* 現れてから失敗した要求（atomic） */
    uint64_t written;   /* 終わった書込みの要求の全部（atomic、blk.c） */
    uint64_t persisted; /* そのうち、終わった残しが始まる前のもの（atomic、blk.c） */
    /* blk_cache_lost を呼ぶ実装は書込みの命令を数える。どれも atomic： */
    uint64_t given;   /* 装置に入ったもの（blk_write_given） */
    uint64_t ended;   /* そのうち、どうであれ終わったもの（blk_write_ended） */
    uint64_t flushed; /* そのうち、終わった残しが始まる前のもの（blk.c） */
    bool cache_lost;  /* blk_cache_lost：これからの残しは全部失敗する（atomic） */
    /* 一つの block の部分を同時に書く者：それぞれの読み・変え・書きを丸ごと一つずつにし、ほかの者のバイトを元のまま
       書き戻さない */
    struct mutex partial;
    /* 実装が blk_register の前に申告するもの。実装に来る要求はこの中にある：blk.c は大きいものを割り、block の一部の
       書込みを読み・変え・書きにするので、実装は block を丸ごと動かすだけである。揃えと境で見る番地は装置が受ける
       もの（DMA の装置は bus、ほかは phys）。 */
    uint32_t max_sectors;   /* 一つの要求で。0：上限なし */
    uint32_t max_segments;  /* 一つの要求で。0：上限なし */
    uint32_t segment_align; /* 区切りの番地と長さはこの倍数。0：512 */
    /* 一つの区切りが終わり次が始まる所は、どちらもこの倍数（NVMe の PRP：頁の大きさ）。0：どこでも */
    uint32_t segment_boundary;
    bool has_cache; /* 装置は揮発の cache を持ちうる：BLK_PERSIST がそれに届く */
    /* データを DMA で動かす装置（HAL の装置）。blk.c は区切りごとに装置へ渡す番地を HAL に問い（hal_dma_reach）、
       届かない区切りを持つ要求は装置の届くメモリーの緩衝を通す。NULL：装置はデータを物理番地で受ける（CPU が写す
       実装、客の物理番地で読む hypervisor の口） */
    struct hal_device *dma;
    const struct blk_ops *ops;
    void *priv;
    int node; /* 近い NUMA node、-1：知らない（blk_register） */
    struct blk_serial *serial;
};

/* 要求。next で鎖になり、実装は一度の submit で鎖を全部受ける（一つの queue を一度の doorbell で満たせる）。区切りは
   一続きのメモリーである。done は要求ごとにちょうど一度、どの文脈でも（割り込みの処理でも）走り、眠らない。status は
   0 か負の N_ERR_*。実装は渡された要求を blk_complete で終える。BLK_PERSIST はデータを動かさない：それを渡す前に done
   が走った全部は、その done が走るときに媒体にある。

   実装に来る要求は整っている：lba と sectors は block の丸ごと、sectors と segment_count は max_sectors と
   max_segments の中、どの区切りも装置の言うとおりに揃い、区切りは合わせて sectors * 512 バイト、DMA の装置なら
   どの区切りの bus にも装置がそのまま届く。blk.c は大きいものを割り、整わないもの（block の一部、揃わない、装置が
   届かない）は自分の緩衝で読み・変え・書く。 */
enum blk_op { BLK_READ, BLK_WRITE, BLK_PERSIST };
struct blk_segment {
    uint64_t phys; /* 物理番地：blkdev.dma の無い実装が読む */
    uint64_t bus;  /* 装置へ渡す番地：blkdev.dma のある実装が読む。blk.c が HAL で求める */
    uint32_t bytes;
};
struct blk_request {
    struct blk_request *next;
    enum blk_op op;
    uint64_t lba;
    uint32_t sectors;
    const struct blk_segment *segments;
    uint32_t segment_count;
    struct blkdev *device;
    uint32_t tries; /* 実装の：装置に渡した回数 */
    int result;     /* 実装の：要求を終える status */
    /* blk.c の： */
    void (*done)(struct blk_request *r, int status);
    void *context;
    void (*done_to)(struct blk_request *r, int status);
    struct blk_request *parent;    /* 割った要求の部分：全体 */
    uint32_t parts;                /* 全体：終わっていない部分、atomic */
    int status;                    /* 全体：最初に失敗した部分の status、atomic */
    uint64_t written_at, ended_at; /* BLK_PERSIST：覆う数 */
};
/* 実装が r を終える：status は 0 か負の N_ERR_*。どの文脈でも。 */
void blk_complete(struct blk_request *r, int status);
/* データの道：blk.c が眠ってよい thread から、どの CPU からでも、一つの装置に同時に幾つでも直に呼ぶ。submit は鎖の
   全部を始めて戻る：queue の空きを待ってよく、要求の終わりは待たない。CPU ごとの queue を持つ実装は呼んだ CPU の
   ものを使う。 */
struct blk_ops {
    void (*submit)(struct blkdev *d, struct blk_request *chain);
};
/* 一度に一つのことをする装置（port の PIO、USB の bulk の pipe）：実装は transfer を渡し、ops に blk_serial_ops を
   置く。blk.c はその装置の thread で要求を一つずつ行う。 */
typedef int (*blk_transfer_fn)(struct blkdev *d, const struct blk_request *r); /* 0 か負の N_ERR_*。三つの op の全部 */
extern const struct blk_ops blk_serial_ops;
void blk_serial_start(struct blkdev *d, blk_transfer_fn transfer);
/* 装置が去った（blkdev.gone を立てた）：thread は持っているものを失敗させて終わる。 */
void blk_serial_stop(struct blkdev *d);

/* ディスクを一覧に載せ、それを持つ装置の上に出す（N_OFFER_DISK、hal_offer）：眠ってよい thread から。 */
void blk_register(struct hal_device *hal, struct blkdev *d);
int blk_count(void);
struct blkdev *blk_get(int i);
/* buf は核のメモリー。眠ってよい thread から、電源の門を blk_hold で待つ。 */
int blk_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf);
int blk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf);
int blk_persist(struct blkdev *d);
/* 電源の変化：blk_hold は動いている転送の全部の終わりを待ち、それからはディスクに届くのは呼んだ thread だけである。
   ほかの者は blk_release まで blk_read・blk_write で待つ。眠りをまたいで controller の状態と一緒に失われる要求は無く、
   休止の写しと電源断の間に、写しの知らないことはディスクの上で変わらない。 */
void blk_hold(void);
void blk_release(void);
/* blk_hold が通した thread。居なければ NULL。 */
struct thread *blk_holder(void);
/* 装置が初めからやり直したかもしれない（link と、一緒に電源を失った、答えなくなった、controller が壊れた）。揮発の
   cache がそれを越えて残るという文書は無い：持っていたものは失われたかもしれず、残りの flush はそれを言えない。
   どの blk_persist も覆わない書込みの命令が装置に入っていたなら、これからの blk_persist は flush の後に全部失敗し、
   それを書いた者は、誰が先でも、知る。そこの volume は先へ進まず、媒体は最後の丸ごとの commit を持ち続ける。残しの
   無い装置では何も変えない。そのとき cache が切れていると知る実装は呼ばない。終わったが数えていない flush は、
   していないと数える：そのときも残しは全部失敗し、何も失われない。 */
void blk_cache_lost(struct blkdev *d);
/* 実装の書込みの命令の数：装置が見る前と、それが終わったとき（受けても受けなくても、もう何も動かないとき）。要求の
   試しごとに数える。どの CPU でも、spinlock の下でも。 */
void blk_write_given(struct blkdev *d);
void blk_write_ended(struct blkdev *d);
