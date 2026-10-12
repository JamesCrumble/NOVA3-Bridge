/* VCMP/VCMPE.F32 + VMRS APSR_nzcv, FPSCR and MSR APSR_nzcvq: every class pair, flags read back with MRS (so the
 * inline flag write is tested too). Build: arm-linux-gnueabihf-gcc -O1 -mfpu=neon-vfpv4 -mfloat-abi=hard -static.
 * The output must be identical with and without QEMU_STRICT_FP=1, and with the previous qemu build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint32_t v[] = {
    0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000,
    0x3f800000, 0xbf800000, 0x3f800001, 0xbf800001, 0x40490fdb, 0xc0490fdb, 0x7f7fffff, 0xff7fffff,
    0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7f800001, 0xff800001, 0x7fffffff, 0x12345678,
};
#define N (sizeof v / sizeof v[0])

static uint32_t cmp(uint32_t a, uint32_t b, int e)
{
    float x, y;
    uint32_t f;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    if (e)
        __asm__ volatile("vcmpe.f32 %1, %2\n vmrs APSR_nzcv, fpscr\n mrs %0, apsr\n" : "=r"(f) : "t"(x), "t"(y) : "cc");
    else
        __asm__ volatile("vcmp.f32 %1, %2\n vmrs APSR_nzcv, fpscr\n mrs %0, apsr\n" : "=r"(f) : "t"(x), "t"(y) : "cc");
    return f >> 28;
}

static uint32_t msr_test(uint32_t val)
{
    uint32_t f;
    __asm__ volatile("msr APSR_nzcvq, %1\n mrs %0, apsr\n" : "=r"(f) : "r"(val) : "cc");
    return f >> 27;
}

int main(void)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < N; i++)
        for (unsigned j = 0; j < N; j++)
            for (int e = 0; e < 2; e++)
                h = (h ^ cmp(v[i], v[j], e)) * 16777619u;
    for (uint32_t x = 0; x < 64; x++)
        h = (h ^ msr_test(x << 26)) * 16777619u;
    printf("%08x\n", h);
    return 0;
}
