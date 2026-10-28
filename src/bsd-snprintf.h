/**
* @file bsd-snprintf.h
* Used if your OS does not provide snprintf() or vsnprintf().
* 
* Part of the core tbaMUD source code distribution, which is a derivative
* of, and continuation of, CircleMUD.               
*/

/* This file taken from openbsd-compat of OpenSSH 3.1:
 *
 * --- 8< --- OpenSSH LICENSE --- 8< ---
 * Remaining components of the software are provided under a standard
 * 2-term BSD licence with the following names as copyright holders:
 *
 *      Markus Friedl
 *      Theo de Raadt
 *      Niels Provos
 *      Dug Song
 *      Aaron Campbell
 *      Damien Miller
 *      Kevin Steves
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * --- 8< --- OpenSSH LICENSE --- 8< ---
 */


#ifndef _BSD_SNPRINTF_H_
#define _BSD_SNPRINTF_H_

/*
 * This fallback cannot be used safely and is disabled.  Its snprintf() and
 * vsnprintf() return the number of characters stored, not the C99
 * "would-have-written" length, so every `>= sizeof(buf)` truncation check in
 * the engine and tools could never fire; it lacks the z, j and t length
 * modifiers (a "%zu" left the size_t unconsumed and shifted every later
 * argument); with count == 0 it wrote str[0]; and its 'w' case could step
 * past the format's terminating NUL.  Build on a platform with a C99
 * snprintf()/vsnprintf() (every supported one has them); a platform without
 * them fails here, at compile time, instead of mis-handling truncation at
 * run time.
 */
#if !defined(HAVE_SNPRINTF) || !defined(HAVE_VSNPRINTF)
#error "tbaMUD requires C99 snprintf() and vsnprintf(); the bsd-snprintf fallback does not report truncation and is disabled"
#endif

#ifndef HAVE_SNPRINTF
int snprintf(char *str, size_t count, const char *fmt, ...);
#endif /* !HAVE_SNPRINTF */

#ifndef HAVE_VSNPRINTF
int vsnprintf(char *str, size_t count, const char *fmt, va_list args);
#endif /* !HAVE_SNPRINTF */


#endif /* _BSD_SNPRINTF_H_ */
