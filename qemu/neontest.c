/* Neon f32 add/sub/mul/mla/mls (vector and by-scalar forms) over ordinary and special bit patterns.
 * Prints one hash per class; run with and without QEMU_STRICT_FP=1 and compare. */
#include <arm_neon.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static uint32_t rs = 88172645u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static uint32_t pick(int cls) {
    uint32_t r = rnd();
    switch (cls) {
    case 0: { float f = ((int32_t)r / 2147483648.f) * (float[]){1e-3f, 1.f, 1.f, 100.f, 1e6f}[rnd() % 5]; uint32_t u; memcpy(&u, &f, 4); return u; }
    case 1: return r;                                                   /* any bits: NaNs, denormals, huge */
    case 2: return (r & 0x80000000u) | (rnd() % 3 ? (r & 0x007fffffu) : 0x00800000u + (r & 7));   /* denormals and smallest normals */
    case 3: { static const uint32_t s[] = {0, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7fa00000u, 0xffc12345u,
                  0x00800000u, 0x00800001u, 0x007fffffu, 0x00000001u, 0x3f800000u, 0x7f7fffffu, 0x80800000u, 0x3f7fffffu, 0x3f800001u};
              return s[r % 16]; }
    default: /* products landing next to the smallest normal: exponents summing to about -126 */
        { int e = (int)(r % 60) + 40; uint32_t m = rnd() & 0x007fffffu; return ((rnd() & 1u) << 31) | ((uint32_t)((rnd() & 1) ? e : (254 - 126 - e + (int)(rnd() % 3) - 1)) << 23) | m; }
    }
}
static uint32_t h;
static void mix(float32x4_t v) { uint32_t u[4]; vst1q_u32(u, vreinterpretq_u32_f32(v)); for (int i = 0; i < 4; i++) { h ^= u[i]; h *= 16777619u; } }
int main(void) {
    for (int cls = 0; cls < 5; cls++) {
        h = 2166136261u;
        for (int t = 0; t < 300000; t++) {
            uint32_t a[4], b[4], c[4];
            for (int i = 0; i < 4; i++) { a[i] = pick(cls); b[i] = pick(t & 1 ? cls : (int)(rnd() % 5)); c[i] = pick(t & 2 ? cls : 0); }
            float32x4_t x = vreinterpretq_f32_u32(vld1q_u32(a)), y = vreinterpretq_f32_u32(vld1q_u32(b)), z = vreinterpretq_f32_u32(vld1q_u32(c));
            float32x2_t sc = vget_low_f32(y);
            mix(vaddq_f32(x, y)); mix(vsubq_f32(x, y)); mix(vmulq_f32(x, y));
            mix(vmlaq_f32(z, x, y)); mix(vmlsq_f32(z, x, y));
            mix(vmulq_lane_f32(x, sc, 1)); mix(vmlaq_lane_f32(z, x, sc, 0)); mix(vmlsq_lane_f32(z, x, sc, 1));
            mix(vcombine_f32(vmla_f32(vget_low_f32(z), vget_low_f32(x), sc), vmul_f32(vget_high_f32(x), sc)));
        }
        printf("class %d: %08x\n", cls, h);
    }
    return 0;
}
