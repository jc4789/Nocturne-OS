/* Host-only header surface for -DMALLOCTRIM_MODEL. Do not use for OS builds. */
#pragma once
#ifndef MALLOCTRIM_MODEL
#error "The allocator break-model header is only for the host model"
#endif
void *sbrk(long inc);
