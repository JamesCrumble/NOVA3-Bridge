/*
 * VFP short-vector compatibility for the Real Racing 3 audio mixer.
 *
 * Register decoding and scalar code generation are adapted from VFPVector by
 * Bythos, commit d95ba13 ("Fix mis-identification of VABS and VSQRT"), MIT.
 * The original library traps unsupported instructions on PlayStation Vita and
 * patches them lazily. Linux/ARM cannot use that mechanism: Cortex-A35 accepts
 * the same opcodes but treats FPSCR LEN/STRIDE as RAZ/WI, silently executing
 * only lane zero. This pinned game binary is therefore patched eagerly.
 *
 * Only the F32 operations present in the measured vector regions are decoded
 * (VMOV/VADD/VSUB/VMUL/VMLA). Every source opcode is checked before it is
 * touched, and the whole list is verified register-for-register by the
 * self-test below - see analysis/vfp_discover.py for how it was derived.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "arm32_encodings.h"
#include "so_util.h"
#include "trace.h"

#include "vfp_vector_patch.h"
#include "port_env.h"

extern uintptr_t so_alloc_arena(so_module *so, uintptr_t range,
                                uintptr_t dst, size_t size);

enum class VfpOp {
    Move,
    Add,
    Subtract,
    Multiply,
    MultiplyAccumulate,
};

struct DecodedVfp {
    VfpOp op;
    uint8_t condition;
    uint8_t destination;
    uint8_t left;
    uint8_t right;
    bool uses_left;
};

struct VectorPatch {
    uint32_t offset;
    uint32_t expected;
    uint8_t lanes;
    uint8_t stride;
};

/*
 * Extracted from the pinned MEI v1.0.58 binary by analysis/vfp_discover.py:
 * every arithmetic instruction inside the 8 FPSCR LEN setup/reset pairs. All
 * 29 live in EA::Audio::Core - the Xas1 decoder, the delay/all-pass filters,
 * and the gain mixers.
 *
 * The lanes/stride columns come from the FPSCR value the game itself ORs in
 * before each region, and this binary uses only two: 0x30000 (LEN=3, STRIDE=0
 * -> 4 lanes, step 1) around Xas1Dec::DecodeChannel, and 0x70000 (LEN=7,
 * STRIDE=0 -> 8 lanes, step 1) everywhere else. Those are exactly the two
 * values a sibling port uses, whose audio is correct on this hardware - there is no
 * STRIDE=2 anywhere in either game.
 *
 * An earlier revision of this table claimed 2/6 lanes and a STRIDE=2 region.
 * That was an artefact of vfp_discover.py reading objdump's decimal immediate
 * as hexadecimal (196608 -> 0x196608), not something in the binary, and it is
 * what made the mixer sound wrong: the 8-lane regions computed 6 of every 8
 * samples and left the other two as whatever the registers last held, which is
 * the constant 768/1024 non-zero ratio the device log reported. analysis/
 * vfp_coverage.py now checks these two columns against the binary so a script
 * artefact cannot reach the audio again.
 *
 * The 0x7e... encodings are conditionally-executed vector ops (vaddvc/vmulvc/
 * vmlavc); the condition the reaching branch already carries, so the expansion
 * runs them unconditionally.
 */
static const VectorPatch kVectorPatches[] = {
    {0x005828ec, 0xee288a04, 4, 1},   /* vmul.f32	s16, s16, s8 */
    {0x005828f0, 0xee2aaa04, 4, 1},   /* vmul.f32	s20, s20, s8 */
    {0x00582904, 0xee0c8a06, 4, 1},   /* vmla.f32	s16, s24, s12 */
    {0x00582918, 0xee0e8a06, 4, 1},   /* vmla.f32	s16, s28, s12 */
    {0x0058292c, 0xeeb06a48, 4, 1},   /* vmov.f32	s12, s16 */
    {0x00582930, 0xee0caa06, 4, 1},   /* vmla.f32	s20, s24, s12 */
    {0x00582944, 0xee0eaa06, 4, 1},   /* vmla.f32	s20, s28, s12 */
    {0x00587194, 0xee24ca00, 8, 1},   /* vmul.f32	s24, s8, s0 */
    {0x0058719c, 0xee3cca08, 8, 1},   /* vadd.f32	s24, s24, s16 */
    {0x005871a8, 0xee3cca20, 8, 1},   /* vadd.f32	s24, s24, s1 */
    {0x005887fc, 0xee244a00, 8, 1},   /* vmul.f32	s8, s8, s0 */
    {0x00588800, 0xee388a4c, 8, 1},   /* vsub.f32	s16, s16, s24 */
    {0x00588804, 0xee04ca08, 8, 1},   /* vmla.f32	s24, s8, s16 */
    {0x00588a68, 0x7e388a04, 8, 1},   /* vaddvc.f32	s16, s16, s8 */
    {0x00588adc, 0x7e048a00, 8, 1},   /* vmlavc.f32	s16, s8, s0 */
    {0x0058f8f0, 0xee28ca00, 8, 1},   /* vmul.f32	s24, s16, s0 */
    {0x0058f8f8, 0xee3cca20, 8, 1},   /* vadd.f32	s24, s24, s1 */
    {0x0058f8fc, 0xee34ca4c, 8, 1},   /* vsub.f32	s24, s8, s24 */
    {0x0058f904, 0xee2cca00, 8, 1},   /* vmul.f32	s24, s24, s0 */
    {0x0058f908, 0xee3cca08, 8, 1},   /* vadd.f32	s24, s24, s16 */
    {0x0058f90c, 0xee2c4a01, 8, 1},   /* vmul.f32	s8, s24, s2 */
    {0x0058f960, 0xee28ca00, 8, 1},   /* vmul.f32	s24, s16, s0 */
    {0x0058f968, 0xee3cca20, 8, 1},   /* vadd.f32	s24, s24, s1 */
    {0x0058f96c, 0xee34ca4c, 8, 1},   /* vsub.f32	s24, s8, s24 */
    {0x0058f974, 0xee2cca00, 8, 1},   /* vmul.f32	s24, s24, s0 */
    {0x0058f97c, 0xee3cca08, 8, 1},   /* vadd.f32	s24, s24, s16 */
    {0x0058f980, 0xee0c4a01, 8, 1},   /* vmla.f32	s8, s24, s2 */
    {0x00591acc, 0x7e244a00, 8, 1},   /* vmulvc.f32	s8, s8, s0 */
    {0x00591ad0, 0x7e288a00, 8, 1},   /* vmulvc.f32	s16, s16, s0 */
};

static bool decode_f32(uint32_t raw, DecodedVfp *decoded)
{
    /* VFP data-processing encoding, single precision only. */
    if ((raw & 0x0f000e10) != 0x0e000a00 || (raw & 0x00000100))
        return false;

    int opc1 = (raw & 0x00b00000) >> 20;
    int opc2 = (raw & 0x000f0000) >> 16;
    int opc3 = (raw & 0x00000040) >> 6;
    decoded->uses_left = true;

    switch (opc1) {
    case 0b0000:
        if (opc3)
            return false;
        decoded->op = VfpOp::MultiplyAccumulate;
        break;
    case 0b0010:
        if (opc3)
            return false;
        decoded->op = VfpOp::Multiply;
        break;
    case 0b0011:
        decoded->op = opc3 ? VfpOp::Subtract : VfpOp::Add;
        break;
    case 0b1011:
        if (!opc3)
            return false; /* VMOV immediate is not used here. */
        opc3 = (raw & 0x000000c0) >> 6;
        if (opc2 != 0 || opc3 != 0b01)
            return false;
        decoded->op = VfpOp::Move;
        decoded->uses_left = false;
        break;
    default:
        return false;
    }

    decoded->condition = raw >> 28;
    decoded->destination =
        ((raw & 0x0000f000) >> 11) | ((raw & 0x00400000) >> 22);
    decoded->left =
        ((raw & 0x000f0000) >> 15) | ((raw & 0x00000080) >> 7);
    decoded->right =
        ((raw & 0x0000000f) << 1) | ((raw & 0x00000020) >> 5);
    return true;
}

static uint32_t encode_f32(const DecodedVfp &decoded,
                           uint32_t destination, uint32_t left,
                           uint32_t right)
{
    uint32_t instruction = 0;
    switch (decoded.op) {
    case VfpOp::Move:               instruction = 0xeeb00a40; break;
    case VfpOp::Add:                instruction = 0xee300a00; break;
    case VfpOp::Subtract:           instruction = 0xee300a40; break;
    case VfpOp::Multiply:           instruction = 0xee200a00; break;
    case VfpOp::MultiplyAccumulate: instruction = 0xee000a00; break;
    }

    instruction |= ((destination & 0x1e) << 11) |
                   ((destination & 0x01) << 22);
    if (decoded.uses_left) {
        instruction |= ((left & 0x1e) << 15) |
                       ((left & 0x01) << 7);
    }
    instruction |= ((right & 0x1e) >> 1) |
                   ((right & 0x01) << 5);
    return instruction;
}

static uint32_t conditional_branch(uintptr_t source, uintptr_t destination,
                                   uint8_t condition)
{
    intptr_t words = ((intptr_t)destination - (intptr_t)source) / 4 - 2;
    return ((uint32_t)condition << 28) | 0x0a000000 |
           ((uint32_t)words & 0x00ffffff);
}

/*
 * Expand one short-vector instruction into its lanes.
 *
 * Kept in one helper so installation and any future opcode self-test cannot
 * drift into two different lane-wrapping implementations.
 *
 * Lane iteration follows VFP: the register file is four banks of eight, a
 * vector wraps inside its own bank, and an operand that lives in bank 0 is a
 * scalar and does not advance. Only the second operand can be scalar in the
 * forms this binary uses.
 */
static void generate_lanes(const DecodedVfp &decoded, unsigned int lanes,
                           unsigned int stride, uint32_t *out)
{
    /* Advance one operand by STRIDE for the next lane, wrapping inside its own
     * bank of eight. The step is a parameter rather than a hard-coded 1 so the
     * expansion states the geometry it was given; this binary asks for STRIDE=1
     * throughout.
     *
     * Only Fm - the second source - can be a scalar: VFP reads it once and
     * holds it if it lives in bank 0, which is how a vector gets multiplied by
     * a single gain. Fd and Fn always advance. (Fd in bank 0 would make the
     * whole instruction scalar, so such an instruction has no business being in
     * the patch list in the first place.) */
    auto next = [stride](int reg) {
        return (reg & 0x18) | ((reg + (int)stride) & 0x7);
    };
    auto next_source = [&next](int reg) {
        if ((reg & 0x18) == 0)  /* bank 0: scalar, read once and held */
            return reg;
        return next(reg);
    };

    int destination = decoded.destination;
    int left = decoded.left;
    int right = decoded.right;

    for (unsigned int lane = 0; lane < lanes; lane++) {
        *out++ = encode_f32(decoded, destination, left, right);
        destination = next(destination);
        left = next(left);
        right = next_source(right);
    }
}

static bool install_vector_patch(so_module *mod, const VectorPatch &patch)
{
    uint32_t *instruction =
        (uint32_t *)(mod->text_base + patch.offset);
    if (*instruction != patch.expected) {
        warning("VFP vector patch at +0x%08x: expected %08x, found %08x - "
                "skipped\n", patch.offset, patch.expected, *instruction);
        return false;
    }

    DecodedVfp decoded = {};
    if (!decode_f32(*instruction, &decoded)) {
        warning("VFP vector patch at +0x%08x: opcode %08x is outside the "
                "validated F32 subset\n", patch.offset, *instruction);
        return false;
    }

    /*
     * push {r4,r5}; vmrs r4,FPSCR; mov r5,r4;
     * bic r4,r4,#0x370000; vmsr FPSCR,r4.
     *
     * Clearing LEN/STRIDE is essential under qemu, which still implements the
     * old mode; it is harmless on ARMv8 where those fields read as zero.
     */
    static const uint32_t prologue[] = {
        0xe92d0030,
        0xeef14a10,
        0xe1a05004,
        0xe3c44837,
        0xeee14a10,
    };
    /*
     * vmsr FPSCR,r5; pop {r4,r5}; ldr pc,[pc,#-4]; .word return_address.
     */
    static const uint32_t epilogue[] = {
        0xeee15a10,
        0xe8bd0030,
        0xe51ff004,
        0x00000000,
    };

    const size_t words =
        sizeof(prologue) / sizeof(prologue[0]) +
        patch.lanes +
        sizeof(epilogue) / sizeof(epilogue[0]);
    uintptr_t trampoline_address =
        so_alloc_arena(mod, B_RANGE, B_OFFSET((uintptr_t)instruction),
                       words * sizeof(uint32_t));
    if (!trampoline_address) {
        warning("VFP vector patch at +0x%08x: no nearby trampoline space\n",
                patch.offset);
        return false;
    }

    uint32_t trampoline[5 + 8 + 4] = {};
    uint32_t *out = trampoline;
    memcpy(out, prologue, sizeof(prologue));
    out += sizeof(prologue) / sizeof(prologue[0]);

    generate_lanes(decoded, patch.lanes, patch.stride, out);
    out += patch.lanes;

    memcpy(out, epilogue, sizeof(epilogue));
    out[3] = (uint32_t)((uintptr_t)instruction + sizeof(uint32_t));
    memcpy((void *)trampoline_address, trampoline, words * sizeof(uint32_t));

    *instruction = conditional_branch((uintptr_t)instruction,
                                      trampoline_address,
                                      decoded.condition);
    __builtin___clear_cache((char *)trampoline_address,
                            (char *)(trampoline_address +
                                     words * sizeof(uint32_t)));
    __builtin___clear_cache((char *)instruction,
                            (char *)(instruction + 1));
    return true;
}

/* ------------------------------------------------------------------------
 * Self-test
 *
 * The patch list was hand-derived from a disassembly, and every part of it -
 * which registers an opcode names, how a vector wraps inside its bank, whether
 * the second operand is a scalar - is a place to be quietly wrong. Wrong here
 * does not crash. It detunes the audio mixer on a console that cannot be
 * attached to a debugger, in a way that sounds like a bad port.
 *
 * qemu-arm still implements FPSCR LEN/STRIDE, so on the verification host the
 * original instruction and the scalar expansion can both be executed, on
 * identical inputs, and compared register for register. That is the strongest
 * statement available without the hardware: not "the expansion looks right"
 * but "the expansion computes what the vector instruction computed".
 *
 * Trying to infer this from the game instead does not work, and the failed
 * attempt is worth recording: the mixer's output depends on how much game time
 * elapsed, so two runs of the same build already disagree once the audio stops
 * being silence. There is nothing to compare against.
 * ------------------------------------------------------------------------ */

/* push {r4,r5,lr} / vldmia r0,{s0-s31} / vmrs r4,fpscr */
static const uint32_t kStubHead[] = {
    0xe92d4030,
    0xec900a20,
    0xeef14a10,
};
/* vmsr fpscr,r4 / vstmia r1,{s0-s31} / pop {r4,r5,pc} */
static const uint32_t kStubTail[] = {
    0xeee14a10,
    0xec810a20,
    0xe8bd8030,
};

typedef void (*StubFn)(const uint32_t *in, uint32_t *out);

/*
 * Assemble and run one stub. `configure` is the single instruction that
 * derives the working FPSCR in r5 from the saved one in r4, and `body` is what
 * runs under it.
 */
static bool run_stub(void *page, uint32_t configure,
                     const uint32_t *body, unsigned int body_words,
                     const uint32_t *in, uint32_t *out)
{
    uint32_t *code = (uint32_t *)page;
    unsigned int n = 0;

    for (unsigned int i = 0; i < sizeof(kStubHead) / sizeof(kStubHead[0]); i++)
        code[n++] = kStubHead[i];
    code[n++] = configure;
    code[n++] = 0xeee15a10;                 /* vmsr fpscr, r5 */
    for (unsigned int i = 0; i < body_words; i++)
        code[n++] = body[i];
    for (unsigned int i = 0; i < sizeof(kStubTail) / sizeof(kStubTail[0]); i++)
        code[n++] = kStubTail[i];

    __builtin___clear_cache((char *)code, (char *)(code + n));
    ((StubFn)(void *)code)(in, out);
    return true;
}

static unsigned int selftest_vfp_expansion(void)
{
    /* Two pages: one holds the stub being assembled, and it is rewritten for
     * every case rather than kept around. */
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        warning("VFP self-test: no executable scratch page\n");
        return 0;
    }

    /* Distinct, exactly representable, mixed-sign inputs. Distinct matters:
     * identical register contents would hide a lane that reads the wrong
     * register, which is the most likely mistake in a hand-decoded list. */
    uint32_t input[32];
    for (int i = 0; i < 32; i++) {
        float v = (float)(i + 1) * ((i & 1) ? -0.25f : 0.5f);
        memcpy(&input[i], &v, sizeof(v));
    }

    unsigned int agreed = 0, disagreed = 0, vector_capable = 0;

    for (const VectorPatch &patch : kVectorPatches) {
        DecodedVfp decoded = {};
        if (!decode_f32(patch.expected, &decoded))
            continue;

        uint32_t lanes[8] = {};
        generate_lanes(decoded, patch.lanes, patch.stride, lanes);

        /* The condition code is carried by the branch that reaches the
         * trampoline, so the arithmetic itself is compared unconditionally. */
        uint32_t original = (patch.expected & 0x0fffffff) | 0xe0000000;

        /* Reference FPSCR carries both LEN (18:16) and STRIDE (21:20). This
         * binary is STRIDE=1 throughout, so the stride term is zero today, but
         * it is derived rather than assumed: the reference has to be built from
         * the same geometry the expansion uses or the comparison below is
         * against the wrong vector. The control value fits one rotated
         * immediate - orr r5,r4,#ctrl with rot=8, low byte = ctrl>>16 - and bic
         * clears the same four bits.
         *
         * Note this makes the self-test blind to the geometry itself: both
         * sides read patch.lanes/patch.stride, so a wrong pair agrees with
         * itself and passes. Checking lanes/stride against the binary is
         * analysis/vfp_coverage.py's job, not this one's. */
        uint32_t ctrl = ((uint32_t)(patch.lanes - 1) << 16) |
                        ((uint32_t)(patch.stride - 1) << 20);
        uint32_t set_len = 0xe3845800 | (ctrl >> 16);   /* orr r5,r4,#ctrl */
        uint32_t clear   = 0xe3c45837;             /* bic r5,r4,#0x370000 */

        uint32_t vector_out[32], scalar_out[32], single_out[32];
        run_stub(page, set_len, &original, 1, input, vector_out);
        run_stub(page, clear, lanes, patch.lanes, input, scalar_out);
        run_stub(page, clear, &original, 1, input, single_out);

        /* Does this host implement short vectors at all? If the vector run and
         * the deliberately-scalar run of the *same* opcode agree, LEN was
         * ignored - and then a mismatch below would say nothing about our
         * expansion. Reported rather than assumed. */
        if (memcmp(vector_out, single_out, sizeof(vector_out)) != 0)
            vector_capable++;

        if (memcmp(vector_out, scalar_out, sizeof(vector_out)) == 0) {
            agreed++;
            continue;
        }

        disagreed++;
        for (int i = 0; i < 32; i++) {
            if (vector_out[i] == scalar_out[i])
                continue;
            warning("VFP self-test +0x%08x (%08x, %u lanes): s%d vector=%08x "
                    "expansion=%08x\n", patch.offset, patch.expected,
                    patch.lanes, i, vector_out[i], scalar_out[i]);
        }
    }

    munmap(page, 4096);

    if (!vector_capable) {
        warning("VFP self-test: this host ignores FPSCR LEN, so the reference "
                "side of the comparison is not a vector - result is not "
                "meaningful here\n");
        return 0;
    }

    trace("VFP self-test: %u/%u expansions reproduce short-vector arithmetic "
          "exactly (%u disagreed, %u opcodes confirmed vectorising on this "
          "host)", agreed, agreed + disagreed, disagreed, vector_capable);
    return disagreed;
}

unsigned int patch_vfp_short_vectors(so_module *mod)
{
    const char *selftest = port_getenv("VFP_SELFTEST");
    if (selftest && *selftest && *selftest != '0')
        selftest_vfp_expansion();

    /*
     * The escape hatch exists to make the expansion falsifiable.
     *
     * qemu-arm still implements FPSCR LEN/STRIDE, so with the patch disabled
     * the mixer runs the game's original short vectors and produces the
     * reference PCM. With it enabled the same mixer runs our scalar
     * trampolines. The AudioTrack digest of the two runs has to match, and if
     * it does not, the bug is here rather than on a console nobody can attach
     * a debugger to.
     *
     * Off by default: the R36S is ARMv8, where LEN/STRIDE read as zero and
     * unpatched vector arithmetic silently computes only lane zero.
     */
    const char *disabled = port_getenv("NO_VFP_PATCH");
    if (disabled && *disabled && *disabled != '0') {
        trace("VFP short vectors: patch disabled by " PORT_ENV_PREFIX "_NO_VFP_PATCH; "
              "relying on the host implementing LEN/STRIDE (qemu only)");
        return 0;
    }

    /*
     * Per-region, not just a total.
     *
     * "29/29" says every region took the patch; it does not say which regions
     * exist or what shape they are, and a wrong lane count patches cleanly and
     * still detunes the mixer - that is exactly how the 6-lane artefact stayed
     * invisible. Printing the geometry means the device log states the effective
     * lanes/stride per region, so "4 and 8, step 1" can be read back off the
     * hardware instead of inferred from the source.
     */
    unsigned int patched = 0;
    for (const VectorPatch &patch : kVectorPatches) {
        const bool ok = install_vector_patch(mod, patch);
        if (ok)
            patched++;
        trace("VFP region +0x%08x lanes=%u stride=%u %s",
              patch.offset, (unsigned int)patch.lanes,
              (unsigned int)patch.stride,
              ok ? "expanded" : "SKIPPED (instruction did not match)");
    }

    trace("VFP short vectors: expanded %u/%zu audio instructions",
          patched, sizeof(kVectorPatches) / sizeof(kVectorPatches[0]));
    return patched;
}
