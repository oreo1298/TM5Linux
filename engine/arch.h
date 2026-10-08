/* TM5 for Linux - CPU specific memory primitives.
 * SPDX-License-Identifier: MIT
 *
 * Writes use non-temporal (streaming) stores so they bypass the CPU caches and really reach the
 * DRAM. Without this, small test blocks would be served from the L3 cache and nothing would be
 * tested. Other architectures fall back to ordinary stores.
 */
#ifndef TM5L_ARCH_H
#define TM5L_ARCH_H

#include <stdint.h>
#include <string.h>

#if defined(__x86_64__) || (defined(__i386__) && defined(__SSE2__))
#include <emmintrin.h>
#define TM5L_X86 1
#endif

/* Store one 64-byte line, bypassing the cache. */
static inline void nt_line(void *dst, const uint64_t *src)
{
#ifdef TM5L_X86
    __m128i *d = (__m128i *)dst;
    const __m128i *s = (const __m128i *)src;
    _mm_stream_si128(d + 0, _mm_load_si128(s + 0));
    _mm_stream_si128(d + 1, _mm_load_si128(s + 1));
    _mm_stream_si128(d + 2, _mm_load_si128(s + 2));
    _mm_stream_si128(d + 3, _mm_load_si128(s + 3));
#else
    memcpy(dst, src, 64);
#endif
}

/* MirrorMove: copy a line with its 64-bit words in reverse order and all bits inverted. */
static inline void copy_line_rev64_inv(void *dst, const void *src)
{
    const uint64_t *s = (const uint64_t *)src;
#if defined(__x86_64__)
    long long *d = (long long *)dst;
    for (int k = 0; k < 8; k++)
        _mm_stream_si64(d + k, (long long)~s[7 - k]);
#else
    uint64_t t[8];
    for (int k = 0; k < 8; k++)
        t[k] = ~s[7 - k];
    memcpy(dst, t, 64);
#endif
}

/* MirrorMove128: copy a line with its 128-bit lanes in reverse order and all bits inverted. */
static inline void copy_line_rev128_inv(void *dst, const void *src)
{
#ifdef TM5L_X86
    const __m128i *s = (const __m128i *)src;
    __m128i *d = (__m128i *)dst;
    const __m128i ones = _mm_set1_epi32(-1);
    __m128i a = _mm_load_si128(s + 3), b = _mm_load_si128(s + 2);
    __m128i c = _mm_load_si128(s + 1), e = _mm_load_si128(s + 0);
    _mm_stream_si128(d + 0, _mm_xor_si128(a, ones));
    _mm_stream_si128(d + 1, _mm_xor_si128(b, ones));
    _mm_stream_si128(d + 2, _mm_xor_si128(c, ones));
    _mm_stream_si128(d + 3, _mm_xor_si128(e, ones));
#else
    const uint64_t *s = (const uint64_t *)src;
    uint64_t t[8];
    for (int l = 0; l < 4; l++) {
        t[2 * l] = ~s[2 * (3 - l)];
        t[2 * l + 1] = ~s[2 * (3 - l) + 1];
    }
    memcpy(dst, t, 64);
#endif
}

/* Make all previous streaming stores globally visible, and stop the compiler from reordering. */
static inline void store_fence(void)
{
#ifdef TM5L_X86
    _mm_sfence();
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
    __asm__ __volatile__("" ::: "memory");
}

/* Evict a cache line so the next read has to come from DRAM. */
static inline void flush_line(const void *p)
{
#ifdef TM5L_X86
    _mm_clflush(p);
    _mm_mfence();
#elif defined(__aarch64__)
    __asm__ __volatile__("dc civac, %0\n\tdsb sy" ::"r"(p) : "memory");
#else
    (void)p;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

#endif
