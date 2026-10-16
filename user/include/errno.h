#pragma once
#include <cdefs.h>
#include "abi.h"
NOCTURNE_BEGIN_DECLS
int *__errno_location(void);
NOCTURNE_END_DECLS
/* ISO C: errno holds the reason the last library call failed. For a failed
   Nocturne operation it is the positive N_ERR_* code (common/abi.h). */
#define errno (*__errno_location())
#define EDOM (100 + 1)
#define ERANGE (100 + 2)
#define EILSEQ (100 + 3)

/* Transitional: earlier names of the N_ERR_* codes, kept only until every
   program calls the native API (docs/nocturne-native-abi.md, stage 5). */
#define EPERM N_ERR_DENIED
#define ENOENT N_ERR_NOT_FOUND
#define ESRCH N_ERR_NO_APP
#define EINTR N_ERR_STOPPED
#define EIO N_ERR_IO
#define E2BIG N_ERR_TOO_LARGE
#define ENOEXEC N_ERR_NOT_PROGRAM
#define EBADF N_ERR_NO_HANDLE
#define ECHILD N_ERR_NO_APP
#define EAGAIN N_ERR_NOT_READY
#define EWOULDBLOCK N_ERR_NOT_READY
#define ENOMEM N_ERR_NO_MEMORY
#define EFAULT N_ERR_BAD_ADDRESS
#define EBUSY N_ERR_BUSY
#define EEXIST N_ERR_EXISTS
#define ENOTDIR N_ERR_NOT_FOLDER
#define EISDIR N_ERR_IS_FOLDER
#define EINVAL N_ERR_INVALID
#define EMFILE N_ERR_NO_MEMORY
#define ENOSPC N_ERR_NO_SPACE
#define ESPIPE N_ERR_WRONG_KIND
#define EROFS N_ERR_READ_ONLY
#define EPIPE N_ERR_CLOSED
#define ENAMETOOLONG N_ERR_NAME_TOO_LONG
#define ENOSYS N_ERR_UNSUPPORTED
#define ENOTEMPTY N_ERR_NOT_EMPTY
#define EADDRINUSE N_ERR_ADDRESS_IN_USE
#define ENETDOWN N_ERR_NETWORK_DOWN
#define ECONNRESET N_ERR_CONNECTION_RESET
#define ENOTCONN N_ERR_NOT_CONNECTED
#define ETIMEDOUT N_ERR_TIMEOUT
#define ECONNREFUSED N_ERR_CONNECTION_REFUSED
#define EHOSTUNREACH N_ERR_UNREACHABLE
#define EPROTO N_ERR_PROTOCOL
#define ECANCELED N_ERR_CANCELLED
/* Transitional: identities that libc++ std::errc and FFmpeg name. No Nocturne
   operation produces them; they only need to be distinct. */
#define ENXIO (100 + 10)
#define EACCES (100 + 11)
#define EXDEV (100 + 12)
#define ENODEV (100 + 13)
#define ENFILE (100 + 14)
#define ENOTTY (100 + 15)
#define ETXTBSY (100 + 16)
#define EFBIG (100 + 17)
#define EMLINK (100 + 18)
#define EDEADLK (100 + 19)
#define ENOLCK (100 + 20)
#define ELOOP (100 + 21)
#define ENOMSG (100 + 22)
#define EIDRM (100 + 23)
#define ENOSTR (100 + 24)
#define ENODATA (100 + 25)
#define ETIME (100 + 26)
#define ENOSR (100 + 27)
#define ENOLINK (100 + 28)
#define EBADMSG (100 + 29)
#define EOVERFLOW (100 + 30)
#define ENOTSOCK (100 + 31)
#define EDESTADDRREQ (100 + 32)
#define EMSGSIZE (100 + 33)
#define EPROTOTYPE (100 + 34)
#define ENOPROTOOPT (100 + 35)
#define EPROTONOSUPPORT (100 + 36)
#define EOPNOTSUPP (100 + 37)
#define ENOTSUP EOPNOTSUPP
#define EAFNOSUPPORT (100 + 38)
#define EADDRNOTAVAIL (100 + 39)
#define ENETUNREACH (100 + 40)
#define ENETRESET (100 + 41)
#define ECONNABORTED (100 + 42)
#define ENOBUFS (100 + 43)
#define EISCONN (100 + 44)
#define EALREADY (100 + 45)
#define EINPROGRESS (100 + 46)
#define EOWNERDEAD (100 + 47)
#define ENOTRECOVERABLE (100 + 48)
