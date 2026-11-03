/* The entry image (common/abi.h, docs/nocturne-native-abi.md §0): every process has one, read-only,
   at an address of its own, and programs reach the kernel only through its slots. So the numbers,
   the form of the SYSCALL and the form of the clocks' data stay inside the image.

     entry_base                  the slots, N_ENTRY_SLOT_BYTES each: of the public image or the whole one
     + slot pages                the code (kernel/entry): the clocks
     + slot pages + code pages   the clocks' data (sys/clock.c)

   The two images differ only in the slot pages. The frames are never written after entry_init, and
   every process maps the same ones. */
#include "kernel.h"
#include "sys/entry.h"
#include "sys/clock.h"
#include "sys/process.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "dev/entropy.h"
#include "abi.h"

/* mov r10, rcx; mov eax, number; syscall; ret. The C call's fourth argument is in rcx, which the
   instruction takes. */
#define STUB_RETURN 10u /* the offset of the ret: where a SYSCALL of the slot returns to */
_Static_assert(STUB_RETURN < N_ENTRY_SLOT_BYTES, "the stub fits its slot");
#define SLOT_BYTES (ALIGN_UP((size_t)N_ENTRY_SLOTS * N_ENTRY_SLOT_BYTES, PAGE_SIZE))

extern const uint8_t entry_code[], entry_code_end[];

static uint64_t slots_all, slots_public, code_first;
static size_t code_pages;

static const bool is_public[SYS_MAX] = {
#define X(call) [call] = true,
    N_PUBLIC_CALLS(X)
#undef X
};

static void slot_call(uint8_t *s, uint32_t number) {
    static const uint8_t head[] = {0x49, 0x89, 0xCA, 0xB8};
    memcpy(s, head, sizeof head);
    memcpy(s + sizeof head, &number, sizeof number);
    s[8] = 0x0F, s[9] = 0x05, s[10] = 0xC3;
}
static void slot_denied(uint8_t *s) { /* mov rax, -N_ERR_DENIED; ret */
    const int32_t result = -N_ERR_DENIED;
    s[0] = 0x48, s[1] = 0xC7, s[2] = 0xC0;
    memcpy(s + 3, &result, sizeof result);
    s[7] = 0xC3;
}
static void slot_jump(uint8_t *s, size_t slot, uint32_t target) { /* jmp to target, from the image's start */
    int32_t relative = (int32_t)(target - (slot * N_ENTRY_SLOT_BYTES + 5));
    s[0] = 0xE9;
    memcpy(s + 1, &relative, sizeof relative);
}

static uint64_t slots_make(bool all_calls, const uint32_t *code_at) {
    uint64_t first = pmm_alloc_contig(SLOT_BYTES / PAGE_SIZE, 64);
    if (!first) panic("entry: no memory for its slots");
    uint8_t *image = phys_to_virt(first);
    memset(image, 0xCC, SLOT_BYTES); /* int3 */
    for (size_t i = 0; i < SYS_MAX; i++) {
        uint8_t *s = image + i * N_ENTRY_SLOT_BYTES;
        if (all_calls || is_public[i]) slot_call(s, (uint32_t)i);
        else slot_denied(s);
    }
    for (size_t i = SYS_MAX; i < N_ENTRY_SLOTS; i++)
        slot_jump(image + i * N_ENTRY_SLOT_BYTES, i, (uint32_t)SLOT_BYTES + code_at[i - SYS_MAX]);
    return first;
}

void entry_init(void) {
    size_t bytes = (size_t)(entry_code_end - entry_code);
    code_pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    code_first = pmm_alloc_contig(code_pages, 64);
    if (!code_first) panic("entry: no memory for its code");
    uint8_t *code = phys_to_virt(code_first);
    memset(code, 0xCC, code_pages * PAGE_SIZE);
    memcpy(code, entry_code, bytes);
    uint32_t code_at[N_ENTRY_SLOTS - SYS_MAX];
    memcpy(code_at, code, sizeof code_at); /* its head */
    slots_all = slots_make(true, code_at);
    slots_public = slots_make(false, code_at);
}

int entry_map(struct process *p, bool all_calls) {
    size_t slot_pages = SLOT_BYTES / PAGE_SIZE, pages = slot_pages + code_pages + clock_data_pages();
    uint64_t room = USER_ENTRY_SPAN / PAGE_SIZE - pages, random;
    entropy_get(&random, sizeof random);
    uint64_t base = USER_MMAP_TOP + random % room * PAGE_SIZE;
    uint64_t slots = all_calls ? slots_all : slots_public;
    const uint64_t code_flags = PTE_U | PTE_SHARED | PTE_SHARED_RO;
    for (size_t i = 0; i < slot_pages; i++) {
        int r = vmm_map_page(p->vmm_space, base + i * PAGE_SIZE, slots + i * PAGE_SIZE, code_flags);
        if (r < 0) return r;
    }
    uint64_t code = base + slot_pages * PAGE_SIZE;
    for (size_t i = 0; i < code_pages; i++) {
        int r = vmm_map_page(p->vmm_space, code + i * PAGE_SIZE, code_first + i * PAGE_SIZE, code_flags);
        if (r < 0) return r;
    }
    int r = clock_map(p->vmm_space, code + code_pages * PAGE_SIZE);
    if (r < 0) return r;
    p->entry_base = base;
    return 0;
}

bool entry_came_from(const struct process *p, uint64_t rip, uint64_t number) {
    return rip - p->entry_base == number * N_ENTRY_SLOT_BYTES + STUB_RETURN;
}
