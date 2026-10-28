/**
* @file zmalloc.h
* A simple memory allocation monitor, header.
*
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
*
* This code was not part of the original CircleMUD distribution. It has been
* included in tbaMUD and is attributed to:
* Version 1.1 Copyright 1996, 1998, 1999, 2000 Eric Murray ericm@lne.com
*/

#ifndef TBAMUD_ZMALLOC_H
#define TBAMUD_ZMALLOC_H

#include <stddef.h>

unsigned char *zmalloc(size_t, char *, int);
unsigned char *zcalloc(size_t, size_t, char *, int);
unsigned char *zrealloc(unsigned char *, size_t, char *, int);
void zfree(unsigned char *, char *, int);
void zmalloc_init(void);
void zmalloc_check(void);
char *zstrdup(const char *, char *, int);

/* Under ASan, zfree()d memory and the guard pads around every block are
 * poisoned (see asan_annotations.h), so a use-after-free or a pad overrun
 * aborts at the faulting access instead of at the next pad check. */
/* Each argument is evaluated exactly once.  The (void *) casts give the
 * macros malloc()'s own return type, so `char *p = malloc(n)` does not trip
 * -Wpointer-sign in MEMORY_DEBUG builds.  calloc() maps to zcalloc(), which
 * multiplies with an overflow check (the old zmalloc((n*x), ...) expansion
 * turned calloc(2, a + b) into a 2*a + b byte allocation). */
#define malloc(x)	((void *)zmalloc((x),__FILE__,__LINE__))
#define calloc(n,x)	((void *)zcalloc((n),(x),__FILE__,__LINE__))
#define realloc(r,x)	((void *)zrealloc((unsigned char *)(r),(x),__FILE__,__LINE__))
#define free(x)		zfree((unsigned char *)(x),__FILE__,__LINE__)
#undef  strdup
#define strdup(x)	zstrdup((x), __FILE__, __LINE__)

#endif /* TBAMUD_ZMALLOC_H */
