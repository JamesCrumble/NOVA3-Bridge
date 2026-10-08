/*
 * S3TC (BC1/BC2/BC3) block decoding.
 *
 * Written here rather than vendored, for the same reason src/atc_decompress.cpp
 * is: the format has no ambiguity, and a dependency would be larger than the
 * thing it replaces. PVRTC is the opposite case and is vendored from PowerVR in
 * third_party/ because its bilinear-upsample decode is genuinely subtle and
 * Imagination published the reference.
 *
 * The colour block, shared by all four formats, is the whole of DXT1:
 *
 *   bytes 0-1   color0, RGB565 little-endian
 *   bytes 2-3   color1, RGB565 little-endian
 *   bytes 4-7   16 two-bit selectors, row-major, texel (0,0) in the low bits
 *
 * THE TRAP, and it is the one that makes DXT3/DXT5 look plausible but wrong:
 * the palette has two modes in DXT1, chosen by whether color0 > color1 -
 *
 *   color0 >  color1   four opaque colours; 2 and 3 are 2:1 and 1:2 blends
 *   color0 <= color1   three colours; 2 is the midpoint and 3 is transparent
 *
 * - and in DXT3 and DXT5 that second mode DOES NOT EXIST. Those formats always
 * use the four-colour palette whatever the endpoint order, because they carry
 * alpha in a block of their own and have no need of a punchthrough slot. Reusing
 * DXT1's colour decode unchanged would silently corrupt every block that happens
 * to encode color0 <= color1, which is common in smooth gradients - exactly what
 * a muzzle flash is made of. Hence decode_colours() taking the mode explicitly
 * instead of inferring it.
 *
 * Alpha:
 *   DXT3  8 bytes, 4 bits per texel, straight values, no interpolation.
 *   DXT5  8 bytes: two endpoints then 16 three-bit indices into an eight-entry
 *         ramp, whose shape - like the colour palette - depends on the endpoint
 *         order, and here that ordering test IS real.
 *
 * Why this port needs any of it: the Mali-G31 in the R36S is Bifrost, which
 * exposes ETC and ASTC and no S3TC at all. Real Racing 3 ships 333
 * DXT1 and 147 DXT3 textures inside its .m3g files, so on the console every one
 * of those uploads is rejected. For DXT1 that rendered the world white; for
 * DXT3 it rendered blood and muzzle flashes as opaque squares and left parts of
 * the world untextured, because a quad with no texture draws as flat white.
 * It all works under the harness only because Mesa's llvmpipe has S3TC.
 */
#include "dxt_decompress.h"

#include <cstring>

namespace dxt {
namespace {

struct Rgba {
    std::uint8_t r, g, b, a;
};

/*
 * RGB565 -> RGB888 by bit replication, not by shifting alone.
 *
 * A plain `r5 << 3` maps 31 to 248, so the brightest red a texture can encode
 * comes out 3% dark and white is never quite white. Replicating the high bits
 * into the low ones maps 31 to 255 exactly, which is what every reference
 * decoder does and what the hardware does.
 */
inline Rgba unpack565(std::uint16_t c)
{
    const std::uint8_t r5 = static_cast<std::uint8_t>((c >> 11) & 0x1f);
    const std::uint8_t g6 = static_cast<std::uint8_t>((c >> 5) & 0x3f);
    const std::uint8_t b5 = static_cast<std::uint8_t>(c & 0x1f);

    Rgba out;
    out.r = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
    out.g = static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4));
    out.b = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 4));
    out.a = 255;
    return out;
}

/* (2a + b) / 3, rounded the way the reference decoders round it. */
inline std::uint8_t blend_2_1(std::uint8_t a, std::uint8_t b)
{
    return static_cast<std::uint8_t>((2 * static_cast<int>(a) +
                                      static_cast<int>(b)) / 3);
}

inline std::uint8_t blend_1_1(std::uint8_t a, std::uint8_t b)
{
    return static_cast<std::uint8_t>((static_cast<int>(a) +
                                      static_cast<int>(b)) / 2);
}

/*
 * The colour half of a block.
 *
 * `allow_punchthrough` is DXT1-only and is the difference described at the top
 * of the file. When it is false the four-colour palette is used unconditionally,
 * which is what DXT3 and DXT5 require.
 */
void decode_colours(const std::uint8_t *block, bool allow_punchthrough,
                    bool punchthrough_alpha, Rgba palette[4],
                    std::uint32_t *selectors)
{
    const std::uint16_t c0 =
        static_cast<std::uint16_t>(block[0] | (block[1] << 8));
    const std::uint16_t c1 =
        static_cast<std::uint16_t>(block[2] | (block[3] << 8));
    *selectors = static_cast<std::uint32_t>(block[4]) |
                 (static_cast<std::uint32_t>(block[5]) << 8) |
                 (static_cast<std::uint32_t>(block[6]) << 16) |
                 (static_cast<std::uint32_t>(block[7]) << 24);

    palette[0] = unpack565(c0);
    palette[1] = unpack565(c1);

    if (!allow_punchthrough || c0 > c1) {
        palette[2].r = blend_2_1(palette[0].r, palette[1].r);
        palette[2].g = blend_2_1(palette[0].g, palette[1].g);
        palette[2].b = blend_2_1(palette[0].b, palette[1].b);
        palette[2].a = 255;

        palette[3].r = blend_2_1(palette[1].r, palette[0].r);
        palette[3].g = blend_2_1(palette[1].g, palette[0].g);
        palette[3].b = blend_2_1(palette[1].b, palette[0].b);
        palette[3].a = 255;
    } else {
        palette[2].r = blend_1_1(palette[0].r, palette[1].r);
        palette[2].g = blend_1_1(palette[0].g, palette[1].g);
        palette[2].b = blend_1_1(palette[0].b, palette[1].b);
        palette[2].a = 255;

        /* The punchthrough slot. Black either way; only the alpha differs,
         * and only the RGBA spelling of DXT1 looks at it. */
        palette[3].r = 0;
        palette[3].g = 0;
        palette[3].b = 0;
        palette[3].a = punchthrough_alpha ? 0 : 255;
    }
}

/* DXT3: four bits per texel, straight through, low nibble first. */
inline std::uint8_t dxt3_alpha(const std::uint8_t *block, int texel)
{
    const std::uint8_t byte = block[texel >> 1];
    const std::uint8_t nibble = (texel & 1) ? static_cast<std::uint8_t>(byte >> 4)
                                            : static_cast<std::uint8_t>(byte & 0x0f);
    /* Replicate rather than shift, for the same reason as unpack565: 15 has to
     * become 255, not 240, or nothing is ever fully opaque. */
    return static_cast<std::uint8_t>((nibble << 4) | nibble);
}

/* DXT5: build the eight-entry alpha ramp from the two endpoints. */
void dxt5_alpha_ramp(std::uint8_t a0, std::uint8_t a1, std::uint8_t ramp[8])
{
    ramp[0] = a0;
    ramp[1] = a1;
    if (a0 > a1) {
        /* Six interpolated values, no special slots. */
        for (int i = 0; i < 6; i++)
            ramp[2 + i] = static_cast<std::uint8_t>(
                ((6 - i) * static_cast<int>(a0) +
                 (1 + i) * static_cast<int>(a1)) / 7);
    } else {
        /* Four interpolated values, then explicit 0 and 255. */
        for (int i = 0; i < 4; i++)
            ramp[2 + i] = static_cast<std::uint8_t>(
                ((4 - i) * static_cast<int>(a0) +
                 (1 + i) * static_cast<int>(a1)) / 5);
        ramp[6] = 0;
        ramp[7] = 255;
    }
}

} // namespace

std::size_t block_bytes(Format format)
{
    return (format == Format::Dxt3 || format == Format::Dxt5) ? 16u : 8u;
}

bool decode(Format format, const void *data, std::size_t image_size,
            int width, int height, std::uint8_t *rgba)
{
    if (!data || !rgba || width <= 0 || height <= 0)
        return false;

    const std::size_t blocks_x = (static_cast<std::size_t>(width) + 3) / 4;
    const std::size_t blocks_y = (static_cast<std::size_t>(height) + 3) / 4;
    const std::size_t stride = block_bytes(format);

    /* The caller's buffer has to actually hold them. A short image_size here is
     * a malformed upload, not something to read past the end of. */
    if (blocks_x && blocks_y > SIZE_MAX / blocks_x)
        return false;
    const std::size_t blocks = blocks_x * blocks_y;
    if (blocks && stride > SIZE_MAX / blocks)
        return false;
    if (image_size < blocks * stride)
        return false;

    const bool is_dxt1 = format == Format::Dxt1Rgb || format == Format::Dxt1Rgba;
    const bool punchthrough_alpha = format == Format::Dxt1Rgba;

    const std::uint8_t *src = static_cast<const std::uint8_t *>(data);
    const std::size_t   row_pitch = static_cast<std::size_t>(width) * 4u;

    for (std::size_t by = 0; by < blocks_y; by++) {
        for (std::size_t bx = 0; bx < blocks_x; bx++) {
            const std::uint8_t *block = src + (by * blocks_x + bx) * stride;
            /* In DXT3 and DXT5 the alpha block comes first and the colour
             * block is the second half. */
            const std::uint8_t *alpha_block = is_dxt1 ? nullptr : block;
            const std::uint8_t *colour_block = is_dxt1 ? block : block + 8;

            Rgba palette[4];
            std::uint32_t selectors = 0;
            decode_colours(colour_block, is_dxt1, punchthrough_alpha,
                           palette, &selectors);

            std::uint8_t ramp[8];
            std::uint64_t alpha_indices = 0;
            if (format == Format::Dxt5) {
                dxt5_alpha_ramp(alpha_block[0], alpha_block[1], ramp);
                /* 16 three-bit indices packed into the remaining six bytes,
                 * little-endian, texel (0,0) in the low bits. */
                for (int i = 0; i < 6; i++)
                    alpha_indices |=
                        static_cast<std::uint64_t>(alpha_block[2 + i]) << (8 * i);
            }

            for (int py = 0; py < 4; py++) {
                const std::size_t y = by * 4u + static_cast<std::size_t>(py);
                if (y >= static_cast<std::size_t>(height))
                    break;      /* the bottom edge of a non-multiple-of-4 image */

                for (int px = 0; px < 4; px++) {
                    const std::size_t x =
                        bx * 4u + static_cast<std::size_t>(px);
                    if (x >= static_cast<std::size_t>(width))
                        break;  /* likewise the right edge */

                    const int texel = py * 4 + px;
                    const Rgba &c =
                        palette[(selectors >> (texel * 2)) & 0x3u];

                    std::uint8_t alpha = c.a;
                    if (format == Format::Dxt3) {
                        alpha = dxt3_alpha(alpha_block, texel);
                    } else if (format == Format::Dxt5) {
                        alpha = ramp[(alpha_indices >> (texel * 3)) & 0x7u];
                    }

                    std::uint8_t *out = rgba + y * row_pitch + x * 4u;
                    out[0] = c.r;
                    out[1] = c.g;
                    out[2] = c.b;
                    out[3] = alpha;
                }
            }
        }
    }

    return true;
}

} // namespace dxt
