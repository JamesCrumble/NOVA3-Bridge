/*
 * GL pipeline census.
 *
 * The port reaches the starting lights of a race with the HUD painted on top of
 * a flat background and not one polygon of track or car behind it. Two
 * explanations fit that screenshot equally well - the engine never submits the
 * scene, or it submits it and something between the game and the driver drops
 * it - and they lead to opposite fixes. A screenshot cannot separate them; a
 * per-frame count of what crossed the boundary can.
 *
 * So every intercepted entry point reports here, and the report is grouped the
 * way the question is asked: draws are bucketed by the state that distinguishes
 * a 3D pass from a 2D one (bound framebuffer, program, depth test, cull face,
 * indexed or not, primitive mode). If the scene is being submitted there will be
 * a bucket with depth testing on and thousands of vertices; if it is not, there
 * will only be the shallow blended quads the UI is made of.
 *
 * Two measurements exist because of a trap this project already paid for once:
 * symtab_glprobe.cpp forwards through a table that is only
 * populated when a particular library opens, and when it is empty the wrappers
 * return without calling the driver - a black screen with nothing in the log.
 *   - gl_stats_note_dropped() makes any such wrapper say so, loudly, the first
 *     time it drops a call, instead of failing silently.
 *   - gl_stats_init() prints, per entry point, which table answered it and
 *     whether that pointer agrees with SDL_GL_GetProcAddress. A game whose
 *     draws go to one dispatch and whose state goes to another renders nothing
 *     while every call "succeeds".
 *
 * Nothing here changes what the game does. The counters are plain longs, not
 * atomics: a GL context is current on exactly one thread, so every function
 * below is already serialised by the thing it is measuring.
 */
#include "gl_stats.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "khronos/gles2.h"
#include "so_util.h"
#include "trace.h"
#include "port_env.h"
#include "gl_bridge.h"

extern DynLibFunction symtable_gles1[];   /* thunks/khronos/gles1.cpp */
extern DynLibFunction symtable_gles2[];   /* thunks/khronos/gles2.cpp */

/* ------------------------------------------------------------------ config */

static int env_flag(const char *key)
{
    const char *v = getenv(key);
    return (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
}

int gl_stats_enabled(void)
{
    static int on = -1;
    if (on < 0)
        on = env_flag(port_env_name("GL_STATS"));
    return on;
}

int gl_stats_error_check_enabled(void)
{
    /* Draining the error queue is observable to the game, and under qemu a
     * glGetError per draw is not free. Opt-in, exactly like GL_DIAG. */
    static int on = -1;
    if (on < 0)
        on = env_flag(port_env_name("GL_STATS_ERRORS"));
    return on;
}

static long dump_period(void)
{
    static long every = -1;
    if (every < 0) {
        const char *v = port_getenv("GL_STATS_EVERY");
        every = (v && *v) ? atol(v) : 60;
        if (every <= 0)
            every = 60;
    }
    return every;
}

/* The census is the answer, so it does not hide behind LOADER_TRACE. */
static void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("GLSTATS: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

/* ------------------------------------------------------------- shadow state */

static GLuint g_program      = 0;
static GLuint g_array_buffer = 0;
static GLuint g_index_buffer = 0;
static GLuint g_framebuffer  = 0;
static GLuint g_texture      = 0;
static int    g_depth_test   = 0;
static int    g_cull_face    = 0;
static int    g_blend        = 0;
static int    g_scissor_test = 0;
static int    g_attrib_enabled_mask = 0;   /* attributes 0..31 */
static int    g_attrib_pointer_mask = 0;
static float  g_clear_r = -1, g_clear_g = -1, g_clear_b = -1, g_clear_a = -1;
static float  g_depth_clear = -1, g_depth_near = -1, g_depth_far = -1;
static int    g_depth_func = 0, g_depth_mask = -1, g_depth_bits = -1;

/* --------------------------------------------------------------- counters */

struct Scalars {
    long draw_arrays, draw_elements;
    long verts_arrays, verts_elements;
    long bind_buffer, buffer_data;
    long buffer_bytes;
    long buffer_data_null;           /* size reserved, contents uploaded later */
    long vertex_attrib_pointer;
    long attrib_pointer_client;      /* no VBO bound: client-side array        */
    long enable_attrib, disable_attrib;
    long use_program;
    long bind_texture, tex_image_2d, compressed_tex_image_2d;
    long texture_bytes;
    long enable_depth, disable_depth;
    long enable_cull, disable_cull;
    long viewport, scissor, clear;
    long clear_colour, clear_depth, clear_stencil;
    long bind_framebuffer, bind_framebuffer_default;
    long errors;
    long dropped;
};

static Scalars g_win;     /* since the last dump */
static Scalars g_all;     /* since process start */

static void bump(long Scalars::*field, long by = 1)
{
    g_win.*field += by;
    g_all.*field += by;
}

/* -------------------------------------------------------------- draw buckets
 *
 * One bucket per distinct render state. 96 is generous: this game links 58
 * programs and only a handful are live in any one screen, and an overflow is
 * reported rather than silently folded into another bucket.
 */
struct DrawBucket {
    GLuint program;
    GLuint framebuffer;
    GLenum mode;
    unsigned char depth;
    unsigned char cull;
    unsigned char blend;
    unsigned char indexed;
    unsigned char vbo;               /* drew out of a bound ARRAY_BUFFER */
    long draws;
    long vertices;
    GLsizei min_count, max_count;
};

enum { kMaxBuckets = 96 };
static DrawBucket g_buckets[kMaxBuckets];
static int  g_bucket_count = 0;
static long g_bucket_overflow = 0;

static void bucket_reset(void)
{
    g_bucket_count = 0;
    g_bucket_overflow = 0;
}

static void bucket_add(GLenum mode, GLsizei count, int indexed)
{
    const unsigned char depth   = (unsigned char)(g_depth_test ? 1 : 0);
    const unsigned char cull    = (unsigned char)(g_cull_face ? 1 : 0);
    const unsigned char blend   = (unsigned char)(g_blend ? 1 : 0);
    const unsigned char vbo     = (unsigned char)(g_array_buffer ? 1 : 0);

    for (int i = 0; i < g_bucket_count; i++) {
        DrawBucket &b = g_buckets[i];
        if (b.program == g_program && b.framebuffer == g_framebuffer &&
            b.mode == mode && b.depth == depth && b.cull == cull &&
            b.blend == blend && b.indexed == (unsigned char)indexed &&
            b.vbo == vbo) {
            b.draws++;
            b.vertices += count;
            if (count < b.min_count) b.min_count = count;
            if (count > b.max_count) b.max_count = count;
            return;
        }
    }

    if (g_bucket_count >= kMaxBuckets) {
        g_bucket_overflow++;
        return;
    }

    DrawBucket &b = g_buckets[g_bucket_count++];
    b.program = g_program;
    b.framebuffer = g_framebuffer;
    b.mode = mode;
    b.depth = depth;
    b.cull = cull;
    b.blend = blend;
    b.indexed = (unsigned char)indexed;
    b.vbo = vbo;
    b.draws = 1;
    b.vertices = count;
    b.min_count = count;
    b.max_count = count;
}

/* ------------------------------------------------------------ texture census
 *
 * Grouped by internal format because that is the axis a rejected upload lives
 * on: the same code path uploads UI atlases and car liveries, and only the
 * compressed ones can be refused by the driver.
 */
struct FormatBucket {
    GLenum format;
    int    compressed;
    long   uploads;
    long   bytes;
    long   dropped;          /* wrapper decided not to forward this one */
    GLsizei max_w, max_h;
};

enum { kMaxFormats = 32 };
static FormatBucket g_formats[kMaxFormats];
static int g_format_count = 0;

static const char *format_name(GLenum format)
{
    switch (format) {
    case 0x1907: return "RGB";
    case 0x1908: return "RGBA";
    case 0x1906: return "ALPHA";
    case 0x1909: return "LUMINANCE";
    case 0x190a: return "LUMINANCE_ALPHA";
    case 0x83f0: return "S3TC_DXT1_RGB";
    case 0x83f1: return "S3TC_DXT1_RGBA";
    case 0x83f2: return "S3TC_DXT3";
    case 0x83f3: return "S3TC_DXT5";
    case 0x8c00: return "PVRTC_RGB_4BPP";
    case 0x8c01: return "PVRTC_RGB_2BPP";
    case 0x8c02: return "PVRTC_RGBA_4BPP";
    case 0x8c03: return "PVRTC_RGBA_2BPP";
    case 0x8c92: return "ATC_RGB";
    case 0x8c93: return "ATC_RGBA_EXPLICIT";
    case 0x87ee: return "ATC_RGBA_INTERPOLATED";
    case 0x8d64: return "ETC1_RGB8";
    case 0x81a5: return "DEPTH_COMPONENT16";
    case 0x8058: return "RGBA8";
    default:     return "?";
    }
}

/* ------------------------------------------------------------- FBO tracking */

struct FboRecord {
    GLuint name;
    long   binds;
    long   draws;
    long   vertices;
    GLenum status;           /* 0 until checked */
    int    checked;
};

enum { kMaxFbos = 24 };
static FboRecord g_fbos[kMaxFbos];
static int g_fbo_count = 0;

static FboRecord *fbo_record(GLuint name)
{
    for (int i = 0; i < g_fbo_count; i++)
        if (g_fbos[i].name == name)
            return &g_fbos[i];
    if (g_fbo_count >= kMaxFbos)
        return NULL;
    FboRecord *r = &g_fbos[g_fbo_count++];
    memset(r, 0, sizeof(*r));
    r->name = name;
    return r;
}

static const char *fbo_status_name(GLenum status)
{
    switch (status) {
    case 0x8CD5: return "COMPLETE";
    case 0x8CD6: return "INCOMPLETE_ATTACHMENT";
    case 0x8CD7: return "INCOMPLETE_MISSING_ATTACHMENT";
    case 0x8CD9: return "INCOMPLETE_DIMENSIONS";
    case 0x8CDD: return "UNSUPPORTED";
    case 0x8D56: return "INCOMPLETE_MULTISAMPLE";
    default:     return "?";
    }
}

/*
 * Ask the driver whether the framebuffer we are about to draw into can be
 * rendered to at all. An incomplete FBO swallows every draw and reports
 * GL_INVALID_FRAMEBUFFER_OPERATION, which is exactly the shape of "the scene is
 * submitted and never appears". Asked once per FBO name, never per draw.
 */
static GLenum check_framebuffer_status(void)
{
    using Check = GLenum (*)(GLenum);
    static Check check =
        (Check)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    return check ? check(GL_FRAMEBUFFER) : 0;
}

/* ------------------------------------------------------------- error census */

struct ErrorBucket { GLenum code; long count; const char *first_where; };
enum { kMaxErrors = 8 };
static ErrorBucket g_errors[kMaxErrors];
static int  g_error_kinds = 0;
static long g_error_lines = 0;

static const char *error_name(GLenum e)
{
    switch (e) {
    case 0x0500: return "INVALID_ENUM";
    case 0x0501: return "INVALID_VALUE";
    case 0x0502: return "INVALID_OPERATION";
    case 0x0505: return "OUT_OF_MEMORY";
    case 0x0506: return "INVALID_FRAMEBUFFER_OPERATION";
    default:     return "?";
    }
}

void gl_stats_check_error(const char *what)
{
    if (!gl_stats_error_check_enabled())
        return;

    using GetError = GLenum (*)(void);
    static GetError get_error = (GetError)SDL_GL_GetProcAddress("glGetError");
    if (!get_error)
        return;

    GLenum e;
    while ((e = get_error()) != 0) {
        bump(&Scalars::errors);
        int found = 0;
        for (int i = 0; i < g_error_kinds; i++) {
            if (g_errors[i].code == e) { g_errors[i].count++; found = 1; break; }
        }
        if (!found && g_error_kinds < kMaxErrors) {
            g_errors[g_error_kinds].code = e;
            g_errors[g_error_kinds].count = 1;
            g_errors[g_error_kinds].first_where = what;
            g_error_kinds++;
        }
        /* The first few in full context; after that the tally is the report. */
        if (g_error_lines < 40) {
            g_error_lines++;
            report("error 0x%04x %s after %s "
                   "(program=%u fbo=%u depth=%d cull=%d arraybuf=%u)",
                   (unsigned)e, error_name(e), what ? what : "?",
                   g_program, g_framebuffer, g_depth_test, g_cull_face,
                   g_array_buffer);
        }
    }
}

/* ------------------------------------------------------- dropped-call alarms */

struct DropRecord { const char *symbol; long count; };
enum { kMaxDrops = 24 };
static DropRecord g_drops[kMaxDrops];
static int g_drop_kinds = 0;

void gl_stats_note_dropped(const char *symbol, const char *why)
{
    bump(&Scalars::dropped);
    for (int i = 0; i < g_drop_kinds; i++) {
        if (g_drops[i].symbol == symbol || (symbol && g_drops[i].symbol &&
            strcmp(g_drops[i].symbol, symbol) == 0)) {
            g_drops[i].count++;
            return;
        }
    }
    if (g_drop_kinds < kMaxDrops) {
        g_drops[g_drop_kinds].symbol = symbol;
        g_drops[g_drop_kinds].count = 1;
        g_drop_kinds++;
    }
    /* Loud on purpose, and not behind LOADER_TRACE: this is the failure mode
     * that produced a black screen with an empty log once already. */
    fprintf(stderr,
            "GLSTATS: *** DROPPED %s - not forwarded to the driver (%s). "
            "Everything this call was supposed to do did not happen. ***\n",
            symbol ? symbol : "(unnamed)", why ? why : "no reason given");
    fflush(stderr);
}

void gl_stats_current_state(GLuint *program, GLuint *framebuffer,
                            int *blend, int *depth_test)
{
    if (program)     *program     = g_program;
    if (framebuffer) *framebuffer = g_framebuffer;
    if (blend)       *blend       = g_blend;
    if (depth_test)  *depth_test  = g_depth_test;
}

/* ---------------------------------------------------------------- observers */

void gl_stats_note_use_program(GLuint program)
{
    g_program = program;
    bump(&Scalars::use_program);
}

void gl_stats_note_bind_buffer(GLenum target, GLuint buffer)
{
    if (target == GL_ARRAY_BUFFER)              g_array_buffer = buffer;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) g_index_buffer = buffer;
    bump(&Scalars::bind_buffer);
}

void gl_stats_note_buffer_data(GLenum target, long size, GLenum usage,
                               int has_data)
{
    (void)target; (void)usage;
    bump(&Scalars::buffer_data);
    bump(&Scalars::buffer_bytes, size);
    if (!has_data)
        bump(&Scalars::buffer_data_null);
}

void gl_stats_note_vertex_attrib_pointer(GLuint index, GLint size, GLenum type,
                                         GLboolean normalized, GLsizei stride,
                                         const void *pointer)
{
    (void)size; (void)type; (void)normalized; (void)stride; (void)pointer;
    bump(&Scalars::vertex_attrib_pointer);
    if (!g_array_buffer)
        bump(&Scalars::attrib_pointer_client);
    if (index < 32)
        g_attrib_pointer_mask |= (1 << index);
}

void gl_stats_note_vertex_attrib_array(GLuint index, int enabled)
{
    if (index < 32) {
        if (enabled) g_attrib_enabled_mask |= (1 << index);
        else         g_attrib_enabled_mask &= ~(1 << index);
    }
    bump(enabled ? &Scalars::enable_attrib : &Scalars::disable_attrib);
}

void gl_stats_note_bind_texture(GLenum target, GLuint texture)
{
    (void)target;
    g_texture = texture;
    bump(&Scalars::bind_texture);
}

void gl_stats_note_bind_framebuffer(GLenum target, GLuint framebuffer)
{
    (void)target;
    g_framebuffer = framebuffer;   /* shadowed in both modes - see note_draw */
    if (!gl_stats_enabled())
        return;
    bump(&Scalars::bind_framebuffer);
    if (framebuffer == 0)
        bump(&Scalars::bind_framebuffer_default);
    FboRecord *r = fbo_record(framebuffer);
    if (r)
        r->binds++;
}

void gl_stats_note_framebuffer_attach(const char *entry, GLenum attachment,
                                      GLuint name)
{
    static int lines = 0;
    if (!gl_stats_enabled())
        return;
    if (lines < 32) {
        lines++;
        report("fbo %u <- %s attachment=0x%04x name=%u",
               g_framebuffer, entry ? entry : "?", (unsigned)attachment, name);
    }
    /* The attachment set changed, so any completeness verdict we cached for
     * this framebuffer is stale. */
    FboRecord *r = fbo_record(g_framebuffer);
    if (r)
        r->checked = 0;
}

void gl_stats_note_capability(GLenum cap, int enabled)
{
    switch (cap) {
    case GL_DEPTH_TEST:
        g_depth_test = enabled;
        bump(enabled ? &Scalars::enable_depth : &Scalars::disable_depth);
        break;
    case GL_CULL_FACE:
        g_cull_face = enabled;
        bump(enabled ? &Scalars::enable_cull : &Scalars::disable_cull);
        break;
    case GL_BLEND:        g_blend = enabled; break;
    case GL_SCISSOR_TEST: g_scissor_test = enabled; break;
    default: break;
    }
}

void gl_stats_note_viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    (void)x; (void)y; (void)width; (void)height;
    bump(&Scalars::viewport);
}

void gl_stats_note_scissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    (void)x; (void)y; (void)width; (void)height;
    bump(&Scalars::scissor);
}

void gl_stats_note_clear(GLbitfield mask)
{
    /* The census is opt-in, and every other note_* checks that first. This one
     * did not, so the six glGet* round trips to the driver further down ran on
     * every frame of every run whether statistics had been asked for or not -
     * the only guard in this file that was in the wrong place. */
    if (!gl_stats_enabled())
        return;

    bump(&Scalars::clear);
    if (mask & GL_COLOR_BUFFER_BIT)   bump(&Scalars::clear_colour);
    if (mask & GL_DEPTH_BUFFER_BIT)   bump(&Scalars::clear_depth);
    if (mask & GL_STENCIL_BUFFER_BIT) bump(&Scalars::clear_stencil);

    /*
     * The colour, read back rather than intercepted.
     *
     * glClearColor takes four floats, so shadowing it would need a softfp
     * bridge on the game's side of the ABI - a real risk for a number that is
     * only wanted for the report. glGetFloatv answers the same question from
     * the driver, and the screen this is chasing is *white*, which is one of
     * the two colours a clear can plausibly be leaving behind.
     */
    using GetFloatv = void (*)(GLenum, GLfloat *);
    using GetIntegerv = void (*)(GLenum, GLint *);
    static GetFloatv get_floatv = (GetFloatv)SDL_GL_GetProcAddress("glGetFloatv");
    static GetIntegerv get_intv =
        (GetIntegerv)SDL_GL_GetProcAddress("glGetIntegerv");
    if (get_floatv) {
        GLfloat c[4] = {-1, -1, -1, -1};
        get_floatv(GL_COLOR_CLEAR_VALUE, c);
        g_clear_r = c[0]; g_clear_g = c[1];
        g_clear_b = c[2]; g_clear_a = c[3];

        /*
         * Depth state, read at the same moment.
         *
         * The screen shows the sun, the lens flare and the HUD - every draw
         * that runs with the depth test off - over a black world, while the
         * census says hundreds of thousands of depth-tested vertices were
         * submitted. Everything that is depth-tested disappearing and nothing
         * else disappearing is not a geometry symptom; it is a depth-buffer
         * one, and it has exactly three inputs: what the depth buffer is
         * cleared to, which comparison is used, and whether writes are on.
         *
         * All three arrive through entry points that take floats
         * (glClearDepthf, glDepthRangef) on a softfp/hardfp boundary, so a
         * wrong bridge would corrupt the value with no error anywhere - which
         * is why they are read back from the driver rather than trusted.
         */
        GLfloat d = -1, range[2] = {-1, -1};
        get_floatv(GL_DEPTH_CLEAR_VALUE, &d);
        get_floatv(GL_DEPTH_RANGE, range);
        g_depth_clear = d;
        g_depth_near = range[0];
        g_depth_far = range[1];
    }
    if (get_intv) {
        GLint v = 0;
        get_intv(GL_DEPTH_FUNC, &v);
        g_depth_func = v;
        v = 0;
        get_intv(GL_DEPTH_WRITEMASK, &v);
        g_depth_mask = v;
        v = 0;
        get_intv(GL_DEPTH_BITS, &v);
        g_depth_bits = v;
    }
}

/*
 * The last few draws of the frame, which is where a full-screen quad that
 * paints over everything would be.
 *
 * A frame that submits two million vertices and shows a flat colour has either
 * lost them or covered them, and the two are told apart by what ran last.
 */
struct TailDraw {
    GLuint program, framebuffer, texture;
    GLenum mode;
    GLsizei count;
    unsigned char depth, cull, blend, indexed;
};
enum { kTail = 10 };
static TailDraw g_tail[kTail];
static int  g_tail_next = 0;
static long g_tail_total = 0;

static void tail_record(GLenum mode, GLsizei count, int indexed)
{
    TailDraw &t = g_tail[g_tail_next];
    g_tail_next = (g_tail_next + 1) % kTail;
    g_tail_total++;
    t.program = g_program;
    t.framebuffer = g_framebuffer;
    t.texture = g_texture;
    t.mode = mode;
    t.count = count;
    t.depth = (unsigned char)(g_depth_test ? 1 : 0);
    t.cull = (unsigned char)(g_cull_face ? 1 : 0);
    t.blend = (unsigned char)(g_blend ? 1 : 0);
    t.indexed = (unsigned char)indexed;
}

void gl_stats_note_draw(GLenum mode, GLsizei count, int indexed, GLenum type)
{
    (void)type;
    /*
     * The draw path is the one place where the census could pay for itself in
     * frame time: a bucket scan, a tail slot and - on the first draw after a
     * binding - a real glCheckFramebufferStatus round trip. None of that may
     * run when the census is off, or an FPS number measured on hardware would
     * be measuring this file. The state shadowing in the other note_ functions
     * stays live in both modes: it is a handful of stores, and the composite
     * quad skip reads it.
     */
    if (!gl_stats_enabled())
        return;
    if (indexed) {
        bump(&Scalars::draw_elements);
        bump(&Scalars::verts_elements, count);
    } else {
        bump(&Scalars::draw_arrays);
        bump(&Scalars::verts_arrays, count);
    }
    bucket_add(mode, count, indexed);
    tail_record(mode, count, indexed);

    FboRecord *r = fbo_record(g_framebuffer);
    if (r) {
        r->draws++;
        r->vertices += count;
        if (!r->checked) {
            r->checked = 1;
            r->status = check_framebuffer_status();
            report("fbo %u first draw this binding: status=0x%04x %s",
                   g_framebuffer, (unsigned)r->status,
                   fbo_status_name(r->status));
        }
    }
}

void gl_stats_note_texture_upload(int compressed, GLenum internalformat,
                                  GLsizei width, GLsizei height, long bytes,
                                  int forwarded)
{
    if (!gl_stats_enabled())
        return;
    bump(compressed ? &Scalars::compressed_tex_image_2d
                    : &Scalars::tex_image_2d);
    bump(&Scalars::texture_bytes, bytes);

    FormatBucket *f = NULL;
    for (int i = 0; i < g_format_count; i++)
        if (g_formats[i].format == internalformat) { f = &g_formats[i]; break; }
    if (!f && g_format_count < kMaxFormats) {
        f = &g_formats[g_format_count++];
        memset(f, 0, sizeof(*f));
        f->format = internalformat;
        f->compressed = compressed;
    }
    if (!f)
        return;
    f->uploads++;
    f->bytes += bytes;
    if (!forwarded)
        f->dropped++;
    if (width  > f->max_w) f->max_w = width;
    if (height > f->max_h) f->max_h = height;
}

/* ------------------------------------------------------------ dispatch audit
 *
 * Which table answers each entry point, and does that pointer agree with the
 * one SDL hands out? A mismatch means the game's draws and the game's state are
 * being executed by two different dispatch layers, which renders nothing while
 * every individual call returns cleanly.
 */
static uintptr_t table_lookup(DynLibFunction *table, const char *name)
{
    for (int i = 0; table && table[i].symbol; i++)
        if (strcmp(table[i].symbol, name) == 0)
            return table[i].func;
    return 0;
}

static const char *kAudited[] = {
    "glClear", "glDrawArrays", "glDrawElements", "glTexImage2D",
    "glCompressedTexImage2D", "glViewport", "glScissor", "glBindBuffer",
    "glBufferData", "glVertexAttribPointer", "glEnableVertexAttribArray",
    "glUseProgram", "glBindTexture", "glEnable", "glDisable",
    "glBindFramebuffer", "glFramebufferTexture2D", "glCheckFramebufferStatus",
    NULL,
};

void gl_stats_init(void)
{
    if (!gl_stats_enabled())
        return;

    report("census on: dump every %ld frames, error checks %s, provider %s",
           dump_period(),
           gl_stats_error_check_enabled() ? "ON" : "off",
           gl_provider_name());

    int diverged = 0;
    for (int i = 0; kAudited[i]; i++) {
        const char *name = kAudited[i];
        uintptr_t g1  = table_lookup(symtable_gles1, name);
        uintptr_t g2  = table_lookup(symtable_gles2, name);
        uintptr_t sdl = (uintptr_t)SDL_GL_GetProcAddress(name);
        /* The winner is the table the loader would have picked: glprobe first,
         * then gles1, then gles2 - see so_dynamic_libraries in symtab.cpp. */
        const char *winner = g1 ? "gles1" : (g2 ? "gles2" : "none");
        uintptr_t chosen = g1 ? g1 : g2;
        int mismatch = (chosen && sdl && chosen != sdl);
        if (mismatch)
            diverged++;
        if (!chosen || mismatch)
            report("dispatch %-28s table=%-5s gles1=%p gles2=%p sdl=%p%s",
                   name, winner, (void *)g1, (void *)g2, (void *)sdl,
                   !chosen ? "  <<< UNRESOLVED" : "  <<< DIFFERENT POINTER");
    }
    report("dispatch audit: %d of %d entry points disagree with "
           "SDL_GL_GetProcAddress (0 is the healthy answer)",
           diverged, (int)(sizeof(kAudited) / sizeof(kAudited[0]) - 1));

    /*
     * Different pointers are only fatal if they are different *drivers*. Ask
     * both paths who they are: identical strings mean one implementation behind
     * two dispatch tables (survivable, and what glvnd does under Mesa);
     * different strings mean the game's draws and the game's state are going to
     * two separate GL contexts, which no amount of correct rendering code can
     * survive.
     */
    using GetString = const GLubyte *(*)(GLenum);
    GetString via_table = (GetString)table_lookup(symtable_gles1, "glGetString");
    GetString via_sdl   = (GetString)SDL_GL_GetProcAddress("glGetString");
    for (int i = 0; i < 3; i++) {
        const GLenum what[3] = {GL_VENDOR, GL_RENDERER, GL_VERSION};
        const char *label[3] = {"VENDOR", "RENDERER", "VERSION"};
        const char *a = via_table ? (const char *)via_table(what[i]) : NULL;
        const char *b = via_sdl   ? (const char *)via_sdl(what[i])   : NULL;
        report("identity %-8s gles1-table=\"%s\"  sdl=\"%s\"%s", label[i],
               a ? a : "(null)", b ? b : "(null)",
               (a && b && strcmp(a, b) == 0) ? "" : "  <<< DIFFERENT DRIVER");
    }
}

/* ------------------------------------------------------------------- report */

static const char *mode_name(GLenum mode)
{
    switch (mode) {
    case 0x0000: return "POINTS";
    case 0x0001: return "LINES";
    case 0x0002: return "LINE_LOOP";
    case 0x0003: return "LINE_STRIP";
    case 0x0004: return "TRIANGLES";
    case 0x0005: return "TRIANGLE_STRIP";
    case 0x0006: return "TRIANGLE_FAN";
    default:     return "?";
    }
}

static void dump(const char *label, long frame)
{
    report("=== %s frame=%ld ===", label, frame);
    report("draws     arrays=%ld (%ld verts)  elements=%ld (%ld verts)"
           "   [total %ld / %ld]",
           g_win.draw_arrays, g_win.verts_arrays,
           g_win.draw_elements, g_win.verts_elements,
           g_all.draw_arrays + g_all.draw_elements,
           g_all.verts_arrays + g_all.verts_elements);
    report("buffers   bindBuffer=%ld bufferData=%ld (%ld B, %ld sized-only)"
           "  attribPointer=%ld (client-side %ld)  attribArray +%ld/-%ld",
           g_win.bind_buffer, g_win.buffer_data, g_win.buffer_bytes,
           g_win.buffer_data_null, g_win.vertex_attrib_pointer,
           g_win.attrib_pointer_client, g_win.enable_attrib,
           g_win.disable_attrib);
    report("state     useProgram=%ld bindTexture=%ld  depth +%ld/-%ld"
           "  cull +%ld/-%ld  viewport=%ld scissor=%ld",
           g_win.use_program, g_win.bind_texture,
           g_win.enable_depth, g_win.disable_depth,
           g_win.enable_cull, g_win.disable_cull,
           g_win.viewport, g_win.scissor);
    report("clear     calls=%ld colour=%ld depth=%ld stencil=%ld"
           "   colour-value=(%.3f %.3f %.3f %.3f)"
           "   bindFramebuffer=%ld (default %ld)",
           g_win.clear, g_win.clear_colour, g_win.clear_depth,
           g_win.clear_stencil, g_clear_r, g_clear_g, g_clear_b, g_clear_a,
           g_win.bind_framebuffer, g_win.bind_framebuffer_default);
    report("textures  plain=%ld compressed=%ld bytes=%ld"
           "   [total plain=%ld compressed=%ld]",
           g_win.tex_image_2d, g_win.compressed_tex_image_2d,
           g_win.texture_bytes, g_all.tex_image_2d,
           g_all.compressed_tex_image_2d);
    report("depth     clear-value=%.4f range=[%.4f %.4f] func=0x%04x "
           "writemask=%d bits=%d",
           g_depth_clear, g_depth_near, g_depth_far, (unsigned)g_depth_func,
           g_depth_mask, g_depth_bits);
    report("current   program=%u fbo=%u arraybuf=%u indexbuf=%u texture=%u"
           "  depth=%d cull=%d blend=%d scissor=%d attribs=0x%08x",
           g_program, g_framebuffer, g_array_buffer, g_index_buffer,
           g_texture, g_depth_test, g_cull_face, g_blend, g_scissor_test,
           (unsigned)g_attrib_enabled_mask);

    /* The line the whole exercise is for: is anything being drawn with depth
     * testing on, and how big is it. */
    long depth_draws = 0, depth_verts = 0, flat_draws = 0, flat_verts = 0;
    long offscreen_draws = 0, offscreen_verts = 0;
    for (int i = 0; i < g_bucket_count; i++) {
        const DrawBucket &b = g_buckets[i];
        if (b.depth) { depth_draws += b.draws; depth_verts += b.vertices; }
        else         { flat_draws  += b.draws; flat_verts  += b.vertices; }
        if (b.framebuffer) {
            offscreen_draws += b.draws;
            offscreen_verts += b.vertices;
        }
    }
    report("split     depth-tested %ld draws / %ld verts   flat %ld draws / "
           "%ld verts   off-screen %ld draws / %ld verts   buckets=%d%s",
           depth_draws, depth_verts, flat_draws, flat_verts,
           offscreen_draws, offscreen_verts, g_bucket_count,
           g_bucket_overflow ? "  (OVERFLOW)" : "");

    for (int i = 0; i < g_bucket_count; i++) {
        const DrawBucket &b = g_buckets[i];
        report("  bucket prog=%-3u fbo=%-3u %-14s %s depth=%d cull=%d "
               "blend=%d vbo=%d draws=%-6ld verts=%-9ld count=%d..%d",
               b.program, b.framebuffer, mode_name(b.mode),
               b.indexed ? "idx" : "arr", b.depth, b.cull, b.blend, b.vbo,
               b.draws, b.vertices, (int)b.min_count, (int)b.max_count);
    }

    for (int i = 0; i < g_fbo_count; i++) {
        const FboRecord &r = g_fbos[i];
        report("  fbo %-3u binds=%-6ld draws=%-7ld verts=%-10ld status=0x%04x %s",
               r.name, r.binds, r.draws, r.vertices, (unsigned)r.status,
               r.checked ? fbo_status_name(r.status) : "(unchecked)");
    }

    for (int i = 0; i < g_format_count; i++) {
        const FormatBucket &f = g_formats[i];
        report("  texfmt 0x%04x %-22s %s uploads=%-6ld bytes=%-11ld "
               "max=%dx%d dropped=%ld",
               (unsigned)f.format, format_name(f.format),
               f.compressed ? "compressed" : "plain     ",
               f.uploads, f.bytes, (int)f.max_w, (int)f.max_h, f.dropped);
    }

    if (g_error_kinds) {
        for (int i = 0; i < g_error_kinds; i++)
            report("  glerror 0x%04x %-30s count=%ld first-after=%s",
                   (unsigned)g_errors[i].code, error_name(g_errors[i].code),
                   g_errors[i].count,
                   g_errors[i].first_where ? g_errors[i].first_where : "?");
    } else if (gl_stats_error_check_enabled()) {
        report("  glerror none");
    }

    /* Oldest first, so the list reads in submission order. */
    if (g_tail_total) {
        const int have = g_tail_total < kTail ? (int)g_tail_total : kTail;
        const int start = (int)((g_tail_next + kTail - have) % kTail);
        for (int i = 0; i < have; i++) {
            const TailDraw &t = g_tail[(start + i) % kTail];
            report("  last-%d prog=%-4u fbo=%-3u tex=%-4u %-14s %s depth=%d "
                   "cull=%d blend=%d count=%d",
                   have - i, t.program, t.framebuffer, t.texture,
                   mode_name(t.mode), t.indexed ? "idx" : "arr", t.depth,
                   t.cull, t.blend, (int)t.count);
        }
    }

    for (int i = 0; i < g_drop_kinds; i++)
        report("  DROPPED %s x%ld", g_drops[i].symbol, g_drops[i].count);

    memset(&g_win, 0, sizeof(g_win));
    bucket_reset();
}

void gl_stats_mark(const char *label, long frame)
{
    if (!gl_stats_enabled())
        return;
    dump(label ? label : "mark", frame);
}

void gl_stats_frame(long frame)
{
    if (!gl_stats_enabled())
        return;
    if (frame % dump_period() != 0)
        return;
    dump("periodic", frame);
}
