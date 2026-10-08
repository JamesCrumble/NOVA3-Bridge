/*
 * The single translation unit that instantiates stb_truetype.
 *
 * Vendored rather than linked because nine of the thirteen faces the game
 * ships (EurostileLTStd*, myriadp0, minion_pro_italic) carry the OTTO tag,
 * i.e. CFF/Type2 outlines. Anything without a CFF interpreter renders nothing
 * from them, and the only packaged alternative that has one - FreeType - drags
 * libpng16, libbrotlidec and libbrotlicommon into the zip for a feature set
 * (hinting) that Skia was not using on Android either.
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb/stb_truetype.h"
