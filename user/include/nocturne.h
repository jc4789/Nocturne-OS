/* Nocturne OS system interface. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "abi.h"
#include "audio_abi.h"
#include "settings_abi.h"
#include "hal_buses.h"
#include "gfx.h"
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define ABS(a) ((a) < 0 ? -(a) : (a))

/* ---- native ABI (docs/nocturne-native-abi.md) ----
   These return a negative n_result on failure and never touch errno. */
typedef int32_t n_handle;
typedef int64_t n_result;

/* malloc's heap: bytes in its n_memory regions, and bytes in allocated blocks. */
void malloc_usage(size_t *heap, size_t *used);
/* Native memory (docs/nocturne-native-abi.md §7): zero-filled pages with
 * N_MEMORY_READ, _READ_WRITE or _READ_EXECUTE access; reserve also takes
 * N_MEMORY_LOW added to the access, for pages below 2 GiB. reserve puts the
 * pages in *memory (NULL on failure). protect on a shared mapping keeps within
 * one mapping and its access (never execution), and holds for its pages not
 * mapped yet too. */
n_result n_memory_reserve(size_t bytes, int access, void **memory);
n_result n_memory_release(void *addr, size_t bytes);
n_result n_memory_protect(void *addr, size_t bytes, int access);
/* A driver image (abi.h) from a document opened to read, into this process. Nothing of it runs yet. */
n_result n_image_map(n_handle document, struct n_image *out);
/* Shared pages: a handle is a private capability that travels to another app
 * only as a channel attachment (n_channel_send). A mapping keeps the pages
 * after the handle closes; n_memory_release of the whole mapping ends it. */
n_handle n_shared_create(size_t bytes);
/* Like n_shared_create, but pages are zero-filled at first touch by any mapper. */
n_handle n_shared_reserve(size_t bytes);
/* Map all of the pages (bytes is their page-rounded size) with N_MEMORY_READ
 * or N_MEMORY_READ_WRITE access into *mapping (NULL on failure). */
n_result n_shared_map(n_handle shared, size_t bytes, int access, void **mapping);

const char *n_result_text(n_result r); /* either sign */
n_result n_handle_close(n_handle h);   /* commits an N_DOCUMENT_REPLACE handle */
n_result n_handle_kind(n_handle h);    /* N_KIND_* */
/* Documents and folders: paths are complete, "C:\a\b" ("\a\b" is the same, and '/' works as '\').
   C: is the only drive. A name is 1 to 255 bytes of UTF-8 without control characters,
   \ / : * ? " < > |, or a space or period at its end; names compare without regard to case. */
n_handle n_document_open(const char *path, int access); /* N_DOCUMENT_* */
n_result n_document_read_at(n_handle h, uint64_t offset, void *buf, size_t n); /* bytes; 0 at the end */
n_result n_document_write_at(n_handle h, uint64_t offset, const void *buf, size_t n);
n_result n_document_size(n_handle h);
/* The open document's entry as n_entry_info gives it, with no name and the size this handle reads. */
n_result n_document_info(n_handle h, struct n_entry *out);
n_result n_document_set_size(n_handle h, uint64_t size); /* new bytes read as zero */
n_result n_document_claim(n_handle h, bool wait);        /* N_ERR_NOT_READY when not waiting */
n_result n_document_release(n_handle h);
n_result n_document_commit(n_handle h, int how);         /* N_COMMIT_*: save it, now or in the background */
n_result n_document_save(const char *path, const void *data, size_t size); /* whole and atomic */
n_result n_document_append(const char *path, const void *data, size_t size);
/* The whole document in fresh n_memory pages; free with n_memory_release(*data, *size). */
n_result n_document_load(const char *path, void **data, size_t *size);
n_result n_entry_info(const char *path, struct n_entry *out);
n_result n_entry_remove(const char *path);
/* パスの持ち主の利用者を変える。N_CLASS_ADMIN 以上（§3）。 */
n_result n_entry_set_owner(const char *path, uint32_t user);
n_result n_entry_move(const char *from, const char *to); /* never replaces: N_ERR_EXISTS */
/* n_entry_move that lets document from take the name of a document, which goes in the same commit
   (as n_entry_remove takes it: its open handles keep it). Any other entry there: N_ERR_EXISTS. */
n_result n_entry_replace(const char *from, const char *to);
/* A watch on C: (n_watch_take). N_WATCH_NAME watches the last name of path in its folder, whether
   an entry has it or not; N_WATCH_FOLDER every entry directly in the folder path names (not what
   is inside its folders). A change counts once a new opening, n_entry_info or n_folder_list sees
   it: an entry made, removed, moved in or out, renamed (case too) or replaced (n_entry_replace), or
   a document's written content taken in (a save, its last writer's close, the system's writer).
   The folder is held by what it is: moved, it is still watched; removed, the watch turns
   N_EVENT_CLOSED. Entries not there for the caller (PRIVATE) count nothing. N_ERR_NOT_FOUND
   without the folder, N_ERR_INVALID for another what. */
n_handle n_entry_watch(const char *path, uint32_t what);
n_result n_folder_create(const char *path);
n_result n_folder_list(const char *path, struct n_entry *out, size_t max, uint64_t start); /* count */
/* Every entry of a folder in a new array (free it); returns the count. */
n_result n_folder_entries(const char *path, struct n_entry **out);
/* Message channels: one send is one message. attach is a document or shared
   handle (or -1) that travels with it and arrives as a new handle in *attached. */
n_result n_channel_pair(n_handle out[2]);
n_result n_channel_send(n_handle h, const void *data, size_t n, n_handle attach);
/* N_CHANNEL_WAIT waits; N_CHANNEL_PEEK only reports the next message's size;
   N_CHANNEL_DISCARD copies at most cap bytes of it, then drops it.
   A message larger than cap stays queued and gives N_ERR_TOO_LARGE. */
n_result n_channel_receive(n_handle h, void *buf, size_t cap, int flags, n_handle *attached);
/* Message channels only. Kernel-authenticated send-time app ID; all threads
   of one app agree, including across connections or transferred endpoints.
   message is required. Peek reports credentials without taking an attachment. */
n_result n_channel_receive_from(n_handle h, void *buf, size_t cap, int flags, struct n_channel_message *message);
/* 身元と能力（docs/nocturne-native-abi.md §2）。 */
n_result n_identity_self(struct n_identity *out);
/* channel のもう一端の、カーネルが証明した身元。 */
n_result n_channel_peer(n_handle channel, struct n_identity *out);
/* cap から kind の能力を切り出す。根は全部を、ほかの能力は自分と同じ種類だけを。 */
n_handle n_cap_derive(n_handle cap, enum n_cap kind);
n_result n_cap_kind(n_handle cap);
/* 自分が持つ kind の能力の handle。無ければ N_ERR_NOT_FOUND。 */
n_handle n_cap_find(enum n_cap kind);
/* 自分が持つ能力の handle を out（N_CAP_COUNT 個の場所）へ並べる：その数。子へ渡す列を作るとき。 */
size_t n_caps_list(int32_t *out);
/* カーネルの窓の系が起こすアプリの身元を決める。 */

/* 受け口：各メッセージが新しい channel の端を一つ運ぶ channel。connect は組を作って一方の端を
   受け口へ送り、もう一方を返す。accept は次の端を受ける（flags は 0 か N_CHANNEL_WAIT、無ければ
   N_ERR_NOT_READY、送る側が全部閉じれば N_ERR_CLOSED）。受け口を受ける側が閉じると待つ端も閉じる。 */
n_handle n_inlet_connect(n_handle inlet);
n_handle n_inlet_accept(n_handle inlet, int flags);

/* サービス：名前はサービスの管理者が決める（カーネルは覚えない）。connect は名前のサービスへの
   channel を返す。listen は名前を管理者に申し込み、受け口を返す。受け口を全部閉じると名前は消える。
   register は管理者だけ：接続の要求（struct n_service_request）を受ける channel を返す。 */
n_handle n_service_connect(const char *name);
n_handle n_service_listen(const char *name);
n_handle n_service_register(n_handle service_root);
/* Apps: the handle reports the end (N_EVENT_APP_ENDED); there is no zombie to reap. */
n_handle n_app_launch(const char *path, const struct n_launch *launch);
n_result n_app_result(n_handle app, uint64_t deadline, int *status); /* a past deadline polls: N_ERR_NOT_READY */
n_result n_app_stop(n_handle app, int flags);                         /* N_APP_STOP_TREE */
n_result n_app_self(void);
n_result n_app_parent(void);
n_handle n_app_open(int64_t id); /* a running app by the identifier the app list shows */
n_result n_system_log(char *buf, size_t cap); /* the newest system log text; its length（ADMIN 以上） */
/* A handle whose messages are added to the system log. 開いた者が ADMIN 以上なら log's text も受ける
   (from the oldest record the kernel holds at its first receive on)。 */
n_handle n_system_log_open(void);
n_result n_app_id(n_handle app); /* its identifier, also after its end (identifiers are not reused) */
/* アプリの中の装置ドライバー（docs/nocturne-native-abi.md §9）。n_hal_claim は N_CAP_HAL_CLAIM の能力で、台帳
   （n_hal_snapshot）の装置を一つ持つ。driver は台帳に見せる実装の名（claim を受けて動くプログラムの名）。持った
   handle（N_KIND_HAL_CLAIM）は、起こすドライバーのアプリへ渡す（サービスの管理者は N_SERVICE_HANDLE_DEVICE に渡す）。
   claim は最後の handle が閉じる（アプリの終わりを含む）か、装置が去ると終わる：写像と割り込みと提供が消え、バスが
   装置を止める。動いている実装がある装置は N_ERR_BUSY、プロセスの実装を許さない装置（PCI のブリッジ）は
   N_ERR_DENIED。IOMMU があれば機能の DMA は claim の DMA の object にしか届かず、無ければ n_hal_pci_info が
   N_PCI_ISOLATED を返さない。動いたまま終わった装置の回復は次のドライバーの仕事で、n_hal_pci_info が
   N_PCI_NEEDS_RECOVERY で知らせる。
   n_hal_claim_resources は装置の資源（struct n_hal_resource）を max 件まで out へ書き、資源の全部の数を返す。slot は
   バスが決める番号で、PCI は BAR の番号である。 */
n_handle n_hal_claim(n_handle cap, uint32_t device, const char *driver);
n_result n_hal_claim_resources(n_handle claim, struct n_hal_resource *out, size_t max);
/* メモリーの資源の slot を、装置のメモリーの上の共有 object にする（uncached。n_shared_view で write-combining の
   部分や小さい窓を作れる：資源が N_HAL_PREFETCHABLE のときだけ）。頁に揃わない資源は N_ERR_UNSUPPORTED。 */
n_handle n_hal_map(n_handle claim, uint32_t slot);
/* count 本までの割り込みを立て（バスが一番良い形を選ぶ：PCI は MSI-X、無ければ MSI。count は装置と機械が与える数へ
   切り下げる）、立てた数を返す。届いた割り込みは、n_hal_interrupt_take が返すまで bit を立てる。handle は bit が
   立っている間 N_EVENT_RECEIVABLE である。take は先頭の 64 * words 本のどれかが届くか deadline（N_CLOCK_NEVER：
   限りなし。N_ERR_TIMEOUT）まで待ち、その bit（n 番目は bits[n / 64] の bit n % 64）を書いて消し、書いた語の数を
   返す。後の割り込みの bit は残る。 */
n_result n_hal_interrupts(n_handle claim, uint32_t count);
n_result n_hal_interrupt_take(n_handle claim, uint64_t *bits, uint32_t words, uint64_t deadline);
/* バスだけの操作（op の番号と in、out の型は hal_buses.h のバスの節）。in は 128 KiB、out は 1 MiB まで。戻りは操作の
   結果（0 以上）か負の N_ERR_*。バスが持たない操作は N_ERR_UNSUPPORTED。 */
n_result n_hal_bus_call(n_handle claim, uint32_t op, const void *in, size_t in_size, void *out, size_t out_size);
/* 実装が装置で提供するもの（N_OFFER_*）を台帳に見せる（kind と instance の組は claim の中で一つ。N_ERR_EXISTS）。
   claim の終わりが全部引っ込める。 */
n_result n_hal_offer(n_handle claim, uint32_t kind, uint32_t instance, const char *label);
n_result n_hal_offer_withdraw(n_handle claim, uint32_t kind, uint32_t instance);
/* 眠りの知らせ。一度でも呼んだ claim は眠りに加わる：次の知らせ（N_HAL_POWER_SUSPEND か N_HAL_POWER_RESUME）を
   deadline まで待って返す。SUSPEND には装置を止めてから n_hal_power_done(claim, true) で答え、止められなければ
   false で答える（眠りは取りやめになる）。眠りでは答えた後に装置が D3 になる。RESUME の前に装置は D0 に戻り、バスの
   構成も戻っている（装置そのものは初めから始まっていることがある）。呼ばない claim を持つ装置がある間、機械は眠らない
   （docs/nocturne-native-abi.md §9）。 */
n_result n_hal_power_wait(n_handle claim, uint64_t deadline);
n_result n_hal_power_done(n_handle claim, bool ok);
/* 装置の ACPI の記述（firmware の書いたまま。どのバスの装置でも）：装置の節（N_ACPI_SELF）、その節の device の子
   （N_ACPI_CHILD(i)、名前空間の順）、木で上の装置の節（N_ACPI_PARENT。PCI の機能なら上のブリッジ、root bus なら
   ホストブリッジ）。
   n_hal_acpi_eval は、その節の直下の名前 name（1 区画の 4 文字：「_DSM」「_ADR」）を、nargs 個（N_ACPI_ARGS_MAX
   まで）の整数、文字列、バッファーの引数（中身は data、N_ACPI_DATA_MAX まで）で評価し、結果を out へ書く：
   struct n_acpi_value の記録、最初が結果、続いて文字列とバッファーのバイト。書いたバイト数を返す（N_ACPI_OUT_MAX
   まで）。N_ERR_DENIED（節があってもなくても）：システムが装置と機械を動かすのに使う method（電源とリセット _PS0
   から _PS3、_ON_、_OFF、_RST。取り外しとドッキング _EJ0 から _EJ4、_LCK、_DCK。資源 _DIS、_SRS。firmware が何を
   制御するかの交渉 _OSC、_OST。起こし _PSW、_DSW。初期化と handler _INI、_REG、_EVT。システムの眠り _PTS、_WAK、
   _TTS、_GTS、_BFS）、そして N_ACPI_PARENT では _ で始まらない全部の名前（上の装置の method は、その下の全部の
   装置と機械のものである）。N_ERR_NOT_FOUND：その節か object が無い。N_ERR_INVALID：最後の子より先の番号（節の
   無い装置に子は無い）、壊れた名前か引数。N_ERR_TOO_LARGE：out が小さい。N_ERR_IO：AML が失敗。
   N_ERR_UNSUPPORTED：AML なしで動いている。
   n_hal_acpi_notify_take は、装置の節への次の Notify の値を、最初の呼出し以後から返す（n_hal_interrupt_take と
   同じに deadline まで待つ。取られない 32 件を持ち、古いものが譲る。N_ERR_NOT_FOUND：装置に節が無い）。 */
n_result n_hal_acpi_eval(n_handle claim, uint32_t node, const char *name, const struct n_acpi_value *args,
                         uint32_t nargs, const void *data, size_t data_size, void *out, size_t out_size);
n_result n_hal_acpi_notify_take(n_handle claim, uint64_t deadline);
/* PCI の機能の claim だけの操作（N_PCI_CALL_*、hal_buses.h）。
   n_hal_pci_info：機能の身元、BAR、上のブリッジ、ファームウェアの画面の置き場所。
   構成空間：幅 1、2、4 で、幅に揃った 4096 未満の offset。読みは別の機能（N_PCI_TARGET。例：上のブリッジ）か
   N_PCI_SELF を名指しでき、非負の値を返す。書きは claim した機能にだけ届く。カーネルが握るレジスター（BAR、MSI、
   MSI-X）と、機能の外へ効くビット（リセット、電源の状態、経路）は書けない：N_ERR_DENIED。 */
n_result n_hal_pci_info(n_handle claim, struct n_hal_pci_info *out);
n_result n_hal_pci_config_read(n_handle claim, uint64_t target, uint32_t offset, uint32_t width);
n_result n_hal_pci_config_write(n_handle claim, uint32_t offset, uint32_t width, uint32_t value);
/* 画面のドライバー（どのバスの claim でもよい）：出力（モニター）の buffer を画面へ渡す。output はドライバーの
   コネクター、head、表示 engine の pipe の番号である。ファームウェアの画面の画素がある装置の出力 0 が画面 0
   （タスクバーのある画面）で、ほかの出力は画面を一つずつ持ち、その装置のドライバーが替わっても同じ画面である。
   n_screen_give：出力が画面を見せる buffer（struct n_screen_buffers）。返った後、画面はそこへ描かれ、その大きさに
   なり、画面 0 は、ドライバーが返す（buffers が NULL）か claim が終わる（落ちも含む）まで、ブート画面へは描かれ
   ない（claim の終わりは先に装置を止める）。そのとき画面は消え、画面 0 はブート画面のメモリーへ描かれるが、表示
   engine はもうそれを見せていないかもしれない。buffer はそれぞれ、この claim で作った書込み可能な共有 object：
   BAR など prefetchable な装置のメモリー（write-combining で描くので、ドライバーが写す view も write-combining
   でなければならない）か、型が write-back でない DMA の object（write-combining か uncached の view がその型にする）、
   またはそれらの view。大きさは、表示 engine が見せる mode のもの（ドライバーが mode を知らせた後は、システムが選んだ
   mode、struct n_screen_state の mode。各辺 65535 まで）で、その画面で全画面か最大化の窓はそれに従う。出力の前の
   buffer へ描くものが無くなってから返り、新しい buffer は全部描かれ、その最初のものが見えている。frame は出力が
   見せていない buffer にだけ描かれ（buffer が一つならその一つへ）、flip として求められる。出力の前の最後の flip の
   serial（0：無い）を返す：それ以下の flip は前の buffer のもので、まだ取られていないものは消える。
   N_ERR_DENIED：ほかの claim の object か、buffer が持てない型。N_ERR_INVALID：count が 0 か N_SCREEN_BUFFERS_MAX
   より多い、大きさが 0 か 65535 より大きい、知らない format、object の外の範囲、返すものが無い。 */
n_result n_screen_give(n_handle claim, uint32_t output, const struct n_screen_buffers *buffers);
/* ドライバーが、claim した装置のメモリー（PCI なら n_hal_pci_info が framebuffer_bar で示す BAR）の中のブート画面への
   CPU の届き方を変える間、システムはそこへ書いてはならない。hold true は書込みが無くなってから返り、画面に触れずに
   保つ。保持は入れ子になり、最後の false（か claim の終わり）が framebuffer の描画を再開する（GPU の走査ではない）。
   ドライバーの落ちやリセットの後の回復は保証しない。時間で保持の上に描くことはしない：それが防ぐ競合そのものである。
   claim の終わりは、CPU の描画が戻る前に装置を止める（写像、割り込み、バスマスター）。起動の引数 fbhold=<秒>（既定は
   無し）の時間続いた保持は、ドライバーのアプリを止め、その claim の終わりが保持を返す。n_screen_give で画面 0 が
   描かれているドライバーも、同じように保持する。N_ERR_NOT_FOUND：ブート画面はほかにあり、画面 0 はこの claim の
   buffer に描かれていない。N_ERR_INVALID：返す保持が無い。 */
n_result n_screen_hold(n_handle claim, bool hold);
/* claim の出力の画面が求める flip（struct n_screen_flip）を out へ最大 max 件、deadline（N_CLOCK_NEVER：限りなし）
   まで待って返す：件数、期限なら 0。まだ取られていない flip は出力の次の flip に替わる。ドライバーは flip の
   buffer を次の垂直帰線から見せ、n_screen_report_vblank で知らせる。 */
n_result n_screen_flips(n_handle claim, struct n_screen_flip *out, size_t max, uint64_t deadline);
/* 画面が buffer に描かれている出力の垂直帰線（struct n_screen_vblank）：表示 engine が測った時刻（システムの
   monotonic の時計）と、それが見せる flip。画面の次の frame は flip の帰線を待つ。帰線を知らせない画面は、
   flip を求められた通りに見せる。N_ERR_DENIED：出力はこの claim の buffer を見せていない。 */
n_result n_screen_report_vblank(n_handle claim, const struct n_screen_vblank *vblank);
/* ドライバーが出力に繋いだモニターと、そこで見せられる mode（struct n_screen_modes）をシステムに知らせる。見せる
   mode は表示のサービスが選ぶ（設定の試し、無ければ利用者のそのモニターの選び、無ければモニターの preferred、無ければ
   最高の refresh の最大）ので、struct n_screen_state の mode が言い、n_screen_read_state が読む。ドライバーはその
   mode を見せ（n_screen_give をその大きさで）、done に state の mode_serial を入れ、見せている mode を書いて、
   mode をもう一度知らせる：望みの mode でなければ、システムはそのモニターでそれを二度と求めず、別のものを選ぶ。
   done 0（ドライバーの始め、モニターの変化か接続のとき）の知らせは、同じ mode でも選び直す。count 0：ドライバーは
   その出力にもうモニターを繋いでいない（claim の終わりも同じ）。N_ERR_INVALID：N_SCREEN_MODES_MAX より多い mode、
   大きさか refresh が 0 の mode、65535 より大きい大きさ。 */
n_result n_screen_offer_modes(n_handle claim, uint32_t output, const struct n_screen_modes *modes);
/* 出力の画面が buffer に描かれているか、システムがその出力の mode を持っているドライバーのために：画面の状態が
   seen の番号のままの間 deadline（N_CLOCK_NEVER：限りなし）まで待ち（0 は番号にならない）、out へ書く
   （struct n_screen_state が見せる mode と、N_SCANOUT_DIRECT なら、アプリの窓を直接見せる間にドライバーが見せ、
   描くものを言う）。N_ERR_TIMEOUT：out は seen の番号の状態。N_ERR_DENIED：出力はこの claim の buffer を見せて
   おらず、mode も知らせていない。 */
n_result n_screen_read_state(n_handle claim, uint32_t output, uint64_t seen, struct n_screen_state *out,
                             uint64_t deadline);
/* そのドライバーのために：出力の表示 engine が buffer の代わりに見せてよい surface（アプリの image を直接見せる）。
   buffer と同じに渡す（format は N_SCANOUT_DIRECT なし。N_SCANOUT_XBGR8888 も）、自分の大きさで（engine が mode の
   大きさへ拡縮してよい）。システムがそこへ描くのは一度だけ：panic の画面で、buffer とそういう surface 全部へ
   その大きさで描かれる：engine は最後に flip された物を見せ続け、ドライバーはもう動かないから。surface の番号
   （正）を返す。n_screen_image_end はそれを、ドライバーがメモリーを解く前に手放す。N_SCANOUT_DIRECT でない
   buffer、返された buffer、claim の終わりは、全部を手放す。N_ERR_DENIED：出力はこの claim の N_SCANOUT_DIRECT の
   buffer を見せていない、または n_screen_give が受けない object。N_ERR_INVALID：大きさが 0 か 65535 より大きい、
   知らない format、object の外の範囲、（end で）そういう番号が無い。 */
n_result n_screen_image_add(n_handle claim, uint32_t output, n_handle surface, const struct n_scanout *desc);
n_result n_screen_image_end(n_handle claim, n_result image);
/* Zeroed RAM that a claimed device may read and write by DMA: a shared object (N_DMA_*
   flags), held while any handle or mapping holds it. N_ERR_UNSUPPORTED: its bus lets no device reach memory.
   n_dma_addresses gives the bus address of each page from first_page (count of them; returns how many it wrote). */
n_handle n_dma_create(n_handle claim, size_t bytes, uint32_t flags);
n_result n_dma_addresses(n_handle shared, size_t first_page, uint64_t *out, size_t count);
/* A new shared object over part of another (page-aligned offset and bytes) with the given
   caching (N_CACHE_*): mapping it maps the same pages. */
n_handle n_shared_view(n_handle shared, size_t offset, size_t bytes, uint32_t cache);
/* A view with an immutable maximum access (N_MEMORY_READ or READ_WRITE).
   Derived views and transferred handles cannot enlarge this access. */
n_handle n_shared_view_access(n_handle shared, size_t offset, size_t bytes, uint32_t cache, uint32_t access);
/* One shared object over ordered slices, retaining their backing, cache types
   and cancellation dependencies. READ/READ_WRITE cannot exceed any source's
   maximum rights. Source handles may close after success. No pages are copied. */
n_handle n_shared_compose(const struct n_shared_range *ranges, size_t count, uint32_t access);
/* Only the app which issued a view may revoke it. Existing mappings and all
   derived views lose access; the underlying object and independent views remain alive. */
n_result n_shared_revoke(n_handle view);
/* Paged memory (docs/nocturne-native-abi.md §7): `bytes` of shared memory (in *memory, an
   ordinary N_KIND_SHARED handle to map or to send) whose pages the creating app, its pager,
   supplies through the returned N_KIND_PAGER handle, which no other app can use or receive.
   A touch of a page without the access it needs waits until the pager answered it:
   n_pager_take gives such faults (up to max, waiting until deadline: N_ERR_TIMEOUT;
   N_ERR_CLOSED once the memory is gone), and the handle is N_EVENT_RECEIVABLE while some wait.
   n_pager_supply binds runs of pages to RAM the pager holds (write-back; a DMA object's too),
   given as a root or a view of one (not a composite), which every mapper then reaches without
   asking again; revoking the view a page came through (or a view it descends from) unbinds
   that page everywhere. n_pager_revoke takes the frames (access 0) or the write access
   (N_MEMORY_READ) of a run away from every mapper at once; n_pager_fail makes the waiting
   touches of a run access violations. When the pager handle closes, bound pages stay and the
   others fail. Paged memory has no views; n_memory_protect holds for its unbound pages too. */
n_handle n_pager_create(size_t bytes, n_handle *memory);
n_result n_pager_take(n_handle pager, struct n_pager_fault *out, size_t max, uint64_t deadline);
n_result n_pager_supply(n_handle pager, const struct n_pager_supply *runs, size_t count);
n_result n_pager_revoke(n_handle pager, uint64_t offset, uint64_t bytes, uint32_t access);
n_result n_pager_fail(n_handle pager, uint64_t offset, uint64_t bytes);
/* Sound: the service audio mixes the streams (docs/nocturne-native-abi.md §17). A stream has the format
   it was opened with, at any rate and with any number of channels; the service converts it to the output
   it has chosen. The sound goes through a ring shared with the service: a stream that keeps playing makes
   no call per buffer. A stream holds about 170 ms; n_audio_submit takes what fits and returns the frames
   taken (with N_AUDIO_WAIT it waits for room; without, a full stream gives N_ERR_NOT_READY). n_audio_flush
   drops what is queued at once, with what the output was given and has not begun to play. n_audio_close
   lets what is queued play out (it waits for that) and frees the stream. A stream outlives the service: when
   the service starts again, the next call opens the stream there and goes on. One thread at a time. */
typedef struct n_audio_stream n_audio_stream;
n_result n_audio_open(const struct n_audio_format *format, n_audio_stream **out);
n_result n_audio_submit(n_audio_stream *stream, const void *frames, size_t count, uint32_t flags);
n_result n_audio_status(n_audio_stream *stream, struct n_audio_status *out);
n_result n_audio_flush(n_audio_stream *stream);
void n_audio_close(n_audio_stream *stream);
/* The outputs and what the service has decided: a fresh array of bytes (free it), a run of struct n_audio_output
   whose length is the result; 0 with none. */
n_result n_audio_outputs(struct n_audio_output **out);
/* The name of what plays the sound, fresh (free it); NULL when nothing does. */
char *n_audio_output_name(void);
/* What n_audio_outputs reports has changed: a byte comes on this channel for each change. Volume and muting
   are settings (n_setting_watch, N_DOMAIN_AUDIO). Close it to stop. */
n_handle n_audio_output_watch(void);
n_result n_audio_output_watch_take(n_handle watch);
/* The sound devices' way, for the one who holds N_CAP_AUDIO_DEVICE (the service; §17). */
n_result n_audio_device_list(n_handle cap, void *out, size_t bytes);
n_handle n_audio_device_open(n_handle cap, uint32_t id, const struct n_audio_device_format *format, struct n_audio_device_ring *ring);
n_result n_audio_device_run(n_handle device, int run);
n_result n_audio_device_status(n_handle device, struct n_audio_device_status *out);
n_handle n_audio_device_watch(n_handle cap);
/* What the kernel goes by (docs/nocturne-native-abi.md §12, settings_abi.h). Applying takes the right
   to (N_CAP_SETTINGS_APPLY), and is the settings service's: a program asks the service. effective:
   the bytes of the value now, however many, and what fits cap in out. */
n_result n_settings_apply(n_handle cap, uint32_t domain, uint32_t key, const void *value, size_t size);
n_result n_settings_effective(uint32_t domain, uint32_t key, void *out, size_t cap);
n_result n_settings_describe(uint32_t domain, uint32_t key, struct n_setting_desc *out);
n_handle n_settings_watch(uint32_t domain);
/* Watches (N_KIND_WATCH, docs/nocturne-native-abi.md §1): N_EVENT_RECEIVABLE once what the watch
   watches has changed, until n_watch_take takes the changes; it returns how many came since the
   last take (0: none, at most N_WATCH_CHANGES_MAX). N_EVENT_CLOSED once what it watches is no
   more; a take then gives what was left, then N_ERR_CLOSED. Wait for one in an event queue. */
n_result n_watch_take(n_handle watch);
/* Event queues: level-triggered; a ready handle is reported by every wait. */
n_handle n_events_create(void);
n_result n_events_watch(n_handle q, n_handle h, uint32_t interest, uint64_t token);
n_result n_events_watch_address(n_handle q, volatile uint32_t *address, uint32_t expected, uint64_t token);
/* A timer due at a moment of the monotonic clock, then every period ns on its grid (0: once). */
n_result n_events_timer(n_handle q, uint64_t due, uint64_t period, uint64_t token);
n_result n_events_unwatch(n_handle q, uint64_t token); /* 0 unknown, 1 removed, 2 already notified */
n_result n_events_wait(n_handle q, struct n_event *out, size_t max, uint64_t deadline); /* count, 0 at the deadline */
/* The clocks in ns (abi.h), read without a call: monotonic (stands still while the machine sleeps;
   every deadline is on it), boot (goes on through sleep), wall (Unix time). */
uint64_t n_clock_monotonic(void);
uint64_t n_clock_boot(void);
int64_t n_clock_wall(void);
uint64_t n_clock_tsc_hz(void); /* how many TSC counts make a second */
uint64_t n_clock_after(uint64_t ns); /* the deadline ns from now (N_CLOCK_NEVER past the clock's end) */
uint64_t n_app_cpu_ns(void); /* the CPU time this app's threads have used, ended ones too (SYS_APP_CPU_TIME) */
void n_sleep_until(uint64_t deadline);
n_result n_random(void *buf, size_t n); /* n bytes from the system entropy pool */
/* Whether path is complete: "C:\...", "\..." or "/...". "C:" and "C:a" are not: they are in the
   current folder, like "a". */
bool n_path_is_complete(const char *path);
/* name taken inside folder (a complete path; the root when it is not one), written as "C:\a\b"
   with "." and ".." resolved. A complete name stands alone. Returns the length, or N_ERR_TOO_LARGE. */
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap);
/* A complete path for a name the user typed: n_path_join inside the current folder (n_folder). */
n_result n_path_complete(const char *name, char *out, size_t cap);
const char *n_folder(void); /* このアプリの今のフォルダー。完全なパス。 */
n_result n_folder_set(const char *name); /* 今のフォルダーを変える。N_ERR_NOT_FOLDER if it is not one */
/* The last name of path, inside it ("" for the root). */
const char *n_path_name(const char *path);
/* The complete path a file: URL names: "file:///C:/Users/User/a%20b.html" is C:\Users\User\a b.html,
   a path without a drive ("file:///Users/User") is on C: too, and "file://localhost/" is "file:///";
   the query and fragment are not part of it. Returns the length, N_ERR_INVALID when url is not a
   file: URL of a path, or N_ERR_TOO_LARGE. */
n_result n_path_from_url(const char *url, char *out, size_t cap);
/* The file: URL of a path, made complete first (n_path_join): C:\Users\User\a b.html is
   file:///C:/Users/User/a%20b.html. Returns the length, or N_ERR_TOO_LARGE. */
n_result n_path_to_url(const char *path, char *out, size_t cap);
/* この process の利用者のフォルダーと一時フォルダー。身元から accounts が引く（libc/services.c）。
   システムの身元には無く、"C:\" を返す。 */
const char *n_user_folder(void);
const char *n_temp_folder(void);
/* The folder an app keeps its data in, <user's folder>\AppData\<name>, made when missing.
   Returns the length of the path written to out. */
n_result n_app_data_folder(const char *name, char *out, size_t cap);
/* Whether handle h reaches a terminal (struct n_start の terminals). */
bool n_terminal(n_handle h);
/* 端末の Ctrl+C の知らせを受ける channel（struct n_start の interrupts）。無ければ -N_ERR_NOT_FOUND。 */
n_handle n_interrupts(void);
/* The environment of this launch (struct n_start), ended by NULL: for ported programs. */
extern char *const *__libc_environment;

/* The count of processes and threads (at most max written), or a negative n_result. */
n_result proclist(struct n_procinfo *out, int max);
/* 全部の行の新しい配列（free する）。数を返す。 */
n_result n_processes(struct n_procinfo **out);
/* 0, or a negative n_result; errno is left alone. */
n_result sysinfo(struct n_sysinfo *si);
n_result cpu_info(struct n_cpuinfo *info);
/* Native tasks sharing this process's address space and descriptors. create
 * returns the thread's id (never 0); a process has at most 128 running threads,
 * past that N_ERR_NOT_READY. The stack is 256 KiB, or with create_sized at least
 * stack_bytes (0: 256 KiB); its pages are given on first touch. A thread ends
 * when fn returns; join waits for that, and detach says no one will join it.
 * All return a negative n_result on failure and leave errno alone. */
n_result thread_create(void (*fn)(void *), void *arg);
n_result thread_create_sized(void (*fn)(void *), void *arg, size_t stack_bytes);
n_result thread_join(int tid);
n_result thread_detach(int tid);
NOCTURNE_NORETURN void thread_exit(void);
int thread_id(void);
/* The class (schedule, abi.h N_SCHEDULE_*) thread tid of this app (0: the calling thread) runs in: value is
   its weight for N_SCHEDULE_FAIR, its period in ns for N_SCHEDULE_DEADLINE. */
n_result n_thread_schedule(int tid, uint32_t schedule, uint64_t value);
unsigned cpu_index(void);
/* Process-library thread-local slots, independent of errno and thread identity.
 * Libraries use these instead of ELF thread-local variables, which tcc -run
 * programs cannot link. */
#define N_TLS_SLOTS 16
#define N_TLS_WASM_RUNTIME 3 /* Slots 0/1/2 are parallel, layout and CSS. */
#define N_TLS_GL_CONTEXT 4   /* the thread's current WebGL/OpenGL context */
#define N_TLS_GL_DISPATCH 5  /* Mesa's current dispatch table */
#define N_TLS_GL_CURRENT 6   /* Mesa's current gl_context; slot 7 is C11 threads */
#define N_TLS_CALL_ONCE 8    /* Mesa's call-once-with-data argument */
#define N_TLS_NET_CANCEL 9   /* net_set_setup_cancel_channel's channel, plus 1 */
void *thread_local_get(unsigned slot);
void thread_local_set(unsigned slot, void *value);
/* Actual reserved stack interval of the calling native task: 0, or a negative n_result. */
n_result thread_stack_bounds(uintptr_t *low, uintptr_t *high);
/* The wait lasts until deadline (N_CLOCK_NEVER: no limit); count == 0 wakes all.
 * The wait returns 0 when woken, -N_ERR_NOT_READY when *address no longer
 * holds expected, -N_ERR_TIMEOUT or -N_ERR_STOPPED; wake returns how many
 * waiters it woke.  errno is left alone. */
n_result wait_on_address(volatile uint32_t *address, uint32_t expected, uint64_t deadline);
/* The 8-byte form; wake_address on the same address wakes it. */
n_result wait_on_address64(volatile uint64_t *address, uint64_t expected, uint64_t deadline);
n_result wake_address(volatile uint32_t *address, unsigned count);
void yield(void);
/* These return only on failure, with a negative n_result. */
n_result poweroff(n_handle cap); /* N_CAP_POWER */
n_result reboot(n_handle cap);   /* N_CAP_POWER */
/* N_CAP_POWER（§16）：POWER_* を実行する。n_power_events は事象の今、n_power_events_watch はその watch。
   n_power_windows はアプリの窓の数を返し、一番上の題を top_title へ写す。ask_to_close なら全部に閉じるよう頼む。
   n_power_windows_watch はその数と題の watch。n_power_say は窓を閉じる段取りを知らせる。 */
n_result n_power_request(n_handle cap, uint32_t action);
n_result n_power_events(n_handle cap, struct n_power_events *out);
n_handle n_power_events_watch(n_handle cap);
n_result n_power_windows(n_handle cap, bool ask_to_close, char *top_title, size_t size);
n_result n_power_say(n_handle cap, const struct n_power_state *state);
n_handle n_power_windows_watch(n_handle cap);
/* 台帳（docs/hal.md）。n_hal_list は記録の全部のバイト数を返し、bytes に入らなければ何も書かない。n_hal_snapshot は
   全部を一つの時点で malloc の記憶へ写し、件数を返す（first を free する）。記録は可変の長さで、n_hal_device_next
   で次へ進む。 */
struct n_hal_snapshot {
    const struct n_hal_device *first;
    uint32_t count;
};
n_result n_hal_list(void *out, size_t bytes);
n_result n_hal_snapshot(struct n_hal_snapshot *out);
n_handle n_hal_watch(void);
long __syscall(long n, long a, long b, long c, long d, long e);
/* Where a slot of this program's entry image is (abi.h). For a language runtime with its own calls. */
uintptr_t n_entry_slot(unsigned slot);

/* run a program (find_program) and wait for it; returns exit status or negative error */
int run_wait(const char *path, char *const argv[]);
/* A program's complete path: a bare name ("ls") is looked for in N_FOLDER_SYSTEM, then in each
   folder of N_FOLDER_PROGRAMS; any other is completed (n_path_complete). False if there is none. */
bool find_program(const char *name, char *out, size_t n);
/* Whether the complete path folder is one find_program looks in: N_FOLDER_SYSTEM or a folder of
   N_FOLDER_PROGRAMS. */
bool program_folder(const char *folder);

/* line editor for terminal programs (echo, editing keys, history). Returns the line length, or -1
   at end of input (Ctrl+D). RL_SECRET echoes '*' and keeps the line out of the history. */
#define RL_SECRET 1
int readline(const char *prompt, char *buf, size_t size, int flags);

/* ---- networking (net.c) ----
   IPv4 addresses are uint32_t in host order (10.0.2.15 is 0x0A00020F). Functions return their
   result or a negative n_result, and leave errno alone. Waits end at a deadline of the monotonic
   clock (N_CLOCK_NEVER: none), and an expired wait is -N_ERR_NOT_READY or -N_ERR_TIMEOUT. */
/* The adapters: how many there are, the first max of them in out. */
n_result n_net_adapters(struct n_net_adapter *out, size_t max);
/* An adapter's driver's name: its whole length, what fits in out (not terminated). */
n_result n_net_adapter_name(uint32_t adapter, char *out, size_t cap);
/* The routes: how many there are, the first max of them in out. */
n_result n_net_routes(struct n_net_route *out, size_t max);
/* A watch (n_watch_take) of the adapters, their addresses and the routes. */
n_handle n_net_watch(void);
/* The service network's own: an adapter's address (ip 0: none), the whole route table, and a UDP
   socket on one adapter. cap: N_CAP_NETWORK. */
n_result n_net_address_set(n_handle cap, uint32_t adapter, uint32_t ip, uint32_t mask);
n_result n_net_routes_set(n_handle cap, const struct n_net_route *routes, size_t count);
n_result udp_socket_on(n_handle cap, uint32_t adapter, uint16_t port);
n_result net_ping(uint32_t ip, int seq, uint64_t deadline); /* round-trip time in ns */
n_result net_stats(struct n_netstats *stats);
/* Calling thread only: abort name lookups, ARP, connects and UDP waits if this request channel's
   peer closes. -1 clears the context. Established/shared TCP I/O is unaffected. */
n_result net_set_setup_cancel_channel(n_handle channel);
bool ip_parse(const char *s, uint32_t *ip);
char *ip_format(uint32_t ip, char *buf); /* buf: at least 16 bytes */
n_result udp_socket(uint16_t port);      /* a connection handle; 0 picks an ephemeral port */
n_result udp_sendto(n_handle s, uint32_t ip, uint16_t port, const void *buf, size_t len);
n_result udp_recvfrom(n_handle s, void *buf, size_t len, uint64_t deadline, struct n_sockaddr *from);
n_result udp_close(n_handle s);
n_result tcp_connect(uint32_t ip, uint16_t port, uint64_t deadline); /* a connection handle */
n_result tcp_send(n_handle s, const void *buf, size_t len);
/* Waits for room until deadline. A positive result is the accepted prefix, never a claim that
   the peer acknowledged it. */
n_result tcp_send_until(n_handle s, const void *buf, size_t len, uint64_t deadline);
/* The accepted prefix, or -N_ERR_NOT_READY; never waits for space. */
n_result tcp_send_some(n_handle s, const void *buf, size_t len);
/* Abort the connection, waking send/receive waiters with -N_ERR_CANCELLED. */
n_result tcp_cancel(n_handle s);
n_result tcp_recv(n_handle s, void *buf, size_t len, uint64_t deadline); /* 0 = connection closed */
n_result tcp_close(n_handle s);
/* Passive open: tcp_listen returns a listener on a local address (0: all of them, or e.g.
   127.0.0.1 for this system only; port 0 picks one; backlog <= 0 uses a default), tcp_accept
   the next established connection, ordinary for tcp_send/tcp_recv/tcp_close.
   tcp_close on the listener refuses connections not yet accepted. */
n_result tcp_listen(uint32_t ip, uint16_t port, int backlog);
n_result tcp_accept(n_handle listener, uint64_t deadline, struct n_sockaddr *remote);
n_result tcp_address(n_handle s, struct n_sockaddr *local, struct n_sockaddr *remote); /* either may be NULL */
n_result tcp_finish(n_handle s); /* stop sending (FIN after the queued data) but keep receiving; tcp_close later */
/* simulated packet loss/reordering on this process's TCP connections, and TCP counters (either may be NULL) */
n_result net_test(const struct n_netfault *set, struct n_tcpstats *stats);
bool net_wait_up(uint64_t deadline); /* wait until an adapter has an address; false if there is no adapter, or at the deadline */

/* ---- GUI ---- */
typedef struct window {
    n_handle handle; /* N_KIND_WINDOW: its input is reported by n_events_watch as N_EVENT_RECEIVABLE */
    int w, h; /* client size */
    /* What the app draws into: the window's surface (WIN_FRAME_QUEUE: the one acquired, px NULL
       while none is) */
    canvas_t c;
    int flags;
    struct gui_event q[32];
    int qn, qi;
    bool closed;
    unsigned nsurf;                        /* its surfaces, mapped here */
    uint32_t *surface[N_WIN_SURFACES_MAX];
    size_t surface_bytes;
    int current;                           /* WIN_FRAME_QUEUE: the surface acquired, -1 for none */
    uint64_t presented;                    /* the last present's number (EV_FRAME's serial) */
} window_t;

/* The calls below return their result (0 unless noted), or a negative n_result,
   and leave errno alone. */
bool gui_available(void);
n_result screen_size(int *w, int *h); /* w and h are written only on success */
/* Opens a window into *out; *out is NULL on failure. */
n_result win_open(int w, int h, const char *title, int flags, window_t **out);
void win_close(window_t *win);
/* Presents what was drawn; its number goes to presented (EV_FRAME's serial says which present a
   frame showed). A window without WIN_FRAME_QUEUE presents a rectangle of its surface (win_update: all
   of it), which the system copies. A WIN_FRAME_QUEUE window presents its acquired surface whole
   and has none after it; -N_ERR_BUSY when no frame was begun. */
n_result win_update(window_t *win);
n_result win_update_rect(window_t *win, int x, int y, int w, int h);
/* WIN_FRAME_QUEUE only: waits for a surface free to draw into (SYS_WIN_ACQUIRE) and makes it c. A
   frame is drawn whole: the surface holds an older picture, not the one presented last. */
n_result win_begin_frame(window_t *win);
/* New surfaces of w x h (1 to N_WIN_SIZE_MAX each) with as much of the picture shown as fits, in
   place of the window's; win_event does this for EV_RESIZE. Without memory the window keeps what it
   had (N_ERR_NO_MEMORY). */
n_result win_resize(window_t *win, int w, int h);
n_result win_set_title(window_t *win, const char *title);
n_result win_move(window_t *win, int x, int y);
n_result win_state(window_t *win); /* WIN_STATE_* bitmask */
n_result win_fullscreen(window_t *win,bool enabled); /* resize delivered by EV_RESIZE */
/* Asks that, while the window is fullscreen, focused and nothing of the system is drawn over it,
   the display driver show the app's own images (nvrm's direct scanout) instead of the desktop.
   EV_DIRECT (key 1) says when that starts and (key 0) when it ends; in between the window's own
   buffer is not shown. Without a display driver that can, it never starts. */
n_result win_direct(window_t *win, bool wanted);
/* A window opened with WIN_FRAME_EVENTS gets EV_FRAME at the display's next frame even if it presents
   nothing: an animation waits for the display before it decides what to draw. One EV_FRAME answers the
   request and the presents before it; a window that is not shown gets it when it is shown again. */
n_result win_frame(window_t *win);
/* The pointer the window wants over its client area (SYS_WIN_POINTER): one of the desktop's shapes
   (N_POINTER_*), or an image of at most N_POINTER_IMAGE_MAX square with its hot spot (copied at the
   call) and the shape for where an image is not shown. The desktop draws it, and its own shapes
   over its menus, frames and resize band. It stays until the next call; a new window has
   N_POINTER_DEFAULT. */
n_result win_pointer(window_t *win, const struct n_win_pointer *pointer);
n_result win_pointer_shape(window_t *win, uint32_t shape);
/* wait until deadline (N_CLOCK_NEVER: no limit) for an event; returns 1 if *e was filled, 0 at the
   deadline, negative once the window is gone. EV_RESIZE is handled before it is returned: the window has new
   surfaces of the size in the event, with as much of the picture shown as fits. An EV_KEY's key is its
   character or NKEY_ in the layout of the keyboard that typed it (n_key_translate). */
n_result win_event(window_t *win, struct gui_event *e, uint64_t deadline);
/* The one place a key's usage, modifiers and layout become a character (user/libc/keymap.c): sets e->key, and leaves in
   e->mods what programs read. */
void n_key_translate(struct gui_event *e);
/* Reads the console (N_KIND_CONSOLE) as text: each press becomes its character, in the layout of the
   keyboard that typed it, or the terminal's sequence for a moving key. flags as n_channel_receive's. */
n_result n_console_receive(n_handle h, void *buf, size_t cap, uint32_t flags);
n_result clipboard_set(const char *s, size_t n);
n_result clipboard_get(char *buf, size_t n); /* the whole length; at most n bytes are copied */
/* Starts a program (find_program: "notepad" is enough), with arg as its one argument and without
   input, output or log: an ordinary launch with the caller's identity. Its identifier, or a negative
   n_result. */
n_result gui_launch(const char *path, const char *arg);
/* copy the whole screen (taskbar included) into buf as 0xAARRGGBB pixels. bytes = 0 only reports
   the size. -N_ERR_TOO_LARGE: the buffer is too small */
n_result screen_grab(n_handle cap, uint32_t *buf, size_t bytes, int *w, int *h); /* N_CAP_SCREEN_READ */

/* ---- sound (sound.c) ----
   Streams come from n_audio_open (above) and hold about 170 ms (SOUND_QUEUE frames at SOUND_RATE). */
#define SOUND_RATE  48000
#define SOUND_QUEUE 8192
#define SOUND_FORMAT ((struct n_audio_format){.rate = SOUND_RATE, .channels = 2, .sample = N_AUDIO_S16})
enum { SND_SINE, SND_SQUARE, SND_TRIANGLE, SND_SAW, SND_NOISE };
/* A sound effect that never makes the program wait: a tone gliding from hz to hz2 over ms
   milliseconds. Effects play one after another; one that does not fit in the queue is dropped.
   Returns false if it was not played (no sound, or no room). */
bool sound_effect(int hz, int hz2, int ms, int wave);

/* A WAV file read as SOUND_RATE stereo frames: integer PCM of 8, 16, 24 or 32 bits or 32-bit
   float, any number of channels (mono goes to both sides; past two, the first two are used) and
   any rate (resampled). */
struct wav {
    n_handle document;
    int tag, channels, bits, block;
    uint64_t offset;                  /* the next byte of the document to read */
    uint32_t rate, frames;           /* the file's own rate and length */
    uint32_t data_off, data_len, pos; /* pos: the next frame to read from the file */
    uint32_t src;                     /* the source frame playing now */
    uint64_t step, frac;
    int32_t al, ar, bl, br;
    bool eof;
    size_t len, at;
    uint8_t buf[8192];
};
const char *wav_open(struct wav *w, const char *path); /* NULL, or what is wrong */
void wav_close(struct wav *w);
void wav_seek(struct wav *w, uint32_t frame); /* in the file's frames */
int wav_read(struct wav *w, int16_t *out, int frames); /* fewer than asked at the end */
uint32_t wav_ms(const struct wav *w, uint32_t frame);
NOCTURNE_END_DECLS
