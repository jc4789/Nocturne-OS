#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PTE_P      (1ULL << 0)
#define PTE_W      (1ULL << 1)
#define PTE_U      (1ULL << 2)
#define PTE_PWT    (1ULL << 3)
#define PTE_PCD    (1ULL << 4)
#define PTE_PS     (1ULL << 7)
#define PTE_PAT4K  (1ULL << 7)
#define PTE_G      (1ULL << 8)
#define PTE_SHARED (1ULL << 9) /* not owned by this address space: don't free */
#define PTE_PAT2M  (1ULL << 12)
#define PTE_ADDR   0x000FFFFFFFFFF000ULL
#define PTE_NX     (1ULL << 63)

/* protection for user mappings (readable is implied) */
#define VM_W 1
#define VM_X 2

#define USER_TOP       0x0000800000000000ULL
#define USER_STACK_TOP 0x00007FFFFFF00000ULL
#define USER_STACK_MAX (8ULL << 20)
#define USER_MMAP_BASE 0x0000600000000000ULL

#define KHEAP_BASE   0xFFFFC00000000000ULL
#define VMALLOC_BASE 0xFFFFD00000000000ULL

struct limine_memmap_response;
struct limine_executable_address_response;

extern uint64_t kernel_pml4;
extern uint64_t pte_nx; /* PTE_NX when the CPU supports no-execute, else 0 */

void vmm_init(struct limine_memmap_response *mm, struct limine_executable_address_response *ka);
bool vmm_map_page(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags);
uint64_t vmm_unmap_page(uint64_t pml4, uint64_t va);
uint64_t vmm_translate(uint64_t pml4, uint64_t va); /* returns phys or 0 */
uint64_t vmm_get_pte(uint64_t pml4, uint64_t va);
void *vmm_map_mmio(uint64_t phys, uint64_t size);
uint64_t vmm_new_space(void);
void vmm_free_space(uint64_t pml4);
int vmm_user_alloc(uint64_t pml4, uint64_t va, uint64_t size, int prot);
int vmm_user_protect(uint64_t pml4, uint64_t va, uint64_t size, int prot);
void vmm_user_free(uint64_t pml4, uint64_t va, uint64_t size);
int vmm_copy_to_space(uint64_t pml4, uint64_t va, const void *src, size_t n);
bool vmm_handle_user_fault(uint64_t addr, uint64_t err);
void vmm_switch(uint64_t pml4);

void *vmalloc(size_t pages);
void vfree(void *p, size_t pages);
void *vmalloc_map_phys(uint64_t pa, size_t pages, uint64_t flags);
