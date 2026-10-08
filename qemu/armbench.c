/*
 * Engine-shaped ARM32 workloads for comparing qemu builds on the phone (no game needed).
 * Build: arm-linux-gnueabihf-gcc -O2 -mcpu=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard -static -o armbench armbench.c -lm
 * Run:   qemu-arm armbench [rounds]   -> one line per workload: name, best ms of the rounds.
 */
#include <arm_neon.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static volatile uint32_t sink;

/* 1. Calls and returns, direct and through a vtable: what a C++ scene graph does all day. */
struct Node;
typedef uint32_t (*visit_fn)(struct Node *, uint32_t);
struct Node { visit_fn visit; struct Node *kids[2]; uint32_t v; };
static uint32_t visit_a(struct Node *n, uint32_t x);
static uint32_t visit_b(struct Node *n, uint32_t x);
static __attribute__((noinline)) uint32_t visit_a(struct Node *n, uint32_t x)
{
    x = x * 33 + n->v;
    for (int i = 0; i < 2; i++)
        if (n->kids[i]) x = n->kids[i]->visit(n->kids[i], x);
    return x;
}
static __attribute__((noinline)) uint32_t visit_b(struct Node *n, uint32_t x)
{
    x ^= n->v + (x >> 3);
    for (int i = 0; i < 2; i++)
        if (n->kids[i]) x = n->kids[i]->visit(n->kids[i], x);
    return x;
}
static struct Node nodes[4095];
static void calls(int iters)
{
    for (int i = 0; i < 4095; i++) {
        nodes[i].visit = (i * 7) % 3 ? visit_a : visit_b;
        nodes[i].v = i * 2654435761u;
        nodes[i].kids[0] = 2 * i + 1 < 4095 ? &nodes[2 * i + 1] : NULL;
        nodes[i].kids[1] = 2 * i + 2 < 4095 ? &nodes[2 * i + 2] : NULL;
    }
    uint32_t x = 1;
    for (int k = 0; k < iters; k++)
        x = nodes[0].visit(&nodes[0], x);
    sink = x;
}

/* 2. Scalar VFP: transforms, normalisation, a little trig - game logic and physics. */
static void vfp(int iters)
{
    float acc = 0, px = 1, py = 2, pz = 3;
    for (int k = 0; k < iters; k++) {
        float a = k * 0.001f;
        float c = cosf(a), s = sinf(a);
        float x = px * c - pz * s, z = px * s + pz * c, y = py + 0.5f * x;
        float l = sqrtf(x * x + y * y + z * z) + 1e-6f;
        px = x / l + 0.3f; py = y / l; pz = z / l - 0.2f;
        acc += px * py - pz;
    }
    sink = (uint32_t)(acc * 1000);
}

/* 3. Neon: mat4 * vec4 with by-lane multiply-accumulate, the shape of the engine's skinning loops. */
static void neon(int iters)
{
    float m[16], v[4096 * 4], o[4096 * 4];
    for (int i = 0; i < 16; i++) m[i] = (i % 5) * 0.25f + 0.1f;
    for (int i = 0; i < 4096 * 4; i++) v[i] = (i % 17) * 0.1f;
    float32x4_t c0 = vld1q_f32(m), c1 = vld1q_f32(m + 4), c2 = vld1q_f32(m + 8), c3 = vld1q_f32(m + 12);
    for (int k = 0; k < iters; k++) {
        for (int i = 0; i < 4096; i++) {
            float32x4_t p = vld1q_f32(v + 4 * i);
            float32x4_t r = vmulq_lane_f32(c0, vget_low_f32(p), 0);
            r = vmlaq_lane_f32(r, c1, vget_low_f32(p), 1);
            r = vmlaq_lane_f32(r, c2, vget_high_f32(p), 0);
            r = vmlaq_lane_f32(r, c3, vget_high_f32(p), 1);
            vst1q_f32(o + 4 * i, r);
        }
        c0 = vaddq_f32(c0, vdupq_n_f32(1e-6f));
    }
    sink = (uint32_t)o[123];
}

/* 4. Integer work with branches: hashing, sorting with a comparator (indirect calls). */
static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static void ints(int iters)
{
    static uint32_t a[20000];
    uint32_t h = 2166136261u;
    for (int k = 0; k < iters; k++) {
        for (int i = 0; i < 20000; i++) a[i] = (h = (h ^ i) * 16777619u);
        qsort(a, 20000, 4, cmp_u32);
    }
    sink = a[777];
}

/* 5. Bulk memory: what the GL bridge does with client vertex arrays. */
static void mem(int iters)
{
    size_t n = 1 << 20;
    uint8_t *s = malloc(n), *d = malloc(n);
    memset(s, 7, n);
    for (int k = 0; k < iters; k++) {
        memcpy(d, s, n);
        s[k & (n - 1)] ^= d[(k * 31) & (n - 1)];
    }
    sink = d[12345];
    free(s);
    free(d);
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 3;
    struct { const char *name; void (*fn)(int); int iters; } w[] = {
        { "calls", calls, 1500 }, { "vfp", vfp, 1000000 }, { "neon", neon, 300 },
        { "ints", ints, 12 }, { "mem", mem, 800 },
    };
    double total = 0;
    for (unsigned i = 0; i < sizeof(w) / sizeof(w[0]); i++) {
        double best = 1e30;
        for (int r = 0; r < rounds; r++) {
            double t0 = now_ms();
            w[i].fn(w[i].iters);
            double t = now_ms() - t0;
            if (t < best) best = t;
        }
        total += best;
        printf("%-6s %8.1f ms\n", w[i].name, best);
    }
    printf("total  %8.1f ms\n", total);
    return 0;
}
