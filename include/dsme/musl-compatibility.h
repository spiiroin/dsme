#ifndef DSME_MUSL_COMPATIBILITY_H_
# define DSME_MUSL_COMPATIBILITY_H_

/* Whether __GLIBC__ gets defined when compiling against gnu libc,
 * depends on what - if any - libc header files have been included
 * so far. To be sure, include features.h that contains the define.
 */
# include <features.h>

/* Define equivalents for glibc macros that dsme sources are using,
 * but are not defined in musl libc headers.
 */
# ifndef __GLIBC__

/* Used to retry syscalls that can return EINTR. Taken from bionic unistd.h
 */
#  ifndef TEMP_FAILURE_RETRY
#   define TEMP_FAILURE_RETRY(exp) ({      \
    __typeof__(exp) _rc;                   \
    do {                                   \
        _rc = (exp);                       \
    } while (_rc == -1 && errno == EINTR); \
    _rc; })
#  endif

# endif /* not __GLIBC__ */

#endif /* DSME_MUSL_COMPATIBILITY_H_ */
