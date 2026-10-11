INCLUDE user/user.ld
/* The loader does not install ELF TLS. Reject language thread_local instead
   of letting it address Nocturne's unrelated native per-thread libc state. */
SECTIONS {
    .tdata : { *(.tdata .tdata.*) }
    .tbss : { *(.tbss .tbss.*) }
}
ASSERT(SIZEOF(.tdata) == 0 && SIZEOF(.tbss) == 0,
       "ELF thread_local is unsupported; use Nocturne native TLS")
