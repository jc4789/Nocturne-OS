#pragma once
#include "media.h"
typedef struct nmedia_mse nmedia_mse;
struct nmedia_time_range { int64_t start_ms,end_ms; };
bool nmedia_mse_type(const char *);
nmedia_mse *nmedia_mse_create(const char *,char *,size_t);
bool nmedia_mse_append(nmedia_mse *,const void *,size_t,int64_t,int64_t,int64_t,bool);
/* Consumes a distinct nmedia_ff_malloc block on every return, including error
 * and NULL media. At least count writable bytes; no alias of retained input.
 * Empty staging adopts it; partial staging copies then releases the block. */
bool nmedia_mse_append_take(nmedia_mse *,void *,size_t,int64_t,int64_t,int64_t,bool);
const char *nmedia_mse_error(const nmedia_mse *);
bool nmedia_mse_quota_error(const nmedia_mse *);
size_t nmedia_mse_quota(const nmedia_mse *);
const struct nmedia_info *nmedia_mse_info(const nmedia_mse *);
uint64_t nmedia_mse_revision(const nmedia_mse *);
bool nmedia_mse_parsing(const nmedia_mse *);
int64_t nmedia_mse_group_end(const nmedia_mse *);
int64_t nmedia_mse_duration(const nmedia_mse *);
int nmedia_mse_step(nmedia_mse *,struct nmedia_output *);
bool nmedia_mse_move_video(nmedia_mse *,const struct nmedia_output *,uint32_t **,size_t *);
/* Valid after AGAIN: true only for absent decoder/input, not bounded decode
 * work (e.g. seek preroll) that must be resumed without another append. */
bool nmedia_mse_waiting_for_input(const nmedia_mse *);
bool nmedia_mse_seek(nmedia_mse *,int64_t);
bool nmedia_mse_remove(nmedia_mse *,int64_t,int64_t);
void nmedia_mse_abort(nmedia_mse *);
bool nmedia_mse_end(nmedia_mse *,bool);
bool nmedia_mse_change_type(nmedia_mse *,const char *);
size_t nmedia_mse_ranges(nmedia_mse *,struct nmedia_time_range *,size_t);
/* Actual retained-frame count, an upper bound for dynamic range allocation. */
size_t nmedia_mse_range_capacity(const nmedia_mse *);
void nmedia_mse_close(nmedia_mse *);
