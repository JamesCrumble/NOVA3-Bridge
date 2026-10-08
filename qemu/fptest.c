/* Prints bit patterns of VFP and Neon float results over awkward inputs; run under qemu with and
 * without QEMU_STRICT_FP=1 and diff. Build: arm-linux-gnueabihf-gcc -O1 -mfpu=neon -static */
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static float f(uint32_t u) { float x; memcpy(&x, &u, 4); return x; }
static uint32_t u(float x) { uint32_t v; memcpy(&v, &x, 4); return v; }

static const uint32_t in[] = {
    0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x00000001, 0x80000001, 0x007fffff, 0x00800000,
    0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00001, 0x7f800001, 0x7fa00000,
    0x4f000000, 0xcf000000, 0x4f800000, 0x3eaaaaab, 0x40490fdb, 0x1e3ce508, 0x5f000000, 0xc1200000,
};
#define N (sizeof(in) / sizeof(in[0]))

int main(void)
{
    unsigned long h = 1469598103u;
#define MIX(v) do { h = (h ^ (uint32_t)(v)) * 16777619u; } while (0)
    for (unsigned i = 0; i < N; i++) {
        float a = f(in[i]);
        volatile float va = a;
        MIX((int32_t)va);              /* vcvt.s32 rtz */
        MIX((uint32_t)va);             /* vcvt.u32 rtz */
        MIX(u((float)(int32_t)in[i])); /* vcvt.f32.s32 */
        MIX(u((float)in[i]));          /* vcvt.f32.u32 */
        for (unsigned j = 0; j < N; j++) {
            volatile float vb = f(in[j]);
            MIX(u(va + vb)); MIX(u(va - vb)); MIX(u(va * vb)); MIX(u(va / vb));
            volatile double da = va, db = vb;
            uint64_t d; double r = da * db + da; memcpy(&d, &r, 8); MIX(d); MIX(d >> 32);
            for (unsigned k = 0; k < N; k += 3) {
                MIX(u(__builtin_fmaf(va, vb, f(in[k]))));
            }
            /* Neon: standard FPSCR (flush to zero, default NaN) */
            float32x4_t x = vdupq_n_f32(va), y = vdupq_n_f32(vb);
            float32x4_t s = vaddq_f32(x, y), m = vmulq_f32(x, y), ml = vmlaq_f32(s, x, y);
            float32x4_t sb = vsubq_f32(x, y);
            MIX(u(vgetq_lane_f32(s, 0))); MIX(u(vgetq_lane_f32(m, 1)));
            MIX(u(vgetq_lane_f32(ml, 2))); MIX(u(vgetq_lane_f32(sb, 3)));
            MIX(vgetq_lane_s32(vcvtq_s32_f32(m), 0));
            /* by-scalar forms (fmul_idx / fmla_nf_idx / fmls_nf_idx) and vmls */
            float32x2_t sc = vset_lane_f32(vb, vdup_n_f32(va), 1);
            float32x4_t li = vmulq_lane_f32(x, sc, 1), la = vmlaq_lane_f32(s, x, sc, 1);
            float32x4_t ls = vmlsq_lane_f32(s, y, sc, 0), mls = vmlsq_f32(m, x, y);
            float32x2_t dd = vmla_f32(vget_low_f32(s), vget_low_f32(x), vget_low_f32(y));
            MIX(u(vgetq_lane_f32(li, 0))); MIX(u(vgetq_lane_f32(la, 1)));
            MIX(u(vgetq_lane_f32(ls, 2))); MIX(u(vgetq_lane_f32(mls, 3)));
            MIX(u(vget_lane_f32(dd, 1)));
            MIX(u(vgetq_lane_f32(vcvtq_f32_s32(vdupq_n_s32((int32_t)in[j])), 0)));
        }
    }
    printf("hash %08lx\n", h & 0xffffffffu);
    return 0;
}
