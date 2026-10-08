/*
 * Shader compilation probe.
 *
 * M4's criterion is "the EGL context is current and the game's shaders
 * compile". The loader cannot answer that by inspection: the engine compiles
 * its shaders from its own thread, checks the status itself, and says nothing
 * on stderr when one fails - it just draws nothing afterwards, which looks
 * exactly like a working port with a black frame.
 *
 * So glCompileShader and glLinkProgram are intercepted. Each call is forwarded
 * to the real driver entry point and the status is read back; a failure is
 * printed with the driver's info log, and the counts are what the loader waits
 * on before it is willing to claim GL is up.
 *
 * This is observation, not emulation: the driver does the work and its verdict
 * is reported verbatim. A shader that does not compile makes the milestone
 * fail, which is the point.
 *
 * The table below is listed before both GL tables in so_dynamic_libraries, so
 * the game binds these instead of the raw entry points. Shader calls forward
 * through the GLES2 glad globals. Draw calls must select the live GLES1 table:
 * this fixed-function game never initialises the separate GLES2 glad globals.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <algorithm>
#include <memory>
#include <new>
#include <vector>

#include <SDL2/SDL.h>

#include "khronos/glad.h"
#include "atc_decompress.h"
#include "dxt_decompress.h"
#include "third_party/powervr/PVRTDecompress.h"

#include "gl_diag.h"
#include "gl_stats.h"
#include "so_util.h"
#include "thunk_gen.h"
#include "trace.h"
#include "port_env.h"
#include "gl_bridge.h"

/* Written by the game's thread, read by the loader's. */
static std::atomic<int> g_shaders_ok(0);
static std::atomic<int> g_shaders_failed(0);
static std::atomic<int> g_programs_ok(0);
static std::atomic<int> g_programs_failed(0);
static std::atomic<long> g_draws(0);
static std::atomic<long> g_textures(0);

/*
 * DIAGNOSTIC (temporary, 3D-scene investigation): the aggregate draw counter
 * cannot tell a 2D UI quad from scene geometry. The engine batches sprites as
 * 4-vertex GL_TRIANGLE_STRIP through glDrawArrays and submits meshes indexed
 * through glDrawElements, so splitting the counter and keeping the largest
 * count seen answers "did any scene geometry ever reach the driver?".
 */
static std::atomic<long> g_draw_arrays(0);
static std::atomic<long> g_draw_elements(0);
static std::atomic<long> g_draw_elements_indices(0);
static std::atomic<long> g_draw_elements_max(0);
static std::atomic<long> g_draw_arrays_max(0);

static void record_draw_max(std::atomic<long> &slot, long value)
{
    long seen = slot.load();
    while (value > seen && !slot.compare_exchange_weak(seen, value))
        ;
}

extern DynLibFunction symtable_gles1[];
extern DynLibFunction symtable_gles2[];

static uintptr_t find_in_table(DynLibFunction *table, const char *name)
{
    for (int i = 0; table && table[i].symbol; i++) {
        if (strcmp(table[i].symbol, name) == 0)
            return table[i].func;
    }
    return 0;
}

/*
 * Resolve an entry point the way the game itself would have.
 *
 * so_dynamic_libraries lists symtable_glprobe, then symtable_gles1, then
 * symtable_gles2, so a wrapper that shadows a name must forward to whichever of
 * the two GL tables would have won without it - otherwise interposing changes
 * which dispatch executes the call, which is a behaviour change smuggled in
 * under a diagnostic.
 *
 * SDL_GL_GetProcAddress is the last resort rather than the first because
 * symtable_gles1 is the console's path: there it is filled from libmali, and
 * that table being empty is precisely the failure this project has already been
 * bitten by: the wrappers returned without ever reaching the driver, and the
 * result was a black screen with nothing in the log to explain it. Returning 0
 * here is not a quiet fallback - the caller reports it through
 * gl_stats_note_dropped().
 */
static uintptr_t resolve_gl_entry(const char *name)
{
    uintptr_t f = find_in_table(symtable_gles1, name);
    if (!f)
        f = find_in_table(symtable_gles2, name);
    if (!f)
        f = (uintptr_t)SDL_GL_GetProcAddress(name);
    return f;
}

static void dump_log(const char *what, unsigned int obj, bool is_shader)
{
    GLint len = 0;
    if (is_shader)
        glad_glGetShaderiv(obj, GL_INFO_LOG_LENGTH, &len);
    else
        glad_glGetProgramiv(obj, GL_INFO_LOG_LENGTH, &len);

    if (len <= 1) {
        fprintf(stderr, "GL: %s failed (driver gave no log)\n", what);
        fflush(stderr);
        return;
    }

    char *log = (char *)malloc((size_t)len + 1);
    if (!log)
        return;
    log[0] = '\0';
    if (is_shader)
        glad_glGetShaderInfoLog(obj, len, NULL, log);
    else
        glad_glGetProgramInfoLog(obj, len, NULL, log);
    log[len] = '\0';

    fprintf(stderr, "GL: %s failed:\n%s\n", what, log);
    fflush(stderr);
    free(log);
}

/*
 * What the driver actually holds for a shader that refused to compile.
 *
 * "syntax error, unexpected end of file" is not a broken shader, it is a
 * shader that arrived short - and the only way to tell a truncated source from
 * a genuinely malformed one is to read back the exact bytes the driver was
 * given and look at where they stop. glGetShaderSource returns them verbatim,
 * after every rewrite this loader performs, so the number printed here is the
 * end of the chain and can be compared against the file on disk.
 */
static void dump_shader_source(GLuint shader)
{
    GLint length = 0;
    glad_glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
    fprintf(stderr, "GLSTATS: failed shader %u source length=%d bytes "
                    "(as held by the driver)\n", shader, length);
    if (length <= 0) {
        fflush(stderr);
        return;
    }

    char *src = (char *)malloc((size_t)length + 1);
    if (!src)
        return;
    src[0] = '\0';
    glad_glGetShaderSource(shader, length, NULL, src);
    src[length] = '\0';

    /* The tail is the evidence: a truncated file stops mid-statement, a
     * malformed one ends on a complete token. */
    const size_t len = strlen(src);
    const size_t tail = len > 240 ? len - 240 : 0;
    fprintf(stderr, "GLSTATS: failed shader %u strlen=%zu last 240 bytes >>>%s<<<\n",
            shader, len, src + tail);

    /* Written out whole so the source can be diffed against the .xsh files in
     * the donor tree; one file per shader, overwritten on repeat runs. */
    const char *dir = port_getenv("SHADER_DUMP_DIR");
    if (dir && *dir) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/failed-%u.glsl", dir, shader);
        FILE *out = fopen(path, "wb");
        if (out) {
            fwrite(src, 1, len, out);
            fclose(out);
            fprintf(stderr, "GLSTATS: failed shader %u written to %s\n",
                    shader, path);
        }
    }
    fflush(stderr);
    free(src);
}

extern "C" void probe_glCompileShader(GLuint shader)
{
    glad_glCompileShader(shader);

    GLint ok = 0;
    glad_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) {
        g_shaders_ok++;
        return;
    }

    g_shaders_failed++;
    dump_log("shader compile", shader, true);
    dump_shader_source(shader);
}

/*
 * Every linked program, written out shader by shader.
 *
 * The GL census can say which program drew what (bucket prog=NN), but a program
 * id is only a number until its source is on disk next to it. This closes that
 * gap: with <PREFIX>_SHADER_DUMP_DIR set, the file prog<NN>-<sid>-vert.glsl
 * is exactly what the driver compiled for the program the census named - after
 * every rewrite this loader performs, so a shader we broke ourselves shows up
 * as broken here and nowhere else.
 *
 * The donor's .vsh/.fsh files cannot answer this: they are packed, and the
 * engine assembles the real translation unit from .xsh fragments plus its own
 * #define block at runtime.
 */
static void dump_program_sources(GLuint program)
{
    const char *dir = port_getenv("SHADER_DUMP_DIR");
    if (!dir || !*dir)
        return;

    GLuint shaders[8] = {0};
    GLsizei count = 0;
    glad_glGetAttachedShaders(program, 8, &count, shaders);

    for (GLsizei i = 0; i < count; i++) {
        GLint length = 0;
        glad_glGetShaderiv(shaders[i], GL_SHADER_SOURCE_LENGTH, &length);
        if (length <= 0)
            continue;

        GLint type = 0;
        glad_glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);

        char *src = (char *)malloc((size_t)length + 1);
        if (!src)
            return;
        src[0] = '\0';
        glad_glGetShaderSource(shaders[i], length, NULL, src);
        src[length] = '\0';

        char path[4096];
        snprintf(path, sizeof(path), "%s/prog%04u-%u-%s.glsl", dir, program,
                 shaders[i], type == GL_VERTEX_SHADER ? "vert" : "frag");
        FILE *out = fopen(path, "wb");
        if (out) {
            fwrite(src, 1, strlen(src), out);
            fclose(out);
        }
        free(src);
    }
}

extern "C" void probe_glLinkProgram(GLuint program)
{
    glad_glLinkProgram(program);

    GLint ok = 0;
    glad_glGetProgramiv(program, GL_LINK_STATUS, &ok);
    dump_program_sources(program);
    if (ok) {
        g_programs_ok++;
        return;
    }

    g_programs_failed++;
    dump_log("program link", program, false);
}

/*
 * Draw calls.
 *
 * The game imports exactly these two draw entry points. Counting here rather
 * than in the frame loop is the difference the milestone cares about: a game
 * that clears the screen to a colour and presents it swaps buffers just as
 * happily as one that renders, so frames are not evidence of content. A draw
 * call is.
 *
 * Neither wrapper inspects or rewrites its arguments; they are forwarded
 * verbatim to the driver, so a miscount is the only thing that can go wrong
 * here, never a misdraw.
 */
/*
 * What the game clears, and whether a scissor is narrowing it.
 *
 * This exists for one question that cannot be answered from the host: the
 * software cursor leaves a trail on the Mali and not under Mesa, on the same
 * title screen. The cursor is painted straight into the default framebuffer
 * before the swap and nothing restores what was underneath, so it only stays
 * invisible while the game repaints the whole screen every frame. Either the
 * game is not clearing the colour buffer on that screen, or it clears through a
 * scissor smaller than the window, or the clear is fine and the difference is
 * in how the two drivers treat the back buffer after a swap. Those are three
 * different fixes, and choosing between them from a screenshot is how we would
 * ship one that makes it worse.
 *
 * The first lines go out on every boot; <PREFIX>_CURSOR_DIAG=1 raises the
 * budget. Capped either way, because a frame contains several clears.
 */
static bool cursor_diag_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = port_getenv("CURSOR_DIAG");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on != 0;
}

extern "C" void probe_glClear(GLbitfield mask)
{
    using Clear = void (*)(GLbitfield);
    static Clear clear = (Clear)resolve_gl_entry("glClear");

    {
        static int lines = 0;
        /*
         * The first few always, the rest only on request. This used to need
         * <PREFIX>_CURSOR_DIAG=1, which put the answer behind an env var the
         * person holding the console has no reason to set. The signature is
         * four short lines and it is what separates three different fixes, so
         * it is worth paying for on every boot.
         */
        if (lines < (cursor_diag_enabled() ? 24 : 4)) {
            using IsEnabled = GLboolean (*)(GLenum);
            using GetIntegerv = void (*)(GLenum, GLint *);
            static IsEnabled is_enabled =
                (IsEnabled)resolve_gl_entry("glIsEnabled");
            static GetIntegerv get_integer =
                (GetIntegerv)resolve_gl_entry("glGetIntegerv");
            GLint box[4] = {0, 0, 0, 0};
            GLint viewport[4] = {0, 0, 0, 0};
            GLboolean scissored = GL_FALSE;
            if (is_enabled)
                scissored = is_enabled(GL_SCISSOR_TEST);
            if (get_integer) {
                get_integer(GL_SCISSOR_BOX, box);
                get_integer(GL_VIEWPORT, viewport);
            }
            lines++;
            trace("cursor-diag: glClear mask=0x%04x colour=%s scissor=%s "
                  "box=%d,%d %dx%d viewport=%d,%d %dx%d",
                  (unsigned int)mask,
                  (mask & GL_COLOR_BUFFER_BIT) ? "yes" : "NO",
                  scissored ? "on" : "off",
                  box[0], box[1], box[2], box[3],
                  viewport[0], viewport[1], viewport[2], viewport[3]);
        }
    }

    gl_stats_note_clear(mask);
    if (clear)
        clear(mask);
    else
        gl_stats_note_dropped("glClear", "glClear resolved nowhere (tables and SDL)");
    gl_stats_check_error("glClear");
}

/*
 * EXPERIMENT (<PREFIX>_SKIP_COMPOSITE_QUAD=1): drop the opaque full-screen
 * quad that ends the frame.
 *
 * The census measured the race frame: 9505 draws and 2.8 M vertices land in the
 * default framebuffer with depth test and cull on, from VBOs - the world is
 * rasterised - and then a single TRIANGLE_STRIP of 4 vertices is drawn over it
 * with blend off, depth off and cull off, followed only by blended UI quads.
 * An opaque full-screen quad drawn last is the one thing that can hide a scene
 * that was correctly drawn, and nothing else in the frame matches that
 * signature (every UI quad has blend on).
 *
 * Skipping it does not fix anything - if the world reappears, the bug is in
 * what that quad composites, and this switch says so in one run instead of
 * three. It is a diagnostic, off by default, and never set by the port.
 */
static bool skip_composite_quad_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = port_getenv("SKIP_COMPOSITE_QUAD");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on != 0;
}

static bool is_opaque_fullscreen_quad(GLenum mode, GLsizei count)
{
    if (mode != GL_TRIANGLE_STRIP || count != 4)
        return false;

    GLuint program = 0, framebuffer = 0;
    int blend = 0, depth_test = 0;
    gl_stats_current_state(&program, &framebuffer, &blend, &depth_test);
    return framebuffer == 0 && !blend && !depth_test;
}

extern "C" void probe_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    long draw_no = ++g_draws;
    g_draw_arrays++;
    record_draw_max(g_draw_arrays_max, count);
    if (draw_no <= 12)
        trace("GL drawArrays #%ld mode=0x%04x first=%d count=%d", draw_no,
              mode, first, count);
    gl_stats_note_draw(mode, count, 0, 0);

    if (skip_composite_quad_enabled() && is_opaque_fullscreen_quad(mode, count)) {
        static long skipped = 0;
        GLuint program = 0;
        gl_stats_current_state(&program, NULL, NULL, NULL);
        if (++skipped <= 8)
            trace("EXPERIMENT: skipped opaque full-screen quad #%ld "
                  "(draw #%ld, program %u)", skipped, draw_no, program);
        return;
    }

    using DrawArrays = void (*)(GLenum, GLint, GLsizei);
    static DrawArrays draw =
        (DrawArrays)resolve_gl_entry("glDrawArrays");
    if (draw)
        draw(mode, first, count);
    else
        gl_stats_note_dropped("glDrawArrays",
                              "glDrawArrays resolved nowhere (tables and SDL)");
    gl_stats_check_error("glDrawArrays");
}

extern "C" void probe_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                                     const void *indices)
{
    long draw_no = ++g_draws;
    g_draw_elements++;
    g_draw_elements_indices += count;
    record_draw_max(g_draw_elements_max, count);
    if (draw_no <= 12)
        trace("GL drawElements #%ld mode=0x%04x count=%d type=0x%04x", draw_no,
              mode, count, type);
    gl_stats_note_draw(mode, count, 1, type);
    using DrawElements = void (*)(GLenum, GLsizei, GLenum, const void *);
    static DrawElements draw =
        (DrawElements)resolve_gl_entry("glDrawElements");
    if (draw)
        draw(mode, count, type, indices);
    else
        gl_stats_note_dropped("glDrawElements",
                              "glDrawElements resolved nowhere (tables and SDL)");
    gl_stats_check_error("glDrawElements");
}

/*
 * Texture uploads are the fixed-function equivalent of a material becoming
 * live GPU content. The dimensions are deliberately not filtered: mip levels
 * and small UI textures are still real uploads performed by the engine.
 */
extern "C" void probe_glTexImage2D(GLenum target, GLint level,
                                   GLint internalformat, GLsizei width,
                                   GLsizei height, GLint border, GLenum format,
                                   GLenum type, const void *pixels)
{
    g_textures++;
    using TexImage2D = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                                GLenum, GLenum, const void *);
    static TexImage2D upload =
        (TexImage2D)resolve_gl_entry("glTexImage2D");
    const int forwarded = upload != NULL;
    if (upload)
        upload(target, level, internalformat, width, height, border,
               format, type, pixels);
    else
        gl_stats_note_dropped("glTexImage2D",
                              "glTexImage2D resolved nowhere (tables and SDL)");
    /* Size in bytes as the driver sees it, so the census can be compared with
     * the device's memory budget. 4 bytes per texel is the worst case and the
     * common one here (RGBA8888); the packed 16-bit types halve it. */
    long bytes = (long)width * (long)height *
                 ((type == GL_UNSIGNED_BYTE) ? 4 : 2);
    gl_stats_note_texture_upload(0, (GLenum)internalformat, width, height,
                                 bytes, forwarded);
    gl_stats_check_error("glTexImage2D");
}

static bool extension_present(const char *extensions, const char *wanted)
{
    if (!extensions || !wanted || !*wanted)
        return false;
    size_t length = strlen(wanted);
    const char *at = extensions;
    while ((at = strstr(at, wanted)) != NULL) {
        bool left = at == extensions || at[-1] == ' ';
        bool right = at[length] == '\0' || at[length] == ' ';
        if (left && right)
            return true;
        at += length;
    }
    return false;
}

/*
 * Pretend the driver supports none of the compressed formats.
 *
 * The software decoders exist for the console, and the console is the one place
 * they cannot be tested from here: Mesa's llvmpipe supports S3TC natively, so
 * under the harness the DXT1 path would forward every upload to the driver and
 * the decoder would never run. That is not a verified decoder, it is an unused
 * one - and an unused decoder that turns out to be wrong is discovered on the
 * SD card, three steps from anywhere useful.
 *
 * <PREFIX>_FORCE_SOFTWARE_TEXTURE_DECODE=1 forces the fallback for all three
 * families, which is what a Mali-G31 looks like for S3TC and what an Adreno
 * looks like for PVRTC. It is a testing switch and nothing reads it in a normal
 * run.
 */
static bool force_software_decode(void)
{
    static int forced = -1;
    if (forced < 0) {
        const char *v = port_getenv("FORCE_SOFTWARE_TEXTURE_DECODE");
        forced = (v && *v && *v != '0') ? 1 : 0;
        if (forced)
            trace("texture decode: native compressed support ignored by "
                  "request (" PORT_ENV_PREFIX "_FORCE_SOFTWARE_TEXTURE_DECODE)");
    }
    return forced != 0;
}

static bool driver_supports_pvrtc(void)
{
    static int supported = -1;
    if (supported >= 0)
        return supported != 0;

    using GetString = const GLubyte *(*)(GLenum);
    GetString get_string =
        (GetString)SDL_GL_GetProcAddress("glGetString");
    const char *extensions = get_string
        ? (const char *)get_string(GL_EXTENSIONS) : NULL;
    supported = (!force_software_decode() && extension_present(
        extensions, "GL_IMG_texture_compression_pvrtc")) ? 1 : 0;
    trace("PVRTC: native driver support %s; software fallback %s",
          supported ? "present" : "absent",
          supported ? "disabled" : "enabled");
    return supported != 0;
}

static bool driver_supports_atc(void)
{
    static int supported = -1;
    if (supported >= 0)
        return supported != 0;

    using GetString = const GLubyte *(*)(GLenum);
    GetString get_string =
        (GetString)SDL_GL_GetProcAddress("glGetString");
    const char *extensions = get_string
        ? (const char *)get_string(GL_EXTENSIONS) : NULL;
    supported = (!force_software_decode() && extension_present(
        extensions, "GL_AMD_compressed_ATC_texture")) ? 1 : 0;
    trace("ATC: native driver support %s; software fallback %s",
          supported ? "present" : "absent",
          supported ? "disabled" : "enabled");
    return supported != 0;
}

/*
 * Names for the compressed formats worth naming, so an unsupported upload
 * reports as something a person can act on instead of a bare number.
 *
 * The list is every format this engine can emit. m3g::OpenGLESRenderer::BindImage
 * (+0x4225e0) switches on the M3G Image2D format byte and builds the GL enum by
 * addition - fmt 110 becomes 0x8300 + 0xf0, fmt 113 becomes 0x8300 + 0xf2 - and
 * the M3G formats it handles run 96..125. Only 110 (DXT1) and 113 (DXT3) appear
 * in this game's 773 .m3g files, but the others are reachable code, so if one
 * ever shows up it should arrive with a name attached.
 */
static const char *compressed_format_name(GLenum format)
{
    switch (format) {
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:       return "S3TC DXT1 RGB";
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:      return "S3TC DXT1 RGBA";
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:      return "S3TC DXT3";
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:      return "S3TC DXT5";
    case GL_ATC_RGB_AMD:                        return "ATC RGB";
    case GL_ATC_RGBA_EXPLICIT_ALPHA_AMD:        return "ATC RGBA explicit";
    case GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG:    return "PVRTC RGB 4bpp";
    case GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG:    return "PVRTC RGB 2bpp";
    case GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG:   return "PVRTC RGBA 4bpp";
    case GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG:   return "PVRTC RGBA 2bpp";
    case 0x87ee:                                return "ATC RGBA interpolated";
    case 0x8d64:                                return "ETC1 RGB8";
    default:                                    return "unknown";
    }
}

/*
 * Is this enum a compressed internal format at all?
 *
 * glCompressedTexImage2D is only ever called with one, so this is really a
 * sanity check on the ranges rather than a filter - but it keeps the "no
 * decoder" warning from firing on something that is not a texture format.
 */
static bool is_compressed_format(GLenum format)
{
    return (format >= 0x83f0 && format <= 0x83f3) ||   /* S3TC        */
           (format >= 0x8c00 && format <= 0x8c03) ||   /* PVRTC       */
           (format >= 0x8c92 && format <= 0x8c93) ||   /* ATC         */
           format == 0x87ee ||                          /* ATC interp  */
           format == 0x8d64 ||                          /* ETC1        */
           (format >= 0x9270 && format <= 0x9279) ||   /* ETC2/EAC    */
           (format >= 0x93b0 && format <= 0x93bd);     /* ASTC        */
}

static bool atc_format(GLenum format, bool *explicit_alpha)
{
    switch (format) {
    case GL_ATC_RGB_AMD:
        *explicit_alpha = false;
        return true;
    case GL_ATC_RGBA_EXPLICIT_ALPHA_AMD:
        *explicit_alpha = true;
        return true;
    default:
        return false;
    }
}

static bool decode_atc(GLenum format, GLsizei width, GLsizei height,
                       GLsizei image_size, const void *data,
                       std::vector<unsigned char> *rgba)
{
    bool explicit_alpha = false;
    if (!atc_format(format, &explicit_alpha) || width <= 0 || height <= 0 ||
        image_size < 0 || !data)
        return false;

    const std::size_t blocks_x = (static_cast<std::size_t>(width) + 3) / 4;
    const std::size_t blocks_y = (static_cast<std::size_t>(height) + 3) / 4;
    const std::size_t bytes_per_block = explicit_alpha ? 16u : 8u;
    if (blocks_x > SIZE_MAX / blocks_y ||
        blocks_x * blocks_y > static_cast<std::size_t>(image_size) /
                                  bytes_per_block)
        return false;

    const std::size_t decoded_size = static_cast<std::size_t>(width) *
                                     static_cast<std::size_t>(height) * 4u;
    try {
        rgba->resize(decoded_size);
    } catch (const std::bad_alloc &) {
        trace("ATC: cannot allocate %llu-byte decode buffer",
              (unsigned long long)decoded_size);
        return false;
    }

    const bool ok = explicit_alpha
        ? atc::decode_rgba_explicit(data, static_cast<std::size_t>(image_size),
                                    width, height, rgba->data())
        : atc::decode_rgb(data, static_cast<std::size_t>(image_size),
                          width, height, rgba->data());
    if (!ok)
        rgba->clear();
    return ok;
}

/*
 * DXT1 support, which the R36S does not have.
 *
 * The Mali-G31 is Bifrost: ETC2 and ASTC, no S3TC in any spelling. Drivers that
 * do have it are inconsistent about which name they advertise - desktop GL and
 * ANGLE say GL_EXT_texture_compression_s3tc, several ES drivers ship only
 * GL_EXT_texture_compression_dxt1 because they support the 8-byte block and not
 * DXT3/DXT5 - so both are accepted. Claiming support we do not have is the
 * failure that renders white; claiming we lack support we do have only costs
 * some decode time.
 */
/*
 * Native S3TC, which the R36S does not have, asked per family.
 *
 * The two extension names are not interchangeable and that matters here.
 * GL_EXT_texture_compression_s3tc is the whole set; several ES drivers ship
 * only GL_EXT_texture_compression_dxt1, which is the 8-byte block and nothing
 * else - a driver advertising it can take a DXT1 upload and will still reject
 * DXT3 and DXT5. Asking one question for all four formats would hand those
 * drivers a format they cannot decode and put us right back at a white texture.
 */
static bool driver_supports_s3tc_family(bool need_dxt3_or_dxt5)
{
    static int full = -1, dxt1_only = -1;
    if (full < 0) {
        using GetString = const GLubyte *(*)(GLenum);
        GetString get_string =
            (GetString)SDL_GL_GetProcAddress("glGetString");
        const char *extensions = get_string
            ? (const char *)get_string(GL_EXTENSIONS) : NULL;
        const bool forced = force_software_decode();
        full = (!forced && extension_present(
                    extensions, "GL_EXT_texture_compression_s3tc")) ? 1 : 0;
        dxt1_only = (!forced && extension_present(
                         extensions, "GL_EXT_texture_compression_dxt1")) ? 1 : 0;
        trace("S3TC: native driver support - DXT1 %s, DXT3/DXT5 %s; "
              "software fallback %s",
              (full || dxt1_only) ? "present" : "absent",
              full ? "present" : "absent",
              (full && dxt1_only) ? "disabled" : "enabled");
    }
    return need_dxt3_or_dxt5 ? (full != 0) : (full != 0 || dxt1_only != 0);
}

/*
 * Safety net for the device tests: put DXT3/DXT5 back the way they were before
 * this decoder existed, which is to say hand them to a driver that will refuse
 * them. It costs a blank texture, but it makes "did the DXT3 work cause this?"
 * answerable in one run instead of one round trip to the console.
 */
static bool dxt3_dxt5_disabled(void)
{
    static int off = -1;
    if (off < 0) {
        const char *v = port_getenv("S3TC_DXT3_OFF");
        off = (v && *v && *v != '0') ? 1 : 0;
        if (off)
            trace("S3TC: DXT3/DXT5 decoding disabled by request "
                  "(" PORT_ENV_PREFIX "_S3TC_DXT3_OFF)");
    }
    return off != 0;
}

static bool dxt_format(GLenum format, dxt::Format *out)
{
    if ((format == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ||
         format == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) && dxt3_dxt5_disabled())
        return false;

    switch (format) {
    case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:  *out = dxt::Format::Dxt1Rgb;  return true;
    case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: *out = dxt::Format::Dxt1Rgba; return true;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: *out = dxt::Format::Dxt3;     return true;
    case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: *out = dxt::Format::Dxt5;     return true;
    default:
        return false;
    }
}

static const char *dxt_format_name(dxt::Format format)
{
    switch (format) {
    case dxt::Format::Dxt1Rgb:  return "DXT1";
    case dxt::Format::Dxt1Rgba: return "DXT1A";
    case dxt::Format::Dxt3:     return "DXT3";
    case dxt::Format::Dxt5:     return "DXT5";
    }
    return "DXT?";
}

/*
 * The largest DXT3/DXT5 worth expanding to RGBA8888, and what happens above it.
 *
 * Decoding S3TC in software costs no decode time worth caring about: the whole
 * DXT3 set of this game is 85 Mpx, about 0.9 s, next to the 642 Mpx of DXT1 the
 * port already decodes on every run - measured at 93.8 Mpx/s for DXT3 against
 * 107.2 Mpx/s for DXT1, i.e. the same work per texel. What it costs is *memory*,
 * and that is what stopped the level load:
 *
 *   DXT1  0.5 bytes/texel -> RGBA8888 is 8x
 *   DXT3  1.0 bytes/texel -> RGBA8888 is 4x
 *
 * Before this decoder existed the Mali rejected every DXT3 upload and allocated
 * nothing for it. Now each one is a real allocation, and the eight largest are
 * 2048x2048 - 21.3 MiB apiece with their mip chains. They are not effects:
 * texture_env_desertplanet_platform, _skydome_02, _beacon, snow_planet_05_lm,
 * medical_bay_*_prop_set_02, moonbase_sky_nebula_01, cerberusprison_turret.
 * World surfaces - which is why the floor was untextured, and why texturing it
 * is what stopped the level fitting.
 *
 * Measured over the environment set of the landing level, resident RGBA bytes:
 *
 *   DXT1 alone (the build that plays)           165.3 MiB
 *   + DXT3 at RGBA8888 (the build that hangs)   237.3 MiB   +43%
 *   + DXT3 capped, big ones as RGBA4444         205.3 MiB   +24%
 *   + DXT3 capped, big ones left to the driver  173.3 MiB    +5%
 *
 * So the default caps at 1024. Every effect and particle texture in the game
 * fits under it - that whole set tops out at 1024 - so they all keep full
 * quality, and the eight big world textures stay exactly as the playable build
 * had them. The RGBA4444 path for those is implemented and one environment
 * variable away, because +24% on a budget that broke at +43% is a coin flip and
 * not something to spend a hardware run on by default.
 */
static int dxt_max_software_dim(void)
{
    static int cap = -1;
    if (cap < 0) {
        const char *v = port_getenv("DXT_MAX_DIM");
        cap = (v && *v) ? atoi(v) : 1024;
        if (cap < 0)
            cap = 0;
        trace("S3TC: DXT3/DXT5 software decode capped at %d px%s "
              "(" PORT_ENV_PREFIX "_DXT_MAX_DIM)", cap, cap ? "" : " - uncapped");
    }
    return cap;
}

/*
 * Above the cap: hand the texture to the driver, or decode it to RGBA4444.
 *
 * RGBA4444 is half the memory and, for this format specifically, costs less
 * than it sounds: DXT3's alpha is *already* four bits per texel, so the alpha
 * channel survives exactly. Only colour is quantised, from the 5:6:5 the block
 * endpoints were stored in down to 4:4:4. And the comparison that matters is
 * not against a perfect texture - it is against the white square these surfaces
 * render as today.
 */
static bool dxt_big_as_rgba4444(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = port_getenv("DXT_BIG_RGBA4444");
        on = (v && *v && *v != '0') ? 1 : 0;
        if (on)
            trace("S3TC: textures over the cap will be decoded to RGBA4444 "
                  "(" PORT_ENV_PREFIX "_DXT_BIG_RGBA4444)");
    }
    return on != 0;
}

/*
 * One buffer, grown as needed, never zero-filled.
 *
 * The code this replaces built a fresh std::vector per upload, which for a
 * 2048x2048 is a 16 MiB malloc, a 16 MiB memset the decode immediately
 * overwrites, and a 16 MiB free - once per mip level, per texture. In a 32-bit
 * address space that churn of large blocks is also the shape that fragments a
 * heap. new[] on a POD default-initialises, i.e. does not touch the memory,
 * which is the whole point of using it here.
 *
 * Per thread because nothing here can promise which thread owns the GL context,
 * and one spare buffer is cheaper than one wrong assumption.
 */
static unsigned char *decode_scratch(std::size_t bytes)
{
    static thread_local std::unique_ptr<unsigned char[]> buf;
    static thread_local std::size_t capacity = 0;

    if (capacity < bytes) {
        buf.reset(new (std::nothrow) unsigned char[bytes]);
        capacity = buf ? bytes : 0;
    }
    return buf.get();
}

/*
 * 8 bits -> 4, quantised for the reconstruction GL actually performs.
 *
 * The obvious spelling is `(v + 8) >> 4`, and it is wrong. That rounds onto a
 * grid of sixteens, but a driver expanding a 4-bit channel replicates the bits -
 * nibble n comes back as (n << 4) | n, a grid of *seventeens*. Rounding to the
 * wrong grid is not a rounding error, it is a systematic one that grows towards
 * white: a texel of 232 lands on nibble 15 and comes back 255, off by 23 out of
 * 255, and the whole bright end of every gradient shifts. Measured against a
 * real DXT3 texture before this was fixed: max RGB error 23/255 and, worse,
 * alpha no longer exact.
 *
 * Rounding onto the seventeens grid instead makes the error what quantisation
 * alone costs, at most 8/255 - and makes DXT3's alpha *lossless*, because its
 * alpha was already four bits and every one of its sixteen values is a fixed
 * point of this map.
 */
inline unsigned quantise4(unsigned v)
{
    return (v * 15u + 127u) / 255u;
}

/* RGBA8888 -> RGBA4444, in place: the write cursor advances at half the speed
 * of the read cursor and starts behind it, so no second buffer is needed -
 * which matters, since a second buffer would put the peak back where it was. */
static void pack_rgba4444(unsigned char *p, std::size_t texels)
{
    for (std::size_t i = 0; i < texels; i++) {
        const unsigned v = (quantise4(p[i * 4 + 0]) << 12) |
                           (quantise4(p[i * 4 + 1]) << 8)  |
                           (quantise4(p[i * 4 + 2]) << 4)  |
                            quantise4(p[i * 4 + 3]);
        p[i * 2 + 0] = static_cast<unsigned char>(v & 0xff);
        p[i * 2 + 1] = static_cast<unsigned char>(v >> 8);
    }
}

/* Bytes of decoded texture this port has handed to the driver. The number that
 * says whether a level load ran out of room; reported from src/main.cpp. */
static std::atomic<long long> g_decoded_bytes(0);

struct DecodedTexture {
    const unsigned char *pixels;
    GLenum               type;      /* GL_UNSIGNED_BYTE or ..._SHORT_4_4_4_4 */
    std::size_t          bytes;
};

static bool decode_dxt(dxt::Format dxt_fmt, GLsizei width, GLsizei height,
                       GLsizei image_size, const void *data,
                       DecodedTexture *out)
{
    if (width <= 0 || height <= 0 || image_size < 0 || !data)
        return false;

    const int cap = dxt_max_software_dim();
    const bool big = cap > 0 && (width > cap || height > cap) &&
                     (dxt_fmt == dxt::Format::Dxt3 ||
                      dxt_fmt == dxt::Format::Dxt5);

    if (big && !dxt_big_as_rgba4444()) {
        static long capped = 0;
        if (capped++ < 8) {
            trace("%s: %dx%d is over the %d px cap - left to the driver, which "
                  "will refuse it (saves %llu KiB of RGBA); "
                  "" PORT_ENV_PREFIX "_DXT_BIG_RGBA4444=1 decodes it at half that",
                  dxt_format_name(dxt_fmt), width, height, cap,
                  (unsigned long long)((std::size_t)width * height * 4u / 1024u));
        }
        return false;
    }

    const std::size_t texels = static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height);
    const std::size_t decoded_size = texels * 4u;

    unsigned char *pixels = decode_scratch(decoded_size);
    if (!pixels) {
        /* Said plainly and always, counted against no budget: this is the line
         * that turns "the level load hangs" into "the level load ran out of
         * memory", and it is worth every byte of log it costs. */
        trace("%s: cannot allocate %llu-byte decode buffer for %dx%d "
              "- out of memory, leaving it to the driver",
              dxt_format_name(dxt_fmt),
              (unsigned long long)decoded_size, width, height);
        return false;
    }

    if (!dxt::decode(dxt_fmt, data, static_cast<std::size_t>(image_size),
                     width, height, pixels))
        return false;

    out->pixels = pixels;
    if (big) {
        pack_rgba4444(pixels, texels);
        out->type  = GL_UNSIGNED_SHORT_4_4_4_4;
        out->bytes = texels * 2u;
    } else {
        out->type  = GL_UNSIGNED_BYTE;
        out->bytes = decoded_size;
    }

    g_decoded_bytes.fetch_add((long long)out->bytes, std::memory_order_relaxed);
    return true;
}

static bool pvrtc_format(GLenum format, bool *two_bpp, bool *opaque)
{
    switch (format) {
    case GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG:
        *two_bpp = false; *opaque = true;  return true;
    case GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG:
        *two_bpp = true;  *opaque = true;  return true;
    case GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG:
        *two_bpp = false; *opaque = false; return true;
    case GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG:
        *two_bpp = true;  *opaque = false; return true;
    default:
        return false;
    }
}

static bool decode_pvrtc(GLenum format, GLsizei width, GLsizei height,
                         GLsizei image_size, const void *data,
                         std::vector<unsigned char> *rgba)
{
    bool two_bpp = false, opaque = false;
    if (!pvrtc_format(format, &two_bpp, &opaque) ||
        width <= 0 || height <= 0 || image_size < 0 || !data)
        return false;

    uint64_t stored_width = std::max<uint64_t>(
        (uint64_t)width, two_bpp ? 16u : 8u);
    uint64_t stored_height = std::max<uint64_t>((uint64_t)height, 8u);
    uint64_t expected = stored_width * stored_height *
                        (two_bpp ? 2u : 4u) / 8u;
    uint64_t decoded_size = (uint64_t)width * (uint64_t)height * 4u;
    if (expected > (uint64_t)image_size ||
        decoded_size > (uint64_t)SIZE_MAX)
        return false;

    try {
        rgba->resize((size_t)decoded_size);
        pvr::PVRTDecompressPVRTC(data, two_bpp ? 1u : 0u,
                                (uint32_t)width, (uint32_t)height,
                                rgba->data());
    } catch (const std::bad_alloc &) {
        trace("PVRTC: cannot allocate %llu-byte decode buffer",
              (unsigned long long)decoded_size);
        return false;
    }

    if (opaque) {
        for (size_t i = 3; i < rgba->size(); i += 4)
            (*rgba)[i] = 255;
    }
    return true;
}

static bool decode_etc1(GLsizei width, GLsizei height, GLsizei image_size,
                        const void *data, std::vector<unsigned char> *rgba)
{
    if (width <= 0 || height <= 0 || image_size < 0 || !data)
        return false;
    size_t blocks_x = (static_cast<size_t>(width) + 3) / 4;
    size_t blocks_y = (static_cast<size_t>(height) + 3) / 4;
    if (blocks_x > SIZE_MAX / blocks_y ||
        blocks_x * blocks_y > static_cast<size_t>(image_size) / 8u)
        return false;
    try {
        rgba->resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    } catch (const std::bad_alloc &) {
        return false;
    }
    return pvr::PVRTDecompressETC(data, static_cast<uint32_t>(width),
                                  static_cast<uint32_t>(height), rgba->data(), 0) != 0;
}

extern "C" void probe_glCompressedTexImage2D(
    GLenum target, GLint level, GLenum internalformat, GLsizei width,
    GLsizei height, GLint border, GLsizei image_size, const void *data)
{
    using CompressedUpload =
        void (*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei,
                 const void *);
    using Upload =
        void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                 GLenum, const void *);
    static CompressedUpload compressed_upload =
        (CompressedUpload)SDL_GL_GetProcAddress("glCompressedTexImage2D");
    static Upload upload =
        (Upload)SDL_GL_GetProcAddress("glTexImage2D");
    static int diagnostic_lines = 0;
    static int fallback_lines = 0;
    g_textures++;
    /* Counted before the decoders run, so the census reports what the engine
     * asked for rather than what we managed to substitute. `forwarded` is 1
     * here because every path below reaches a driver call; the one that does
     * not is reported separately at the bottom. */
    gl_stats_note_texture_upload(1, internalformat, width, height,
                                 (long)image_size, 1);

    if (gl_diag_enabled()) {
        gl_diag_before("glCompressedTexImage2D");
        if (diagnostic_lines++ < 64) {
            trace("GLDIAG: compressed upload target=0x%04x level=%d "
                  "format=0x%04x size=%dx%d border=%d bytes=%d data=%p",
                  target, level, internalformat, width, height, border,
                  image_size, data);
        }
    }

    bool known_atc = false, ignored_explicit_alpha = false;
    known_atc = atc_format(internalformat, &ignored_explicit_alpha);
    if (known_atc && !driver_supports_atc() && upload) {
        std::vector<unsigned char> rgba;
        if (decode_atc(internalformat, width, height, image_size, data,
                       &rgba)) {
            upload(target, level, GL_RGBA, width, height, border,
                   GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            if (fallback_lines++ < 16) {
                trace("ATC: decoded level=%d format=0x%04x %dx%d "
                      "(%d compressed bytes -> %zu RGBA bytes)",
                      level, internalformat, width, height, image_size,
                      rgba.size());
            }
            if (gl_diag_enabled())
                gl_diag_after("ATC fallback glTexImage2D");
            gl_stats_check_error("ATC fallback glTexImage2D");
            return;
        }
        trace("ATC: invalid upload; forwarding to driver for GL error "
              "(level=%d format=0x%04x %dx%d bytes=%d)",
              level, internalformat, width, height, image_size);
    }

    bool known_pvrtc = false, ignored_two_bpp = false, ignored_opaque = false;
    known_pvrtc = pvrtc_format(internalformat, &ignored_two_bpp,
                              &ignored_opaque);
    if (known_pvrtc && !driver_supports_pvrtc() && upload) {
        std::vector<unsigned char> rgba;
        if (decode_pvrtc(internalformat, width, height, image_size, data,
                         &rgba)) {
            upload(target, level, GL_RGBA, width, height, border,
                   GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            if (fallback_lines++ < 16) {
                trace("PVRTC: decoded level=%d format=0x%04x %dx%d "
                      "(%d compressed bytes -> %zu RGBA bytes)",
                      level, internalformat, width, height, image_size,
                      rgba.size());
            }
            if (gl_diag_enabled())
                gl_diag_after("PVRTC fallback glTexImage2D");
            gl_stats_check_error("PVRTC fallback glTexImage2D");
            return;
        }
        trace("PVRTC: invalid upload; forwarding to driver for GL error "
              "(level=%d format=0x%04x %dx%d bytes=%d)",
              level, internalformat, width, height, image_size);
    }

    if (internalformat == 0x8d64 && upload &&
        (port_getenv("FORCE_SOFTWARE_TEXTURE_DECODE") ||
         port_getenv("FORCE_ETC1_DECODE"))) {
        std::vector<unsigned char> rgba;
        if (decode_etc1(width, height, image_size, data, &rgba)) {
            upload(target, level, GL_RGBA, width, height, border,
                   GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            trace("ETC1: decoded level=%d %dx%d (%d compressed bytes -> %zu RGBA bytes)",
                  level, width, height, image_size, rgba.size());
            gl_stats_check_error("ETC1 fallback glTexImage2D");
            return;
        }
        warning("ETC1 decode failed; forwarding compressed upload\n");
    }

    dxt::Format dxt_fmt = dxt::Format::Dxt1Rgb;
    bool known_dxt = dxt_format(internalformat, &dxt_fmt);
    if (known_dxt) {
        const bool needs_full_s3tc = dxt_fmt == dxt::Format::Dxt3 ||
                                     dxt_fmt == dxt::Format::Dxt5;
        if (!driver_supports_s3tc_family(needs_full_s3tc) && upload) {
            DecodedTexture decoded;
            if (decode_dxt(dxt_fmt, width, height, image_size, data,
                           &decoded)) {
                upload(target, level, GL_RGBA, width, height, border,
                       GL_RGBA, decoded.type, decoded.pixels);
                /*
                 * Counted per format, not globally. A single shared budget is
                 * how the DXT3 problem stayed invisible: the first sixteen
                 * DXT1 uploads spent it during the menus, so every later
                 * format was silent by the time the world loaded, and the log
                 * read as "this game only uses DXT1".
                 *
                 * The running total is what the level-load question needs -
                 * one line per texture would drown the log, and the total is
                 * the quantity that either fits in the device or does not.
                 */
                static long lines[4];
                const int slot = (int)dxt_fmt;
                if (lines[slot]++ < 16) {
                    trace("%s: decoded level=%d format=0x%04x %dx%d "
                          "(%d compressed bytes -> %zu bytes %s, "
                          "%lld MiB decoded so far)",
                          dxt_format_name(dxt_fmt), level, internalformat,
                          width, height, image_size, decoded.bytes,
                          decoded.type == GL_UNSIGNED_BYTE ? "RGBA8888"
                                                           : "RGBA4444",
                          g_decoded_bytes.load() >> 20);
                }
                if (gl_diag_enabled())
                    gl_diag_after("S3TC fallback glTexImage2D");
                gl_stats_check_error("S3TC fallback glTexImage2D");
                return;
            }
            trace("%s: invalid upload; forwarding to driver for GL error "
                  "(level=%d format=0x%04x %dx%d bytes=%d)",
                  dxt_format_name(dxt_fmt), level, internalformat, width,
                  height, image_size);
        }
    }

    /*
     * Anything that reaches here goes to a driver that may well refuse it, so
     * say which format it was. This used to be silent, and that silence cost a
     * whole hardware test: 147 DXT3 textures - blood, muzzle flashes, and a
     * third of the world's surfaces - were uploaded and rejected without a
     * single line in the log, so the evidence said "only DXT1 is ever
     * uploaded" when what it meant was "DXT1 is the only path that talks".
     * Not gated behind GL_DIAG and not counted against fallback_lines: an
     * unknown compressed format is rare and always worth a line.
     */
    if (!known_dxt && is_compressed_format(internalformat)) {
        static unsigned reported[8];
        static int reported_count = 0;
        bool seen = false;
        for (int i = 0; i < reported_count; i++)
            if (reported[i] == (unsigned)internalformat)
                seen = true;
        if (!seen) {
            if (reported_count < (int)(sizeof(reported) / sizeof(reported[0])))
                reported[reported_count++] = (unsigned)internalformat;
            warning("glCompressedTexImage2D: no decoder for format 0x%04x (%s) "
                    "- forwarding %dx%d to the driver, which will likely "
                    "reject it and leave the texture blank\n",
                    internalformat, compressed_format_name(internalformat),
                    width, height);
        }
    }

    if (compressed_upload) {
        compressed_upload(target, level, internalformat, width, height,
                          border, image_size, data);
    } else {
        gl_stats_note_dropped("glCompressedTexImage2D",
                              "SDL_GL_GetProcAddress returned NULL");
    }

    if (gl_diag_enabled())
        gl_diag_after("glCompressedTexImage2D");
    gl_stats_check_error("glCompressedTexImage2D");
}

/*
 * Aspect-correct scaling to the device's real panel.
 *
 * The engine renders to one fixed logical panel (<PREFIX>_SCREEN_W/H, default
 * 640x480) and - per ea_DisplayAndroidDelegate - issues a single full-screen
 * glViewport at that size, plus glScissor calls in the same space. On a device
 * whose framebuffer already is that size (the R36S, 640x480) nothing needs to
 * change. On a wider panel - the RG34XXSP is 720x480 - the engine's 640x480
 * viewport lands in a corner and the rest of the panel stays black. We remap
 * that one full-screen viewport onto the real drawable and apply the matching
 * affine transform to scissor rectangles so clipped UI stays aligned; every
 * other viewport (a render target of a different size) is left untouched, since
 * that is exactly the call blind remapping would break.
 *
 * <PREFIX>_SCALE picks the policy:
 *   fit      (default) keep 4:3, letterbox with side bars, no distortion
 *   stretch  fill the panel, minor distortion
 *   integer  largest whole-number fit, letterboxed
 * The panel size comes from SDL_GL_GetDrawableSize (main.cpp calls the init);
 * <PREFIX>_PANEL_W/H override it, both to force a size and to exercise the
 * path under the harness, which otherwise renders at the logical 640x480.
 */
enum { SCALE_FIT = 0, SCALE_STRETCH = 1, SCALE_INTEGER = 2 };

static int     g_scale_active = 0;
static int     g_log_w = 640, g_log_h = 480;   /* engine's logical panel        */
static GLint   g_dst_x = 0, g_dst_y = 0;       /* full-screen rect on the panel */
static GLsizei g_dst_w = 0, g_dst_h = 0;
static float   g_scale_x = 1.0f, g_scale_y = 1.0f;

static int glprobe_env_int(const char *key, int fallback)
{
    const char *v = getenv(key);
    return (v && *v) ? atoi(v) : fallback;
}

static int g_scale_armed = 0;

extern "C" void viewport_scale_init(int phys_w, int phys_h)
{
    g_scale_armed = 1;
    g_log_w = glprobe_env_int(port_env_name("SCREEN_W"), 640);
    g_log_h = glprobe_env_int(port_env_name("SCREEN_H"), 480);

    phys_w = glprobe_env_int(port_env_name("PANEL_W"), phys_w);
    phys_h = glprobe_env_int(port_env_name("PANEL_H"), phys_h);

    /* No panel info, or the panel already matches the logical size: pass every
     * call through untouched. This is the R36S path - identity, zero cost. */
    if (phys_w <= 0 || phys_h <= 0 ||
        (phys_w == g_log_w && phys_h == g_log_h)) {
        g_scale_active = 0;
        trace("viewport scale: identity (panel %dx%d, logical %dx%d)",
              phys_w, phys_h, g_log_w, g_log_h);
        return;
    }

    const char *m = port_getenv("SCALE");
    int mode = SCALE_FIT;
    if (m && !strcmp(m, "stretch"))      mode = SCALE_STRETCH;
    else if (m && !strcmp(m, "integer")) mode = SCALE_INTEGER;

    if (mode == SCALE_STRETCH) {
        g_dst_x = 0; g_dst_y = 0;
        g_dst_w = phys_w; g_dst_h = phys_h;
    } else {
        float sx = (float)phys_w / (float)g_log_w;
        float sy = (float)phys_h / (float)g_log_h;
        float s  = sx < sy ? sx : sy;          /* largest that fits          */
        if (mode == SCALE_INTEGER) {
            s = (float)(int)s;                 /* floor to a whole multiple  */
            if (s < 1.0f) s = 1.0f;
        }
        g_dst_w = (GLsizei)(g_log_w * s + 0.5f);
        g_dst_h = (GLsizei)(g_log_h * s + 0.5f);
        g_dst_x = (phys_w - g_dst_w) / 2;
        g_dst_y = (phys_h - g_dst_h) / 2;
    }

    g_scale_x = (float)g_dst_w / (float)g_log_w;
    g_scale_y = (float)g_dst_h / (float)g_log_h;
    g_scale_active = 1;
    trace("viewport scale: %s panel=%dx%d logical=%dx%d -> dst=%d,%d %dx%d",
          mode == SCALE_STRETCH ? "stretch" :
          mode == SCALE_INTEGER ? "integer" : "fit",
          phys_w, phys_h, g_log_w, g_log_h,
          g_dst_x, g_dst_y, g_dst_w, g_dst_h);
}

/*
 * Self-arm if the port never called viewport_scale_init.
 *
 * Two ports in a row shipped the scaler declared and disarmed - the call
 * lives in the port's own main.cpp and forgetting it produces no error,
 * just every <PREFIX>_SCALE mode silently ignored on any panel that is not
 * 640x480 (mc3 issue #2, root-caused by Codebr0ken). By the first glViewport
 * a context is current and its window knows the drawable, so the base can
 * arm itself; an explicit init still wins because it runs earlier.
 */
static void viewport_scale_autoarm(void)
{
    if (g_scale_armed)
        return;
    SDL_Window *win = SDL_GL_GetCurrentWindow();
    if (!win)
        return; /* no context yet; try again on the next call */
    int pw = 0, ph = 0;
    SDL_GL_GetDrawableSize(win, &pw, &ph);
    if (pw <= 0 || ph <= 0)
        return;
    trace("viewport scale: self-armed from the current drawable %dx%d "
          "(the port never called viewport_scale_init)", pw, ph);
    viewport_scale_init(pw, ph);
}

/* The engine's one full-screen viewport, the only one we remap. */
static inline int is_fullscreen_viewport(GLint x, GLint y,
                                         GLsizei w, GLsizei h)
{
    return g_scale_active && x == 0 && y == 0 &&
           w == g_log_w && h == g_log_h;
}

/*
 * Geometry diagnostics for real devices, now also the scaling seam.
 *
 * A black/cropped frame can be a correct draw into the wrong rectangle.
 * Logging only changes, and only the first few, keeps LOADER_TRACE useful
 * without turning every frame into hundreds of lines.
 */
extern "C" void probe_glViewport(GLint x, GLint y, GLsizei width,
                                 GLsizei height)
{
    viewport_scale_autoarm();
    using Viewport = void (*)(GLint, GLint, GLsizei, GLsizei);
    static Viewport viewport =
        (Viewport)resolve_gl_entry("glViewport");

    if (is_fullscreen_viewport(x, y, width, height)) {
        x = g_dst_x; y = g_dst_y; width = g_dst_w; height = g_dst_h;
    }

    static GLint last_x = -1, last_y = -1;
    static GLsizei last_w = -1, last_h = -1;
    static int lines = 0;

    if (lines < 24 &&
        (x != last_x || y != last_y || width != last_w || height != last_h)) {
        trace("GL viewport: x=%d y=%d width=%d height=%d",
              x, y, width, height);
        last_x = x; last_y = y; last_w = width; last_h = height;
        lines++;
    }
    gl_stats_note_viewport(x, y, width, height);
    if (viewport)
        viewport(x, y, width, height);
    else
        gl_stats_note_dropped("glViewport",
                              "glViewport resolved nowhere (tables and SDL)");
}

extern "C" void probe_glScissor(GLint x, GLint y, GLsizei width,
                                GLsizei height)
{
    viewport_scale_autoarm();
    using Scissor = void (*)(GLint, GLint, GLsizei, GLsizei);
    static Scissor scissor =
        (Scissor)resolve_gl_entry("glScissor");

    /* Every scissor is in the logical panel's space (the engine uses one
     * viewport), so the same affine transform keeps clipped UI aligned with
     * the scaled content. Identity when scaling is off. */
    if (g_scale_active) {
        x = g_dst_x + (GLint)(x * g_scale_x + 0.5f);
        y = g_dst_y + (GLint)(y * g_scale_y + 0.5f);
        width  = (GLsizei)(width  * g_scale_x + 0.5f);
        height = (GLsizei)(height * g_scale_y + 0.5f);
    }

    static GLint last_x = -1, last_y = -1;
    static GLsizei last_w = -1, last_h = -1;
    static int lines = 0;

    if (lines < 24 &&
        (x != last_x || y != last_y || width != last_w || height != last_h)) {
        trace("GL scissor: x=%d y=%d width=%d height=%d",
              x, y, width, height);
        last_x = x; last_y = y; last_w = width; last_h = height;
        lines++;
    }
    gl_stats_note_scissor(x, y, width, height);
    if (scissor)
        scissor(x, y, width, height);
    else
        gl_stats_note_dropped("glScissor",
                              "glScissor resolved nowhere (tables and SDL)");
}

/*
 * The rest of the pipeline, observed on the way past.
 *
 * These wrappers exist for the 3D-scene investigation and do nothing but count.
 * Each one forwards through resolve_gl_entry(), which reproduces the table
 * order the game would have resolved without the interposition, and each one
 * reports a null pointer instead of returning quietly - the failure mode this
 * project has already paid for is a wrapper that silently declines to call the
 * driver, and it has to be impossible to repeat without noticing.
 *
 * None of these signatures carries a float, so THUNK_SPECIFIC hands the game
 * the pointer directly and no softfp/hardfp bridge is involved.
 */
#define FORWARD_OR_REPORT(name, type, call)                                   \
    using Fn = type;                                                          \
    static Fn fn = (Fn)resolve_gl_entry(name);                                \
    if (fn) call;                                                             \
    else gl_stats_note_dropped(name, "not present in any GL table")

extern "C" void probe_glUseProgram(GLuint program)
{
    gl_stats_note_use_program(program);
    FORWARD_OR_REPORT("glUseProgram", void (*)(GLuint), fn(program));
}

extern "C" void probe_glBindBuffer(GLenum target, GLuint buffer)
{
    gl_stats_note_bind_buffer(target, buffer);
    FORWARD_OR_REPORT("glBindBuffer", void (*)(GLenum, GLuint),
                      fn(target, buffer));
}

extern "C" void probe_glBufferData(GLenum target, GLsizeiptr size,
                                   const void *data, GLenum usage)
{
    gl_stats_note_buffer_data(target, (long)size, usage, data != NULL);
    FORWARD_OR_REPORT("glBufferData",
                      void (*)(GLenum, GLsizeiptr, const void *, GLenum),
                      fn(target, size, data, usage));
    gl_stats_check_error("glBufferData");
}

extern "C" void probe_glVertexAttribPointer(GLuint index, GLint size,
                                            GLenum type, GLboolean normalized,
                                            GLsizei stride, const void *pointer)
{
    gl_stats_note_vertex_attrib_pointer(index, size, type, normalized, stride,
                                        pointer);
    FORWARD_OR_REPORT("glVertexAttribPointer",
                      void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                               const void *),
                      fn(index, size, type, normalized, stride, pointer));
}

extern "C" void probe_glEnableVertexAttribArray(GLuint index)
{
    gl_stats_note_vertex_attrib_array(index, 1);
    FORWARD_OR_REPORT("glEnableVertexAttribArray", void (*)(GLuint),
                      fn(index));
}

extern "C" void probe_glDisableVertexAttribArray(GLuint index)
{
    gl_stats_note_vertex_attrib_array(index, 0);
    FORWARD_OR_REPORT("glDisableVertexAttribArray", void (*)(GLuint),
                      fn(index));
}

extern "C" void probe_glBindTexture(GLenum target, GLuint texture)
{
    gl_stats_note_bind_texture(target, texture);
    FORWARD_OR_REPORT("glBindTexture", void (*)(GLenum, GLuint),
                      fn(target, texture));
}

extern "C" void probe_glEnable(GLenum cap)
{
    gl_stats_note_capability(cap, 1);
    FORWARD_OR_REPORT("glEnable", void (*)(GLenum), fn(cap));
}

extern "C" void probe_glDisable(GLenum cap)
{
    gl_stats_note_capability(cap, 0);
    FORWARD_OR_REPORT("glDisable", void (*)(GLenum), fn(cap));
}

extern "C" void probe_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
    gl_stats_note_bind_framebuffer(target, framebuffer);
    FORWARD_OR_REPORT("glBindFramebuffer", void (*)(GLenum, GLuint),
                      fn(target, framebuffer));
}

extern "C" void probe_glFramebufferTexture2D(GLenum target, GLenum attachment,
                                             GLenum textarget, GLuint texture,
                                             GLint level)
{
    FORWARD_OR_REPORT("glFramebufferTexture2D",
                      void (*)(GLenum, GLenum, GLenum, GLuint, GLint),
                      fn(target, attachment, textarget, texture, level));
    /* Reported after the call so the completeness verdict the next draw asks
     * for reflects this attachment, not the previous one. */
    gl_stats_note_framebuffer_attach("texture2D", attachment, texture);
    gl_stats_check_error("glFramebufferTexture2D");
}

extern "C" void probe_glFramebufferRenderbuffer(GLenum target,
                                                GLenum attachment,
                                                GLenum renderbuffertarget,
                                                GLuint renderbuffer)
{
    FORWARD_OR_REPORT("glFramebufferRenderbuffer",
                      void (*)(GLenum, GLenum, GLenum, GLuint),
                      fn(target, attachment, renderbuffertarget,
                         renderbuffer));
    gl_stats_note_framebuffer_attach("renderbuffer", attachment, renderbuffer);
    gl_stats_check_error("glFramebufferRenderbuffer");
}

#undef FORWARD_OR_REPORT

extern "C" long android_gl_draw_calls(void) { return g_draws.load(); }
extern "C" long android_gl_textures_uploaded(void) { return g_textures.load(); }

/* DIAGNOSTIC (temporary): see the counters near the top of this file. */
extern "C" long android_gl_draw_arrays(void) { return g_draw_arrays.load(); }
extern "C" long android_gl_draw_arrays_max(void) { return g_draw_arrays_max.load(); }
extern "C" long android_gl_draw_elements(void) { return g_draw_elements.load(); }
extern "C" long android_gl_draw_elements_indices(void) { return g_draw_elements_indices.load(); }
extern "C" long android_gl_draw_elements_max(void) { return g_draw_elements_max.load(); }

/* MiB of software-decoded texture handed to the driver. On a device with a
 * gigabyte shared between the game and the GPU this is the number that decides
 * whether a level load finishes. */
extern "C" long android_gl_decoded_mib(void)
{
    return (long)(g_decoded_bytes.load() >> 20);
}

extern "C" int android_gl_shaders_compiled(void) { return g_shaders_ok.load(); }
extern "C" int android_gl_shaders_failed(void)   { return g_shaders_failed.load(); }
extern "C" int android_gl_programs_linked(void)  { return g_programs_ok.load(); }
extern "C" int android_gl_programs_failed(void)  { return g_programs_failed.load(); }

/*
 * Neither function takes or returns a float, so select_either() hands the game
 * the pointer directly with no ABI bridge - same as the GLES1 entries these
 * shadow.
 */
DynLibFunction symtable_glprobe[] = {
    THUNK_SPECIFIC("glCompileShader", probe_glCompileShader),
    THUNK_SPECIFIC("glLinkProgram",   probe_glLinkProgram),
    THUNK_SPECIFIC("glClear",         probe_glClear),
    THUNK_SPECIFIC("glDrawArrays",    probe_glDrawArrays),
    THUNK_SPECIFIC("glDrawElements",  probe_glDrawElements),
    THUNK_SPECIFIC("glTexImage2D",    probe_glTexImage2D),
    THUNK_SPECIFIC("glCompressedTexImage2D",
                   probe_glCompressedTexImage2D),
    THUNK_SPECIFIC("glViewport",      probe_glViewport),
    THUNK_SPECIFIC("glScissor",       probe_glScissor),
    /* Census-only entries; see the block above probe_glUseProgram. */
    THUNK_SPECIFIC("glUseProgram",    probe_glUseProgram),
    THUNK_SPECIFIC("glBindBuffer",    probe_glBindBuffer),
    THUNK_SPECIFIC("glBufferData",    probe_glBufferData),
    THUNK_SPECIFIC("glVertexAttribPointer", probe_glVertexAttribPointer),
    THUNK_SPECIFIC("glEnableVertexAttribArray",
                   probe_glEnableVertexAttribArray),
    THUNK_SPECIFIC("glDisableVertexAttribArray",
                   probe_glDisableVertexAttribArray),
    THUNK_SPECIFIC("glBindTexture",   probe_glBindTexture),
    THUNK_SPECIFIC("glEnable",        probe_glEnable),
    THUNK_SPECIFIC("glDisable",       probe_glDisable),
    THUNK_SPECIFIC("glBindFramebuffer", probe_glBindFramebuffer),
    THUNK_SPECIFIC("glFramebufferTexture2D", probe_glFramebufferTexture2D),
    THUNK_SPECIFIC("glFramebufferRenderbuffer",
                   probe_glFramebufferRenderbuffer),
    { NULL, 0 },
};
