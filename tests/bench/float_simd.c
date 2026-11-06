// Float / SIMD game math: 4x4 matrix products, vertex transforms, particle physics, n-body, and an SSE
// intrinsics kernel (the hand-vectorised style of game math libraries). Compiled without FP contraction on
// every target so all three builds produce bit-identical results.
#include "bench.h"
#include <math.h>
#if defined(__x86_64__)
#include <xmmintrin.h>
#include <emmintrin.h>
#define HAVE_SSE 1
#else
#include <arm_neon.h>
#endif

typedef struct { float m[16]; } Mat4;
typedef struct { float x, y, z, w; } Vec4;

BENCH_NOINLINE static void mat_mul(Mat4 *o, const Mat4 *a, const Mat4 *b) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a->m[i * 4 + k] * b->m[k * 4 + j];
            o->m[i * 4 + j] = s;
        }
}

BENCH_NOINLINE static void transform(Vec4 *out, const Vec4 *in, int n, const Mat4 *m) {
    for (int i = 0; i < n; i++) {
        Vec4 v = in[i];
        out[i].x = m->m[0] * v.x + m->m[4] * v.y + m->m[8] * v.z + m->m[12] * v.w;
        out[i].y = m->m[1] * v.x + m->m[5] * v.y + m->m[9] * v.z + m->m[13] * v.w;
        out[i].z = m->m[2] * v.x + m->m[6] * v.y + m->m[10] * v.z + m->m[14] * v.w;
        out[i].w = m->m[3] * v.x + m->m[7] * v.y + m->m[11] * v.z + m->m[15] * v.w;
    }
}

typedef struct { float px, py, pz, vx, vy, vz; } Particle;

BENCH_NOINLINE static void particles_step(Particle *p, int n, float dt) {
    for (int i = 0; i < n; i++) {
        float dx = -p[i].px, dy = -p[i].py, dz = -p[i].pz;
        float d2 = dx * dx + dy * dy + dz * dz + 0.01f;
        float inv = 1.0f / sqrtf(d2);
        float f = inv * inv * inv;
        p[i].vx += dx * f * dt; p[i].vy += (dy * f - 9.8f) * dt; p[i].vz += dz * f * dt;
        p[i].px += p[i].vx * dt; p[i].py += p[i].vy * dt; p[i].pz += p[i].vz * dt;
        if (p[i].py < -10.0f) { p[i].py = -10.0f; p[i].vy = -p[i].vy * 0.5f; }
    }
}

BENCH_NOINLINE static void nbody(double *pos, double *vel, int n, double dt) {
    for (int i = 0; i < n; i++) {
        double ax = 0, ay = 0, az = 0;
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            double dx = pos[j * 3] - pos[i * 3], dy = pos[j * 3 + 1] - pos[i * 3 + 1], dz = pos[j * 3 + 2] - pos[i * 3 + 2];
            double d2 = dx * dx + dy * dy + dz * dz + 1e-3;
            double inv = 1.0 / (d2 * sqrt(d2));
            ax += dx * inv; ay += dy * inv; az += dz * inv;
        }
        vel[i * 3] += ax * dt; vel[i * 3 + 1] += ay * dt; vel[i * 3 + 2] += az * dt;
    }
    for (int i = 0; i < n * 3; i++) pos[i] += vel[i] * dt;
}

// Hand-written SIMD: 4-wide dot products + normalisation (x86 SSE intrinsics / NEON on arm64).
BENCH_NOINLINE static void simd_normalize(float *xs, float *ys, float *zs, int n) {
    for (int i = 0; i < n; i += 4) {
#ifdef HAVE_SSE
        __m128 x = _mm_loadu_ps(xs + i), y = _mm_loadu_ps(ys + i), z = _mm_loadu_ps(zs + i);
        __m128 d = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, x), _mm_mul_ps(y, y)), _mm_add_ps(_mm_mul_ps(z, z), _mm_set1_ps(1e-6f)));
        __m128 inv = _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(d));
        x = _mm_add_ps(_mm_mul_ps(x, inv), _mm_set1_ps(0.25f));
        y = _mm_sub_ps(_mm_mul_ps(y, inv), _mm_set1_ps(0.125f));
        z = _mm_max_ps(_mm_mul_ps(z, inv), _mm_set1_ps(-0.5f));
        _mm_storeu_ps(xs + i, x); _mm_storeu_ps(ys + i, y); _mm_storeu_ps(zs + i, z);
#else
        float32x4_t x = vld1q_f32(xs + i), y = vld1q_f32(ys + i), z = vld1q_f32(zs + i);
        float32x4_t d = vaddq_f32(vaddq_f32(vmulq_f32(x, x), vmulq_f32(y, y)), vaddq_f32(vmulq_f32(z, z), vdupq_n_f32(1e-6f)));
        float32x4_t inv = vdivq_f32(vdupq_n_f32(1.0f), vsqrtq_f32(d));
        x = vaddq_f32(vmulq_f32(x, inv), vdupq_n_f32(0.25f));
        y = vsubq_f32(vmulq_f32(y, inv), vdupq_n_f32(0.125f));
        z = vmaxq_f32(vmulq_f32(z, inv), vdupq_n_f32(-0.5f));
        vst1q_f32(xs + i, x); vst1q_f32(ys + i, y); vst1q_f32(zs + i, z);
#endif
    }
}

static uint64_t fsum(const float *p, int n) { uint64_t h = 0; for (int i = 0; i < n; i++) { uint32_t b; memcpy(&b, &p[i], 4); h = h * 31 + b; } return h; }
static uint64_t dsum(const double *p, int n) { uint64_t h = 0; for (int i = 0; i < n; i++) { uint64_t b; memcpy(&b, &p[i], 8); h = h * 31 + b; } return h; }

int main(void) {
    uint64_t s = 1;
    Mat4 a, b, o;
    for (int i = 0; i < 16; i++) { a.m[i] = (float)(bench_rng(&s) % 1000) / 1000.0f; b.m[i] = (float)(bench_rng(&s) % 1000) / 1000.0f; }
    double t0 = bench_now_ms();
    uint64_t mh = 0;
    for (int r = 0; r < 4000000; r++) { a.m[r & 15] = (float)(r & 1023) * 0.001f; mat_mul(&o, &a, &b); mh = mh * 31 + fsum(o.m + (r & 15), 1); }
    bench_report("mat4_mul", bench_now_ms() - t0, mh);

    enum { NV = 4096 };
    static Vec4 vin[NV], vout[NV];
    for (int i = 0; i < NV; i++) { vin[i].x = (float)i; vin[i].y = (float)(i * 2); vin[i].z = 1.0f; vin[i].w = 1.0f; }
    t0 = bench_now_ms();
    for (int r = 0; r < 8000; r++) { a.m[r & 15] = (float)(r & 255) * 0.01f; transform(vout, vin, NV, &a); }
    bench_report("vec_transform", bench_now_ms() - t0, fsum(&vout[0].x, NV * 4));

    enum { NP = 16384 };
    static Particle ps[NP];
    for (int i = 0; i < NP; i++) {
        ps[i].px = (float)(bench_rng(&s) % 2000) / 100.0f - 10.0f; ps[i].py = (float)(bench_rng(&s) % 2000) / 100.0f;
        ps[i].pz = (float)(bench_rng(&s) % 2000) / 100.0f - 10.0f; ps[i].vx = ps[i].vy = ps[i].vz = 0;
    }
    t0 = bench_now_ms();
    for (int r = 0; r < 800; r++) particles_step(ps, NP, 0.001f);
    bench_report("particles", bench_now_ms() - t0, fsum(&ps[0].px, NP * 6));

    enum { NB = 400 };
    static double pos[NB * 3], vel[NB * 3];
    for (int i = 0; i < NB * 3; i++) { pos[i] = (double)(bench_rng(&s) % 10000) / 100.0; vel[i] = 0; }
    t0 = bench_now_ms();
    for (int r = 0; r < 150; r++) nbody(pos, vel, NB, 0.01);
    bench_report("nbody", bench_now_ms() - t0, dsum(pos, NB * 3));

    enum { NS = 65536 };
    static float xs[NS], ys[NS], zs[NS];
    for (int i = 0; i < NS; i++) { xs[i] = (float)(i % 97) - 48.0f; ys[i] = (float)(i % 89) - 44.0f; zs[i] = (float)(i % 83) - 41.0f; }
    t0 = bench_now_ms();
    for (int r = 0; r < 1500; r++) simd_normalize(xs, ys, zs, NS);
    bench_report("sse_normalize", bench_now_ms() - t0, fsum(xs, NS) ^ fsum(ys, NS) ^ fsum(zs, NS));
    return 0;
}
