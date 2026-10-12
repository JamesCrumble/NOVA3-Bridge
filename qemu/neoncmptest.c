/* Neon f32 vmax/vmin/vceq/vcge/vcgt over ordinary and special bit patterns, in registers that are all
 * ordinary, all special and mixed. Prints one hash; run with and without QEMU_STRICT_FP=1 and compare.
 * Build: arm-linux-gnueabihf-gcc -O1 -mfpu=neon -static */
#include <arm_neon.h>
#include <stdio.h>
#include <stdint.h>

static const uint32_t pat[] = {
    0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x40490fdb, 0xc0490fdb, 0x7f7fffff, 0xff7fffff,
    0x00800000, 0x80800000, 0x00800001, 0x007fffff, 0x807fffff, 0x00000001, 0x80000001, 0x00400000,
    0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7f800001, 0x7fa00000, 0xff812345, 0x7fffffff,
    0x3eaaaaab, 0x42c80000, 0xc2c80000, 0x33800000, 0x4b800000, 0x1e3ce508, 0x5d5e0b6b, 0xbdcccccd,
};
#define NP (sizeof(pat) / sizeof(pat[0]))

static uint32_t h = 2166136261u;
static void mix(uint32x4_t v)
{
    uint32_t o[4];
    vst1q_u32(o, v);
    for (int i = 0; i < 4; i++) {
        h = (h ^ o[i]) * 16777619u;
    }
}

static uint32_t rnd = 12345;
static uint32_t next(void) { rnd = rnd * 1664525u + 1013904223u; return rnd; }

/* volatile: keep the compiler from folding the operations */
static volatile uint32_t va[4], vb[4];

static void one(const uint32_t *a, const uint32_t *b)
{
    for (int i = 0; i < 4; i++) { va[i] = a[i]; vb[i] = b[i]; }
    uint32_t ta[4] = { va[0], va[1], va[2], va[3] }, tb[4] = { vb[0], vb[1], vb[2], vb[3] };
    float32x4_t x = vreinterpretq_f32_u32(vld1q_u32(ta)), y = vreinterpretq_f32_u32(vld1q_u32(tb));
    mix(vreinterpretq_u32_f32(vmaxq_f32(x, y)));
    mix(vreinterpretq_u32_f32(vminq_f32(x, y)));
    mix(vceqq_f32(x, y));
    mix(vcgeq_f32(x, y));
    mix(vcgtq_f32(x, y));
    mix(vcleq_f32(x, y));
    mix(vcltq_f32(x, y));
}

int main(void)
{
    uint32_t a[4], b[4];
    /* every pattern against every pattern, in each lane with ordinary neighbours and with special ones */
    for (unsigned i = 0; i < NP; i++) {
        for (unsigned j = 0; j < NP; j++) {
            for (int lane = 0; lane < 4; lane++) {
                for (int k = 0; k < 4; k++) { a[k] = 0x3f800000 + k * 0x1000; b[k] = 0x40000000 - k * 0x1000; }
                a[lane] = pat[i]; b[lane] = pat[j];
                one(a, b);
                for (int k = 0; k < 4; k++) { a[k] = pat[(i + k) % NP]; b[k] = pat[(j + 3 * k) % NP]; }
                one(a, b);
            }
        }
    }
    /* random ordinary values, with equal lanes now and then */
    for (int n = 0; n < 200000; n++) {
        for (int k = 0; k < 4; k++) {
            a[k] = (next() & 0x80000000u) | (0x30000000u + (next() % 0x20000000u));
            b[k] = (next() & 3) == 0 ? a[k] : (next() & 0x80000000u) | (0x30000000u + (next() % 0x20000000u));
        }
        one(a, b);
    }
    printf("%08x\n", h);
    return 0;
}
