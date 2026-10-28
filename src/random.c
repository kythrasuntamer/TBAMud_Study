/**************************************************************************
*   File: random.c                                         Part of tbaMUD *
*  Usage: Pseudo-random number generator.                                 *
**************************************************************************/

/* I am bothered by the non-portablility of 'rand' and 'random' -- rand is ANSI
 * C, but on some systems such as Suns, rand has seriously tragic spectral 
 * properties (the low bit alternates between 0 and 1!).  random is better but 
 * isn't supported by all systems.  So, in my quest for portability, I decided 
 * to include this code for a simple but relatively effective random number 
 * generator.  It's not the best RNG code around, but I like it because it's 
 * very short and simple, and for our purposes it's "random enough". -JE */

/* This program is public domain and was written by William S. England. It is 
 * based on an article by: Stephen K. Park and Keith W. Miller.
 * The following is a portable c program for generating random numbers. The 
 * modulus and multipilier have been extensively tested and should not be 
 * changed except by someone who is a professional Lehmer generator writer. 
 * THIS GENERATOR REPRESENTS THE MINIMUM STANDARD AGAINST WHICH OTHER 
 * GENERATORS SHOULD BE JUDGED. */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h" /* for function prototypes */

#define	m  (unsigned long)2147483647
#define	q  (unsigned long)127773

#define	a (unsigned int)16807
#define	r (unsigned int)2836

/* F(z)	= (az)%m
**	= az-m(az/m)
**
** F(z)  = G(z)+mT(z)
** G(z)  = a(z%q)- r(z/q)
** T(z)  = (z/q) - (az/m)
**
** F(z)  = a(z%q)- rz/q+ m((z/q) - a(z/m))
** 	 = a(z%q)- rz/q+ m(z/q) - az */

/* Generator state.  Invariant: 1 <= seed <= m - 1 once circle_srandom() has
 * run.  Outside that range the recurrence breaks: 0 (or any multiple of m)
 * maps to m, which is a fixed point, so every later roll is identical. */
static unsigned long seed;

/**
 * Seed the generator.  Any value is accepted and reduced modulo m
 * (2^31 - 1) into the generator's domain -- comm.c seeds with time(0),
 * which passes m in 2038.  A seed that reduces to 0 (0 itself, m, 2m, ...)
 * would freeze the generator; it is reported ("rng-seed") and replaced by 1.
 */
void circle_srandom(unsigned long initial_seed)
{
    unsigned long reduced = initial_seed % m;

    if (reduced == 0) {
      TB_WARN_MSG("rng", "rng-seed", "circle_srandom",
                  "seed %lu is a multiple of 2^31-1, a fixed point of the "
                  "generator; using 1", initial_seed);
      reduced = 1;
    }
    seed = reduced;
}

/**
 * Next value of the Park-Miller minimal standard generator, in
 * [1, 2^31 - 2].  Not cryptographic and not thread-safe (one global
 * state).  Schrage's method keeps every intermediate within 32 bits for a
 * seed in [1, m - 1]; the arithmetic is done in long long so no step
 * depends on int width or on an implementation-defined conversion.
 */
unsigned long circle_random(void)
{
    long long lo, hi, test;

    hi   = (long long)(seed / q);
    lo   = (long long)(seed % q);

    test = (long long)a * lo - (long long)r * hi;

    if (test > 0)
	seed = (unsigned long)test;
    else
	seed = (unsigned long)(test + (long long)m);

    return (seed);
}
