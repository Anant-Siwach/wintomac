// MOVBE (load/store with byte swap, 16/32/64-bit) and F16C round trips as used by PS5 titles (Zen 2 baseline,
// docs/ps5.md). Reference: the same source built for x86_64 macOS and run under Rosetta 2.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>

static uint64_t ld64(const void *p) { uint64_t r; __asm__ volatile("movbe %1, %0" : "=r"(r) : "m"(*(const uint64_t *)p)); return r; }
static uint32_t ld32(const void *p) { uint32_t r; __asm__ volatile("movbe %1, %0" : "=r"(r) : "m"(*(const uint32_t *)p)); return r; }
static uint16_t ld16(const void *p) { uint16_t r; __asm__ volatile("movbe %1, %0" : "=r"(r) : "m"(*(const uint16_t *)p)); return r; }
static void st64(void *p, uint64_t v) { __asm__ volatile("movbe %1, %0" : "=m"(*(uint64_t *)p) : "r"(v)); }
static void st32(void *p, uint32_t v) { __asm__ volatile("movbe %1, %0" : "=m"(*(uint32_t *)p) : "r"(v)); }
static void st16(void *p, uint16_t v) { __asm__ volatile("movbe %1, %0" : "=m"(*(uint16_t *)p) : "r"(v)); }

int main(void) {
    uint8_t buf[32];
    for (int i = 0; i < 32; i++) buf[i] = (uint8_t)(i * 37 + 11);
    uint64_t acc = 0;
    for (int o = 0; o < 16; o++) {   // unaligned offsets too
        acc = acc * 31 + ld64(buf + o);
        acc = acc * 31 + ld32(buf + o);
        acc = acc * 31 + ld16(buf + o);
    }
    printf("load  %016llx\n", (unsigned long long)acc);
    uint8_t out[32]; memset(out, 0xEE, sizeof out);
    st64(out + 1, 0x0123456789ABCDEFull); st32(out + 11, 0xDEADBEEFu); st16(out + 17, 0xCAFE);
    for (int i = 0; i < 24; i++) printf("%02x", out[i]);
    printf("\n");
    // upper bits of the destination register for a 16-bit load stay as they were
    uint64_t r = 0xFFFFFFFFFFFFFFFFull; uint16_t h = 0x1234;
    __asm__ volatile("movbe %1, %w0" : "+r"(r) : "m"(h));
    printf("16-bit merge %016llx\n", (unsigned long long)r);
    // F16C: float <-> half, all rounding modes of vcvtps2ph
    float f[8] = { 1.0f, -2.5f, 65504.0f, 1e-7f, 3.14159265f, -0.0f, 70000.0f, 0.333333f };
    __m256 v = _mm256_loadu_ps(f);
    __m128i h0 = _mm256_cvtps_ph(v, _MM_FROUND_TO_NEAREST_INT), h1 = _mm256_cvtps_ph(v, _MM_FROUND_TO_ZERO);
    __m128i h2 = _mm256_cvtps_ph(v, _MM_FROUND_TO_POS_INF), h3 = _mm256_cvtps_ph(v, _MM_FROUND_TO_NEG_INF);
    uint16_t hs[32]; _mm_storeu_si128((__m128i *)hs, h0); _mm_storeu_si128((__m128i *)(hs + 8), h1);
    _mm_storeu_si128((__m128i *)(hs + 16), h2); _mm_storeu_si128((__m128i *)(hs + 24), h3);
    for (int i = 0; i < 32; i++) printf("%04x%c", hs[i], i % 8 == 7 ? '\n' : ' ');
    float back[8]; _mm256_storeu_ps(back, _mm256_cvtph_ps(h0));
    for (int i = 0; i < 8; i++) printf("%.9g ", back[i]);
    printf("\n");
    return 0;
}
