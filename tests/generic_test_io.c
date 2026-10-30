/* SPDX-License-Identifier: GPL-3.0-or-later
 * Only in the standalone module's regression executable: provide generic
 * bounded exact positioned I/O without linking a Defragmenter implementation.
 */
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "infiltratr/posix_io.h"
ssize_t ld_pread_full(int fd,void *dst,size_t n,uint64_t offset) {
 if(n>(size_t)SSIZE_MAX){errno=EOVERFLOW;return -1;}
 return infiltratr_pread_full(fd,dst,n,offset)==0 ? (ssize_t)n : -1;
}
ssize_t ld_pwrite_full(int fd,const void *src,size_t n,uint64_t offset) {
 if(n>(size_t)SSIZE_MAX){errno=EOVERFLOW;return -1;}
 return infiltratr_pwrite_full(fd,src,n,offset)==0 ? (ssize_t)n : -1;
}
