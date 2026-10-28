/**
 * @file asan_annotations.h
 * AddressSanitizer poisoning for tbaMUD's own allocators.
 *
 * A custom allocator hides bugs from ASan: memory that zfree() keeps, or a
 * large output buffer parked in comm.c's bufpool, is still a live heap block
 * as far as ASan knows, so a use-after-free into it goes unreported. Poison
 * such memory while the allocator owns it and unpoison it when it is handed
 * back out:
 *
 *   TB_ASAN_POISON(ptr, size);     the caller must not touch [ptr, ptr+size)
 *   TB_ASAN_UNPOISON(ptr, size);   the region is usable again
 *
 * An access to poisoned memory aborts with "use-after-poison" and the stack
 * of the access. Outside ASan builds both macros evaluate their arguments
 * and do nothing, so they cost nothing in production.
 *
 * Rules: poison only memory the allocator itself owns (never a caller's
 * live object), unpoison before the allocator reads or frees it, and keep
 * list links outside the poisoned range (the pool walks them).
 */
#ifndef TBAMUD_ASAN_ANNOTATIONS_H
#define TBAMUD_ASAN_ANNOTATIONS_H

#include <stddef.h>

#if defined(__SANITIZE_ADDRESS__)
#define TB_ASAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TB_ASAN_ENABLED 1
#endif
#endif

#ifdef TB_ASAN_ENABLED
#include <sanitizer/asan_interface.h>
#define TB_ASAN_POISON(addr, size) \
  ASAN_POISON_MEMORY_REGION((addr), (size_t)(size))
#define TB_ASAN_UNPOISON(addr, size) \
  ASAN_UNPOISON_MEMORY_REGION((addr), (size_t)(size))
#else
#define TB_ASAN_POISON(addr, size) ((void)(addr), (void)(size))
#define TB_ASAN_UNPOISON(addr, size) ((void)(addr), (void)(size))
#endif

#endif /* TBAMUD_ASAN_ANNOTATIONS_H */
