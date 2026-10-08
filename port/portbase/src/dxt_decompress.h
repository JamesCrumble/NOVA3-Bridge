#pragma once

#include <cstddef>
#include <cstdint>

namespace dxt {

/*
 * The four S3TC formats this game can ship, in GL's spelling:
 *
 *   Dxt1Rgb   GL_COMPRESSED_RGB_S3TC_DXT1_EXT    0x83f0   8 bytes/block
 *   Dxt1Rgba  GL_COMPRESSED_RGBA_S3TC_DXT1_EXT   0x83f1   8 bytes/block
 *   Dxt3      GL_COMPRESSED_RGBA_S3TC_DXT3_EXT   0x83f2  16 bytes/block
 *   Dxt5      GL_COMPRESSED_RGBA_S3TC_DXT5_EXT   0x83f3  16 bytes/block
 */
enum class Format {
    Dxt1Rgb,
    Dxt1Rgba,
    Dxt3,
    Dxt5,
};

/* 8 for the DXT1 pair, 16 for DXT3 and DXT5. */
std::size_t block_bytes(Format format);

/* Decode to tightly packed RGBA8888. */
bool decode(Format format, const void *data, std::size_t image_size,
            int width, int height, std::uint8_t *rgba);

} // namespace dxt
