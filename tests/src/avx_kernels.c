// AVX/AVX2/FMA/F16C/BMI coverage. Each group prints a hash so a mismatch names the instruction family.
// Reference output: the same source built for x86_64 macOS and run under Rosetta 2.
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t H;
static void mix(const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) { H ^= b[i]; H *= 1099511628211ULL; } }
static void group(const char *name) { printf("%-12s %016llx\n", name, (unsigned long long)H); H = 1469598103934665603ULL; }
#ifdef TRACE
#define MIX(v) (mix(&(v), sizeof(v)), printf("  %d %s %016llx\n", __LINE__, #v, (unsigned long long)H))
#else
#define MIX(v) mix(&(v), sizeof(v))
#endif

static float fa[64], fb[64], fc[64]; static double da[32], db[32]; static int32_t ia[64], ib[64]; static int16_t sa[64]; static uint8_t ba[128];

static void init(void) {
    uint32_t s = 0x12345678;
    for (int i = 0; i < 64; i++) { s = s * 1664525u + 1013904223u; fa[i] = (float)(int32_t)s / 1e7f; fb[i] = (float)(s >> 9) / 3e5f - 7.f; fc[i] = (float)i * 0.37f - 5.f;
        ia[i] = (int32_t)s >> 3; ib[i] = (int32_t)(s * 2654435761u) >> 7; sa[i] = (int16_t)(s >> 11); }
    for (int i = 0; i < 32; i++) { da[i] = fa[i] * 1.0001 + 3; db[i] = fb[2 * i] - fc[i]; }
    for (int i = 0; i < 128; i++) ba[i] = (uint8_t)(i * 37 + (i >> 3));
}

__attribute__((noinline)) static void t_float(void) {
    for (int i = 0; i < 64; i += 8) {
        __m256 a = _mm256_loadu_ps(fa + i), b = _mm256_loadu_ps(fb + i), c = _mm256_loadu_ps(fc + i);
        __m256 r = _mm256_add_ps(_mm256_mul_ps(a, b), _mm256_div_ps(c, _mm256_set1_ps(3.f)));
        r = _mm256_max_ps(_mm256_min_ps(r, _mm256_set1_ps(50.f)), _mm256_set1_ps(-50.f));
        r = _mm256_sqrt_ps(_mm256_andnot_ps(_mm256_set1_ps(-0.f), r));
        __m256 h = _mm256_hadd_ps(a, b), s = _mm256_addsub_ps(a, c), sh = _mm256_shuffle_ps(a, b, 0x9c);
        __m256 ul = _mm256_unpacklo_ps(a, b), uh = _mm256_unpackhi_ps(b, c), bl = _mm256_blend_ps(a, b, 0xa5), bv = _mm256_blendv_ps(a, b, c);
        __m256 rd = _mm256_round_ps(c, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC), dp = _mm256_dp_ps(a, b, 0xf3);
        MIX(r); MIX(h); MIX(s); MIX(sh); MIX(ul); MIX(uh); MIX(bl); MIX(bv); MIX(rd); MIX(dp);
    }
    group("float256");
    for (int i = 0; i < 32; i += 4) {
        __m256d a = _mm256_loadu_pd(da + i), b = _mm256_loadu_pd(db + i);
        __m256d r = _mm256_fmadd_pd(a, b, _mm256_set1_pd(0.5)), h = _mm256_hadd_pd(a, b), sh = _mm256_shuffle_pd(a, b, 0x6);
        __m256d c1 = _mm256_cmp_pd(a, b, _CMP_LT_OQ), c2 = _mm256_cmp_pd(a, b, _CMP_NEQ_UQ);
        __m128 cv = _mm256_cvtpd_ps(a); __m128i ci = _mm256_cvttpd_epi32(b); __m256d back = _mm256_cvtps_pd(cv);
        MIX(r); MIX(h); MIX(sh); MIX(c1); MIX(c2); MIX(cv); MIX(ci); MIX(back);
        printf("%d", _mm256_movemask_pd(c1));
    }
    printf("\n"); group("double256");
}

__attribute__((noinline)) static void t_fma(void) {
    for (int i = 0; i < 64; i += 8) {
        __m256 a = _mm256_loadu_ps(fa + i), b = _mm256_loadu_ps(fb + i), c = _mm256_loadu_ps(fc + i);
        __m256 r1 = _mm256_fmadd_ps(a, b, c), r2 = _mm256_fmsub_ps(a, b, c), r3 = _mm256_fnmadd_ps(a, b, c), r4 = _mm256_fnmsub_ps(a, b, c);
        __m256 r5 = _mm256_fmaddsub_ps(a, b, c), r6 = _mm256_fmsubadd_ps(a, b, c);
        __m128 s1 = _mm_fmadd_ss(_mm256_castps256_ps128(a), _mm256_castps256_ps128(b), _mm256_castps256_ps128(c));
        MIX(r1); MIX(r2); MIX(r3); MIX(r4); MIX(r5); MIX(r6); MIX(s1);
    }
    group("fma");
}

__attribute__((noinline)) static void t_int(void) {
    for (int i = 0; i < 64; i += 8) {
        __m256i a = _mm256_loadu_si256((void *)(ia + i)), b = _mm256_loadu_si256((void *)(ib + i));
        __m256i r[24] = {
            _mm256_add_epi32(a, b), _mm256_sub_epi64(a, b), _mm256_mullo_epi32(a, b), _mm256_mul_epu32(a, b), _mm256_mul_epi32(a, b),
            _mm256_max_epi32(a, b), _mm256_min_epu32(a, b), _mm256_cmpgt_epi32(a, b), _mm256_cmpeq_epi8(a, b), _mm256_cmpgt_epi64(a, b),
            _mm256_slli_epi32(a, 5), _mm256_srai_epi32(b, 7), _mm256_srli_epi64(a, 13), _mm256_sll_epi16(a, _mm_cvtsi32_si128(3)),
            _mm256_sllv_epi32(a, _mm256_and_si256(b, _mm256_set1_epi32(31))), _mm256_srav_epi32(a, _mm256_and_si256(b, _mm256_set1_epi32(63))),
            _mm256_srlv_epi64(a, _mm256_and_si256(b, _mm256_set1_epi64x(63))), _mm256_abs_epi16(a), _mm256_sign_epi8(a, b),
            _mm256_adds_epi16(a, b), _mm256_subs_epu8(a, b), _mm256_avg_epu8(a, b), _mm256_madd_epi16(a, b), _mm256_maddubs_epi16(a, b) };
        __m256i p[16] = {
            _mm256_shuffle_epi8(a, b), _mm256_shuffle_epi32(a, 0x4e), _mm256_shufflelo_epi16(a, 0x1b), _mm256_shufflehi_epi16(b, 0xb1),
            _mm256_unpacklo_epi8(a, b), _mm256_unpackhi_epi16(a, b), _mm256_unpacklo_epi64(a, b), _mm256_alignr_epi8(a, b, 5),
            _mm256_packs_epi32(a, b), _mm256_packus_epi16(a, b), _mm256_permutevar8x32_epi32(a, b), _mm256_permute4x64_epi64(a, 0x93),
            _mm256_permute2x128_si256(a, b, 0x21), _mm256_blend_epi32(a, b, 0x5a), _mm256_blend_epi16(a, b, 0x93), _mm256_bslli_epi128(a, 3) };
        __m256i q[6] = { _mm256_hadd_epi32(a, b), _mm256_sad_epu8(a, b), _mm256_mulhi_epi16(a, b), _mm256_mulhrs_epi16(a, b),
                         _mm256_blendv_epi8(a, b, _mm256_xor_si256(a, b)), _mm256_bsrli_epi128(b, 7) };
        MIX(r); MIX(p); MIX(q);
        printf("%08x ", (unsigned)_mm256_movemask_epi8(r[7]));
        printf("%d%d ", _mm256_testz_si256(a, b), _mm256_testc_si256(a, a));
    }
    printf("\n"); group("int256");
}

__attribute__((noinline)) static void t_cross(void) {
    __m256 a = _mm256_loadu_ps(fa), b = _mm256_loadu_ps(fb);
    __m256 bs = _mm256_broadcast_ss(fa + 3), bsr = _mm256_broadcastss_ps(_mm256_castps256_ps128(b));
    __m256 ins = _mm256_insertf128_ps(a, _mm256_castps256_ps128(b), 1); __m128 ext = _mm256_extractf128_ps(b, 1);
    __m256 pl = _mm256_permute_ps(a, 0x39), pv = _mm256_permutevar_ps(a, _mm256_loadu_si256((void *)ib)), pps = _mm256_permutevar8x32_ps(a, _mm256_loadu_si256((void *)ia));
    __m256 p2 = _mm256_permute2f128_ps(a, b, 0x13);
    __m256i bw = _mm256_cvtepu8_epi16(_mm_loadu_si128((void *)ba)), sx = _mm256_cvtepi16_epi32(_mm_loadu_si128((void *)sa)), dq = _mm256_cvtepi32_epi64(_mm_loadu_si128((void *)ia));
    __m256i bb = _mm256_broadcastb_epi8(_mm_loadu_si128((void *)(ba + 5))), bq = _mm256_broadcastq_epi64(_mm_loadu_si128((void *)ib));
    __m256d cd = _mm256_cvtepi32_pd(_mm_loadu_si128((void *)ia)), md = _mm256_movedup_pd(_mm256_loadu_pd(da));
    __m128i ph = _mm256_cvtps_ph(a, 0); __m256 back = _mm256_cvtph_ps(ph);
    __m256i g = _mm256_i32gather_epi32(ia, _mm256_and_si256(_mm256_loadu_si256((void *)ib), _mm256_set1_epi32(63)), 4);
    __m256 gf = _mm256_mask_i32gather_ps(a, fb, _mm256_and_si256(_mm256_loadu_si256((void *)ia), _mm256_set1_epi32(63)), b, 4);
    __m256d gd = _mm256_i64gather_pd(db, _mm256_set_epi64x(3, 17, 9, 30), 8);
    __m256 ml = _mm256_maskload_ps(fc, _mm256_loadu_si256((void *)ia));
    float st[8] = {0}; _mm256_maskstore_ps(st, _mm256_loadu_si256((void *)ib), b);
    MIX(bs); MIX(bsr); MIX(ins); MIX(ext); MIX(pl); MIX(pv); MIX(pps); MIX(p2); MIX(bw); MIX(sx); MIX(dq); MIX(bb); MIX(bq);
    MIX(cd); MIX(md); MIX(ph); MIX(back); MIX(g); MIX(gf); MIX(gd); MIX(ml); MIX(st);
    printf("movmsk=%02x ptestps=%d\n", _mm256_movemask_ps(a), _mm256_testc_ps(a, b));
    group("crosslane");
}

__attribute__((noinline)) static void t_sse4(void) {
    for (int i = 0; i < 64; i += 4) {
        __m128i a = _mm_loadu_si128((void *)(ia + i)), b = _mm_loadu_si128((void *)(ib + i));
        __m128 f = _mm_loadu_ps(fa + i), g = _mm_loadu_ps(fb + i);
        __m128i r[8] = { _mm_insert_epi32(a, ib[i], 2), _mm_insert_epi8(b, 0x7f, 13), _mm_cvtepu16_epi64(a), _mm_minpos_epu16(_mm_setzero_si128()),
                         _mm_packus_epi32(a, b), _mm_mullo_epi32(a, b), _mm_cmpeq_epi64(a, a), _mm_hsub_epi16(a, b) };
        __m128 s[4] = { _mm_insert_ps(f, g, 0x9a), _mm_round_ss(f, g, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC), _mm_dp_ps(f, g, 0x7f), _mm_blendv_ps(f, g, f) };
        MIX(r[0]); MIX(r[1]); MIX(r[2]); MIX(r[4]); MIX(r[5]); MIX(r[6]); MIX(r[7]); MIX(s);
        int e = _mm_extract_epi32(b, 3) ^ _mm_extract_epi8(a, 9) ^ _mm_extract_ps(g, 2); MIX(e);
        uint32_t crc = _mm_crc32_u32(0xffffffffu, (uint32_t)ia[i]); crc = _mm_crc32_u8(crc, ba[i]); MIX(crc);
    }
    group("sse4");
}

__attribute__((noinline)) static void t_bmi(void) {
    for (int i = 0; i < 64; i++) {
        uint64_t x = (uint64_t)(uint32_t)ia[i] << 17 ^ (uint32_t)ib[i], y = (uint64_t)(uint32_t)ib[i] * 0x9E3779B97F4A7C15ull;
        uint32_t x32 = (uint32_t)ia[i];
        uint64_t r[14] = { _andn_u64(x, y), _bextr_u64(y, i % 40, i % 23), _blsi_u64(x), _blsmsk_u64(y), _blsr_u64(x), _bzhi_u64(y, i),
                           _pdep_u64(x, y), _pext_u64(y, x), _tzcnt_u64(x), _lzcnt_u64(y >> (i % 60)), (uint64_t)__builtin_popcountll(x),
                           _bzhi_u32(x32, i % 40), _pdep_u32(x32, 0x0f0f0f0f), _pext_u32(x32, 0xff00ff00u) };
        unsigned long long hi; unsigned long long lo = _mulx_u64(x, y, &hi);
        uint64_t sh = (x << (i & 63)) ^ (y >> (i & 63)) ^ (uint64_t)((int64_t)y >> (i & 63));
        MIX(r); MIX(lo); MIX(hi); MIX(sh);
    }
    group("bmi");
}

// Plain C that -O3 -mavx2 auto-vectorizes: this is what most game code actually looks like.
__attribute__((noinline)) static void t_autovec(void) {
    static float out[64]; static int32_t iout[64]; static int16_t sout[64]; float acc = 0; int64_t iacc = 0;
    for (int i = 0; i < 64; i++) out[i] = fa[i] * fb[i] + fc[i] * 0.25f - (fa[i] > fb[i] ? fa[i] : fb[i]);
    for (int i = 0; i < 64; i++) iout[i] = (ia[i] >> 3) * (ib[i] & 1023) + (ia[i] < ib[i] ? ia[i] : ib[i]);
    for (int i = 0; i < 64; i++) sout[i] = (int16_t)(sa[i] * 3 + (sa[i] >> 2));
    for (int i = 0; i < 64; i++) acc += out[i] * out[i];
    for (int i = 0; i < 64; i++) iacc += (int64_t)iout[i] * (i + 1);
    uint8_t hist[16] = {0}; for (int i = 0; i < 128; i++) hist[ba[i] & 15]++;
    MIX(out); MIX(iout); MIX(sout); MIX(acc); MIX(iacc); MIX(hist);
    printf("acc=%.6f iacc=%lld\n", acc, (long long)iacc);
    group("autovec");
}

int main(void) {
    H = 1469598103934665603ULL; init();
    t_float(); t_fma(); t_int(); t_cross(); t_sse4(); t_bmi(); t_autovec();
    return 0;
}
