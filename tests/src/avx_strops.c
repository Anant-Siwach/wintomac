// (avx_* family: reference runs under Rosetta 2.) Inline-asm test for string compares, rcr/rcl and SSE4.2 pcmpistri/pcmpestri/pcmpistrm.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <nmmintrin.h>
static int repe_cmpsb(const char *a, const char *b, unsigned long n, unsigned long *left) {
    unsigned char eq; __asm__ volatile("repe cmpsb" : "=@cce"(eq), "+S"(a), "+D"(b), "+c"(n) :: "memory"); *left = n; return eq;
}
static unsigned long repne_scasb_len(const char *s) {
    unsigned long n = ~0ul; __asm__ volatile("repne scasb" : "+D"(s), "+c"(n) : "a"(0) : "memory", "cc"); return ~n - 1;
}
int main(void) {
    unsigned long left; int eq = repe_cmpsb("hello world", "hello there", 11, &left);
    printf("repe cmpsb: eq=%d remaining=%lu\n", eq, left);
    eq = repe_cmpsb("abcdef", "abcdef", 6, &left); printf("repe cmpsb same: eq=%d remaining=%lu\n", eq, left);
    printf("repne scasb strlen: %lu\n", repne_scasb_len("recompiled!"));
    uint64_t v = 0x8000000000000001ull; unsigned char cf;
    __asm__ volatile("stc; rcr $1, %0; setc %1" : "+r"(v), "=q"(cf)); printf("rcr: %016llx cf=%d\n", (unsigned long long)v, cf);
    uint32_t w = 0x80000000u; __asm__ volatile("clc; rcl $3, %0; setc %1" : "+r"(w), "=q"(cf)); printf("rcl: %08x cf=%d\n", w, cf);
    uint8_t b8 = 0x81; __asm__ volatile("stc; rcl $5, %0; setc %1" : "+q"(b8), "=q"(cf)); printf("rcl8: %02x cf=%d\n", b8, cf);
    __m128i set = _mm_loadu_si128((const __m128i *)"aeiou\0\0\0\0\0\0\0\0\0\0\0");
    __m128i txt = _mm_loadu_si128((const __m128i *)"xyz quick brown!");
    printf("pcmpistri any: %d\n", _mm_cmpistri(set, txt, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY));
    printf("pcmpistri any msb: %d\n", _mm_cmpistri(set, txt, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY | _SIDD_MOST_SIGNIFICANT));
    __m128i rng = _mm_loadu_si128((const __m128i *)"az\0\0\0\0\0\0\0\0\0\0\0\0\0\0");
    printf("pcmpistri ranges negated (first non-lower): %d\n", _mm_cmpistri(rng, txt, _SIDD_UBYTE_OPS | _SIDD_CMP_RANGES | _SIDD_NEGATIVE_POLARITY));
    __m128i sub = _mm_loadu_si128((const __m128i *)"brown\0\0\0\0\0\0\0\0\0\0\0");
    printf("pcmpistri ordered (substring): %d\n", _mm_cmpistri(sub, txt, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ORDERED));
    printf("pcmpestri each len 3/5: %d\n", _mm_cmpestri(txt, 3, _mm_loadu_si128((const __m128i *)"xyQ quick brown!"), 5, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_EACH | _SIDD_NEGATIVE_POLARITY));
    __m128i m = _mm_cmpistrm(set, txt, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY | _SIDD_UNIT_MASK);
    unsigned char mb[16]; _mm_storeu_si128((__m128i *)mb, m); printf("pcmpistrm mask:"); for (int i = 0; i < 16; i++) printf(" %02x", mb[i]); printf("\n");
    printf("flags: cf=%d zf=%d sf=%d\n", _mm_cmpistrc(set, txt, _SIDD_CMP_EQUAL_ANY), _mm_cmpistrz(set, txt, _SIDD_CMP_EQUAL_ANY), _mm_cmpistrs(set, txt, _SIDD_CMP_EQUAL_ANY));
    return 0;
}
