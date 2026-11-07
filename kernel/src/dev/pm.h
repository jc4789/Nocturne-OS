/* 電源の段取り：電源の変化は全部一つのカーネルの thread を通り、決まった順で進む。カーネルは起きたこと（電源
   ボタン、蓋、起こし、ホストの頼み：struct n_power_events）を言い、頼まれたこと（SYS_POWER）を行う。押下が何を
   意味するか、機械が眠ってよいか、いつ眠るか、窓に閉じるよう頼んでどれだけ待つかはサービス machine が決める
   （user/apps/machine.c）。

   装置の段は HAL の電源の walk が持つ（hal/hal.h の hal_power_suspend、hal_power_resume）：動いている装置を子と
   依存する側から段ごとに止め、眠りでは D3 にし、戻りは逆の順。実装の suspend と resume は眠ってよい thread で走る。

   電源断と再起動：
     1. アプリ：走っている全部を止める。
     2. ボリュームの保存。電源断なら firmware の _TTS(5) と _PTS(5)（ACPI 6.5 7.4.4、16.1.7）。
     3. ディスク：各 block 装置が cache を書き戻す。
     4. 装置：HAL_POWER_OFF の walk（止まらない装置があっても進む）。
     5. firmware が電源を切るか、機械を立て直す。

   眠り（ACPI S3、RAM への眠り）：アプリは居た所で走り続ける（ACPI 6.5 7.5、16.1.6）。
     1. 拒みの確かめ（機械の部分と、装置の部分 hal_power_refused）。_TTS(3)。
     2. ボリュームの保存、ディスクの hold と書き戻し。
     3. 装置：HAL_POWER_SLEEP の walk。止まらない装置があれば取りやめる。
     4. _PTS(3)。ほかの CPU を止め、PCI の構成、割り込みの制御器、CPU を保存し、firmware が RAM のほかを切る。
     5. 起きたら CPU、時計、IOMMU、割り込みの制御器、PCI の割り込みの link、電源資源、PCI の構成が戻り、ほかの CPU が
        動き出す。続けて _WAK(3)、walk の戻し、_TTS(0)。取りやめでも _TTS(0)、_PTS の後なら _WAK(3) も呼ぶ。

   休止（dev/hibernate.h）：眠りと同じだが firmware に頼まない。walk は HAL_POWER_IMAGE（装置は電源を保つ）、写しを
   取った後に装置を戻し、RAM を C: へ書いて電源断（上の電源断の 2 から）。

   眠りと休止を許すのは、機械の部分（pm_sleep_check が起動で一度決める）が許し、結んでいる全部の実装が眠りの後に
   装置を戻せるとき（hal_power_cannot）。
   段は全部、watch が知らせる型の状態（struct n_power_state、abi.h）：カーネルは文を言わず、シェル（N_CAP_SHELL）が
   利用者に言う。 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "abi.h"

enum pm_action { PM_POWER_OFF = POWER_OFF, PM_REBOOT = POWER_REBOOT, PM_SLEEP = POWER_SLEEP, PM_HIBERNATE = POWER_HIBERNATE };

void pm_init(void);
/* Carry out a power change. A sleep never replaces a change already waiting. Never blocks: any CPU,
   any context under no spinlock other than the wait queues'. */
void pm_request(enum pm_action action);
/* What happened: told to the machine service through struct n_power_events. Any CPU, any context. */
void pm_button_pressed(void);
void pm_lid_is(bool closed);
void pm_host_asks(enum pm_action action);
/* 機械が眠れるか：firmware に S3 があり、起こしの頁があり、結んでいる実装が全部、眠りの後に装置を戻せる。休止は
   C: がディスクにあり、1 MiB より下に場所があり、実装が全部、休止の後に装置を戻せる。pm_sleep_check は機械の部分を
   起動で一度決めてログに書く（C: と IOMMU の後）。 */
bool pm_can_sleep(void);
bool pm_can_hibernate(void);
void pm_sleep_check(void);
/* SYS_POWER_STATE and SYS_POWER_WATCH (N_CAP_SHELL checked by the caller). */
int64_t pm_state_syscall(void *out, size_t size);
int64_t pm_watch_syscall(void);
/* SYS_POWER_EVENTS, SYS_POWER_EVENTS_WATCH and SYS_POWER_SAY (N_CAP_POWER checked by the caller). */
int64_t pm_events_syscall(void *out, size_t size);
int64_t pm_events_watch_syscall(void);
int64_t pm_say_syscall(const void *state, size_t size);
