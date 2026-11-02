/* 音の型（docs/nocturne-native-abi.md §17）。

   カーネルは音の出口の driver だけを持つ：装置の形式の申告、DMA の輪、位置。混合と変換、音量、出口と形式の
   選択はサービス audio（user/apps/audio.c）が持つ。アプリの流れは audio への channel と共有の輪で運ぶ。 */
#ifndef COMMON_AUDIO_ABI_H
#define COMMON_AUDIO_ABI_H
#include <stdint.h>

/* ---- カーネルの口：N_CAP_AUDIO_DEVICE を持つ者（サービス audio）だけが使う ---- */

enum n_audio_kind {
    N_AUDIO_KIND_SPEAKER,
    N_AUDIO_KIND_HEADPHONE,
    N_AUDIO_KIND_LINE,
    N_AUDIO_KIND_DISPLAY,
    N_AUDIO_KIND_DIGITAL,
    N_AUDIO_KIND_REMOTE,  /* 遠隔デスクトップの相手 */
    N_AUDIO_KIND_OTHER
};
enum n_audio_jack {
    N_AUDIO_JACK_NONE,    /* 刺さり方が分からないか、刺す物が要らない */
    N_AUDIO_JACK_EMPTY,
    N_AUDIO_JACK_PLUGGED
};

/* 標本の大きさ：bits の標本が container バイトの中で左に寄る。 */
struct n_audio_size {
    uint8_t bits, container;
};

/* SYS_AUDIO_DEVICE_LIST の一件の頭。bytes が全長で、次の一件はそこから始まる。頭の後に順に
   rates（uint32_t、Hz、小さい順）、sizes（struct n_audio_size、小さい順）、name、key が続く。name と key は
   終端を含む長さで、上限は無い。形式は率、大きさ、channel の数の組で、どの組も開ける。 */
struct n_audio_device {
    uint32_t bytes;
    uint32_t id;           /* 申告の間変わらない。使い直さない。 */
    uint32_t kind;         /* enum n_audio_kind */
    uint32_t jack;         /* enum n_audio_jack */
    uint32_t available;    /* 0：開けない（装置が止まっている） */
    uint32_t channels_low, channels_high;
    uint32_t rate_count, size_count;
    uint32_t name_bytes;   /* 人に見せる名前。刺さった物で変わる。 */
    uint32_t key_bytes;    /* 起動をまたいで同じ語。設定の装置の鍵（n_setting_device）にする。 */
};
static inline const uint32_t *n_audio_device_rates(const struct n_audio_device *d) { return (const uint32_t *)(d + 1); }
static inline const struct n_audio_size *n_audio_device_sizes(const struct n_audio_device *d) {
    return (const struct n_audio_size *)(n_audio_device_rates(d) + d->rate_count);
}
static inline const char *n_audio_device_name(const struct n_audio_device *d) { return (const char *)(n_audio_device_sizes(d) + d->size_count); }
static inline const char *n_audio_device_key(const struct n_audio_device *d) { return n_audio_device_name(d) + d->name_bytes; }

struct n_audio_device_format {
    uint32_t rate;
    uint16_t channels;
    uint8_t bits, container;
};

/* SYS_AUDIO_DEVICE_OPEN の輪。輪は等しい blocks 個の塊で、装置はそれを終わりなく回る。block_frames と blocks は
   開く者の望みで、答えは装置が取れる一番近い形（その装置の仕様が決める）。 */
struct n_audio_device_ring {
    int32_t memory;        /* 輪の共有 object のハンドル */
    uint32_t block_frames;
    uint32_t blocks;
    uint32_t flush_bytes;  /* 0 でなければ、書いた行をこの大きさごとに clflush する（装置が CPU の cache を覗かない） */
};

/* SYS_AUDIO_DEVICE_STATUS。読むと周期の知らせ（N_EVENT_RECEIVABLE）が消える。 */
struct n_audio_device_status {
    uint64_t position;      /* 流れの始めからの、聞こえた frame の数 */
    uint64_t at_ns;         /* position を読んだ時の monotonic の時計 */
    uint32_t generation;    /* 電源の変化などで輪の中身が失われると増える。流れは止まっている。 */
    uint32_t guard_frames;  /* position からこれだけ先までは装置が読んだか読みかけで、書いても届かない */
    uint32_t running;
    uint32_t errors;        /* 流れの誤りの累積 */
};

/* ---- サービス audio の口 ---- */
#define N_AUDIO_SERVICE "audio"

enum n_audio_sample {
    N_AUDIO_S16,  /* int16_t */
    N_AUDIO_S32,  /* int32_t */
    N_AUDIO_F32   /* float、-1 から 1 */
};
/* 流れの形式。channel の順は WAVE と同じ（左前、右前、中央、低音、左後、右後、左と右の中央寄り、後ろの中央、左横、
   右横と続く）。出口と数が同じならそのまま出し、違えば左右に混ぜる。名前の無い位置の channel は混ぜない。 */
struct n_audio_format {
    uint32_t rate;      /* 0 より大きい Hz */
    uint16_t channels;  /* 1 以上 */
    uint16_t sample;    /* enum n_audio_sample */
};

enum n_audio_op {
    N_AUDIO_OP_OPEN = 1, /* この接続を一つの流れにする。答えの添付が輪。閉じれば残りを鳴らして消える。 */
    N_AUDIO_OP_FLUSH,    /* 流れの音を捨てる。答えが来た時には捨ててある。 */
    N_AUDIO_OP_KICK,     /* 輪に音を書いた（asleep を見た者が送る）。答えは無い。 */
    N_AUDIO_OP_OUTPUTS,  /* 答えの bytes の分の一通が続く：struct n_audio_output の並び。 */
    N_AUDIO_OP_WATCH     /* この接続を見張りにする。変わるたびに 1 バイトが届く。 */
};
struct n_audio_request {
    uint32_t op;       /* enum n_audio_op */
    uint32_t generation;
    struct n_audio_format format;
    /* OPEN：前の輪から続けるとき（サービスが起き直した）の数。全部 0 は新しい流れ。 */
    uint64_t first, base;
    uint64_t underruns, underrun_ns;
};
struct n_audio_reply {
    int32_t result;    /* 0 か -N_ERR_* */
    uint32_t reserved;
    uint64_t bytes;    /* OPEN：輪の共有 object の大きさ。OUTPUTS：続く一通の大きさ。 */
};

/* 流れの輪：共有の頁の頭にこれがあり、data_offset から capacity 個の frame が続く。数は全部、流れを開いてからの
   frame の累積で、輪の場所は数 % capacity。書くのは欄ごとに一方だけ。アプリの書いた head はサービスが確かめる。 */
struct n_audio_ring {
    /* アプリが書く */
    uint64_t head;        /* ここまで音を書いた */
    uint32_t wants_room;  /* 空きを待っている */
    uint32_t reserved0;
    uint64_t reserved1[6];
    /* サービスが書く */
    uint64_t tail;        /* ここまで混ぜた。取り戻しで下がることがある */
    uint64_t keep;        /* これより前は書き潰してよい */
    uint32_t room_bell;   /* tail が進むと増え、wants_room なら起こす */
    uint32_t asleep;      /* 1：次の KICK まで head を見ない */
    uint32_t sequence;    /* 下の欄を書く間は奇数 */
    uint32_t broken;      /* head が壊れていて、流れは止まった */
    uint64_t base;        /* flush で動く：committed と played の 0 */
    uint64_t played;      /* 出口が聞かせ終えた数 */
    uint64_t played_at_ns;
    uint64_t underruns;
    uint64_t underrun_ns;
    int64_t pace_ppb;
    uint32_t generation;
    uint32_t flags;       /* N_AUDIO_CLOCK_* */
    /* 開いた時に決まる */
    uint32_t capacity;    /* frame */
    uint32_t queue;       /* 混ぜる前に持てる frame の上限 */
    uint32_t frame_bytes;
    uint32_t data_offset;
};

/* n_audio_status：流れの今。played_frames と played_at_ns は一組で、出口が位置を最後に報告した時の値。 */
struct n_audio_status {
    uint32_t queued_frames;
    uint32_t device_pending_frames;
    uint64_t played_frames;
    uint64_t committed_frames;
    uint32_t generation;
    uint32_t flags;
    uint64_t underruns;
    uint64_t underrun_ns;
    uint64_t played_at_ns;
    int64_t pace_ppb;
};
#define N_AUDIO_CLOCK_DEVICE 1u     /* 出口の装置の位置 */
#define N_AUDIO_CLOCK_ESTIMATED 2u  /* 出口が無く、時計で数えた */
#define N_AUDIO_CLOCK_REMOTE 4u     /* 遠隔の相手の報告 */
#define N_AUDIO_WAIT 1u             /* n_audio_submit：満杯なら空きを待つ */

/* N_AUDIO_OP_OUTPUTS の一件の頭：サービスの今の決めの後に、出口の申告（struct n_audio_device）が続く。
   bytes が申告を含む全長で、次の一件はそこから始まる。 */
struct n_audio_output {
    uint32_t bytes;
    uint32_t playing;                    /* 今これが鳴らしている */
    struct n_audio_device_format format; /* 開くときの形式（選んだものか、既定） */
};
static inline const struct n_audio_device *n_audio_output_device(const struct n_audio_output *o) { return (const struct n_audio_device *)(o + 1); }
#endif
