/* Nocturne's static, exception-free C++ ABI. The compiler runs on the host;
   allocation, initialization guards and finalization use the native libc. */
#include <cstddef>
#include <cstdint>
#include <new>

extern "C" {
void *malloc(size_t);
void free(void *);
[[noreturn]] void abort(void);
int thread_id(void);
int wait_on_address(volatile uint32_t *, uint32_t, unsigned);
int wake_address(volatile uint32_t *, unsigned);
void yield(void);
void *__dso_handle = &__dso_handle;
}

namespace {
std::new_handler allocation_handler;

void *allocate(size_t size, size_t alignment, bool must_succeed) {
    if (!size) size = 1;
    for (;;) {
        void *result = nullptr;
        if (!alignment) result = malloc(size);
        else if (!(alignment & (alignment - 1)) && alignment <= SIZE_MAX - sizeof(void *) + 1) {
            if (alignment < alignof(void *)) alignment = alignof(void *);
            const size_t overhead = alignment - 1 + sizeof(void *);
            if (size <= SIZE_MAX - overhead) {
                void *base = malloc(size + overhead);
                if (base) {
                    const uintptr_t address = (reinterpret_cast<uintptr_t>(base) + sizeof(void *) + alignment - 1)
                                              & ~(static_cast<uintptr_t>(alignment) - 1);
                    result = reinterpret_cast<void *>(address);
                    static_cast<void **>(result)[-1] = base;
                }
            }
        }
        if (result) return result;
        std::new_handler handler = std::get_new_handler();
        if (handler) { handler(); continue; }
        if (!must_succeed) return nullptr;
        /* This build has no exception unwinder: throwing allocation fails
           explicitly instead of returning a null pointer to a new-expression. */
        abort();
    }
}
void free_aligned(void *pointer) {
    if (pointer) free(static_cast<void **>(pointer)[-1]);
}
}

/* libc++ deliberately puts allocation ABI types/functions in unversioned std. */
namespace std {
const nothrow_t nothrow{};
new_handler set_new_handler(new_handler handler) noexcept {
    return __atomic_exchange_n(&allocation_handler, handler, __ATOMIC_ACQ_REL);
}
new_handler get_new_handler() noexcept {
    return __atomic_load_n(&allocation_handler, __ATOMIC_ACQUIRE);
}
}

/* Standard replacement functions stay weak so an application can supply its
   own allocator while retaining guards, finalization and the standard library. */
#define N_CXX_REPLACEABLE __attribute__((weak))
N_CXX_REPLACEABLE void *operator new(size_t size) { return allocate(size, 0, true); }
N_CXX_REPLACEABLE void *operator new[](size_t size) { return ::operator new(size); }
N_CXX_REPLACEABLE void *operator new(size_t size, const std::nothrow_t &) noexcept { return allocate(size, 0, false); }
N_CXX_REPLACEABLE void *operator new[](size_t size, const std::nothrow_t &tag) noexcept { return ::operator new(size, tag); }
N_CXX_REPLACEABLE void operator delete(void *pointer) noexcept { free(pointer); }
N_CXX_REPLACEABLE void operator delete[](void *pointer) noexcept { ::operator delete(pointer); }
N_CXX_REPLACEABLE void operator delete(void *pointer, size_t) noexcept { ::operator delete(pointer); }
N_CXX_REPLACEABLE void operator delete[](void *pointer, size_t) noexcept { ::operator delete[](pointer); }
N_CXX_REPLACEABLE void operator delete(void *pointer, const std::nothrow_t &) noexcept { ::operator delete(pointer); }
N_CXX_REPLACEABLE void operator delete[](void *pointer, const std::nothrow_t &) noexcept { ::operator delete[](pointer); }

N_CXX_REPLACEABLE void *operator new(size_t size, std::align_val_t alignment) {
    return allocate(size, static_cast<size_t>(alignment), true);
}
N_CXX_REPLACEABLE void *operator new[](size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
N_CXX_REPLACEABLE void *operator new(size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept {
    return allocate(size, static_cast<size_t>(alignment), false);
}
N_CXX_REPLACEABLE void *operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t &tag) noexcept {
    return ::operator new(size, alignment, tag);
}
N_CXX_REPLACEABLE void operator delete(void *pointer, std::align_val_t) noexcept { free_aligned(pointer); }
N_CXX_REPLACEABLE void operator delete[](void *pointer, std::align_val_t alignment) noexcept { ::operator delete(pointer, alignment); }
N_CXX_REPLACEABLE void operator delete(void *pointer, size_t, std::align_val_t alignment) noexcept { ::operator delete(pointer, alignment); }
N_CXX_REPLACEABLE void operator delete[](void *pointer, size_t, std::align_val_t alignment) noexcept { ::operator delete[](pointer, alignment); }
N_CXX_REPLACEABLE void operator delete(void *pointer, std::align_val_t alignment, const std::nothrow_t &) noexcept {
    ::operator delete(pointer, alignment);
}
N_CXX_REPLACEABLE void operator delete[](void *pointer, std::align_val_t alignment, const std::nothrow_t &) noexcept {
    ::operator delete[](pointer, alignment);
}

/* Itanium ABI reserves guard byte 0 for compiler-generated acquire reads.
   The upper word belongs to the runtime: zero is idle, otherwise it stores
   the initializing native thread ID. Waiting releases the CPU to the kernel. */
extern "C" int __cxa_guard_acquire(uint64_t *guard) {
    auto *complete = reinterpret_cast<uint8_t *>(guard);
    auto *owner = reinterpret_cast<uint32_t *>(guard) + 1;
    const uint32_t self = static_cast<uint32_t>(thread_id());
    for (;;) {
        if (__atomic_load_n(complete, __ATOMIC_ACQUIRE)) return 0;
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(owner, &expected, self, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            /* Completion may have been published between our first read and
               acquiring the owner word. Never initialize the same object twice. */
            if (!__atomic_load_n(complete, __ATOMIC_ACQUIRE)) return 1;
            __atomic_store_n(owner, 0, __ATOMIC_RELEASE);
            wake_address(owner, UINT32_MAX);
            return 0;
        }
        if (expected == self) abort(); /* Recursive local-static initialization. */
        if (wait_on_address(owner, expected, UINT32_MAX) < 0) yield();
    }
}
extern "C" void __cxa_guard_release(uint64_t *guard) {
    auto *complete = reinterpret_cast<uint8_t *>(guard);
    auto *owner = reinterpret_cast<uint32_t *>(guard) + 1;
    __atomic_store_n(complete, 1, __ATOMIC_RELEASE);
    __atomic_store_n(owner, 0, __ATOMIC_RELEASE);
    wake_address(owner, UINT32_MAX);
}
extern "C" void __cxa_guard_abort(uint64_t *guard) {
    auto *owner = reinterpret_cast<uint32_t *>(guard) + 1;
    __atomic_store_n(owner, 0, __ATOMIC_RELEASE);
    wake_address(owner, UINT32_MAX);
}
extern "C" [[noreturn]] void __cxa_pure_virtual(void) { abort(); }
extern "C" [[noreturn]] void __cxa_deleted_virtual(void) { abort(); }
