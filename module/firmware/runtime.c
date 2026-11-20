/* SPDX-License-Identifier: GPL-3.0-or-later
 * Freestanding Common V2 byte primitives: no libc, VFS, or EFI drivers.
 */
#include <stddef.h>
void *memcpy(void *restrict destination,const void *restrict source,size_t n)
{
 unsigned char *d=destination;const unsigned char *s=source;
 for(size_t i=0;i<n;++i)d[i]=s[i];
 return destination;
}
void *memset(void *destination,int value,size_t n)
{
 unsigned char *d=destination;
 for(size_t i=0;i<n;++i)d[i]=(unsigned char)value;
 return destination;
}
void *memchr(const void *buffer,int ch,size_t n)
{
 const unsigned char *p=buffer;
 for(size_t i=0;i<n;++i)if(p[i]==(unsigned char)ch)return (void *)(p+i);
 return NULL;
}
