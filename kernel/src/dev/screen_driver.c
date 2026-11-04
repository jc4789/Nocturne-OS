/* 画面を出すドライバー（docs/nocturne-native-abi.md §9）：claim を持つアプリのドライバーが、出力（モニター）ごとの
   buffer を画面（dev/fb.h の struct screen）へ渡し、画面が求める flip を受け、垂直帰線を知らせる。どのバスの claim
   でも使える（hal/hal.h）：画面の持ち主は装置の id と出力の番号である。
   claim の記録（struct screen_claim）は最初の呼出しで作り、claim の終わりの hook（hal_claim_on_end）が片づける。
   hook はバスが装置を止めた後に走るので、ドライバーが装置へ届く間は、画面は描き戻されない。
   錠の順：claim の終わりの mutex → screen_lock → mm/shared.c の錠。flip_lock は screen_lock の下で取り、葉である。
   画面は一度できたら消えない。ドライバーが去った画面は、buffer をもらうまで消えている。 */
#include "kernel.h"
#include "abi.h"
#include "arch/cpu.h"
#include "arch/irq.h"
#include "arch/spin.h"
#include "dev/fb.h"
#include "dev/fbcon.h"
#include "hal/hal.h"
#include "mm/heap.h"
#include "mm/shared.h"
#include "sys/clock.h"
#include "sys/object.h"
#include "sys/proc.h"
#include "sys/process.h"
#include "sys/settings.h"
#include "sys/thread.h"
#include "sys/wait.h"

/* ファームウェアの画面の画素が置かれたメモリーの資源の slot。無ければ -1。 */
int screen_driver_boot_slot(const struct hal_device *d) {
    if (!fb.addr) return -1;
    for (uint32_t i = 0; i < d->resource_count; i++) {
        const struct n_hal_resource *r = &d->resources[i];
        if (r->kind == N_HAL_MEMORY && r->size && fb.phys >= r->base && fb.phys - r->base < r->size) return (int)r->slot;
    }
    return -1;
}

struct screen_output {
    struct screen_claim *claim;
    uint32_t output;
    struct screen *screen;
    bool modes;     /* it offers modes */
    bool direct;    /* its buffers have N_SCANOUT_DIRECT */
    unsigned count; /* its buffers, 0: none given */
    struct shared_scanout surface[SCREEN_BUFFERS_MAX];
    struct scanout *buf[SCREEN_BUFFERS_MAX];
};

/* 一つの claim が持つ画面の物。screen_lock の下で変わる（flips と flip_lock の分は別）。 */
struct screen_claim {
    struct hal_claim_end end; /* 先頭でなくてよい：claim_of_end が引く */
    struct screen_claim *next; /* screen_claims */
    struct hal_claim *claim;
    unsigned holds; /* screen_driver_hold の入れ子。claim が終わると全部返す */
    unsigned users; /* flips を待っている thread。終わった記録は最後の一人が返す */
    bool ended;
    struct screen_output **outputs;
    size_t noutputs, outputs_room;
    /* 画面が求めて、ドライバーがまだ取っていない flip（出力ごとに一枠：新しい flip が古いのを取る）。 */
    struct spinlock flip_lock;
    struct wait_queue flip_wq;
    struct n_screen_flip *flips;
    size_t nflips, flips_room;
    bool flips_closed; /* flip_lock の下：終わった */
};

static struct mutex screen_lock; /* 全 claim の出力、output_screens、image、screen_claims */
static struct screen_claim *screen_claims;

#define CLAIM_OF_END(e) ((struct screen_claim *)((char *)(e) - offsetof(struct screen_claim, end)))

/* 出力の画面：同じ装置の同じ出力は、ドライバーが替わっても同じ画面。 */
struct output_screen {
    uint32_t device;
    uint32_t output;
    struct screen *screen;
};
static struct output_screen *output_screens;
static size_t noutput_screens, output_screens_room;

/* ドライバーが画面の代わりに見せてよい image（n_screen_image_add）。panic の画面もここへ描く
   （dev/fb.c の fb_image_add）。出力の buffer が N_SCANOUT_DIRECT の間だけ生きる。 */
struct screen_image {
    struct screen_image *next;
    struct screen_output *output;
    int64_t number; /* 0 にも負にもならない */
    struct shared_scanout surface;
    struct scanout *out;
};
static struct screen_image *screen_images;
static int64_t screen_image_last;

static void screen_say(const struct screen_claim *sc, uint32_t output, const char *what) {
    const struct hal_device *d = hal_claim_device(sc->claim);
    kprintf("screen: device %u (%s): output %u: %s\n", hal_id(d), d->location, output, what);
}

/* ---- 画面を持つドライバー（N_PCI_CALL_HOLD_SCREEN、dev/fb.c） ----
   ファームウェアの画面の画素がある装置の claim だけが保持できるので、保持するのは高々一つの claim である。保持は
   ドライバーが返すか claim が終わると終わる。終わりはバスが装置を止めた後に来るので、ドライバーが装置へ届く間は
   描画は再開しない。起動の引数 fbhold=<秒> の秒数を超えた保持は、画面の見張りの thread がドライバーのアプリを
   止め、その claim の終わりが画面を返す。既定では時間で保持を終わらせない：RM の待ちは上限なく続くので、遅いだけの
   ドライバーを止めると GPU を失う。 */
/* 見張りが見る保持：screen_holder と下の三つ、screen_hold_lock の下（単独で取り、screen_watch_wq の錠より先）。
   保持の数は screen_lock の下で変わる。screen_watch_started は atomic。 */
static struct spinlock screen_hold_lock;
static struct screen_claim *screen_holder;
static char screen_holder_who[64];
static int screen_holder_app;
static uint64_t screen_held_since, screen_hold_limit_ns;
static bool screen_holder_stopped, screen_watch_started;
static struct wait_queue screen_watch_wq;

/* 保持しているドライバーのアプリを止める。保持が無ければ false。 */
static bool screen_holder_stop(const char *why) {
    uint64_t irq = spin_lock(&screen_hold_lock);
    int app = screen_holder && !screen_holder_stopped ? screen_holder_app : 0;
    char who[sizeof screen_holder_who];
    memcpy(who, screen_holder_who, sizeof who);
    if (app) screen_holder_stopped = true;
    spin_unlock(&screen_hold_lock, irq);
    if (!app) return false;
    kprintf("screen: %s: %s; stopping app %d; framebuffer writes resume at release, device recovery may be needed\n",
            who, why, app);
    if (process_kill(app) < 0) kprintf("screen: app %d cannot be stopped; the screen stays held\n", app);
    return true;
}

/* N_DISPLAY_HOLD_SECONDS が初めて上限を決めるときに起こす。 */
static void screen_watch(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t irq = spin_lock(&screen_hold_lock);
        uint64_t limit = screen_hold_limit_ns;
        bool holding = screen_holder && !screen_holder_stopped && limit;
        uint64_t until = holding ? screen_held_since + limit : CLOCK_NEVER;
        if (holding && clock_now_ns() >= until) {
            spin_unlock(&screen_hold_lock, irq);
            char why[64];
            ksnprintf(why, sizeof why, "its driver has held the screen for %lu s", (unsigned long)(limit / NS_PER_S));
            screen_holder_stop(why);
            continue;
        }
        /* 保持の始まりと終わりは screen_hold_lock を取ってからこれを起こす */
        wq_wait_locked_until(&screen_watch_wq, &screen_hold_lock, until);
        irq_restore(irq);
    }
}

int screen_hold_setting_put(uint32_t key, const void *value) {
    int32_t seconds;
    memcpy(&seconds, value, sizeof seconds);
    bool start = seconds && !__atomic_exchange_n(&screen_watch_started, true, __ATOMIC_RELAXED);
    if (start && !kthread_create("screen hold", screen_watch, NULL)) return -N_ERR_NO_MEMORY;
    uint64_t irq = spin_lock(&screen_hold_lock);
    screen_hold_limit_ns = (uint64_t)seconds * NS_PER_S;
    wq_wake_all(&screen_watch_wq);
    spin_unlock(&screen_hold_lock, irq);
    settings_changed(N_DOMAIN_DISPLAY);
    return 0;
}

int64_t screen_hold_setting_get(uint32_t key, void *out, size_t cap) {
    uint64_t irq = spin_lock(&screen_hold_lock);
    uint64_t limit = screen_hold_limit_ns;
    spin_unlock(&screen_hold_lock, irq);
    return setting_int(out, cap, (int32_t)(limit / NS_PER_S));
}

/* screen_lock held. */
static void screen_hold_begin(struct screen_claim *sc) {
    const struct hal_device *d = hal_claim_device(sc->claim);
    int app = process_current()->pid;
    /* 画面がまだ映っている間に言う：次に画面が止まれば、その理由になる。 */
    kprintf("screen: device %u (%s): app %d holds the screen while its driver starts or stops the device\n", hal_id(d),
            d->location, app);
    uint64_t irq = spin_lock(&screen_hold_lock);
    screen_holder = sc;
    ksnprintf(screen_holder_who, sizeof screen_holder_who, "device %u (%s)", hal_id(d), d->location);
    screen_holder_app = app;
    screen_held_since = clock_now_ns();
    screen_holder_stopped = false;
    wq_wake_all(&screen_watch_wq);
    spin_unlock(&screen_hold_lock, irq);
}

static void screen_hold_end(struct screen_claim *sc) {
    uint64_t irq = spin_lock(&screen_hold_lock);
    if (screen_holder == sc) screen_holder = NULL;
    wq_wake_all(&screen_watch_wq);
    spin_unlock(&screen_hold_lock, irq);
}

/* 画面を描く者（デスクトップ、無ければ文字の console）が全部を描き直す。 */
static void screen_repaint(void) {
    screen_display->repaint();
    fbcon_repaint();
}

/* 保持を一つ返す。保持の間は何も描かれていないので、最後の保持が終わると全部を描き直す。 */
static void screen_release(void) {
    if (fb_release()) screen_repaint();
}

/* ---- 出力 ----
   ドライバーは出力（モニター）を持つ。出力のモニターが出せる mode を知らせ（n_screen_offer_modes、選ぶのは
   システム）、選ばれた mode の大きさの buffer を作って出力の画面へ渡す（n_screen_give）。画面はその大きさになり、
   画面の frame は出力が見せていない buffer へ描かれ、一つずつ flip として求められる（n_screen_flips）。flip が
   見えたら、ドライバーは表示の engine が測った垂直帰線と共に知らせる（n_screen_report_vblank）。ファームウェア
   の画面の画素がある装置の出力 0 は画面 0。ほかの出力は、その機能のドライバーが替わっても同じ画面を持つ
   （output_screens）。出力の画面は、ドライバーが buffer を返したときと、claim が終わって装置が止まった後に消える。
   画面 0 はファームウェアの framebuffer を見せ直すが、表示の engine がもう見せていないかもしれない：去った
   ドライバーが設定を替えており、戻す者はいない。 */

static void screen_image_free(struct screen_image *i) {
    fb_image_remove(i->out);
    fb_buffer_unmap(i->out);
    shared_scanout_close(&i->surface);
    kfree(i);
}

/* 出力 o の image を全部：buffer が無くなったか、N_SCANOUT_DIRECT でなくなったとき。screen_lock の下。 */
static void screen_images_end(struct screen_output *o) {
    for (struct screen_image **at = &screen_images; *at;) {
        struct screen_image *i = *at;
        if (i->output != o) {
            at = &i->next;
            continue;
        }
        *at = i->next;
        screen_image_free(i);
    }
}

/* 装置 d の出力 output の画面：ファームウェアの画面がある装置の出力 0 は画面 0、ほかは前にあったもの、無ければ新しい
   画面。screen_lock の下。メモリーが無ければ NULL。 */
static struct screen *output_screen_of(const struct hal_device *d, uint32_t output) {
    if (output == 0 && screen_driver_boot_slot(d) >= 0) return screen_get(0);
    for (size_t i = 0; i < noutput_screens; i++) {
        struct output_screen *e = &output_screens[i];
        if (e->device == hal_id(d) && e->output == output) return e->screen;
    }
    struct screen *s = screen_new();
    if (!s) return NULL;
    struct output_screen *e = kappend(&output_screens, &noutput_screens, &output_screens_room, sizeof *e);
    if (!e) return NULL; /* 画面は残る。消えている */
    *e = (struct output_screen){hal_id(d), output, s};
    return s;
}

/* claim の出力。make なら無ければ作る（NULL：無い、またはメモリーが無い）。screen_lock の下。 */
static struct screen_output *output_of(struct screen_claim *sc, uint32_t output, bool make) {
    for (size_t i = 0; i < sc->noutputs; i++)
        if (sc->outputs[i]->output == output) return sc->outputs[i];
    if (!make) return NULL;
    struct screen *s = output_screen_of(hal_claim_device(sc->claim), output);
    struct screen_output *o = s ? kzalloc(sizeof *o) : NULL;
    if (!o) return NULL;
    struct screen_output **at = kappend(&sc->outputs, &sc->noutputs, &sc->outputs_room, sizeof *at);
    if (!at) {
        kfree(o);
        return NULL;
    }
    *o = (struct screen_output){.claim = sc, .output = output, .screen = s};
    *at = o;
    return o;
}

/* claim が今 buffer を渡している画面があるか。screen_lock の下。 */
static bool claim_shows(const struct screen_claim *sc) {
    for (size_t i = 0; i < sc->noutputs; i++)
        if (sc->outputs[i]->count) return true;
    return false;
}

/* 画面の frame が flip を求める（dev/fb.h）：claim の出力の枠へ入れる。ドライバーがまだ取っていない flip は捨てる。
   合成の thread で走る。枠はある（n_screen_give が、buffer を持つ出力の数だけ作った）。 */
static void output_flip(struct screen *s, void *owner, unsigned buffer, uint64_t serial) {
    (void)s;
    struct screen_output *o = owner;
    struct screen_claim *sc = o->claim;
    struct n_screen_flip f = {o->output, buffer, serial};
    uint64_t irq = spin_lock(&sc->flip_lock);
    size_t i = 0;
    while (i < sc->nflips && sc->flips[i].output != o->output) i++;
    if (i == sc->nflips && sc->nflips < sc->flips_room) sc->nflips++;
    if (i < sc->nflips) sc->flips[i] = f;
    spin_unlock(&sc->flip_lock, irq);
    wq_wake_all(&sc->flip_wq);
}
static const struct screen_ops output_ops = {.flip = output_flip};

/* 出力の buffer を画面へ渡す（count 0：返す。画面は消え、画面 0 はファームウェアの画面に戻る）。古い buffer は
   frame が書かなくなってから外し、unmap し、surface を閉じる。古い buffer へのまだ取られていない flip は消える。
   古い buffer の最後の flip の serial を返す（dev/fb.h）。screen_lock の下。 */
static uint64_t output_buffers(struct screen_output *o, const struct screen_mode_desc *desc,
                               struct shared_scanout *surface, struct scanout **buf, unsigned count) {
    struct shared_scanout old_surface[SCREEN_BUFFERS_MAX];
    struct scanout *old_buf[SCREEN_BUFFERS_MAX];
    unsigned old = o->count;
    memcpy(old_surface, o->surface, sizeof old_surface);
    memcpy(old_buf, o->buf, sizeof old_buf);
    memset(o->surface, 0, sizeof o->surface);
    memset(o->buf, 0, sizeof o->buf);
    for (unsigned i = 0; i < count; i++) {
        o->surface[i] = surface[i];
        o->buf[i] = buf[i];
    }
    o->count = count;
    o->direct = count && desc->direct;
    if (!o->direct) screen_images_end(o); /* buffer の代わりに見せるものはもう無い */
    uint64_t last = screen_set_buffers(o->screen, &output_ops, o, desc, buf, count, 0);
    struct screen_claim *sc = o->claim;
    uint64_t irq = spin_lock(&sc->flip_lock);
    for (size_t i = 0; i < sc->nflips; i++)
        if (sc->flips[i].output == o->output && sc->flips[i].serial <= last) {
            sc->flips[i] = sc->flips[--sc->nflips];
            break;
        }
    spin_unlock(&sc->flip_lock, irq);
    for (unsigned i = 0; i < old; i++) {
        fb_buffer_unmap(old_buf[i]);
        shared_scanout_close(&old_surface[i]);
    }
    screen_display->direct(screen_id(o->screen), o->direct);
    return last;
}

/* buffer を持つ出力の数だけ flips に枠がある。 */
static bool flips_room(struct screen_claim *sc) {
    size_t want = sc->noutputs;
    if (want <= sc->flips_room) return true;
    struct n_screen_flip *room = kmalloc(want * sizeof *room);
    if (!room) return false;
    uint64_t irq = spin_lock(&sc->flip_lock);
    memcpy(room, sc->flips, sc->nflips * sizeof *room);
    struct n_screen_flip *old = sc->flips;
    sc->flips = room;
    sc->flips_room = want;
    spin_unlock(&sc->flip_lock, irq);
    kfree(old);
    return true;
}

/* claim の出力を全部返し、mode を引っ込める。claim が終わった。screen_lock の下。画面へ buffer を渡していたなら true。 */
static bool outputs_end(struct screen_claim *sc) {
    bool shown = false;
    for (size_t i = 0; i < sc->noutputs; i++) {
        struct screen_output *o = sc->outputs[i];
        if (o->count) {
            shown = true;
            output_buffers(o, NULL, NULL, NULL, 0);
            screen_say(sc, o->output,
                       "its driver ended while a screen was drawn into its buffers; the screen is off (screen 0 the "
                       "boot screen again, which the display may not show: nothing sets the display engine back)");
        }
        if (o->modes) screen_display->modes_gone(screen_id(o->screen));
        kfree(o);
    }
    kfree(sc->outputs);
    sc->outputs = NULL;
    sc->noutputs = sc->outputs_room = 0;
    uint64_t irq = spin_lock(&sc->flip_lock);
    struct n_screen_flip *flips = sc->flips;
    sc->flips = NULL;
    sc->nflips = sc->flips_room = 0;
    sc->flips_closed = true;
    spin_unlock(&sc->flip_lock, irq);
    kfree(flips);
    wq_wake_all(&sc->flip_wq);
    return shown;
}

/* claim が終わった（hal/claim.c：装置は止まり、写像は外れている）。保持を返し、出力を返す。flips を待つ者がいれば
   記録はその最後の一人が返す。 */
static void claim_ended(struct hal_claim_end *e, struct hal_claim *c) {
    (void)c;
    struct screen_claim *sc = CLAIM_OF_END(e);
    mutex_lock(&screen_lock);
    for (struct screen_claim **at = &screen_claims; *at; at = &(*at)->next)
        if (*at == sc) {
            *at = sc->next;
            break;
        }
    sc->ended = true;
    bool given_back = outputs_end(sc);
    unsigned holds = sc->holds;
    sc->holds = 0;
    bool free_now = !sc->users;
    mutex_unlock(&screen_lock);
    /* 装置はもう止まっている：保持を返してよい。画面の描き直しは最後の保持が終わってからだけ。 */
    if (holds) screen_hold_end(sc);
    if (!holds && given_back) screen_repaint();
    for (; holds; holds--) screen_release();
    if (free_now) kfree(sc);
}

/* claim の記録。make なら無ければ作る。NULL は claim が終わった（-N_ERR_CLOSED）か、メモリーが無い（*error）。
   screen_lock の下。 */
static struct screen_claim *screen_claim_of(struct hal_claim *c, bool make, int64_t *error) {
    for (struct screen_claim *sc = screen_claims; sc; sc = sc->next)
        if (sc->claim == c) return sc;
    if (hal_claim_revoked(c) || !make) {
        *error = hal_claim_revoked(c) ? -N_ERR_CLOSED : -N_ERR_DENIED;
        return NULL;
    }
    struct screen_claim *sc = kzalloc(sizeof *sc);
    if (!sc) {
        *error = -N_ERR_NO_MEMORY;
        return NULL;
    }
    sc->claim = c;
    sc->end.ended = claim_ended;
    if (!hal_claim_on_end(c, &sc->end)) {
        kfree(sc);
        *error = -N_ERR_CLOSED;
        return NULL;
    }
    sc->next = screen_claims;
    screen_claims = sc;
    return sc;
}

static const char *format_name(uint32_t format) {
    format &= N_SCANOUT_FORMAT_MASK;
    return format == N_SCANOUT_XRGB2101010 ? "XRGB2101010" : format == N_SCANOUT_XBGR8888 ? "XBGR8888" : "XRGB8888";
}

/* ---- 呼出し：h は呼ぶ側が持つ claim の handle ---- */

int64_t screen_driver_give(struct handle *h, uint32_t output, const struct n_screen_buffers *b) {
    struct hal_claim *c = hal_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (b && (!b->count || b->count > SCREEN_BUFFERS_MAX)) return -N_ERR_INVALID;
    uint64_t bytes = 0;
    int64_t r = 0;
    for (uint32_t i = 0; b && i < b->count && !r; i++)
        r = fb_buffer_check(b->width, b->height, b->pitch, b->format, b->buffer[i].offset, &bytes);
    if (r) return r;
    /* surface は screen_lock より先：handle を引くと眠ることがある */
    struct handle *surfaces[SCREEN_BUFFERS_MAX] = {0};
    for (uint32_t i = 0; b && i < b->count && !r; i++)
        if (!(surfaces[i] = handle_get(b->buffer[i].surface))) r = -N_ERR_NO_HANDLE;
    struct shared_scanout surface[SCREEN_BUFFERS_MAX] = {0};
    struct scanout *buf[SCREEN_BUFFERS_MAX] = {0};
    for (uint32_t i = 0; b && i < b->count && !r; i++) {
        r = shared_scanout_open(surfaces[i], c, b->buffer[i].offset, bytes, &surface[i]);
        if (!r && !(buf[i] = fb_buffer_map(surface[i].frames, surface[i].pages, surface[i].type, surface[i].first_byte,
                                           b->width, b->height, b->pitch, b->format)))
            r = -N_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < SCREEN_BUFFERS_MAX; i++) handle_close(surfaces[i]);
    mutex_lock(&screen_lock);
    struct screen_claim *sc = r ? NULL : screen_claim_of(c, b != NULL, &r);
    struct screen_output *o = sc ? output_of(sc, output, b != NULL) : NULL;
    if (!r && b && (!o || !flips_room(sc))) r = -N_ERR_NO_MEMORY;
    if (!b && r == -N_ERR_DENIED) r = -N_ERR_INVALID; /* 返す画面が無い */
    if (!r && !b && (!o || !o->count)) r = -N_ERR_INVALID;
    if (!r) {
        struct screen_mode_desc desc = {0};
        if (b) {
            desc = (struct screen_mode_desc){.width = b->width, .height = b->height,
                                             .direct = (b->format & N_SCANOUT_DIRECT) != 0};
            screen_display->modes_shown(screen_id(o->screen), &desc);
        }
        uint64_t last = output_buffers(o, &desc, surface, buf, b ? b->count : 0);
        r = (int64_t)last;
        char line[200];
        if (b)
            ksnprintf(line, sizeof line, "screen %u is drawn into its driver's %u %ux%u %s buffer%s (pitch %u)%s",
                      screen_id(o->screen), b->count, b->width, b->height, format_name(b->format),
                      b->count > 1 ? "s" : "", b->pitch, desc.direct ? ", which can show fullscreen windows directly" : "");
        else
            ksnprintf(line, sizeof line, "its driver gives screen %u back", screen_id(o->screen));
        screen_say(sc, output, line);
    }
    mutex_unlock(&screen_lock);
    if (r < 0)
        for (uint32_t i = 0; b && i < b->count; i++) {
            fb_buffer_unmap(buf[i]);
            shared_scanout_close(&surface[i]);
        }
    return r;
}

int64_t screen_driver_flips(struct handle *h, struct n_screen_flip *out, size_t max, uint64_t deadline) {
    struct hal_claim *c = hal_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (!max) return -N_ERR_INVALID;
    int64_t r = 0;
    mutex_lock(&screen_lock);
    struct screen_claim *sc = screen_claim_of(c, true, &r);
    if (sc) sc->users++;
    mutex_unlock(&screen_lock);
    if (!sc) return r;
    uint64_t irq = spin_lock(&sc->flip_lock);
    for (;;) {
        if (sc->nflips) {
            size_t n = MIN(max, sc->nflips);
            memcpy(out, sc->flips, n * sizeof *out);
            memmove(sc->flips, sc->flips + n, (sc->nflips - n) * sizeof *out);
            sc->nflips -= n;
            r = (int64_t)n;
            break;
        }
        if (sc->flips_closed) r = -N_ERR_CLOSED;
        else if (thread_current()->killed) r = -N_ERR_STOPPED;
        else if (clock_now_ns() >= deadline) r = 0;
        else r = 1;
        if (r <= 0) break;
        /* output_flip と outputs_end は flip_lock を取って枠を変え、それから flip_wq を起こす */
        wq_wait_locked_until(&sc->flip_wq, &sc->flip_lock, deadline);
        spin_take(&sc->flip_lock);
    }
    spin_unlock(&sc->flip_lock, irq);
    mutex_lock(&screen_lock);
    bool last = !--sc->users && sc->ended;
    mutex_unlock(&screen_lock);
    if (last) kfree(sc);
    return r;
}

int64_t screen_driver_vblank(struct handle *h, const struct n_screen_vblank *v) {
    struct hal_claim *c = hal_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (v->buffer >= SCREEN_BUFFERS_MAX) return -N_ERR_INVALID;
    int64_t error = 0;
    mutex_lock(&screen_lock);
    struct screen_claim *sc = screen_claim_of(c, false, &error);
    struct screen_output *o = sc ? output_of(sc, v->output, false) : NULL;
    struct screen *s = o && o->count ? o->screen : NULL;
    mutex_unlock(&screen_lock);
    if (!s) return -N_ERR_DENIED;
    /* 画面は claim より長く生きる（画面は消えない）：出力の buffer が去る頃に報告された帰線は、もう待っていない
       画面のものである */
    if (v->flip_serial) screen_flipped(s, v->buffer, v->flip_serial, v->time_ns);
    screen_vblank(s, v->count, v->time_ns);
    return 0;
}

int64_t screen_driver_state(struct handle *h, uint32_t output, uint64_t seen, struct n_screen_state *out,
                            uint64_t deadline) {
    struct hal_claim *c = hal_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    int64_t error = 0;
    mutex_lock(&screen_lock);
    struct screen_claim *sc = screen_claim_of(c, false, &error);
    struct screen_output *o = sc ? output_of(sc, output, false) : NULL;
    int64_t screen = o && (o->count || o->modes) ? (int64_t)screen_id(o->screen) : -1;
    mutex_unlock(&screen_lock);
    if (screen < 0) return -N_ERR_DENIED;
    return screen_display->state((unsigned)screen, seen, out, deadline);
}

int64_t screen_driver_modes(struct handle *h, uint32_t output, const struct n_screen_modes *m) {
    struct hal_claim *c = hal_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    if (m->count > N_SCREEN_MODES_MAX) return -N_ERR_INVALID;
    for (uint32_t i = 0; i < m->count; i++)
        if (!m->modes[i].width || !m->modes[i].height || m->modes[i].width > UINT16_MAX ||
            m->modes[i].height > UINT16_MAX || !m->modes[i].refresh_mhz)
            return -N_ERR_INVALID;
    int64_t r = 0;
    mutex_lock(&screen_lock);
    struct screen_claim *sc = screen_claim_of(c, m->count != 0, &r);
    struct screen_output *o = sc ? output_of(sc, output, m->count != 0) : NULL;
    if (sc && !o) r = m->count ? -N_ERR_NO_MEMORY : 0;
    else if (!sc) r = m->count ? r : 0;
    else {
        r = 0;
        o->modes = m->count != 0;
        screen_display->modes_offer(screen_id(o->screen), m);
    }
    mutex_unlock(&screen_lock);
    return r;
}

int64_t screen_driver_image(struct handle *h, uint32_t output, struct handle *surface, const struct n_scanout *desc) {
    struct hal_claim *c = hal_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    uint64_t bytes = 0;
    int64_t r = desc->format & N_SCANOUT_DIRECT
                    ? -N_ERR_INVALID
                    : fb_buffer_check(desc->width, desc->height, desc->pitch, desc->format, desc->offset, &bytes);
    if (r) return r;
    struct screen_image *i = kzalloc(sizeof *i);
    if (!i) return -N_ERR_NO_MEMORY;
    mutex_lock(&screen_lock);
    struct screen_claim *sc = screen_claim_of(c, false, &r);
    struct screen_output *o = sc ? output_of(sc, output, false) : NULL;
    if (!o || !o->direct) r = -N_ERR_DENIED;
    else r = 0;
    if (!r) r = shared_scanout_open(surface, c, desc->offset, bytes, &i->surface);
    if (!r && !(i->out = fb_buffer_map(i->surface.frames, i->surface.pages, i->surface.type, i->surface.first_byte,
                                       desc->width, desc->height, desc->pitch, desc->format))) {
        shared_scanout_close(&i->surface);
        r = -N_ERR_NO_MEMORY;
    }
    if (!r) {
        fb_image_add(i->out);
        i->output = o;
        i->number = r = ++screen_image_last;
        i->next = screen_images;
        screen_images = i;
    }
    mutex_unlock(&screen_lock);
    if (r < 0) kfree(i);
    return r;
}

int64_t screen_driver_image_end(struct handle *h, int64_t number) {
    struct hal_claim *c = hal_claim_of(h);
    if (!c) return -N_ERR_WRONG_KIND;
    int64_t r = -N_ERR_INVALID;
    mutex_lock(&screen_lock);
    for (struct screen_image **at = &screen_images; *at; at = &(*at)->next)
        if ((*at)->number == number && (*at)->output->claim->claim == c) {
            struct screen_image *i = *at;
            *at = i->next;
            screen_image_free(i);
            r = 0;
            break;
        }
    mutex_unlock(&screen_lock);
    return r;
}

/* claim の thread を screen_lock の下で一度に一つ：数と、それが始め、終える保持が一緒に変わる。最後の保持の後の
   描き直しは、その後に来る。 */
int64_t screen_driver_hold(struct hal_claim *c, bool hold) {
    int64_t r = 0;
    mutex_lock(&screen_lock);
    struct screen_claim *sc = screen_claim_of(c, hold, &r);
    if (!sc) {
        if (!hold && r == -N_ERR_DENIED) r = -N_ERR_INVALID; /* 返す保持が無い */
    } else if (screen_driver_boot_slot(hal_claim_device(c)) < 0 && !claim_shows(sc)) {
        r = -N_ERR_NOT_FOUND;
    } else if (hold) {
        if (sc->holds == UINT32_MAX) r = -N_ERR_BUSY;
        else {
            if (!sc->holds++) screen_hold_begin(sc);
            fb_hold();
        }
    } else if (!sc->holds) {
        r = -N_ERR_INVALID;
    } else if (!--sc->holds) {
        screen_hold_end(sc);
    }
    mutex_unlock(&screen_lock);
    if (!hold && !r) screen_release();
    return r;
}
