INCLUDE user/user.ld
/* C++ programs share the C layout; user.ld installs ELF thread_local, so a
   language thread_local and libc's native per-thread state stay distinct. */
