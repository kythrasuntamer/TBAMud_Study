/**
* @file sysdep.h
* Machine-specific defs based on values in conf.h (from configure)
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.
*                                                                        
* All rights reserved.  See license for complete information.                                                                
* Copyright (C) 1993, 94 by the Trustees of the Johns Hopkins University 
* CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               
*/
#ifndef TBAMUD_SYSDEP_H_INCLUDED
#define TBAMUD_SYSDEP_H_INCLUDED

/* Configurables: tbaMUD uses the crypt(3) function to encrypt player passwords
 * in the players file so that they are never stored in plaintext form. However,
 * due to U.S. export restrictions on machine-readable cryptographic software, 
 * the crypt() function is not available on some operating systems such as 
 * FreeBSD.  By default, the 'configure' script will determine if you have 
 * crypt() available and enable or disable password encryption appropriately.  
 * #define NOCRYPT (by uncommenting the line below) if you'd like to explicitly
 * disable password encryption (i.e., if you have moved your MUD from an OS that
 * does not support encryption to one that does). */
/* #define NOCRYPT */

/* If you are porting tbaMUD to a new (untested) platform and you find that 
 * POSIX-standard non-blocking I/O does *not* work, you can define the constant
 * below to work around the problem.  Not having non-blocking I/O can cause the
 * MUD to freeze if someone types part of a command while the MUD waits for the
 * remainder of the command.
 *
 * NOTE: **DO** **NOT** use this constant unless you are SURE you understand
 * exactly what non-blocking I/O is, and you are SURE that your operating system
 * does NOT have it!  (The only UNIX system I've ever seen that has broken POSIX
 * non-blocking I/O is AIX 3.2.)  If your MUD is freezing but you're not sure 
 * why, do NOT use this constant.  Use this constant ONLY if you're sure that 
 * your MUD is freezing because of a non-blocking I/O problem. */
/* #define POSIX_NONBLOCK_BROKEN */

/* The code prototypes library functions to avoid compiler warnings. (Operating
 * system header files *should* do this, but sometimes don't.) However, Circle's
 * prototypes cause the compilation to fail under some combinations of operating
 * systems and compilers. If your compiler reports "conflicting types" for 
 * functions, you need to define this constant to turn off library function 
 * prototyping.  Note, **DO** **NOT** blindly turn on this constant unless you 
 * are sure the problem is type conflicts between my header files and the header
 * files of your operating system.  The error message will look something like
 * this: In file included from comm.c:14:
 *    sysdep.h:207: conflicting types for `random'
 * /usr/local/lib/gcc-lib/alpha-dec-osf3.2/2.7.2/include/stdlib.h:253:
 *    previous declaration of `random' */
/* #define NO_LIBRARY_PROTOTYPES */

/* If using the GNU C library, version 2+, then you can have it trace memory 
 * allocations to check for leaks, uninitialized uses, and bogus free() calls.
 * To see if your version supports it, run:
 * info libc 'Allocation Debugging' 'Tracing malloc'
 * Example usage (Bourne shell):
 *      MALLOC_TRACE=/tmp/circle-trace bin/circle
 * Read the entire "Allocation Debugging" section of the GNU C library
 * documentation before setting this to '1'. */
#define CIRCLE_GNU_LIBC_MEMORY_TRACK	0	/* 0 = off, 1 = on */

/* Do not change anything below this line. */

#if defined(__APPLE__) && defined(__MACH__)
/* Machine-specific dependencies for running on modern macOS systems 10.13+ (High Sierra)
*  Updated by Victor Augusto Borges Dias de Almeida (aka Stoneheart), 26 June 2024.
* 
*  Tested on:
*    - macOS 10.13: High Sierra (Lobo) - September 25, 2017 (Latest: 10.13.6)
*    - macOS 10.14: Mojave (Liberty) - September 24, 2018 (Latest: 10.14.6)
*    - macOS 10.15: Catalina (Jazz) - October 7, 2019 (Latest: 10.15.7)
*    - macOS 11: Big Sur (GoldenGate) - November 12, 2020 (Latest: 11.7.10)
*    - macOS 12: Monterey (Star) - October 25, 2021 (Latest: 12.7)
*    - macOS 13: Ventura (Rome) - November 7, 2022 (Latest: 13.7)
*    - macOS 14: Sonoma (Sunburst) - November 7, 2023 (Latest: 14.3)
*
*  This file works on Apple Silicon Chips (M1, M2, M3) without futher configurations.
*/
#define CIRCLE_MAC_OS   1
#endif

/* Set up various machine-specific things based on the values determined from 
 * configure and conf.h. */

#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#if     (defined (STDC_HEADERS) || defined (__GNU_LIBRARY__) || defined(CIRCLE_MAC_OS))
#include <stdlib.h>

#else   /* No standard headers.  */

#ifdef  HAVE_MEMORY_H
#include <memory.h>
#endif

extern char *malloc(), *calloc(), *realloc();
extern void free();
extern void abort(), exit();

#endif  /* Standard headers.  */

/* POSIX compliance */
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#ifdef CIRCLE_WINDOWS
# include <sys\types.h>
#endif

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

/* Now, we #define POSIX if we have a POSIX system. */

#ifdef HAVE_UNISTD_H
/* Ultrix's unistd.h always defines _POSIX_VERSION, but you only get
   POSIX.1 behavior with `cc -YPOSIX', which predefines POSIX itself!  */
#if defined (_POSIX_VERSION) && !defined (ultrix)
#define POSIX
#endif

/* Some systems define _POSIX_VERSION but are not really POSIX.1.  */
#if (defined (butterfly) || defined (__arm) || \
     (defined (__mips) && defined (_SYSTYPE_SVR3)) || \
     (defined (sequent) && defined (i386)))
#undef POSIX
#endif
#endif /* HAVE_UNISTD_H */

#if !defined (POSIX) && defined (_AIX) && defined (_POSIX_SOURCE)
#define POSIX
#endif

#if defined(_AIX)
#define POSIX_NONBLOCK_BROKEN
#endif

/* Header files common to all source files */
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

/*
 * Checked replacements for legacy atoi()/atol() call sites.
 * They preserve the historical "no conversion => 0" behavior while detecting
 * range errors instead of invoking undefined or implementation-dependent
 * overflow behavior.
 */
static inline long tb_parse_long_or_zero(const char *text)
{
  char *end = NULL;
  long value;

  if (text == NULL)
    return 0;

  errno = 0;
  value = strtol(text, &end, 10);
  if (end == text || errno == ERANGE)
    return 0;

  return value;
}

static inline int tb_parse_int_or_zero(const char *text)
{
  long value = tb_parse_long_or_zero(text);

  if (value < INT_MIN || value > INT_MAX)
    return 0;

  return (int)value;
}

static inline int tb_ascii_space(unsigned char ch)
{
  return ch == ' ' || ch == '\t' || ch == '\n' ||
         ch == '\r' || ch == '\f' || ch == '\v';
}

/*
 * Strict numeric parsers for input where malformed text must not silently
 * become zero.  They require full consumption (aside from ASCII whitespace)
 * and report success explicitly.
 */
static inline int tb_parse_long_checked(const char *text, long *out)
{
  char *end = NULL;
  long value;

  if (text == NULL || out == NULL)
    return 0;

  errno = 0;
  value = strtol(text, &end, 10);
  if (end == text || errno == ERANGE)
    return 0;

  while (*end != '\0' && tb_ascii_space((unsigned char)*end))
    end++;

  if (*end != '\0')
    return 0;

  *out = value;
  return 1;
}

static inline int tb_parse_int_checked(const char *text, int *out)
{
  long value;

  if (out == NULL || !tb_parse_long_checked(text, &value) ||
      value < INT_MIN || value > INT_MAX)
    return 0;

  *out = (int)value;
  return 1;
}

static inline int tb_parse_int_prefix_checked(const char *text, int *out,
                                              const char **end_out)
{
  char *end = NULL;
  long value;

  if (text == NULL || out == NULL)
    return 0;

  errno = 0;
  value = strtol(text, &end, 10);
  if (end == text || errno == ERANGE || value < INT_MIN || value > INT_MAX)
    return 0;

  *out = (int)value;
  if (end_out != NULL)
    *end_out = end;
  return 1;
}

static inline int tb_parse_float_checked(const char *text, float *out)
{
  char *end = NULL;
  float value;

  if (text == NULL || out == NULL)
    return 0;

  errno = 0;
  value = strtof(text, &end);
  if (end == text || errno == ERANGE)
    return 0;

  while (*end != '\0' && tb_ascii_space((unsigned char)*end))
    end++;

  if (*end != '\0')
    return 0;

  *out = value;
  return 1;
}

#ifdef HAVE_NET_ERRNO_H
#include <net/errno.h>
#endif

/* Macintosh */
#ifdef HAVE_SYS_ERRNO_H
#include <sys/errno.h>
#endif

#ifndef CIRCLE_MAC_OS
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif
#endif

#ifdef TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#ifdef HAVE_ASSERT_H
#include <assert.h>
#else
#define assert(arg)
#endif

#ifndef HAVE_STRUCT_IN_ADDR
struct in_addr {
  unsigned long int s_addr;	/* for inet_addr, etc. */
}
#endif

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_SYS_FCNTL_H
#include <sys/fcntl.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif

#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif

#ifdef HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif

#ifdef HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif

#ifdef HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif

#ifdef HAVE_NETDB_H
# include <netdb.h>
#endif

#ifdef HAVE_SIGNAL_H
# ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 2
#  include <signal.h>
#  undef _POSIX_C_SOURCE
# else
#  include <signal.h>	/* GNU libc 6 already defines _POSIX_C_SOURCE. */
# endif
#endif

#ifdef HAVE_SYS_UIO_H
# include <sys/uio.h>
#endif

#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif

/* Basic system dependencies. */
#if CIRCLE_GNU_LIBC_MEMORY_TRACK && !defined(HAVE_MCHECK_H)
#error "Cannot use GNU C library memory tracking without <mcheck.h>"
#endif

/* strcasecmp -> stricmp -> str_cmp */
#if defined(HAVE_STRCASECMP)
# define str_cmp strcasecmp
#elif defined(HAVE_STRICMP)
# define str_cmp stricmp
#endif

/* strncasecmp -> strnicmp -> strn_cmp */
#if defined(HAVE_STRNCASECMP)
# define strn_cmp strncasecmp
#elif defined(HAVE_STRNICMP)
# define strn_cmp strnicmp
#endif

#if !defined(__GNUC__)
# define __attribute__(x)	/* nothing */
#endif

#if defined(__MWERKS__)
# define isascii(c)	(((c) & ~0x7f) == 0)	/* So easy to have, but ... */
#endif

/* Socket/header miscellany. */

#if defined(CIRCLE_WINDOWS)	/* Definitions for Win32 */

# define snprintf _snprintf
# define vsnprintf _vsnprintf
# define PATH_MAX MAX_PATH

# if !defined(__BORLANDC__) && !defined(LCC_WIN32)	/* MSVC */
#  define chdir _chdir
#  pragma warning(disable:4761)		/* Integral size mismatch. */
#  pragma warning(disable:4244)		/* Possible loss of data. */
# endif

# if defined(__BORLANDC__)	/* Silence warnings we don't care about. */
#  pragma warn -par	/* to turn off >parameter< 'ident' is never used. */
#  pragma warn -pia	/* to turn off possibly incorrect assignment. 'if (!(x=a))' */
#  pragma warn -sig	/* to turn off conversion may lose significant digits. */
# endif

# ifndef _WINSOCK2API_	/* Winsock1 and Winsock 2 conflict. */
#  include <winsock.h>
# endif

# ifndef FD_SETSIZE	/* MSVC 6 is reported to have 64. */
#  define FD_SETSIZE		1024
# endif

#elif defined(CIRCLE_VMS)

/* Necessary Definitions For DEC C With DEC C Sockets Under OpenVMS. */
# if defined(DECC)
#  include <stdio.h>
#  include <time.h>
#  include <stropts.h>
#  include <unixio.h>
# endif

#elif !defined(CIRCLE_MACINTOSH) && !defined(CIRCLE_UNIX) && !defined(CIRCLE_ACORN)
# error "You forgot to include conf.h or do not have a valid system define."
#endif

/* SOCKET -- must be after the winsock.h #include. */
#ifdef CIRCLE_WINDOWS
# define CLOSE_SOCKET(sock)	closesocket(sock)
  typedef SOCKET		socket_t;
#else
# define CLOSE_SOCKET(sock)	close(sock)
  typedef int			socket_t;
#endif

#if defined(__cplusplus)	/* C++ */
#define cpp_extern	extern
#else				/* C */
#define cpp_extern	/* Nothing */
#endif

#ifndef CIRCLE_OS_X
/* Guess if we have the getrlimit()/setrlimit() functions */
#if defined(RLIMIT_NOFILE) || defined (RLIMIT_OFILE)
#define HAS_RLIMIT
#if !defined (RLIMIT_NOFILE)
# define RLIMIT_NOFILE RLIMIT_OFILE
#endif
#endif
#endif /*CIRCLE_OS_X*/

/* Make sure we have STDERR_FILENO */
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

/* Make sure we have STDOUT_FILENO too. */
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif

#if !defined(HAVE_SNPRINTF) || !defined(HAVE_VSNPRINTF)
# include "bsd-snprintf.h"
#endif

/*
 * Checked replacements for standard-library calls whose return values must not
 * be silently discarded (CERT ERR33-C).
 *
 * Error-locality rule: report failures at the caller, not merely inside this
 * header.  The public wrappers are macros that inject the source file, line,
 * and function into implementation helpers.
 */
typedef enum {
  TB_SEV_WARN = 0,
  TB_SEV_ERROR,
  TB_SEV_FATAL
} tb_error_severity;

typedef enum {
  TB_ERR_GENERAL = 0,
  TB_ERR_PERSISTENCE,
  TB_ERR_PARSER,
  TB_ERR_NETWORK,
  TB_ERR_SCRIPT,
  TB_ERR_INVARIANT,
  TB_ERR_BOOT
} tb_error_category;

static inline const char *tb_error_severity_name(tb_error_severity severity)
{
  switch (severity) {
  case TB_SEV_WARN:  return "WARN";
  case TB_SEV_FATAL: return "FATAL";
  case TB_SEV_ERROR:
  default:           return "ERROR";
  }
}

static inline const char *tb_error_category_name(tb_error_category category)
{
  switch (category) {
  case TB_ERR_PERSISTENCE: return "PERSISTENCE";
  case TB_ERR_PARSER:      return "PARSER";
  case TB_ERR_NETWORK:     return "NETWORK";
  case TB_ERR_SCRIPT:      return "SCRIPT";
  case TB_ERR_INVARIANT:   return "INVARIANT";
  case TB_ERR_BOOT:        return "BOOT";
  case TB_ERR_GENERAL:
  default:                 return "GENERAL";
  }
}

static inline tb_error_category tb_error_category_for_operation(const char *operation)
{
  if (operation == NULL)
    return TB_ERR_GENERAL;

  if (!strcmp(operation, "fopen") || !strcmp(operation, "fclose") ||
      !strcmp(operation, "fflush") || !strcmp(operation, "fwrite") ||
      !strcmp(operation, "fread") || !strcmp(operation, "fprintf") ||
      !strcmp(operation, "vfprintf") || !strcmp(operation, "fputs") ||
      !strcmp(operation, "fputc") || !strcmp(operation, "rename") ||
      !strcmp(operation, "remove") || !strcmp(operation, "unlink") ||
      !strcmp(operation, "mkdir") || !strcmp(operation, "stat") ||
      !strcmp(operation, "fsync"))
    return TB_ERR_PERSISTENCE;

  if (!strcmp(operation, "socket") || !strcmp(operation, "bind") ||
      !strcmp(operation, "listen") || !strcmp(operation, "accept") ||
      !strcmp(operation, "connect") || !strcmp(operation, "select") ||
      !strcmp(operation, "recv") || !strcmp(operation, "send") ||
      !strcmp(operation, "read socket") || !strcmp(operation, "write socket") ||
      !strcmp(operation, "setsockopt") || !strcmp(operation, "gethostbyaddr") ||
      !strcmp(operation, "getaddrinfo"))
    return TB_ERR_NETWORK;

  if (!strcmp(operation, "parse") || !strcmp(operation, "strtol") ||
      !strcmp(operation, "strtoul") || !strcmp(operation, "strtof") ||
      !strcmp(operation, "sscanf") || !strcmp(operation, "fscanf"))
    return TB_ERR_PARSER;

  if (!strcmp(operation, "script") || !strcmp(operation, "trigger") ||
      !strcmp(operation, "exec") || !strcmp(operation, "execve"))
    return TB_ERR_SCRIPT;

  return TB_ERR_GENERAL;
}

/*
 * Best-effort symbolic errno names.  Keep this allocation-free and side-effect
 * free: the error reporter must remain usable when memory, stdio, or another
 * subsystem is already damaged.
 */
static inline const char *tb_errno_name(int errnum)
{
  switch (errnum) {
#ifdef E2BIG
  case E2BIG: return "E2BIG";
#endif
#ifdef EACCES
  case EACCES: return "EACCES";
#endif
#ifdef EADDRINUSE
  case EADDRINUSE: return "EADDRINUSE";
#endif
#ifdef EADDRNOTAVAIL
  case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
#endif
#ifdef EAGAIN
  case EAGAIN: return "EAGAIN";
#endif
#ifdef EBADF
  case EBADF: return "EBADF";
#endif
#ifdef EBUSY
  case EBUSY: return "EBUSY";
#endif
#ifdef ECONNABORTED
  case ECONNABORTED: return "ECONNABORTED";
#endif
#ifdef ECONNREFUSED
  case ECONNREFUSED: return "ECONNREFUSED";
#endif
#ifdef ECONNRESET
  case ECONNRESET: return "ECONNRESET";
#endif
#ifdef EEXIST
  case EEXIST: return "EEXIST";
#endif
#ifdef EFAULT
  case EFAULT: return "EFAULT";
#endif
#ifdef EFBIG
  case EFBIG: return "EFBIG";
#endif
#ifdef EINTR
  case EINTR: return "EINTR";
#endif
#ifdef EINVAL
  case EINVAL: return "EINVAL";
#endif
#ifdef EIO
  case EIO: return "EIO";
#endif
#ifdef EISDIR
  case EISDIR: return "EISDIR";
#endif
#ifdef EMFILE
  case EMFILE: return "EMFILE";
#endif
#ifdef ENFILE
  case ENFILE: return "ENFILE";
#endif
#ifdef ENAMETOOLONG
  case ENAMETOOLONG: return "ENAMETOOLONG";
#endif
#ifdef ENETDOWN
  case ENETDOWN: return "ENETDOWN";
#endif
#ifdef ENETUNREACH
  case ENETUNREACH: return "ENETUNREACH";
#endif
#ifdef ENOENT
  case ENOENT: return "ENOENT";
#endif
#ifdef ENOMEM
  case ENOMEM: return "ENOMEM";
#endif
#ifdef ENOSPC
  case ENOSPC: return "ENOSPC";
#endif
#ifdef ENOTDIR
  case ENOTDIR: return "ENOTDIR";
#endif
#ifdef ENOTEMPTY
  case ENOTEMPTY: return "ENOTEMPTY";
#endif
#ifdef ENOTSOCK
  case ENOTSOCK: return "ENOTSOCK";
#endif
#ifdef ENOTSUP
  case ENOTSUP: return "ENOTSUP";
#endif
#ifdef EPERM
  case EPERM: return "EPERM";
#endif
#ifdef EPIPE
  case EPIPE: return "EPIPE";
#endif
#ifdef EROFS
  case EROFS: return "EROFS";
#endif
#ifdef ETIMEDOUT
  case ETIMEDOUT: return "ETIMEDOUT";
#endif
  case 0: return "NONE";
  default: return "UNKNOWN";
  }
}

/*
 * Escape record-breaking/control characters so each diagnostic is exactly one
 * physical line.  That makes logs grep-friendly and prevents untrusted context
 * strings from forging additional log records.
 */
static inline size_t tb_diag_escape(char *dst, size_t dst_size, const char *src)
{
  size_t used = 0;
  const unsigned char *p = (const unsigned char *)(src ? src : "(null)");

  if (dst == NULL || dst_size == 0)
    return 0;

  while (*p != '\0' && used + 1 < dst_size) {
    unsigned char ch = *p++;

    if (ch == '\\' || ch == '"') {
      if (used + 2 >= dst_size)
        break;
      dst[used++] = '\\';
      dst[used++] = (char)ch;
    } else if (ch == '\n') {
      if (used + 2 >= dst_size)
        break;
      dst[used++] = '\\';
      dst[used++] = 'n';
    } else if (ch == '\r') {
      if (used + 2 >= dst_size)
        break;
      dst[used++] = '\\';
      dst[used++] = 'r';
    } else if (ch == '\t') {
      if (used + 2 >= dst_size)
        break;
      dst[used++] = '\\';
      dst[used++] = 't';
    } else if (ch < 0x20 || ch == 0x7f) {
      if (used + 1 >= dst_size)
        break;
      dst[used++] = '?';
    } else {
      dst[used++] = (char)ch;
    }
  }

  dst[used] = '\0';
  return used;
}

/*
 * Emit diagnostics through the lowest-risk path available.  POSIX builds use
 * write(2), retrying EINTR and handling partial writes.  This deliberately
 * avoids dependence on the state of the stdio stderr object.  Other platforms
 * fall back to stdio, with return values checked locally.
 */
static inline int tb_diag_emit(const char *record, size_t length)
{
  int saved_errno = errno;
  int ok = 1;

  if (record == NULL) {
    errno = saved_errno;
    return 0;
  }

#if defined(HAVE_UNISTD_H) || defined(POSIX)
  {
    size_t offset = 0;

    while (offset < length) {
      ssize_t written = write(STDERR_FILENO, record + offset, length - offset);

      if (written > 0) {
        offset += (size_t)written;
        continue;
      }
      if (written < 0 && errno == EINTR)
        continue;

      ok = 0;
      break;
    }
  }
#else
  {
    FILE *err_stream = stderr;

    if (err_stream == NULL ||
        fwrite(record, 1, length, err_stream) != length ||
        fflush(err_stream) == EOF)
      ok = 0;
  }
#endif

  errno = saved_errno;
  return ok;
}

static inline void tb_report_domain_error_at(tb_error_severity severity,
                                             tb_error_category category,
                                             const char *operation,
                                             const char *context,
                                             int errnum,
                                             const char *source_file,
                                             int source_line,
                                             const char *source_func)
{
  int caller_errno = errno;
  char record[4096];
  char escaped_op[256];
  char escaped_ctx[1024];
  char escaped_file[512];
  char escaped_func[256];
  char escaped_error[512];
  char timestamp[32] = "unknown";
  const char *error_text = "none";
  const char *errno_name = tb_errno_name(errnum);
  long process_id = -1;
  time_t now;
  struct tm *utc_tm;
  int result;

  (void)tb_diag_escape(escaped_op, sizeof(escaped_op),
                       operation ? operation : "operation");
  (void)tb_diag_escape(escaped_ctx, sizeof(escaped_ctx),
                       context ? context : "(no context)");
  (void)tb_diag_escape(escaped_file, sizeof(escaped_file),
                       source_file ? source_file : "(unknown file)");
  (void)tb_diag_escape(escaped_func, sizeof(escaped_func),
                       source_func ? source_func : "(unknown function)");

  if (errnum != 0) {
    const char *candidate = strerror(errnum);
    error_text = candidate ? candidate : "(strerror unavailable)";
  }
  (void)tb_diag_escape(escaped_error, sizeof(escaped_error), error_text);

  now = time(NULL);
  if (now != (time_t)-1) {
    utc_tm = gmtime(&now);
    if (utc_tm != NULL) {
      size_t stamp_len = strftime(timestamp, sizeof(timestamp),
                                  "%Y-%m-%dT%H:%M:%SZ", utc_tm);
      if (stamp_len == 0)
        (void)snprintf(timestamp, sizeof(timestamp), "%ld", (long)now);
    } else {
      (void)snprintf(timestamp, sizeof(timestamp), "%ld", (long)now);
    }
  }

#if defined(HAVE_UNISTD_H) || defined(POSIX)
  process_id = (long)getpid();
#endif

  result = snprintf(record, sizeof(record),
                    "TBERR timestamp=%s pid=%ld severity=%s category=%s "
                    "operation=\"%s\" context=\"%s\" errno=%d errno_name=%s "
                    "error=\"%s\" source=\"%s:%d\" function=\"%s\"\n",
                    timestamp, process_id,
                    tb_error_severity_name(severity),
                    tb_error_category_name(category),
                    escaped_op, escaped_ctx, errnum, errno_name,
                    escaped_error, escaped_file, source_line, escaped_func);

  if (result < 0) {
    static const char fallback[] =
      "TBERR severity=ERROR category=GENERAL operation=\"diagnostic-format\" "
      "context=\"failed to format diagnostic record\"\n";
    (void)tb_diag_emit(fallback, sizeof(fallback) - 1);
  } else {
    size_t length = (size_t)result;

    if (length >= sizeof(record)) {
      static const char suffix[] = " [diagnostic-truncated]\n";
      size_t suffix_len = sizeof(suffix) - 1;

      length = sizeof(record) - 1;
      if (length > suffix_len) {
        length -= suffix_len;
        memcpy(record + length, suffix, suffix_len);
        length += suffix_len;
      }
    }

    (void)tb_diag_emit(record, length);
  }

  /* The caller's error state is part of the diagnostic contract. */
  errno = caller_errno;
}

static inline void tb_report_error_at(const char *operation, const char *context,
                                      int errnum, const char *source_file,
                                      int source_line, const char *source_func)
{
  tb_report_domain_error_at(TB_SEV_ERROR,
                            tb_error_category_for_operation(operation),
                            operation, context, errnum,
                            source_file, source_line, source_func);
}

#define TB_REPORT_ERRNO(operation, context) do { \
  int tb_saved_errno_ = errno; \
  tb_report_error_at((operation), (context), tb_saved_errno_, \
                     __FILE__, __LINE__, __func__); \
} while (0)

#define TB_REPORT_ERROR(operation, context) \
  tb_report_error_at((operation), (context), 0, __FILE__, __LINE__, __func__)

#define TB_REPORT_WARN(category, operation, context) \
  tb_report_domain_error_at(TB_SEV_WARN, (category), (operation), (context), 0, \
                            __FILE__, __LINE__, __func__)

#define TB_REPORT_FATAL(category, operation, context) \
  tb_report_domain_error_at(TB_SEV_FATAL, (category), (operation), (context), 0, \
                            __FILE__, __LINE__, __func__)

#define TB_REPORT_FATAL_ERRNO(category, operation, context) do { \
  int tb_saved_errno_ = errno; \
  tb_report_domain_error_at(TB_SEV_FATAL, (category), (operation), (context), \
                            tb_saved_errno_, __FILE__, __LINE__, __func__); \
} while (0)

#define TB_REPORT_DOMAIN_ERRNO(severity, category, operation, context) do { \
  int tb_saved_errno_ = errno; \
  tb_report_domain_error_at((severity), (category), (operation), (context), \
                            tb_saved_errno_, __FILE__, __LINE__, __func__); \
} while (0)

#define TB_REPORT_DOMAIN_ERROR(severity, category, operation, context) \
  tb_report_domain_error_at((severity), (category), (operation), (context), 0, \
                            __FILE__, __LINE__, __func__)

/*
 * Formatted-context form of tb_report_domain_error_at(), for callers that must
 * name several entities (path, line number, idnum, vnum) in one record.
 * The context is formatted into a fixed 1024-byte buffer; when it does not
 * fit, it ends in "[context-truncated]" so a reader knows text is missing.
 * Not async-signal-safe (vsnprintf), like tb_report_domain_error_at().
 */
static inline void tb_report_domain_errorf_at(tb_error_severity severity,
                                              tb_error_category category,
                                              const char *operation,
                                              int errnum,
                                              const char *source_file,
                                              int source_line,
                                              const char *source_func,
                                              const char *context_fmt, ...)
  __attribute__ ((format (printf, 8, 9)));
static inline void tb_report_domain_errorf_at(tb_error_severity severity,
                                              tb_error_category category,
                                              const char *operation,
                                              int errnum,
                                              const char *source_file,
                                              int source_line,
                                              const char *source_func,
                                              const char *context_fmt, ...)
{
  static const char marker[] = " [context-truncated]";
  int caller_errno = errno;
  char context[1024];
  va_list args;
  int result;

  if (context_fmt == NULL) {
    context[0] = '\0';
  } else {
    va_start(args, context_fmt);
    result = vsnprintf(context, sizeof(context), context_fmt, args);
    va_end(args);
    if (result < 0)
      (void)snprintf(context, sizeof(context), "%s", "(context format failed)");
    else if ((size_t)result >= sizeof(context))
      memcpy(context + sizeof(context) - sizeof(marker), marker, sizeof(marker));
  }

  tb_report_domain_error_at(severity, category, operation, context, errnum,
                            source_file, source_line, source_func);
  errno = caller_errno;
}

/* TB_REPORT_DOMAIN_ERRORF(sev, cat, "op", "path=%s line=%ld", ...) */
#define TB_REPORT_DOMAIN_ERRORF(severity, category, operation, ...) \
  tb_report_domain_errorf_at((severity), (category), (operation), 0, \
                             __FILE__, __LINE__, __func__, __VA_ARGS__)

/* Same, capturing errno at the call site before any argument is evaluated. */
#define TB_REPORT_DOMAIN_ERRNOF(severity, category, operation, ...) do { \
  int tb_saved_errnof_ = errno; \
  tb_report_domain_errorf_at((severity), (category), (operation), \
                             tb_saved_errnof_, __FILE__, __LINE__, __func__, \
                             __VA_ARGS__); \
} while (0)

#define TB_INVARIANT(expr, context) \
  ((expr) ? 1 : \
   (tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_INVARIANT, "invariant", \
                              (context), 0, __FILE__, __LINE__, __func__), 0))

/* format(printf) type-checks every caller's format and arguments. */
static inline void tb_snprintf_at(char *dst, size_t dst_size,
                                  const char *source_file, int source_line,
                                  const char *source_func,
                                  const char *format, ...)
  __attribute__ ((format (printf, 6, 7)));
static inline void tb_snprintf_at(char *dst, size_t dst_size,
                                  const char *source_file, int source_line,
                                  const char *source_func,
                                  const char *format, ...)
{
  va_list args;
  int result;

  if (dst == NULL || dst_size == 0 || format == NULL) {
    tb_report_error_at("snprintf", "invalid argument", 0,
                       source_file, source_line, source_func);
    return;
  }

  va_start(args, format);
  result = vsnprintf(dst, dst_size, format, args);
  va_end(args);

  if (result < 0) {
    int saved_errno = errno;
    dst[0] = '\0';
    tb_report_error_at("vsnprintf", "formatting output", saved_errno,
                       source_file, source_line, source_func);
  }
}

#define tb_snprintf(dst, dst_size, ...) \
  tb_snprintf_at((dst), (dst_size), __FILE__, __LINE__, __func__, __VA_ARGS__)

/* format(printf) type-checks every caller's format and arguments. */
static inline void tb_fprintf_at(FILE *stream,
                                 const char *source_file, int source_line,
                                 const char *source_func,
                                 const char *format, ...)
  __attribute__ ((format (printf, 5, 6)));
static inline void tb_fprintf_at(FILE *stream,
                                 const char *source_file, int source_line,
                                 const char *source_func,
                                 const char *format, ...)
{
  va_list args;
  int result;

  if (stream == NULL || format == NULL) {
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fprintf", "invalid stream or format", 0,
                              source_file, source_line, source_func);
    return;
  }

  va_start(args, format);
  result = vfprintf(stream, format, args);
  va_end(args);

  if (result < 0) {
    int saved_errno = errno;
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "vfprintf", "stdio stream", saved_errno,
                              source_file, source_line, source_func);
  }
}

#define tb_fprintf(stream, ...) \
  tb_fprintf_at((stream), __FILE__, __LINE__, __func__, __VA_ARGS__)

static inline void tb_fputs_at(const char *text, FILE *stream,
                               const char *source_file, int source_line,
                               const char *source_func)
{
  if (text == NULL || stream == NULL) {
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fputs", "invalid text or stream", 0,
                              source_file, source_line, source_func);
    return;
  }
  if (fputs(text, stream) == EOF) {
    int saved_errno = errno;
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fputs", "stdio stream", saved_errno,
                              source_file, source_line, source_func);
  }
}

#define tb_fputs(text, stream) \
  tb_fputs_at((text), (stream), __FILE__, __LINE__, __func__)

static inline void tb_fputc_at(int ch, FILE *stream,
                               const char *source_file, int source_line,
                               const char *source_func)
{
  if (stream == NULL) {
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fputc", "NULL stream", 0,
                              source_file, source_line, source_func);
    return;
  }
  if (fputc(ch, stream) == EOF) {
    int saved_errno = errno;
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fputc", "stdio stream", saved_errno,
                              source_file, source_line, source_func);
  }
}

#define tb_fputc(ch, stream) \
  tb_fputc_at((ch), (stream), __FILE__, __LINE__, __func__)

static inline void tb_fwrite_at(const void *ptr, size_t size, size_t count,
                                FILE *stream, const char *source_file,
                                int source_line, const char *source_func)
{
  if (stream == NULL || (ptr == NULL && size != 0 && count != 0)) {
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fwrite", "invalid buffer or stream", 0,
                              source_file, source_line, source_func);
    return;
  }
  if (count != 0 && fwrite(ptr, size, count, stream) != count) {
    int saved_errno = errno;
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fwrite", "stdio stream", saved_errno,
                              source_file, source_line, source_func);
  }
}

#define tb_fwrite(ptr, size, count, stream) \
  tb_fwrite_at((ptr), (size), (count), (stream), __FILE__, __LINE__, __func__)

static inline void tb_fflush_at(FILE *stream, const char *source_file,
                                int source_line, const char *source_func)
{
  if (fflush(stream) == EOF) {
    int saved_errno = errno;
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fflush", stream ? "stdio stream" : "all output streams",
                              saved_errno, source_file, source_line, source_func);
  }
}

#define tb_fflush(stream) \
  tb_fflush_at((stream), __FILE__, __LINE__, __func__)

static inline void tb_fclose_at(FILE *stream, const char *source_file,
                                int source_line, const char *source_func)
{
  if (stream == NULL) {
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fclose", "NULL stream", 0,
                              source_file, source_line, source_func);
    return;
  }
  if (fclose(stream) == EOF) {
    int saved_errno = errno;
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fclose", "stdio stream", saved_errno,
                              source_file, source_line, source_func);
  }
}

#define tb_fclose(stream) \
  tb_fclose_at((stream), __FILE__, __LINE__, __func__)

/*
 * Status-returning variants for transactional code.  These both report the
 * failure at the call site and let the caller contain/rollback the operation.
 */
static inline int tb_fflush_checked_at(FILE *stream, const char *context,
                                       const char *source_file, int source_line,
                                       const char *source_func)
{
  if (fflush(stream) == EOF) {
    int saved_errno = errno;
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fflush", context ? context : "stdio stream",
                              saved_errno, source_file, source_line, source_func);
    return 0;
  }
  return 1;
}

#define tb_fflush_checked(stream, context) \
  tb_fflush_checked_at((stream), (context), __FILE__, __LINE__, __func__)

static inline int tb_fclose_checked_at(FILE *stream, const char *context,
                                       const char *source_file, int source_line,
                                       const char *source_func)
{
  if (stream == NULL) {
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fclose", context ? context : "NULL stream", 0,
                              source_file, source_line, source_func);
    return 0;
  }
  if (fclose(stream) == EOF) {
    int saved_errno = errno;
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fclose", context ? context : "stdio stream",
                              saved_errno, source_file, source_line, source_func);
    return 0;
  }
  return 1;
}

#define tb_fclose_checked(stream, context) \
  tb_fclose_checked_at((stream), (context), __FILE__, __LINE__, __func__)

/* format(printf) type-checks every caller's format and arguments. */
static inline int tb_fprintf_checked_at(FILE *stream, const char *context,
                                        const char *source_file, int source_line,
                                        const char *source_func,
                                        const char *format, ...)
  __attribute__ ((format (printf, 6, 7)));
static inline int tb_fprintf_checked_at(FILE *stream, const char *context,
                                        const char *source_file, int source_line,
                                        const char *source_func,
                                        const char *format, ...)
{
  va_list args;
  int result;

  if (stream == NULL || format == NULL) {
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fprintf",
                              context ? context : "invalid stream or format",
                              0, source_file, source_line, source_func);
    return 0;
  }

  va_start(args, format);
  result = vfprintf(stream, format, args);
  va_end(args);
  if (result < 0) {
    int saved_errno = errno;
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "vfprintf",
                              context ? context : "stdio stream",
                              saved_errno, source_file, source_line, source_func);
    return 0;
  }
  return 1;
}

#define tb_fprintf_checked(stream, context, ...) \
  tb_fprintf_checked_at((stream), (context), __FILE__, __LINE__, __func__, __VA_ARGS__)

static inline int tb_fwrite_checked_at(const void *ptr, size_t size, size_t count,
                                       FILE *stream, const char *context,
                                       const char *source_file, int source_line,
                                       const char *source_func)
{
  size_t written;

  if (stream == NULL || (ptr == NULL && size != 0 && count != 0)) {
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fwrite",
                              context ? context : "invalid buffer or stream",
                              0, source_file, source_line, source_func);
    return 0;
  }

  written = fwrite(ptr, size, count, stream);
  if (written != count) {
    int saved_errno = errno;
    tb_report_domain_error_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                              "fwrite",
                              context ? context : "stdio stream",
                              saved_errno, source_file, source_line, source_func);
    return 0;
  }
  return 1;
}

#define tb_fwrite_checked(ptr, size, count, stream, context) \
  tb_fwrite_checked_at((ptr), (size), (count), (stream), (context), \
                       __FILE__, __LINE__, __func__)

/*
 * Test-only failure injection. Production builds compile this to a constant
 * false and expose no runtime switch that can sabotage I/O.
 */
#define TB_FAIL_REPLACE_INITIAL  0x01u
#define TB_FAIL_REPLACE_COMMIT   0x02u
#define TB_FAIL_REPLACE_ROLLBACK 0x04u

#ifdef TB_TEST_FAILURE_INJECTION
static unsigned int tb_test_failpoints_ = 0;

static inline void tb_test_set_failpoints(unsigned int mask)
{
  tb_test_failpoints_ = mask;
}

static inline int tb_test_failpoint_active(unsigned int mask)
{
  return (tb_test_failpoints_ & mask) != 0;
}
#else
static inline int tb_test_failpoint_active(unsigned int mask)
{
  (void)mask;
  return 0;
}
#endif

/*
 * Replace a live file with a fully-written temporary file.
 *
 * On POSIX, rename() replaces the destination atomically, so a failure of
 * the first rename() (ENOENT for a missing temp file, EXDEV, EBUSY, ...) is
 * a real error: it is reported with both paths and nothing else is touched.
 *
 * Only when the first rename() fails with EEXIST or EACCES -- what platforms
 * whose rename() cannot replace an existing file (Windows CRT) return -- and
 * the destination exists, it falls back to: dest -> backup, temp -> dest,
 * then backup removed.  That sequence is not atomic: between the two renames
 * the destination does not exist.  The backup is "<dest>.tbreplace.<pid>",
 * never "<dest>.bak" (operators and shopconv keep their own .bak files there,
 * and an older version deleted it), and an existing file of that name is
 * reported and left alone.  A fallback that succeeds is reported as a
 * "rename-fallback" warning, because the first rename did fail.
 *
 * Failure: returns 0 with errno set to the error of the failing step.  Every
 * report names temp, dest and (in the fallback) backup.  If the commit fails
 * and the rollback also fails, "rename-rollback" names the backup path: that
 * file is then the last good copy of dest.
 */
static inline int tb_replace_file_checked_at(const char *temp_path,
                                             const char *dest_path,
                                             const char *source_file,
                                             int source_line,
                                             const char *source_func)
{
  char *backup_path;
  size_t backup_len;
  int first_errno, written;
  struct stat st;

  if (temp_path == NULL || dest_path == NULL) {
    tb_report_error_at("rename", "invalid replacement path", 0,
                       source_file, source_line, source_func);
    return 0;
  }

  if (!tb_test_failpoint_active(TB_FAIL_REPLACE_INITIAL)) {
    if (rename(temp_path, dest_path) == 0)
      return 1;
  } else {
    errno = EACCES;
  }

  first_errno = errno;
  if ((first_errno != EEXIST && first_errno != EACCES) ||
      stat(dest_path, &st) != 0) {
    /* Not the "cannot replace an existing file" case: no fallback. */
    tb_report_domain_errorf_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE, "rename",
                               first_errno, source_file, source_line,
                               source_func, "temp=%s dest=%s", temp_path,
                               dest_path);
    errno = first_errno;
    return 0;
  }

  /* 2 x 20 digits covers any long pid. */
  backup_len = strlen(dest_path) + sizeof(".tbreplace.") + 21;
  backup_path = (char *)malloc(backup_len);
  if (backup_path == NULL) {
    int saved_errno = errno;
    tb_report_domain_errorf_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE, "malloc",
                               saved_errno, source_file, source_line,
                               source_func, "replacement backup path: temp=%s "
                               "dest=%s first_errno=%d", temp_path, dest_path,
                               first_errno);
    errno = first_errno;
    return 0;
  }

  written = snprintf(backup_path, backup_len, "%s.tbreplace.%ld", dest_path,
                     (long)getpid());
  if (written < 0 || (size_t)written >= backup_len) {
    free(backup_path);
    tb_report_domain_errorf_at(TB_SEV_ERROR, TB_ERR_INVARIANT, "snprintf", 0,
                               source_file, source_line, source_func,
                               "replacement backup path: dest=%s", dest_path);
    errno = first_errno;
    return 0;
  }

  if (stat(backup_path, &st) == 0) {
    /* Left by an earlier failed rollback (that report named it): it may be
     * the only good copy, so never delete or overwrite it. */
    tb_report_domain_errorf_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE,
                               "rename-backup-exists", EEXIST, source_file,
                               source_line, source_func, "temp=%s dest=%s "
                               "backup=%s", temp_path, dest_path, backup_path);
    free(backup_path);
    errno = EEXIST;
    return 0;
  }

  if (rename(dest_path, backup_path) != 0) {
    int saved_errno = errno;
    tb_report_domain_errorf_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE, "rename",
                               saved_errno, source_file, source_line,
                               source_func, "dest to backup: temp=%s dest=%s "
                               "backup=%s first_errno=%d", temp_path,
                               dest_path, backup_path, first_errno);
    free(backup_path);
    errno = saved_errno;
    return 0;
  }

  {
    int commit_failed = 0;

    if (tb_test_failpoint_active(TB_FAIL_REPLACE_COMMIT)) {
      errno = EIO;
      commit_failed = 1;
    } else if (rename(temp_path, dest_path) != 0) {
      commit_failed = 1;
    }

    if (commit_failed) {
      int commit_errno = errno;
      int rollback_failed = 0;

      if (tb_test_failpoint_active(TB_FAIL_REPLACE_ROLLBACK)) {
        errno = EIO;
        rollback_failed = 1;
      } else if (rename(backup_path, dest_path) != 0) {
        rollback_failed = 1;
      }

      tb_report_domain_errorf_at(TB_SEV_ERROR, TB_ERR_PERSISTENCE, "rename",
                                 commit_errno, source_file, source_line,
                                 source_func, "commit: temp=%s dest=%s "
                                 "backup=%s", temp_path, dest_path,
                                 backup_path);
      if (rollback_failed) {
        int rollback_errno = errno;
        tb_report_domain_errorf_at(TB_SEV_FATAL, TB_ERR_PERSISTENCE,
                                   "rename-rollback", rollback_errno,
                                   source_file, source_line, source_func,
                                   "dest=%s is missing; last good copy is "
                                   "backup=%s", dest_path, backup_path);
      }
      free(backup_path);
      errno = commit_errno;
      return 0;
    }
  }

  if (remove(backup_path) != 0 && errno != ENOENT) {
    int saved_errno = errno;
    tb_report_domain_errorf_at(TB_SEV_WARN, TB_ERR_PERSISTENCE, "remove",
                               saved_errno, source_file, source_line,
                               source_func, "stale replacement backup=%s "
                               "dest=%s", backup_path, dest_path);
  }

  tb_report_domain_errorf_at(TB_SEV_WARN, TB_ERR_PERSISTENCE, "rename-fallback",
                             first_errno, source_file, source_line,
                             source_func, "first rename failed; replaced via "
                             "backup: temp=%s dest=%s", temp_path, dest_path);
  free(backup_path);
  return 1;
}

#define tb_replace_file_checked(temp_path, dest_path) \
  tb_replace_file_checked_at((temp_path), (dest_path), __FILE__, __LINE__, __func__)

/* format(strftime) makes the compiler check each caller's format literal,
 * just as format(printf) does for the output functions. */
static inline void tb_strftime_at(char *dst, size_t dst_size,
                                  const char *source_file, int source_line,
                                  const char *source_func,
                                  const char *format,
                                  const struct tm *timeptr)
  __attribute__ ((format (strftime, 6, 0)));
static inline void tb_strftime_at(char *dst, size_t dst_size,
                                  const char *source_file, int source_line,
                                  const char *source_func,
                                  const char *format,
                                  const struct tm *timeptr)
{
  if (dst == NULL || dst_size == 0 || format == NULL || timeptr == NULL) {
    tb_report_error_at("strftime", "invalid argument", 0,
                       source_file, source_line, source_func);
    return;
  }
  /* format is the caller's literal, already checked at the call site via the
   * format(strftime) attribute above; forwarding it is the one sanctioned
   * non-literal format in the tree (-Werror=format-nonliteral elsewhere). */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
  size_t written = strftime(dst, dst_size, format, timeptr);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
  if (written == 0) {
    dst[0] = '\0';
    tb_report_error_at("strftime", "buffer too small or conversion failed", 0,
                       source_file, source_line, source_func);
  }
}

#define tb_strftime(dst, dst_size, format, timeptr) \
  tb_strftime_at((dst), (dst_size), __FILE__, __LINE__, __func__, \
                 (format), (timeptr))

/* Function prototypes. */
/* Header files of many OS's do not contain function prototypes for the
 * standard C library functions.  This produces annoying warning messages 
 * (sometimes, a lot of them) on such OS's when compiling with gcc's -Wall.
 *
 * Configuration script has been changed to detect which prototypes exist 
 * already; this header file only prototypes functions that aren't already 
 * prototyped by the system headers.  A clash should be impossible.  This 
 * should give us our strong type-checking back. */

#ifndef NO_LIBRARY_PROTOTYPES

#ifdef NEED_ATOI_PROTO
   int atoi(const char *str);
#endif

#ifdef NEED_ATOL_PROTO
   long atol(const char *str);
#endif

/* bzero is deprecated - use memset() instead. This prototype is needed for 
 * FD_xxx macros on some machines. */
#ifdef NEED_BZERO_PROTO
   void bzero(char *b, int length);
#endif

#ifdef NEED_CRYPT_PROTO
   char *crypt(const char *key, const char *salt);
#endif

#ifdef NEED_FCLOSE_PROTO
   int fclose(FILE *stream);
#endif

#ifdef NEED_FDOPEN_PROTO
   FILE *fdopen(int fd, const char *mode);
#endif

#ifdef NEED_FFLUSH_PROTO
   int fflush(FILE *stream);
#endif

#ifdef NEED_FPRINTF_PROTO
   int fprintf(FILE *strm, const char *format, /* args */ ... );
#endif

#ifdef NEED_FREAD_PROTO
   size_t fread(void *ptr, size_t size, size_t nitems, FILE *stream);
#endif

#ifdef NEED_FSCANF_PROTO
  int fscanf(FILE *strm, const char *format, ...);
#endif

#ifdef NEED_FSEEK_PROTO
   int fseek(FILE *stream, long offset, int ptrname);
#endif

#ifdef NEED_FWRITE_PROTO
  size_t fwrite(const void *ptr, size_t size, size_t nitems, FILE *stream);
#endif

#ifdef NEED_GETPID_PROTO
   pid_t getpid(void);
#endif

#ifdef NEED_PERROR_PROTO
   void perror(const char *s);
#endif

#ifdef NEED_QSORT_PROTO
   void qsort(void *base, size_t nel, size_t width,
          int (*compar) (const void *, const void *));
#endif

#ifdef NEED_REWIND_PROTO
   void rewind(FILE *stream);
#endif

#ifdef NEED_SPRINTF_PROTO
   int sprintf(char *s, const char *format, /* args */ ... );
#endif

#ifdef NEED_SSCANF_PROTO
   int sscanf(const char *s, const char *format, ...);
#endif

#ifdef NEED_STRDUP_PROTO
   char *strdup(const char *txt);
#endif

#ifdef NEED_STRERROR_PROTO
   char *strerror(int errnum);
#endif

#ifndef CIRCLE_MAC_OS
#ifdef NEED_STRLCPY_PROTO
   size_t strlcpy(char *dest, const char *src, size_t copylen);
#endif
#endif

#ifdef NEED_SYSTEM_PROTO
   int system(const char *string);
#endif

#ifdef NEED_TIME_PROTO
   time_t time(time_t *tloc);
#endif

#ifdef NEED_UNLINK_PROTO
   int unlink(const char *path);
#endif

#ifdef NEED_REMOVE_PROTO
   int remove(const char *path);
#endif

#ifdef NEED_ACCEPT_PROTO
   int accept(socket_t s, struct sockaddr *addr, int *addrlen);
#endif

#ifdef NEED_BIND_PROTO
   int bind(socket_t s, const struct sockaddr *name, int namelen);
#endif

#ifdef NEED_CHDIR_PROTO
   int chdir(const char *path);
#endif

#ifdef NEED_CLOSE_PROTO
   int close(int fildes);
#endif

#ifdef NEED_FCNTL_PROTO
   int fcntl(int fildes, int cmd, /* arg */ ...);
#endif

#ifdef NEED_FPUTC_PROTO
   int fputc(char c, FILE *stream);
#endif

#ifdef NEED_FPUTS_PROTO
   int fputs(const char *s, FILE *stream);
#endif

#ifdef NEED_GETPEERNAME_PROTO
   int getpeername(socket_t s, struct sockaddr *name, int *namelen);
#endif

#if defined(HAS_RLIMIT) && defined(NEED_GETRLIMIT_PROTO)
   int getrlimit(int resource, struct rlimit *rlp);
#endif

#ifdef NEED_GETSOCKNAME_PROTO
   int getsockname(socket_t s, struct sockaddr *name, int *namelen);
#endif

#ifdef NEED_GETTIMEOFDAY_PROTO
   void gettimeofday(struct timeval *tp, void * );
#endif

#ifdef NEED_HTONL_PROTO
   ulong htonl(u_long hostlong);
#endif

#ifdef NEED_HTONS_PROTO
   u_short htons(u_short hostshort);
#endif

#if defined(HAVE_INET_ADDR) && defined(NEED_INET_ADDR_PROTO)
   unsigned long int inet_addr(const char *cp);
#endif

#if defined(HAVE_INET_ATON) && defined(NEED_INET_ATON_PROTO)
   int inet_aton(const char *cp, struct in_addr *inp);
#endif

#ifdef NEED_INET_NTOA_PROTO
   char *inet_ntoa(const struct in_addr in);
#endif

#ifdef NEED_LISTEN_PROTO
   int listen(socket_t s, int backlog);
#endif

#ifdef NEED_NTOHL_PROTO
   u_long ntohl(u_long netlong);
#endif

#ifdef NEED_PRINTF_PROTO
   int printf(char *format, ...);
#endif

#ifdef NEED_READ_PROTO
   ssize_t read(int fildes, void *buf, size_t nbyte);
#endif

#ifdef NEED_SELECT_PROTO
   int select(int nfds, fd_set *readfds, fd_set *writefds,
          fd_set *exceptfds, struct timeval *timeout);
#endif

#ifdef NEED_SETITIMER_PROTO
   int setitimer(int which, const struct itimerval *value,
          struct itimerval *ovalue);
#endif

#if defined(HAS_RLIMIT) && defined(NEED_SETRLIMIT_PROTO)
   int setrlimit(int resource, const struct rlimit *rlp);
#endif

#ifdef NEED_SETSOCKOPT_PROTO
   int setsockopt(socket_t s, int level, int optname, const char *optval,
		  int optlen);
#endif

#ifdef NEED_SOCKET_PROTO
   int socket(int domain, int type, int protocol);
#endif

#ifdef NEED_WRITE_PROTO
    ssize_t write(int fildes, const void *buf, size_t nbyte);
#endif

#endif /* NO_LIBRARY_PROTOTYPES */

#endif /* TBAMUD_SYSDEP_H_INCLUDED */

