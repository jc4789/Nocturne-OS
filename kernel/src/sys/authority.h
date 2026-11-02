/* 身元と能力（docs/nocturne-native-abi.md §13）。process の身元は起動の時に決まり、その後は変わらない。
   読むのに錠は要らない。特権は能力の handle で持ち、数も持ち主も handle の表が決める。 */
#pragma once
#include "kernel.h"

struct handle;

/* カーネルが作った channel の端の身元：渡されるたびに受け手の身元へ下がる。 */
extern const struct n_identity identity_kernel;

/* 呼ぶ側のアプリの身元。核の thread では identity_kernel。 */
const struct n_identity *identity_current(void);
/* 呼ぶ側の級が cls 以上か。 */
bool identity_at_least(enum n_class cls);
/* holder が other を名乗っても権限が増えないか：上の級は下の全部を、SYSTEM は SYSTEM の全部を、
   それ以外の同じ級は全く同じ身元だけを。channel の端の身元はこの向きにだけ動く。 */
bool identity_covers(const struct n_identity *holder, const struct n_identity *other);
/* who が target のアプリに触れてよいか（開く、一覧で見る、clipboard を読む）。 */
bool identity_may_reach(const struct n_identity *who, const struct n_identity *target);

/* 子の身元を決める：n_launch の identity と identity_grant を見て親の身元から作る。 */
int identity_for_child(const struct n_launch *launch, struct n_identity *child);

/* 新しい能力（根を最初の process へ渡すためにだけカーネルが作る）。 */
int cap_new(enum n_cap kind, struct handle **out);
/* 呼ぶ側の handle number が kind の能力か：0、-N_ERR_NO_HANDLE か -N_ERR_DENIED。
   根でも kind 以外は通さない：使うには切り出す。 */
int cap_check(int number, enum n_cap kind);

int64_t sys_identity_self(struct n_identity *out);
int64_t sys_cap_derive(int cap, uint64_t kind);
int64_t sys_cap_kind(int cap);
int64_t sys_cap_find(uint64_t kind);

/* サービスの管理者への道（sys/service.c）。 */
int64_t sys_service_register(int cap);
int64_t sys_service_connect(const char *name, size_t length);
