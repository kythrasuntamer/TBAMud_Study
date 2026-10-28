/**************************************************************************
*  File: zmalloc.c                                         Part of tbaMUD *
*  Usage: A simple memory allocation monitor.                             *
*                                                                         *
*  Version 2. Copyright 1996, 1998, 1999, 2000 Eric Murray ericm@lne.com  *
**************************************************************************/

/*  Usage: To run tbaMUD in debug mode change the flags line in your Makefile
 *  as below, make clean, and reboot.
 *
 *  Makefile: # Any special flags you want to pass to the compiler
 *  Makefile: MYFLAGS = -Wall -DMEMORY_DEBUG
 *  CMake:    -DMEMORY_DEBUG=ON (refused together with ENABLE_SANITIZERS)
 *
 *  How it works and what it costs:
 *  - With MEMORY_DEBUG, structs.h includes zmalloc.h, whose macros route
 *    malloc/calloc/realloc/free/strdup in every server file through here
 *    with the caller's __FILE__/__LINE__.  comm.c calls zmalloc_init() at
 *    start-up and zmalloc_check() at shutdown.
 *  - Each block is calloc()ed with a 16-byte guard pad on each side (keeps
 *    malloc's alignment) and recorded in memlist with its allocation site.
 *  - zfree() NEVER releases memory: it checks the pads, counts the free
 *    and keeps the block until zmalloc_check(), which reports unfreed
 *    blocks and only then frees everything.  The process therefore grows by
 *    every byte it ever frees, and ASan, LSan and Valgrind cannot see
 *    use-after-free, double free or leaks in such a build -- use one tool
 *    or the other, not both.  (Under ASan, freed blocks are poisoned as a
 *    partial backstop.)
 *  - Detected misuse -- double free, free or realloc of a pointer zmalloc
 *    did not hand out, realloc after free, an overwritten guard pad -- is
 *    reported on stderr (TBERR record at the caller's file:line) and
 *    aborts.  Allocation failure is reported and returns NULL.
 *  - ./zmalloc.log (in the server's working directory) receives the
 *    verbose trace, hex dumps and the leak summary; if it cannot be opened
 *    that is reported once and the trace is skipped.
 *  - Not thread-safe (one global table); the server is single-threaded. */

#include "conf.h"
#include "sysdep.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>	/* SIZE_MAX, uintptr_t */
#include <stddef.h>	/* offsetof */

/*
 * zfree() never releases memory: every block stays reachable from memlist
 * until zmalloc_check() at shutdown.  ASan, LSan and Valgrind therefore see
 * no use-after-free, double free or leak in a MEMORY_DEBUG build, so the
 * CMake build refuses MEMORY_DEBUG together with ENABLE_SANITIZERS.  When
 * this file is nevertheless built with ASan (another build system, or the
 * test_zmalloc unit test), freed blocks are poisoned so a later access is
 * still reported.
 */
#if defined(__SANITIZE_ADDRESS__)
# define ZM_HAVE_ASAN 1
#elif defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define ZM_HAVE_ASAN 1
# endif
#endif
#ifdef ZM_HAVE_ASAN
# include <sanitizer/asan_interface.h>
# define ZM_POISON(addr, size)   ASAN_POISON_MEMORY_REGION((addr), (size_t)(size))
# define ZM_UNPOISON(addr, size) ASAN_UNPOISON_MEMORY_REGION((addr), (size_t)(size))
#else
# define ZM_POISON(addr, size)   ((void)(addr), (void)(size))
# define ZM_UNPOISON(addr, size) ((void)(addr), (void)(size))
#endif

#define NUM_ZBUCKETS 256
#define GET_ZBUCKET(addr) (((long)(addr) >> 3) & 0xFF)

//#define NO_MEMORY_PADDING

/*
 * Strictest fundamental alignment, as malloc() guarantees it.  The block
 * handed out is calloc() + sizeof(beginPad), so the begin pad must be a
 * multiple of this or every MEMORY_DEBUG allocation is misaligned (it was 4
 * bytes: a struct holding a double, long or pointer then sat at an address
 * % 8 == 4, which is undefined behaviour, a SIGBUS on strict-alignment
 * CPUs and a UBSan "misaligned address" report on nearly every CREATE).
 * max_align_t is C11; this union stands in for it in the C99 build.
 */
typedef union zm_max_align {
  long double ld;
  long long ll;
  double d;
  void *p;
  void (*fp)(void);
} zm_max_align;
struct zm_align_probe { char c; zm_max_align u; };
#define ZM_ALIGN offsetof(struct zm_align_probe, u)

#ifndef NO_MEMORY_PADDING
#define ZM_PAD_BYTES 16
static unsigned char beginPad[ZM_PAD_BYTES] = {
  0xde, 0xad, 0xde, 0xad, 0xde, 0xad, 0xde, 0xad,
  0xde, 0xad, 0xde, 0xad, 0xde, 0xad, 0xde, 0xad };

/* The end pad starts at ret + len, wherever that falls: it is only ever
 * accessed with memcpy/memcmp, so it needs no alignment. */
static unsigned char endPad[ZM_PAD_BYTES] = {
  0xde, 0xad, 0xde, 0xad, 0xde, 0xad, 0xde, 0xad,
  0xde, 0xad, 0xde, 0xad, 0xde, 0xad, 0xde, 0xad };

/* Compile-time: the begin pad keeps calloc()'s alignment. */
typedef char zm_pad_keeps_alignment[(sizeof(beginPad) % ZM_ALIGN == 0) ? 1 : -1];
#endif

/* Runtime backstop for the same invariant (calloc() itself misbehaving). */
#define ZM_CHECK_ALIGNED(ptr, file, line) do { \
  if (((uintptr_t)(ptr)) % ZM_ALIGN != 0) \
    ZM_FATAL("zmalloc-align", (file), (line), \
             "addr=%p is not %zu-byte aligned", (void *)(ptr), \
             (size_t)ZM_ALIGN); \
} while (0)

static FILE *zfd = NULL;

static void zlogf(const char *format, ...)
  __attribute__ ((format (printf, 1, 2)));
static void zlogf(const char *format, ...)
{
  va_list args;

  if (zfd == NULL || format == NULL)
    return;

  va_start(args, format);
  if (vfprintf(zfd, format, args) < 0)
    TB_REPORT_ERRNO("vfprintf", "zmalloc.log");
  va_end(args);
}

/*
 * Detected heap misuse (double free, freeing an untracked pointer, guard pad
 * overwritten) means the program's ownership state is already corrupt, so it
 * is reported through the structured reporter -- on stderr, whether or not
 * zmalloc.log could be opened -- at the caller's file:line, and the process
 * aborts.  zmalloc.log keeps the verbose trace and hex dumps.
 */
#define ZM_FATAL(op, file, line, ...) do { \
  tb_report_domain_errorf_at(TB_SEV_FATAL, TB_ERR_INVARIANT, (op), 0, \
                             (file), (line), __func__, __VA_ARGS__); \
  if (zfd) \
    (void)fflush(zfd); \
  abort(); \
} while (0)

typedef struct meminfo {
  struct meminfo *next;
  size_t size; /* number of bytes malloced */
  unsigned char *addr;  /* address of memory returned */
  int frees; /* number of times that 'free' was called on this memory */
  char *file; /* file where malloc was called */
  int line; /* line in the code where malloc was called */
} meminfo;

static meminfo *memlist[NUM_ZBUCKETS];

/*
 * 0 = only end summary
 * 1 = show errors
 * 2 = errors with dumps
 * 3 = all of the above plus all mallocs/frees
 */
static int zmalloclogging = 2;

/* functions: */
unsigned char *zmalloc(size_t len, char *file, int line);
unsigned char *zrealloc(unsigned char *what, size_t len, char *file, int line);
unsigned char *zcalloc(size_t count, size_t size, char *file, int line);
void zdump(meminfo *m);
void zfree(unsigned char *what, char *file, int line);
char *zstrdup(const char *src, char *file, int line);
void zmalloc_init(void);
void zmalloc_check(void);
void pad_check(meminfo *m);
void zmalloc_free_list(meminfo *m);


void zmalloc_init(void)
{
  int i;

  for (i = 0; i < NUM_ZBUCKETS; i++)
    memlist[i] = NULL;

  zfd = fopen("zmalloc.log","w+");
  if (zfd == NULL)
    TB_REPORT_ERRNO("fopen", "zmalloc.log");
}


void zdump(meminfo *m)
{
#define MAX_ZDUMP_SIZE 32
  const unsigned char *hextab = (unsigned char *)"0123456789ABCDEF";
  unsigned char hexline[37], ascline[17], *hexp, *ascp, *inp;
  int len, c = 1;

  if (m->addr == NULL || m->size == 0)
    return;
  /* Dumping a freed (poisoned) block is diagnostic, not a program access. */
  if (m->frees > 0)
    ZM_UNPOISON(m->addr, m->size);

  hexp = hexline;
  ascp = ascline;
  inp = m->addr;
  len = (int)(m->size > MAX_ZDUMP_SIZE ? MAX_ZDUMP_SIZE : m->size);

  for ( ; len > 0; len--, inp++, c++) {
    *(hexp++) = hextab[(int) (*inp & 0xF0) >> 4];	/* high 4 bit */
    *(hexp++) = hextab[(int) (*inp & 0x0F)];		/* low 4 bit */
    if (c % 4 == 0) *(hexp++) = ' ';
    *(ascp++) = isprint(*inp) ? *inp : '.';
    if (c % 16 == 0 || len <= 1) {
      *hexp = '\0';
      *ascp = '\0';
      zlogf( "     %-40.40s%s\n", hexline, ascline);
      hexp = hexline;
      ascp = ascline;
    }
  }
  zlogf( "\n");
}


/* Bytes of guard padding around each block. */
#ifndef NO_MEMORY_PADDING
# define ZM_PAD_SIZE  sizeof(beginPad)
# define ZM_PAD_TOTAL (sizeof(beginPad) + sizeof(endPad))
#else
# define ZM_PAD_SIZE  ((size_t)0)
# define ZM_PAD_TOTAL ((size_t)0)
#endif

/* Convert between the pointer the caller holds (user) and the pointer the
 * C library allocated (base, ZM_PAD_SIZE bytes earlier).  Freeing or
 * reallocating the user pointer instead of the base one is an invalid free
 * that glibc and ASan abort on, so every libc call goes through ZM_BASE().
 * Each evaluates its argument once. */
#define ZM_BASE(user_ptr) ((unsigned char *)(user_ptr) - ZM_PAD_SIZE)
#define ZM_USER(base_ptr) ((unsigned char *)(base_ptr) + ZM_PAD_SIZE)

/* Sizes are size_t end to end.  They used to be int, so a request of
 * 2 GiB or more became negative and `len + padding` wrapped to a few bytes
 * (calloc(0xFFFFFFFC, 1) returned a 4-byte block).  A request that cannot
 * be padded without overflowing is reported and refused (NULL), like any
 * other allocation failure. */
static int zm_size_ok(size_t len, const char *file, int line, const char *func)
{
  if (len > SIZE_MAX - ZM_PAD_TOTAL) {
    tb_report_domain_errorf_at(TB_SEV_ERROR, TB_ERR_INVARIANT, "zmalloc-size",
                               0, file, line, func, "request of %zu bytes "
                               "plus %zu bytes of padding overflows size_t",
                               len, ZM_PAD_TOTAL);
    return 0;
  }
  return 1;
}

/* calloc(count, size) under MEMORY_DEBUG: the multiplication is checked
 * here (the old macro expanded to zmalloc((n*x)) on int, unparenthesised). */
unsigned char *zcalloc(size_t count, size_t size, char *file, int line)
{
  if (size != 0 && count > SIZE_MAX / size) {
    tb_report_domain_errorf_at(TB_SEV_ERROR, TB_ERR_INVARIANT, "zmalloc-size",
                               0, file, line, __func__, "calloc(%zu, %zu) "
                               "overflows size_t", count, size);
    return NULL;
  }
  return zmalloc(count * size, file, line);	/* zmalloc zero-fills */
}

unsigned char *zmalloc(size_t len, char *file, int line)
{
  unsigned char *ret;
  meminfo *m;

  if (!zm_size_ok(len, file, line, __func__))
    return NULL;

  ret = (unsigned char *) calloc(1, len + ZM_PAD_TOTAL);

  if (!ret) {
    /* Not misuse: the caller gets NULL and its own failure path runs. */
    tb_report_domain_errorf_at(TB_SEV_ERROR, TB_ERR_INVARIANT, "zmalloc-alloc",
                               errno, file, line, __func__, "%zu bytes", len);
    zlogf("zmalloc: malloc FAILED\n");
    return NULL;
  }
#ifndef NO_MEMORY_PADDING
  /* insert begin and end padding to detect buffer under/overruns: */
  memcpy(ret, beginPad, sizeof(beginPad));
  ret = ZM_USER(ret);  /* make ret skip begin pad */
  ZM_CHECK_ALIGNED(ret, file, line);
  memcpy(ret + len, endPad, sizeof(endPad));
#endif

  if (zmalloclogging > 2)
    zlogf("zmalloc: 0x%p  %zu bytes %s:%d\n", (void *)ret, len, file, line);

  m = (meminfo *) calloc(1, sizeof(meminfo));
  if (!m) {
    tb_report_domain_errorf_at(TB_SEV_ERROR, TB_ERR_INVARIANT, "zmalloc-alloc",
                               errno, file, line, __func__,
                               "bookkeeping record for %zu bytes", len);
    zlogf("zmalloc: FAILED mem alloc for zmalloc struct... bailing!\n");
    free(ZM_BASE(ret));
    return NULL;
  }
  m->addr = ret;
  m->size = len;
  m->frees = 0;
  m->file = strdup(file);
  if (!m->file) {
    tb_report_domain_errorf_at(TB_SEV_ERROR, TB_ERR_INVARIANT, "zmalloc-alloc",
                               errno, file, line, __func__,
                               "bookkeeping record for %zu bytes", len);
    zlogf("zmalloc: FAILED mem alloc for zmalloc struct... bailing!\n");
    free(m);
    free(ZM_BASE(ret));
    return NULL;
  }
  m->line = line;
  m->next = memlist[GET_ZBUCKET(ret)];
  memlist[GET_ZBUCKET(ret)] = m;
  return (ret);
}

unsigned char *zrealloc(unsigned char *what, size_t len, char *file, int line)
{
  unsigned char *ret;
  meminfo *m, *prev_m;

  if (!zm_size_ok(len, file, line, __func__))
    return NULL;

  if (what) {
    for (prev_m = NULL, m = memlist[GET_ZBUCKET(what)]; m; prev_m = m, m = m->next) {
      if (m->addr == what) {
        /* zfree() keeps the record (it never releases memory), so a
         * realloc of a freed block would otherwise go ahead on memory the
         * program already gave up: a use-after-free that neither ASan nor
         * Valgrind can see under MEMORY_DEBUG. */
        if (m->frees > 0)
          ZM_FATAL("zrealloc-after-free", file, line,
                   "addr=%p size=%zu allocated=%s:%d freed %d time(s)",
                   (void *)m->addr, m->size, m->file, m->line, m->frees);
	ret = (unsigned char *) realloc(ZM_BASE(what), len + ZM_PAD_TOTAL);
	if (!ret) {
	  /* Like realloc(): the old block is untouched and still tracked. */
	  tb_report_domain_errorf_at(TB_SEV_ERROR, TB_ERR_INVARIANT,
	                             "zmalloc-alloc", errno, file, line, __func__,
	                             "realloc of addr=%p (%zu bytes, allocated "
	                             "%s:%d) to %zu bytes", (void *)m->addr,
	                             m->size, m->file, m->line, len);
	  zlogf("zrealloc: FAILED for 0x%p %zu bytes mallocd at %s:%d,\n"
		      "          %zu bytes reallocd at %s:%d.\n",
		    (void *)m->addr, m->size, m->file, m->line, len, file, line);
	  if (zmalloclogging > 1) zdump(m);
	  return NULL;
	}
#ifndef NO_MEMORY_PADDING
	/* insert begin and end padding to detect buffer under/overruns: */
	memcpy(ret, beginPad, sizeof(beginPad));
	ret = ZM_USER(ret);  /* make ret skip begin pad */
	ZM_CHECK_ALIGNED(ret, file, line);
	memcpy(ret + len, endPad, sizeof(endPad));
#endif
	if (zmalloclogging > 2)
	  zlogf("zrealloc: 0x%p %zu bytes mallocd at %s:%d, %zu bytes reallocd at %s:%d.\n",
		    (void *)m->addr, m->size, m->file, m->line, len, file, line);

	m->addr = ret;
	m->size = len;
	if (m->file) free(m->file);
	m->file = strdup(file);
	m->line = line;

        /* detach node */
        if (prev_m)
          prev_m->next = m->next;
        else
          memlist[GET_ZBUCKET(what)] = m->next;

        /* readd to proper bucket */
        m->next = memlist[GET_ZBUCKET(ret)];
        memlist[GET_ZBUCKET(ret)] = m;

	/* could continue the loop to check for multiply-allocd memory */
	/* but that's highly improbable so lets just return instead. */
	return (ret);
      }
    }
  }

  if (what != NULL) {
    /* Not a block zmalloc handed out (libc-allocated, already released by
     * zmalloc_check, or a wild pointer).  Returning a fresh zeroed block, as
     * this used to, silently dropped the caller's contents and leaked the
     * old block. */
    zlogf("zrealloc: invalid pointer 0x%p, %zu bytes to realloc at %s:%d.\n",
	      (void *)what, len, file, line);
    ZM_FATAL("zrealloc-untracked", file, line,
             "addr=%p was not allocated through zmalloc; %zu bytes requested",
             (void *)what, len);
  }

  /* realloc(NULL, len) is malloc(len). */
  return (zmalloc(len, file, line));
}

/* free() under MEMORY_DEBUG.  Doesn't actually free memory: the block is
 * pad-checked, marked freed (and poisoned under ASan) and kept until
 * zmalloc_check(), so a second free or a realloc of it can be detected and
 * reported with both sites.  free(NULL) is legal and only traced.  Any
 * other misuse is reported and aborts (see the file header). */
void zfree(unsigned char *what, char *file, int line)
{
  meminfo *m;
  int gotit = 0;

  if (!what) {
    zlogf("zfree: ERR: Null pointer free'd: %s:%d.\n", file, line);
    return;
  }

  /* look up allocated mem in list: */
  for (m = memlist[GET_ZBUCKET(what)]; m; m = m->next) {
    if (m->addr == what) {
      /* got it.  Print it if verbose: */
      if (zmalloclogging > 2) {
	zlogf("zfree: Freed 0x%p %zu bytes mallocd at %s:%d, freed at %s:%d\n",
		    (void *)m->addr, m->size, m->file, m->line, file, line);
      }
      /* check the padding: */
      pad_check(m);

      /* note that we freed the memory */
      m->frees++;
      /* The block stays allocated until zmalloc_check(); under ASan make
       * any later access to it a reported use-after-poison. */
      if (m->frees == 1)
        ZM_POISON(m->addr, m->size);

      /* check to see if it was freed > once */
      if (m->frees > 1) {
        zlogf("zfree: ERR: multiple frees! 0x%p %zu bytes\n"
		    "       mallocd at %s:%d, freed at %s:%d.\n",
                    (void *)m->addr, m->size, m->file, m->line, file, line);
	if (zmalloclogging > 1) zdump(m);
        ZM_FATAL("zfree-double", file, line,
                 "addr=%p size=%zu allocated=%s:%d", (void *)m->addr, m->size,
                 m->file, m->line);
      }
      gotit++;
    }
  } /* for.. */

  if (!gotit) {
    zlogf("zfree: ERR: attempt to free unallocated memory 0x%p at %s:%d.\n",
		(void *)what, file, line);
    ZM_FATAL("zfree-unknown", file, line,
             "addr=%p was not allocated through zmalloc", (void *)what);
  }
  if (gotit > 1) {
    /* this shouldn't happen, eh? */
    zlogf("zfree: ERR: Multiply-allocd memory 0x%p.\n", (void *)what);
    ZM_FATAL("zfree-multiple-records", file, line,
             "addr=%p is tracked %d times", (void *)what, gotit);
  }
}


char *zstrdup(const char *src, char *file, int line)
{
  char *result;
#ifndef NO_MEMORY_STRDUP    
  result = (char*)zmalloc(strlen(src) + 1, file, line);
  if (!result)
    return NULL;
  memcpy(result, src, strlen(src) + 1);
  return result;
#else
  result = (char*)malloc(strlen(src) + 1);
  if (!result)
    return NULL;
  memcpy(result, src, strlen(src) + 1);
  return result;
#endif
}


void zmalloc_check()
{
  meminfo *m, *next_m;
  char *admonishemnt;
  size_t total_leak = 0;
  int num_leaks = 0, i;

  zlogf( "\n------------ Checking leaks ------------\n\n");

  for (i = 0; i < NUM_ZBUCKETS; i++) {
    for (m = memlist[i]; m; m = next_m) {
      next_m = m->next;
      if (m->addr != 0 && m->frees <= 0) {
        zlogf("zmalloc: UNfreed memory 0x%p %zu bytes mallocd at %s:%d\n",
		(void *)m->addr, m->size, m->file, m->line);
        if (zmalloclogging > 1) zdump(m);

        /* check padding on un-freed memory too: */
        pad_check(m);

        total_leak += m->size;
        num_leaks++;
      }
      if (m->addr)
        ZM_UNPOISON(m->addr, m->size);
      if (m->addr) free(ZM_BASE(m->addr));
      if (m->file) free(m->file);
      free(m);
    }
  }

  if (total_leak) {
    if (total_leak > 10000)
      admonishemnt = "you must work for Microsoft.";
    else if (total_leak > 5000)
      admonishemnt = "you should be ashamed!";
    else if (total_leak > 2000)
      admonishemnt = "you call yourself a programmer?";
    else if (total_leak > 1000)
      admonishemnt = "the X consortium has a job for you...";
    else
      admonishemnt = "close, but not there yet.";
    zlogf("zmalloc: %d leaks totalling %zu bytes... %s\n",
		num_leaks, total_leak, admonishemnt);
  }
  else {
    zlogf("zmalloc: Congratulations: leak-free code!\n");
  }

  if (zfd) {
    if (fflush(zfd) == EOF)
      TB_REPORT_ERRNO("fflush", "zmalloc.log");
    if (fclose(zfd) == EOF)
      TB_REPORT_ERRNO("fclose", "zmalloc.log");
  }
}


void pad_check(meminfo *m)
{
#ifndef NO_MEMORY_PADDING
  /* An overwritten guard pad means a buffer under- or overrun already
   * happened; the report names the allocation site (the overrun itself is
   * somewhere between allocation and this check). */
  if (memcmp(ZM_BASE(m->addr), beginPad, sizeof(beginPad)) != 0) {
    zlogf("pad_check: ERR: beginPad was modified! (mallocd@ %s:%d)\n", m->file, m->line);
    if (zmalloclogging > 1) zdump(m);
    ZM_FATAL("zmalloc-pad", m->file, m->line,
             "beginPad overwritten (underrun): addr=%p size=%zu",
             (void *)m->addr, m->size);
  }
  if (memcmp(m->addr + m->size, endPad, sizeof(endPad)) != 0) {
    zlogf("pad_check: ERR: endPad was modified! (mallocd@ %s:%d)\n", m->file, m->line);
    if (zmalloclogging > 1) zdump(m);
    ZM_FATAL("zmalloc-pad", m->file, m->line,
             "endPad overwritten (overrun): addr=%p size=%zu",
             (void *)m->addr, m->size);
  }
#endif
}


#ifdef ZTEST
#undef ZMALLOC_H

#include "zmalloc.h"

int main()
{
  unsigned char * tmp;

  zmalloc_init();

/* You should see no error here. */
  tmp = (unsigned char*)malloc(200);
  free(tmp);

/* Multiple frees test */
  tmp = (unsigned char*)malloc(200);
  memcpy(tmp, "This should show up in the dump but truncated to MAX_ZDUMP_SIZE chars", sizeof("This should show up in the dump but truncated to MAX_ZDUMP_SIZE chars"));
  free(tmp);
  free(tmp);

/* Free unallocated mem test */
  tmp += 4;
  free(tmp);

/* Unfreed mem test... You should see "UNfreed mem at line 370" (at end) because of this */
  tmp = (unsigned char*)malloc(200);
  memcpy(tmp, "This is unfreed memory!", sizeof("This is unfreed memory!"));

/* Buffer overrun test... You should see an ERR:endPad here */
  tmp = (unsigned char*)malloc(200);
  tmp[202] = 0xbb;
  free(tmp);

/* Buffer underrun test... You should see an ERR:beginPad here */
  tmp = (unsigned char*)malloc(200);
  tmp[-3] = 0x0f;
  free(tmp);

/* Free NULL pointer test... */
  tmp = NULL;
  free(tmp);

  printf("Test completed. See zmalloc.log for the messages.\n");

  zmalloc_check();
  exit(0);
}
#endif /* ZTEST */
