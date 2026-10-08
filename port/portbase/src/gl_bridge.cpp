/*
 * GLES bridge, guest side.
 *
 * The game is an ARM32 process under qemu-user, where the only OpenGL on offer
 * is Mesa's software rasteriser. This file replaces every GL entry point the
 * game can reach with a stub that serialises the call into a pipe; the Android
 * app on the other end replays the stream on the phone's GPU (GlBridge.java).
 *
 * Wire format, guest -> host, little endian:
 *     u32 total length (this header included, multiple of 4)   u32 op   payload
 * Synchronous queries are answered on a second pipe:
 *     u32 length   bytes
 *
 * What makes it work without a round trip per call:
 *  - object names (textures, buffers, framebuffers, renderbuffers, shaders,
 *    programs) are allocated here and translated to real names by the host;
 *  - state the game can only have set itself is never asked for back;
 *  - uniform and attribute locations are queried once per program and cached;
 *  - client-side vertex arrays (pointers into guest memory) are captured at
 *    draw time and shipped inside the draw message, which is the only moment
 *    their contents are defined.
 *
 * Calls that must see the host's answer (compile/link status, locations, info
 * logs, glGet*, glReadPixels) flush the stream and block on the reply.
 *
 * Ops and scalar stubs come from android/tools/gen_glbridge.py.
 */
#define PORT_GL_NO_REDIRECT
#include "gl_bridge.h"

#include <SDL2/SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "khronos/glad.h"
#include "trace.h"
#include "port_env.h"

/* ------------------------------------------------------------- transport */

static int  g_cmd_fd = -1;
static int  g_rep_fd = -1;
static int  g_enabled = -1;
static long g_frames = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
alignas(8) static uint8_t g_out[1 << 20];
static size_t  g_out_len = 0;

/*
 * <PREFIX>_GL_PROFILE: 1 times each frame's step() and swap wait; 2 also
 * times every bridge call. Under qemu-user clock_gettime is a real syscall,
 * so level 2 inflates what it measures - use it for proportions only.
 */
static int      g_profile = -1;
static int      g_lock_depth;
static uint64_t g_lock_t0, g_bridge_ns, g_bridge_calls;

static uint64_t now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/*
 * The engine makes every GL call from its render thread, and under emulation
 * a mutex per call (thousands per frame) is a real cost. So the first thread
 * owns the bridge without locking; if a second thread ever calls in, locking
 * is switched on for good from then on.
 */
static pthread_t g_owner;
static bool      g_owner_set;
static bool      g_multi;

struct Lock {
    bool locked;
    Lock() : locked(false)
    {
        if (__builtin_expect(g_multi, 0)) {
            pthread_mutex_lock(&g_lock);
            locked = true;
        } else {
            pthread_t me = pthread_self();
            if (!g_owner_set) {
                g_owner = me;
                g_owner_set = true;
            } else if (!pthread_equal(me, g_owner)) {
                g_multi = true;
                trace("gl bridge: GL called from a second thread; locking from now on");
                pthread_mutex_lock(&g_lock);
                locked = true;
            }
        }
        if (g_profile > 1 && g_lock_depth++ == 0) {
            g_lock_t0 = now_ns();
            g_bridge_calls++;
        }
    }
    ~Lock()
    {
        if (g_profile > 1 && --g_lock_depth == 0)
            g_bridge_ns += now_ns() - g_lock_t0;
        if (locked)
            pthread_mutex_unlock(&g_lock);
    }
};

/* The host is gone (app closed or crashed); nothing sensible is left to do. */
static void gone(const char *what)
{
    fprintf(stderr, "GLBRIDGE: %s failed (%s): host is gone, exiting\n", what, strerror(errno));
    fflush(stderr);
    _exit(0);
}

static void write_all(const void *p, size_t n)
{
    const uint8_t *s = (const uint8_t *)p;
    while (n > 0) {
        ssize_t w = write(g_cmd_fd, s, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            gone("write");
        }
        s += w;
        n -= (size_t)w;
    }
}

static void read_all(void *p, size_t n)
{
    uint8_t *d = (uint8_t *)p;
    while (n > 0) {
        ssize_t r = read(g_rep_fd, d, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            gone("read");
        }
        if (r == 0) {
            errno = EPIPE;
            gone("read (eof)");
        }
        d += r;
        n -= (size_t)r;
    }
}

static void out_flush()
{
    if (g_out_len) {
        write_all(g_out, g_out_len);
        g_out_len = 0;
    }
}

static void out_raw(const void *p, size_t n)
{
    if (n > sizeof(g_out) - g_out_len) {
        out_flush();
        if (n >= sizeof(g_out) / 2) {
            write_all(p, n);
            return;
        }
    }
    memcpy(g_out + g_out_len, p, n);
    g_out_len += n;
}

static void msg_begin(int op, size_t payload)
{
    uint32_t h[2] = { (uint32_t)((8 + payload + 3) & ~3u), (uint32_t)op };
    out_raw(h, 8);
}

static void msg_end(size_t payload)
{
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    size_t pad = ((payload + 3) & ~(size_t)3) - payload;
    if (pad)
        out_raw(zeros, pad);
}

static void send_msg(int op, const void *hdr, size_t hlen, const void *data, size_t dlen)
{
    /* The common case - a few words of arguments and no data - written as
     * words straight into the buffer: no memcpy calls, which are expensive
     * for tiny sizes when the code is emulated. hlen is always whole words. */
    if (__builtin_expect(dlen == 0, 1) && 8 + hlen <= sizeof(g_out) - g_out_len) {
        uint32_t *w = (uint32_t *)(g_out + g_out_len);
        const uint32_t *h = (const uint32_t *)hdr;
        size_t n = hlen / 4;
        w[0] = (uint32_t)(8 + hlen);
        w[1] = (uint32_t)op;
        for (size_t i = 0; i < n; i++)
            w[2 + i] = h[i];
        g_out_len += 8 + hlen;
        return;
    }
    size_t payload = hlen + dlen;
    msg_begin(op, payload);
    if (hlen)
        out_raw(hdr, hlen);
    if (dlen)
        out_raw(data, dlen);
    msg_end(payload);
}

static void gb_cmd(int op, const void *hdr, size_t hlen, const void *data, size_t dlen)
{
    Lock l;
    send_msg(op, hdr, hlen, data, dlen);
}

static void emit(int op, std::initializer_list<int32_t> hdr, const void *data = NULL, size_t dlen = 0)
{
    gb_cmd(op, hdr.begin(), hdr.size() * 4, data, dlen);
}

static inline void gb_put(int32_t *d, int v) { *d = v; }
static inline void gb_put(int32_t *d, unsigned v) { *d = (int32_t)v; }
static inline void gb_put(int32_t *d, unsigned char v) { *d = v; }
static inline void gb_put(int32_t *d, float v) { memcpy(d, &v, 4); }

/* Frames sent but not yet presented by the host; the guest may run <PREFIX>_GL_INFLIGHT frames ahead
 * (default 2: while the host replays one frame and waits for vsync, the guest already builds the next). */
static int g_inflight = 0;
static int g_max_inflight = 2;
static const uint32_t kAck = 0xFFFFFFFFu;

/* Next reply length, absorbing frame acknowledgements that arrive in between. */
static uint32_t read_reply_len()
{
    for (;;) {
        uint32_t len = 0;
        read_all(&len, 4);
        if (len != kAck)
            return len;
        if (g_inflight > 0)
            g_inflight--;
    }
}

/* Sends a query and waits for the reply. */
static void query(int op, std::initializer_list<int32_t> args, std::vector<uint8_t> &rep,
                  const void *data = NULL, size_t dlen = 0)
{
    Lock l;
    send_msg(op, args.begin(), args.size() * 4, data, dlen);
    out_flush();
    uint32_t len = read_reply_len();
    if (len > (256u << 20)) {
        errno = EPROTO;
        gone("reply length");
    }
    rep.resize(len);
    if (len)
        read_all(rep.data(), len);
}

static int32_t query_int(int op, std::initializer_list<int32_t> args, const void *data = NULL, size_t dlen = 0)
{
    std::vector<uint8_t> rep;
    query(op, args, rep, data, dlen);
    int32_t v = 0;
    if (rep.size() >= 4)
        memcpy(&v, rep.data(), 4);
    return v;
}

#include "gl_bridge_gen.inc"

/* ------------------------------------------------------------ local state */

enum { KIND_TEX = 0, KIND_BUF = 1, KIND_FBO = 2, KIND_RBO = 3, KIND_SHADER = 4, KIND_PROG = 5 };
static uint32_t g_next_obj[6];

static GLuint g_array_buf = 0;
static GLuint g_elem_buf = 0;
static std::vector<uint32_t> g_buf_size;

static int g_unpack_align = 4;
static int g_pack_align = 4;
static int g_unpack_row_length = 0;

/* Mirrors of GL state the redundancy filters below compare against. */
struct UniSlot {
    uint8_t  kind;    /* 0 = unknown */
    uint8_t  words;
    uint32_t v[16];
};
static std::vector<std::vector<UniSlot> > g_uni;   /* [program][location] */
static GLuint   g_cur_prog;
static GLenum   g_active_unit = 0x84C0;            /* GL_TEXTURE0 */
static GLuint   g_bound_tex[32][2];                /* [unit][2D, cube map] */
static std::vector<std::vector<uint8_t> > g_elem_shadow;  /* index buffers, see elem_shadow() */

struct Attrib {
    bool enabled;
    bool client;
    GLint size;
    GLenum type;
    GLboolean normalized;
    GLsizei stride;
    const void *ptr;
};
static const int kMaxAttribs = 16;
static Attrib g_attr[kMaxAttribs];

static std::map<std::pair<uint32_t, std::string>, int> g_attrib_loc;
static std::map<std::pair<uint32_t, std::string>, int> g_uniform_loc;

static int comps(GLenum f)
{
    switch (f) {
    case 0x1906: case 0x1909: case 0x1903: case 0x8D94: case 0x1902: case 0x84F9: return 1;
    case 0x190A: case 0x8227: case 0x8228: return 2;
    case 0x1907: case 0x8D98: return 3;
    default: return 4;
    }
}

static int bytes_per_pixel(GLenum format, GLenum type)
{
    int c = comps(format);
    switch (type) {
    case 0x1401: case 0x1400: return c;
    case 0x1403: case 0x1402: case 0x140B: case 0x8D61: return 2 * c;
    case 0x8363: case 0x8033: case 0x8034: return 2;
    case 0x1405: case 0x1404: case 0x1406: return 4 * c;
    case 0x84FA: case 0x8368: case 0x8C3B: case 0x8C3E: return 4;
    default: return c;
    }
}

/* Bytes a glTexImage call or glReadPixels touches, honouring the row alignment. */
static size_t image_bytes(GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, int align, int row_length)
{
    if (w <= 0 || h <= 0 || d <= 0)
        return 0;
    size_t bpp = (size_t)bytes_per_pixel(format, type);
    size_t row = (size_t)(row_length > 0 ? row_length : w) * bpp;
    if (align > 1)
        row = (row + (size_t)align - 1) / (size_t)align * (size_t)align;
    size_t rows = (size_t)h * (size_t)d;
    return row * (rows - 1) + (size_t)w * bpp;
}

static int type_size(GLenum t)
{
    switch (t) {
    case 0x1400: case 0x1401: return 1;
    case 0x1402: case 0x1403: case 0x140B: return 2;
    default: return 4;
    }
}

/* -------------------------------------------------- stateful call wrappers */

static void gb_glPixelStorei(GLenum pname, GLint param)
{
    if (pname == 0x0CF5) g_unpack_align = param;
    else if (pname == 0x0D05) g_pack_align = param;
    else if (pname == 0x0CF2) g_unpack_row_length = param;
    gb_raw_glPixelStorei(pname, param);
}

/*
 * Everything a freshly linked program will be asked, fetched in one round trip.
 *
 * After linking, the engine walks the program: status, log length, the count
 * of attributes and uniforms, then glGetActive* and glGet*Location for each -
 * a couple of dozen synchronous queries per shader, each draining the whole
 * pipeline, during loading and whenever a new effect first appears in play.
 * The host answers them all at once (GB_OP_Q_PROGRAM_INFO) and they are served
 * from here; anything not covered still goes to the host.
 */
enum { GB_OP_Q_PROGRAM_INFO = 250 };

struct ProgVar {
    GLint size;
    GLenum type;
    std::string name;
};
struct ProgInfo {
    bool valid;
    GLint link_status, log_len, max_attr, max_uni;
    std::vector<ProgVar> attrs, unis;
};
static std::vector<ProgInfo> g_prog_info;

static void program_info_fetch(GLuint program)
{
    static int on = -1;
    if (on < 0)
        on = port_getenv_long("PROGRAM_INFO", 1) != 0;
    if (!on || program == 0)
        return;
    std::vector<uint8_t> rep;
    query(GB_OP_Q_PROGRAM_INFO, { (int32_t)program }, rep);
    if (program >= g_prog_info.size())
        g_prog_info.resize((size_t)program + 16);
    ProgInfo &pi = g_prog_info[program];
    pi = ProgInfo();
    /* link, loglen, nattr, maxattr, nuni, maxuni; then per var: size, type, loc, namelen, name (padded 4). */
    if (rep.size() < 24)
        return;
    const uint8_t *p = rep.data(), *end = p + rep.size();
    int32_t h[6];
    memcpy(h, p, 24);
    p += 24;
    pi.link_status = h[0];
    pi.log_len = h[1];
    pi.max_attr = h[3];
    pi.max_uni = h[5];
    for (int pass = 0; pass < 2; pass++) {
        int n = pass == 0 ? h[2] : h[4];
        std::vector<ProgVar> &vars = pass == 0 ? pi.attrs : pi.unis;
        for (int i = 0; i < n; i++) {
            if (end - p < 16)
                return;
            int32_t v[4];
            memcpy(v, p, 16);
            p += 16;
            size_t nl = (size_t)v[3];
            if ((size_t)(end - p) < ((nl + 3) & ~(size_t)3))
                return;
            ProgVar var;
            var.size = v[0];
            var.type = (GLenum)v[1];
            var.name.assign((const char *)p, nl);
            p += (nl + 3) & ~(size_t)3;
            vars.push_back(var);
            /* Locations go straight into the caches glGet*Location reads. */
            std::map<std::pair<uint32_t, std::string>, int> &cache = pass == 0 ? g_attrib_loc : g_uniform_loc;
            cache[std::make_pair((uint32_t)program, var.name)] = v[2];
            if (pass == 1 && var.name.size() > 3 && var.name.compare(var.name.size() - 3, 3, "[0]") == 0)
                cache[std::make_pair((uint32_t)program, var.name.substr(0, var.name.size() - 3))] = v[2];
        }
    }
    pi.valid = true;
}

static const ProgInfo *program_info(GLuint program)
{
    return program < g_prog_info.size() && g_prog_info[program].valid ? &g_prog_info[program] : NULL;
}

static void gb_glLinkProgram(GLuint program)
{
    {
        Lock l;
        g_attrib_loc.clear();
        g_uniform_loc.clear();
    }
    /* Linking resets the program's uniforms; forget what they held. */
    if (program < g_uni.size())
        g_uni[program].clear();
    gb_raw_glLinkProgram(program);
    program_info_fetch(program);
}

static void gb_glEnableVertexAttribArray(GLuint index)
{
    if (index < (GLuint)kMaxAttribs) g_attr[index].enabled = true;
    gb_raw_glEnableVertexAttribArray(index);
}

static void gb_glDisableVertexAttribArray(GLuint index)
{
    if (index < (GLuint)kMaxAttribs) g_attr[index].enabled = false;
    gb_raw_glDisableVertexAttribArray(index);
}

/* ------------------------------------------------------------ objects */

static void gen_objects(int kind, GLsizei n, GLuint *out)
{
    if (n <= 0 || !out)
        return;
    Lock l;
    std::vector<int32_t> w((size_t)n + 2);
    w[0] = kind;
    w[1] = n;
    for (GLsizei i = 0; i < n; i++) {
        out[i] = ++g_next_obj[kind];
        w[2 + i] = (int32_t)out[i];
    }
    send_msg(GB_OP_GEN, w.data(), w.size() * 4, NULL, 0);
}

static void delete_objects(int kind, GLsizei n, const GLuint *ids)
{
    if (n <= 0 || !ids)
        return;
    std::vector<int32_t> w((size_t)n + 2);
    w[0] = kind;
    w[1] = n;
    for (GLsizei i = 0; i < n; i++)
        w[2 + i] = (int32_t)ids[i];
    gb_cmd(GB_OP_DELETE, w.data(), w.size() * 4, NULL, 0);
    if (kind == KIND_BUF) {
        for (GLsizei i = 0; i < n; i++) {
            if (ids[i] == g_array_buf) g_array_buf = 0;
            if (ids[i] == g_elem_buf) g_elem_buf = 0;
            if (ids[i] < g_elem_shadow.size())
                std::vector<uint8_t>().swap(g_elem_shadow[ids[i]]);
        }
    }
}

static void gb_glGenTextures(GLsizei n, GLuint *t) { gen_objects(KIND_TEX, n, t); }
static void gb_glGenBuffers(GLsizei n, GLuint *b) { gen_objects(KIND_BUF, n, b); }
static void gb_glGenFramebuffers(GLsizei n, GLuint *f) { gen_objects(KIND_FBO, n, f); }
static void gb_glGenRenderbuffers(GLsizei n, GLuint *r) { gen_objects(KIND_RBO, n, r); }
static void gb_glDeleteTextures(GLsizei n, const GLuint *t)
{
    /* GL unbinds a deleted texture everywhere it is bound. */
    for (GLsizei i = 0; t && i < n; i++)
        for (int u = 0; u < 32; u++)
            for (int k = 0; k < 2; k++)
                if (g_bound_tex[u][k] == t[i])
                    g_bound_tex[u][k] = 0;
    delete_objects(KIND_TEX, n, t);
}
static void gb_glDeleteBuffers(GLsizei n, const GLuint *b) { delete_objects(KIND_BUF, n, b); }
static void gb_glDeleteFramebuffers(GLsizei n, const GLuint *f) { delete_objects(KIND_FBO, n, f); }
static void gb_glDeleteRenderbuffers(GLsizei n, const GLuint *r) { delete_objects(KIND_RBO, n, r); }

static GLuint gb_glCreateShader(GLenum type)
{
    Lock l;
    GLuint id = ++g_next_obj[KIND_SHADER];
    int32_t w[2] = { (int32_t)type, (int32_t)id };
    send_msg(GB_OP_CREATE_SHADER, w, 8, NULL, 0);
    return id;
}

static GLuint gb_glCreateProgram(void)
{
    Lock l;
    GLuint id = ++g_next_obj[KIND_PROG];
    int32_t w[1] = { (int32_t)id };
    send_msg(GB_OP_CREATE_PROGRAM, w, 4, NULL, 0);
    return id;
}

static void gb_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
    std::string src;
    for (GLsizei i = 0; i < count; i++) {
        if (!strings[i])
            continue;
        if (lengths && lengths[i] >= 0)
            src.append(strings[i], (size_t)lengths[i]);
        else
            src.append(strings[i]);
    }
    emit(GB_OP_SHADER_SOURCE, { (int32_t)shader, (int32_t)src.size() }, src.data(), src.size());
}

/* ------------------------------------------------------------ buffers */

static void gb_glBindBuffer(GLenum target, GLuint buffer)
{
    if (target == 0x8892) g_array_buf = buffer;
    else if (target == 0x8893) g_elem_buf = buffer;
    emit(GB_OP_BIND_BUFFER, { (int32_t)target, (int32_t)buffer });
}

/*
 * Index buffers, shadowed. A draw that takes its indices from a buffer but its
 * vertices from client memory has to ship the vertices it touches, and only
 * the indices say which those are; the host's copy cannot be read back
 * cheaply, so the guest keeps its own.
 */
static std::vector<uint8_t> *elem_shadow(GLuint id)
{
    if (id == 0)
        return NULL;
    if (id >= g_elem_shadow.size())
        g_elem_shadow.resize((size_t)id + 64);
    return &g_elem_shadow[id];
}

static void shadow_write(GLenum target, size_t offset, const void *data, size_t size, bool whole)
{
    if (target != 0x8893)
        return;
    std::vector<uint8_t> *s = elem_shadow(g_elem_buf);
    if (!s)
        return;
    if (whole)
        s->assign(size, 0);
    if (offset + size > s->size())
        s->resize(offset + size, 0);
    if (data && size)
        memcpy(s->data() + offset, data, size);
}

static void gb_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    GLuint id = target == 0x8893 ? g_elem_buf : g_array_buf;
    if (id >= g_buf_size.size())
        g_buf_size.resize((size_t)id + 64, 0);
    g_buf_size[id] = size > 0 ? (uint32_t)size : 0;
    shadow_write(target, 0, data, size > 0 ? (size_t)size : 0, true);
    emit(GB_OP_BUFFER_DATA, { (int32_t)target, (int32_t)size, (int32_t)usage, data ? (int32_t)size : -1 },
         data, data && size > 0 ? (size_t)size : 0);
}

static void gb_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
    if (!data || size <= 0)
        return;
    shadow_write(target, (size_t)offset, data, (size_t)size, false);
    emit(GB_OP_BUFFER_SUB_DATA, { (int32_t)target, (int32_t)offset, (int32_t)size, (int32_t)size }, data, (size_t)size);
}

/* The engine maps buffers for writing; hand it scratch memory and upload on unmap. */
struct MapState { void *ptr; size_t size; };
static MapState g_map[2];

static void *gb_glMapBufferOES(GLenum target, GLenum access)
{
    (void)access;
    int slot = target == 0x8893 ? 1 : 0;
    GLuint id = slot ? g_elem_buf : g_array_buf;
    if (id >= g_buf_size.size() || g_buf_size[id] == 0)
        return NULL;
    free(g_map[slot].ptr);
    g_map[slot].size = g_buf_size[id];
    g_map[slot].ptr = calloc(1, g_map[slot].size);
    return g_map[slot].ptr;
}

static GLboolean gb_glUnmapBufferOES(GLenum target)
{
    int slot = target == 0x8893 ? 1 : 0;
    if (!g_map[slot].ptr)
        return GL_FALSE;
    shadow_write(target, 0, g_map[slot].ptr, g_map[slot].size, false);
    emit(GB_OP_BUFFER_SUB_DATA, { (int32_t)target, 0, (int32_t)g_map[slot].size, (int32_t)g_map[slot].size },
         g_map[slot].ptr, g_map[slot].size);
    free(g_map[slot].ptr);
    g_map[slot].ptr = NULL;
    g_map[slot].size = 0;
    return GL_TRUE;
}

/* ------------------------------------------------------------ textures */

static void gb_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                            GLint border, GLenum format, GLenum type, const void *pixels)
{
    size_t n = pixels ? image_bytes(width, height, 1, format, type, g_unpack_align, g_unpack_row_length) : 0;
    emit(GB_OP_TEX_IMAGE_2D, { (int32_t)target, level, internalformat, width, height, border, (int32_t)format,
                               (int32_t)type, pixels ? (int32_t)n : -1 }, pixels, n);
}

static void gb_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                               GLsizei height, GLenum format, GLenum type, const void *pixels)
{
    if (!pixels)
        return;
    size_t n = image_bytes(width, height, 1, format, type, g_unpack_align, g_unpack_row_length);
    emit(GB_OP_TEX_SUB_IMAGE_2D, { (int32_t)target, level, xoffset, yoffset, width, height, (int32_t)format,
                                   (int32_t)type, (int32_t)n }, pixels, n);
}

static void gb_glCompressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                      GLsizei height, GLint border, GLsizei imageSize, const void *data)
{
    size_t n = data && imageSize > 0 ? (size_t)imageSize : 0;
    emit(GB_OP_COMPRESSED_TEX_IMAGE_2D, { (int32_t)target, level, (int32_t)internalformat, width, height, border,
                                          imageSize, data ? (int32_t)n : -1 }, data, n);
}

static void gb_glCompressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                         GLsizei height, GLenum format, GLsizei imageSize, const void *data)
{
    if (!data || imageSize <= 0)
        return;
    emit(GB_OP_COMPRESSED_TEX_SUB_IMAGE_2D, { (int32_t)target, level, xoffset, yoffset, width, height,
                                              (int32_t)format, imageSize, imageSize }, data, (size_t)imageSize);
}

static void gb_glTexImage3DOES(GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height,
                               GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels)
{
    size_t n = pixels ? image_bytes(width, height, depth, format, type, g_unpack_align, g_unpack_row_length) : 0;
    emit(GB_OP_TEX_IMAGE_3D, { (int32_t)target, level, (int32_t)internalformat, width, height, depth, border,
                               (int32_t)format, (int32_t)type, pixels ? (int32_t)n : -1 }, pixels, n);
}

static void gb_glTexSubImage3DOES(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                  GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                  const void *pixels)
{
    if (!pixels)
        return;
    size_t n = image_bytes(width, height, depth, format, type, g_unpack_align, g_unpack_row_length);
    emit(GB_OP_TEX_SUB_IMAGE_3D, { (int32_t)target, level, xoffset, yoffset, zoffset, width, height, depth,
                                   (int32_t)format, (int32_t)type, (int32_t)n }, pixels, n);
}

static void gb_glCompressedTexImage3DOES(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                         GLsizei height, GLsizei depth, GLint border, GLsizei imageSize,
                                         const void *data)
{
    size_t n = data && imageSize > 0 ? (size_t)imageSize : 0;
    emit(GB_OP_COMPRESSED_TEX_IMAGE_3D, { (int32_t)target, level, (int32_t)internalformat, width, height, depth,
                                          border, imageSize, data ? (int32_t)n : -1 }, data, n);
}

static void gb_glCompressedTexSubImage3DOES(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                            GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                                            GLenum format, GLsizei imageSize, const void *data)
{
    if (!data || imageSize <= 0)
        return;
    emit(GB_OP_COMPRESSED_TEX_SUB_IMAGE_3D, { (int32_t)target, level, xoffset, yoffset, zoffset, width, height,
                                              depth, (int32_t)format, imageSize, imageSize }, data, (size_t)imageSize);
}

/* ------------------------------------------------------------ uniforms */

/* ------------------------------------------------------- redundant state */

/*
 * The engine re-sends state it has already set: the same texture to the same
 * unit, the same program, uniforms with the values they already hold. Each
 * of those costs an encode here (emulated) and a JNI call on the host, and a
 * frame carries a few thousand. What GL would ignore is not sent.
 *
 * Uniform values belong to the program, so they are cached per program and
 * location, up to a mat4; anything larger - bone palettes - always goes, and
 * clears the cached slots it overlaps. Relinking a program resets its cache.
 */
static UniSlot *uni_slot(GLint loc, bool create)
{
    if (loc < 0 || loc >= 4096 || g_cur_prog == 0)
        return NULL;
    if (g_cur_prog >= g_uni.size()) {
        if (!create)
            return NULL;
        g_uni.resize((size_t)g_cur_prog + 16);
    }
    std::vector<UniSlot> &p = g_uni[g_cur_prog];
    if ((size_t)loc >= p.size()) {
        if (!create)
            return NULL;
        p.resize((size_t)loc + 8, UniSlot());
    }
    return &p[(size_t)loc];
}

/* True when the value is what the slot already holds (caller then skips the call). */
static bool uni_same(int kind, GLint loc, GLsizei count, const void *v, size_t bytes)
{
    if (count != 1 || bytes > 64) {
        /* An array upload: forget every slot it writes over. */
        for (GLsizei i = 0; i < count; i++) {
            UniSlot *s = uni_slot(loc + i, false);
            if (s)
                s->kind = 0;
        }
        return false;
    }
    UniSlot *s = uni_slot(loc, true);
    if (!s)
        return false;
    size_t words = bytes / 4;
    if (s->kind == kind + 1 && s->words == words && memcmp(s->v, v, bytes) == 0)
        return true;
    s->kind = (uint8_t)(kind + 1);
    s->words = (uint8_t)words;
    memcpy(s->v, v, bytes);
    return false;
}

static void gbf_glUseProgram(GLuint program)
{
    if (program == g_cur_prog && program != 0)
        return;
    g_cur_prog = program;
    gb_glUseProgram(program);
}

static void gbf_glActiveTexture(GLenum unit)
{
    if (unit == g_active_unit)
        return;
    g_active_unit = unit;
    gb_glActiveTexture(unit);
}

static void gbf_glBindTexture(GLenum target, GLuint tex)
{
    unsigned unit = g_active_unit - 0x84C0;
    int slot = target == 0x0DE1 ? 0 : (target == 0x8513 ? 1 : -1);
    if (slot >= 0 && unit < 32) {
        if (g_bound_tex[unit][slot] == tex)
            return;
        g_bound_tex[unit][slot] = tex;
    }
    gb_glBindTexture(target, tex);
}

static void gbf_glUniform1i(GLint loc, GLint v)
{
    if (loc < 0 || uni_same(100, loc, 1, &v, 4))
        return;
    gb_glUniform1i(loc, v);
}

static void gbf_glUniform1f(GLint loc, GLfloat v)
{
    if (loc < 0 || uni_same(101, loc, 1, &v, 4))
        return;
    gb_glUniform1f(loc, v);
}

static void uniform_v(int kind, int comps_per, GLint loc, GLsizei count, const void *v, GLboolean transpose)
{
    if (count <= 0 || !v || loc < 0)
        return;
    size_t n = (size_t)count * (size_t)comps_per * 4;
    /* glUniform1i and glUniform1iv set the same slot; key both by kind+comps. */
    int key = kind == 0 ? 101 : (kind == 1 ? 100 : kind + (transpose ? 50 : 0));
    if (uni_same(key, loc, count, v, n))
        return;
    emit(GB_OP_UNIFORM_V, { kind, loc, count, (int32_t)transpose }, v, n);
}

static void gb_glUniform1fv(GLint l, GLsizei c, const GLfloat *v) { uniform_v(0, 1, l, c, v, 0); }
static void gb_glUniform1iv(GLint l, GLsizei c, const GLint *v) { uniform_v(1, 1, l, c, v, 0); }
static void gb_glUniform2fv(GLint l, GLsizei c, const GLfloat *v) { uniform_v(2, 2, l, c, v, 0); }
static void gb_glUniform2iv(GLint l, GLsizei c, const GLint *v) { uniform_v(3, 2, l, c, v, 0); }
static void gb_glUniform3fv(GLint l, GLsizei c, const GLfloat *v) { uniform_v(4, 3, l, c, v, 0); }
static void gb_glUniform3iv(GLint l, GLsizei c, const GLint *v) { uniform_v(5, 3, l, c, v, 0); }
static void gb_glUniform4fv(GLint l, GLsizei c, const GLfloat *v) { uniform_v(6, 4, l, c, v, 0); }
static void gb_glUniform4iv(GLint l, GLsizei c, const GLint *v) { uniform_v(7, 4, l, c, v, 0); }
static void gb_glUniformMatrix4fv(GLint l, GLsizei c, GLboolean t, const GLfloat *v) { uniform_v(8, 16, l, c, v, t); }

/* ------------------------------------------------------------ draws */

static void gb_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride,
                                     const void *pointer)
{
    if (index >= (GLuint)kMaxAttribs)
        return;
    Attrib &a = g_attr[index];
    a.size = size;
    a.type = type;
    a.normalized = normalized;
    a.stride = stride;
    a.ptr = pointer;
    a.client = g_array_buf == 0;
    if (!a.client)
        emit(GB_OP_ATTRIB_POINTER_VBO, { (int32_t)index, size, (int32_t)type, (int32_t)normalized, stride,
                                         (int32_t)(uintptr_t)pointer });
}

struct Block {
    int idx;
    const Attrib *a;
    const uint8_t *data;
    size_t len;
};

/* Client arrays that are switched on, covering vertices [v0, v0 + vcount). */
static void collect_client(GLint v0, GLsizei vcount, std::vector<Block> &out)
{
    for (int i = 0; i < kMaxAttribs; i++) {
        const Attrib &a = g_attr[i];
        if (!a.enabled || !a.client || !a.ptr || vcount <= 0)
            continue;
        size_t elem = (size_t)a.size * (size_t)type_size(a.type);
        size_t stride = a.stride ? (size_t)a.stride : elem;
        Block b;
        b.idx = i;
        b.a = &a;
        b.data = (const uint8_t *)a.ptr + (size_t)v0 * stride;
        b.len = (size_t)(vcount - 1) * stride + elem;
        out.push_back(b);
    }
}

static inline size_t pad4(size_t n) { return (n + 3) & ~(size_t)3; }

static void write_blocks(const std::vector<Block> &blocks)
{
    for (size_t i = 0; i < blocks.size(); i++) {
        const Block &b = blocks[i];
        int32_t h[6] = { b.idx, b.a->size, (int32_t)b.a->type, (int32_t)b.a->normalized, b.a->stride,
                         (int32_t)b.len };
        out_raw(h, sizeof(h));
        out_raw(b.data, b.len);
        msg_end(b.len);
    }
}

static size_t blocks_bytes(const std::vector<Block> &blocks)
{
    size_t n = 0;
    for (size_t i = 0; i < blocks.size(); i++)
        n += 24 + pad4(blocks[i].len);
    return n;
}

static void gb_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (count <= 0)
        return;
    Lock l;
    std::vector<Block> blocks;
    collect_client(first, count, blocks);
    size_t payload = 16 + blocks_bytes(blocks);
    msg_begin(GB_OP_DRAW_ARRAYS, payload);
    int32_t h[4] = { (int32_t)mode, first, count, (int32_t)blocks.size() };
    out_raw(h, sizeof(h));
    write_blocks(blocks);
}

static uint32_t max_index_of(const void *indices, GLsizei count, GLenum type)
{
    uint32_t m = 0;
    if (type == 0x1401) {
        const uint8_t *p = (const uint8_t *)indices;
        for (GLsizei i = 0; i < count; i++) if (p[i] > m) m = p[i];
    } else if (type == 0x1403) {
        const uint16_t *p = (const uint16_t *)indices;
        for (GLsizei i = 0; i < count; i++) if (p[i] > m) m = p[i];
    } else {
        const uint32_t *p = (const uint32_t *)indices;
        for (GLsizei i = 0; i < count; i++) if (p[i] > m) m = p[i];
    }
    return m;
}

static void gb_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    if (count <= 0)
        return;
    Lock l;
    std::vector<Block> blocks;
    const bool client_indices = g_elem_buf == 0;
    size_t idx_bytes = 0;

    if (client_indices) {
        if (!indices)
            return;
        idx_bytes = (size_t)count * (size_t)type_size(type);
        collect_client(0, (GLsizei)max_index_of(indices, count, type) + 1, blocks);
    } else {
        bool any_client = false;
        for (int i = 0; i < kMaxAttribs; i++)
            any_client |= g_attr[i].enabled && g_attr[i].client && g_attr[i].ptr;
        if (any_client) {
            /* Indices in a buffer, vertices in client memory: the shadow copy
             * says how many vertices the draw reaches. */
            std::vector<uint8_t> *s = elem_shadow(g_elem_buf);
            size_t off = (size_t)(uintptr_t)indices;
            size_t need = off + (size_t)count * (size_t)type_size(type);
            if (s && need <= s->size()) {
                collect_client(0, (GLsizei)max_index_of(s->data() + off, count, type) + 1, blocks);
            } else {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    trace("gl bridge: indexed draw past its index buffer's known contents (%zu > %zu)",
                          need, s ? s->size() : (size_t)0);
                }
            }
        }
    }

    size_t payload = 24 + blocks_bytes(blocks) + (client_indices ? pad4(idx_bytes) : 0);
    msg_begin(GB_OP_DRAW_ELEMENTS, payload);
    int32_t h[6] = { (int32_t)mode, count, (int32_t)type, client_indices ? 1 : 0,
                     client_indices ? (int32_t)idx_bytes : (int32_t)(uintptr_t)indices, (int32_t)blocks.size() };
    out_raw(h, sizeof(h));
    write_blocks(blocks);
    if (client_indices) {
        out_raw(indices, idx_bytes);
        msg_end(idx_bytes);
    }
}

static void gb_glInvalidateFramebuffer(GLenum target, GLsizei num, const GLenum *attachments)
{
    if (num <= 0 || !attachments)
        return;
    emit(GB_OP_INVALIDATE_FB, { (int32_t)target, num, 0, 0, 0, 0, 0 }, attachments, (size_t)num * 4);
}

static void gb_glInvalidateSubFramebuffer(GLenum target, GLsizei num, const GLenum *attachments, GLint x, GLint y,
                                          GLsizei w, GLsizei h)
{
    if (num <= 0 || !attachments)
        return;
    emit(GB_OP_INVALIDATE_FB, { (int32_t)target, num, 1, x, y, w, h }, attachments, (size_t)num * 4);
}

static void gb_glFlush(void)
{
    Lock l;
    out_flush();
}

static GLenum gb_glGetError(void) { return 0; }

/* ------------------------------------------------------------ queries */

/* Only extensions whose entry points the bridge can serve are advertised. */
static std::string filter_extensions(const std::string &all)
{
    static const char *const kKeep[] = {
        "GL_OES_compressed_ETC1_RGB8_texture", "GL_EXT_texture_compression_s3tc",
        "GL_EXT_texture_compression_dxt1", "GL_AMD_compressed_ATC_texture",
        "GL_ATI_texture_compression_atitc", "GL_OES_depth_texture", "GL_OES_depth24",
        "GL_OES_packed_depth_stencil", "GL_OES_rgb8_rgba8", "GL_OES_standard_derivatives",
        "GL_OES_element_index_uint", "GL_EXT_texture_filter_anisotropic", "GL_OES_texture_half_float",
        "GL_OES_texture_half_float_linear", "GL_OES_texture_float", "GL_OES_texture_float_linear",
        "GL_EXT_texture_format_BGRA8888", "GL_EXT_blend_minmax", "GL_OES_mapbuffer", "GL_OES_texture_3D",
        "GL_EXT_discard_framebuffer", "GL_EXT_shader_framebuffer_fetch", "GL_OES_fbo_render_mipmap",
        NULL,
    };
    std::string out;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(' ', pos);
        if (end == std::string::npos)
            end = all.size();
        std::string tok = all.substr(pos, end - pos);
        for (int i = 0; kKeep[i]; i++) {
            if (tok == kKeep[i]) {
                if (!out.empty())
                    out += ' ';
                out += tok;
                break;
            }
        }
        pos = end + 1;
    }
    return out;
}

static const GLubyte *gb_glGetString(GLenum name)
{
    static std::map<GLenum, std::string> cache;
    static pthread_mutex_t str_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&str_lock);
    std::map<GLenum, std::string>::iterator it = cache.find(name);
    if (it == cache.end()) {
        std::vector<uint8_t> rep;
        query(GB_OP_Q_GET_STRING, { (int32_t)name }, rep);
        std::string s((const char *)rep.data(), rep.size());
        /* The engine picks its quality profile by renderer name and gives an
         * unknown GPU the heaviest one; <PREFIX>_GL_RENDERER names a known one. */
        const char *renderer = port_getenv("GL_RENDERER");
        if (name == 0x1F01 && renderer && *renderer) {
            trace("gl bridge: renderer %s reported as %s", s.c_str(), renderer);
            s = renderer;
        }
        if (name == 0x1F03) {
            trace("gl bridge: host extensions: %s", s.c_str());
            s = filter_extensions(s);
            trace("gl bridge: exposed extensions: %s", s.c_str());
        } else if (name == 0x1F00 || name == 0x1F01 || name == 0x1F02) {
            trace("gl bridge: host GL string 0x%04x = %s", name, s.c_str());
        }
        it = cache.insert(std::make_pair(name, s)).first;
    }
    const GLubyte *result = (const GLubyte *)it->second.c_str();
    pthread_mutex_unlock(&str_lock);
    return result;
}

static void gb_glGetIntegerv(GLenum pname, GLint *data)
{
    std::vector<uint8_t> rep;
    query(GB_OP_Q_GET_INTEGERV, { (int32_t)pname }, rep);
    size_t n = rep.size() < 64 ? rep.size() : 64;
    if (data && n)
        memcpy(data, rep.data(), n);
}

static void gb_glGetFloatv(GLenum pname, GLfloat *data)
{
    std::vector<uint8_t> rep;
    query(GB_OP_Q_GET_FLOATV, { (int32_t)pname }, rep);
    size_t n = rep.size() < 64 ? rep.size() : 64;
    if (data && n)
        memcpy(data, rep.data(), n);
}

static void gb_glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
    int32_t v = query_int(GB_OP_Q_GET_SHADERIV, { (int32_t)shader, (int32_t)pname });
    if (params)
        *params = v;
}

static void gb_glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
    const ProgInfo *pi = program_info(program);
    if (pi && params) {
        switch (pname) {
        case 0x8B82: *params = pi->link_status; return;           /* GL_LINK_STATUS */
        case 0x8B84: *params = pi->log_len; return;               /* GL_INFO_LOG_LENGTH */
        case 0x8B89: *params = (GLint)pi->attrs.size(); return;   /* GL_ACTIVE_ATTRIBUTES */
        case 0x8B8A: *params = pi->max_attr; return;              /* GL_ACTIVE_ATTRIBUTE_MAX_LENGTH */
        case 0x8B86: *params = (GLint)pi->unis.size(); return;    /* GL_ACTIVE_UNIFORMS */
        case 0x8B87: *params = pi->max_uni; return;               /* GL_ACTIVE_UNIFORM_MAX_LENGTH */
        default: break;
        }
    }
    int32_t v = query_int(GB_OP_Q_GET_PROGRAMIV, { (int32_t)program, (int32_t)pname });
    if (params)
        *params = v;
}

static void copy_text(const std::vector<uint8_t> &rep, size_t skip, GLsizei bufsize, GLsizei *length, GLchar *out)
{
    size_t have = rep.size() > skip ? rep.size() - skip : 0;
    size_t n = bufsize > 0 ? (size_t)bufsize - 1 : 0;
    if (have < n)
        n = have;
    if (out && bufsize > 0) {
        if (n)
            memcpy(out, rep.data() + skip, n);
        out[n] = 0;
    }
    if (length)
        *length = (GLsizei)n;
}

static void gb_glGetShaderInfoLog(GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *log)
{
    std::vector<uint8_t> rep;
    query(GB_OP_Q_SHADER_LOG, { (int32_t)shader, bufsize }, rep);
    copy_text(rep, 0, bufsize, length, log);
}

static void gb_glGetProgramInfoLog(GLuint program, GLsizei bufsize, GLsizei *length, GLchar *log)
{
    std::vector<uint8_t> rep;
    query(GB_OP_Q_PROGRAM_LOG, { (int32_t)program, bufsize }, rep);
    copy_text(rep, 0, bufsize, length, log);
}

static void gb_glGetShaderSource(GLuint shader, GLsizei bufsize, GLsizei *length, GLchar *source)
{
    std::vector<uint8_t> rep;
    query(GB_OP_Q_SHADER_SOURCE, { (int32_t)shader, bufsize }, rep);
    copy_text(rep, 0, bufsize, length, source);
}

static void active_var(int op, GLuint program, GLuint index, GLsizei bufsize, GLsizei *length, GLint *size,
                       GLenum *type, GLchar *name)
{
    std::vector<uint8_t> rep;
    query(op, { (int32_t)program, (int32_t)index, bufsize }, rep);
    int32_t sz = 0, ty = 0;
    if (rep.size() >= 8) {
        memcpy(&sz, rep.data(), 4);
        memcpy(&ty, rep.data() + 4, 4);
    }
    if (size)
        *size = sz;
    if (type)
        *type = (GLenum)ty;
    copy_text(rep, 8, bufsize, length, name);
}

/* glGetActive* from the program info fetched at link time; false when it does not cover the call. */
static bool active_var_local(const std::vector<ProgVar> *vars, GLuint index, GLsizei bufsize, GLsizei *length,
                             GLint *size, GLenum *type, GLchar *name)
{
    if (!vars || index >= vars->size())
        return false;
    const ProgVar &v = (*vars)[index];
    if (size)
        *size = v.size;
    if (type)
        *type = v.type;
    size_t n = bufsize > 0 ? (size_t)bufsize - 1 : 0;
    if (v.name.size() < n)
        n = v.name.size();
    if (name && bufsize > 0) {
        memcpy(name, v.name.data(), n);
        name[n] = 0;
    }
    if (length)
        *length = (GLsizei)n;
    return true;
}

static void gb_glGetActiveAttrib(GLuint p, GLuint i, GLsizei b, GLsizei *len, GLint *size, GLenum *type, GLchar *name)
{
    const ProgInfo *pi = program_info(p);
    if (active_var_local(pi ? &pi->attrs : NULL, i, b, len, size, type, name))
        return;
    active_var(GB_OP_Q_ACTIVE_ATTRIB, p, i, b, len, size, type, name);
}

static void gb_glGetActiveUniform(GLuint p, GLuint i, GLsizei b, GLsizei *len, GLint *size, GLenum *type, GLchar *name)
{
    const ProgInfo *pi = program_info(p);
    if (active_var_local(pi ? &pi->unis : NULL, i, b, len, size, type, name))
        return;
    active_var(GB_OP_Q_ACTIVE_UNIFORM, p, i, b, len, size, type, name);
}

static GLint location(int op, std::map<std::pair<uint32_t, std::string>, int> &cache, GLuint program,
                      const GLchar *name)
{
    if (!name)
        return -1;
    std::pair<uint32_t, std::string> key(program, name);
    {
        Lock l;
        std::map<std::pair<uint32_t, std::string>, int>::iterator it = cache.find(key);
        if (it != cache.end())
            return it->second;
    }
    int32_t loc = query_int(op, { (int32_t)program }, name, strlen(name) + 1);
    Lock l;
    cache[key] = loc;
    return loc;
}

static GLint gb_glGetAttribLocation(GLuint program, const GLchar *name)
{
    return location(GB_OP_Q_ATTRIB_LOC, g_attrib_loc, program, name);
}

static GLint gb_glGetUniformLocation(GLuint program, const GLchar *name)
{
    return location(GB_OP_Q_UNIFORM_LOC, g_uniform_loc, program, name);
}

static GLenum gb_glCheckFramebufferStatus(GLenum target)
{
    return (GLenum)query_int(GB_OP_Q_CHECK_FB, { (int32_t)target });
}

/*
 * Reading pixels back is a full stall: every queued call is replayed and the
 * GPU drained before the answer comes. The engine does it every frame for lens
 * flare visibility, so a repeated read of the same rectangle is answered from
 * the last real one and refreshed only every <PREFIX>_READPIXELS_EVERY frames
 * (default 3; 1 = always read). A one-off read - a screenshot - is always real.
 */
struct ReadCache {
    int32_t key[7];
    long frame;
    std::vector<uint8_t> data;
};
static std::vector<ReadCache> g_read_cache;

static void gb_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                            void *pixels)
{
    static long every = -1;
    if (every < 0)
        every = port_getenv_long("READPIXELS_EVERY", 3);
    size_t want0 = image_bytes(width, height, 1, format, type, g_pack_align, 0);
    int32_t key[7] = { x, y, width, height, (int32_t)format, (int32_t)type, g_pack_align };
    ReadCache *hit = NULL;
    if (every > 1 && want0 <= 4096) {
        for (size_t i = 0; i < g_read_cache.size(); i++)
            if (memcmp(g_read_cache[i].key, key, sizeof(key)) == 0)
                hit = &g_read_cache[i];
        if (hit && g_frames - hit->frame < every && hit->frame != g_frames) {
            if (pixels && hit->data.size() >= want0)
                memcpy(pixels, hit->data.data(), want0);
            return;
        }
    }
    std::vector<uint8_t> rep;
    query(GB_OP_Q_READ_PIXELS, { x, y, width, height, (int32_t)format, (int32_t)type, g_pack_align }, rep);
    size_t want = want0;
    size_t n = rep.size() < want ? rep.size() : want;
    if (pixels && n)
        memcpy(pixels, rep.data(), n);
    if (every > 1 && want0 <= 4096) {
        if (!hit && g_read_cache.size() < 64) {
            g_read_cache.push_back(ReadCache());
            hit = &g_read_cache.back();
            memcpy(hit->key, key, sizeof(key));
            static int logged;
            if (logged++ < 8)
                trace("gl bridge: glReadPixels %dx%d at %d,%d fmt 0x%x type 0x%x - cached, refreshed every %ld frames",
                      width, height, x, y, format, type, every);
        }
        if (hit) {
            hit->frame = g_frames;
            hit->data.assign(rep.begin(), rep.begin() + n);
        }
    }
}

/* ------------------------------------------------------------ resolver */

struct Entry { const char *name; void *fn; };

#define GB_ENTRY(n) { #n, (void *)gb_##n },
static const Entry kEntries[] = {
    /* State filters first: the first match wins over the generated stubs. */
    { "glUseProgram", (void *)gbf_glUseProgram },
    { "glActiveTexture", (void *)gbf_glActiveTexture },
    { "glBindTexture", (void *)gbf_glBindTexture },
    { "glUniform1i", (void *)gbf_glUniform1i },
    { "glUniform1f", (void *)gbf_glUniform1f },
    GB_SIMPLE_CALLS(GB_ENTRY)
#undef GB_ENTRY
#define E(n) { #n, (void *)gb_##n },
    E(glGenTextures) E(glGenBuffers) E(glGenFramebuffers) E(glGenRenderbuffers)
    E(glDeleteTextures) E(glDeleteBuffers) E(glDeleteFramebuffers) E(glDeleteRenderbuffers)
    E(glCreateShader) E(glCreateProgram) E(glShaderSource)
    E(glBindBuffer) E(glBufferData) E(glBufferSubData) E(glMapBufferOES) E(glUnmapBufferOES)
    E(glTexImage2D) E(glTexSubImage2D) E(glCompressedTexImage2D) E(glCompressedTexSubImage2D)
    E(glTexImage3DOES) E(glTexSubImage3DOES) E(glCompressedTexImage3DOES) E(glCompressedTexSubImage3DOES)
    E(glUniform1fv) E(glUniform1iv) E(glUniform2fv) E(glUniform2iv) E(glUniform3fv) E(glUniform3iv)
    E(glUniform4fv) E(glUniform4iv) E(glUniformMatrix4fv)
    E(glVertexAttribPointer) E(glDrawArrays) E(glDrawElements)
    E(glInvalidateFramebuffer) E(glInvalidateSubFramebuffer)
    E(glFlush) E(glGetError)
    E(glGetString) E(glGetIntegerv) E(glGetFloatv) E(glGetShaderiv) E(glGetProgramiv)
    E(glGetShaderInfoLog) E(glGetProgramInfoLog) E(glGetShaderSource)
    E(glGetActiveAttrib) E(glGetActiveUniform) E(glGetAttribLocation) E(glGetUniformLocation)
    E(glCheckFramebufferStatus) E(glReadPixels)
#undef E
    { "glDiscardFramebufferEXT", (void *)gb_glInvalidateFramebuffer },
    { NULL, NULL },
};

extern "C" int gl_bridge_enabled(void)
{
    if (g_enabled < 0) {
        const char *v = port_getenv("GL_BRIDGE");
        g_enabled = (v && *v && *v != '0') ? 1 : 0;
    }
    return g_enabled;
}

extern "C" int gl_bridge_init(void)
{
    const char *cmd = port_getenv("GL_CMD");
    const char *rep = port_getenv("GL_REPLY");
    if (!cmd || !rep) {
        fatal("GL bridge: set the GL_CMD and GL_REPLY pipe paths");
        return 0;
    }
    g_cmd_fd = open(cmd, O_WRONLY);
    g_rep_fd = open(rep, O_RDONLY);
    if (g_cmd_fd < 0 || g_rep_fd < 0) {
        fatal("GL bridge: cannot open the pipes (%s, %s): %s", cmd, rep, strerror(errno));
        return 0;
    }
    g_max_inflight = (int)port_getenv_long("GL_INFLIGHT", 2);
    if (g_max_inflight < 1)
        g_max_inflight = 1;
    /* A bigger command pipe lets the guest write ahead while the host is busy with the previous frame. */
    long kb = port_getenv_long("GL_PIPE_KB", 1024);
    int got = fcntl(g_cmd_fd, 1031 /* F_SETPIPE_SZ */, (int)(kb * 1024));
    trace("gl bridge: connected (%s, %s), %d frame(s) in flight, command pipe %d KB", cmd, rep,
          g_max_inflight, got > 0 ? got / 1024 : -1);
    return 1;
}

/*
 * Per-frame timings with <PREFIX>_GL_PROFILE=1: the engine's step(), the part
 * of it spent encoding GL calls, and the wait for the host to present. Logged
 * as averages every 120 frames.
 */
static uint64_t g_step_ns, g_swap_ns, g_frame_t0;
static long     g_prof_frames;

extern "C" void gl_bridge_profile_step(int begin)
{
    if (g_profile < 0)
        g_profile = port_getenv_long("GL_PROFILE", 0) ? 1 : 0;
    if (g_profile <= 0)
        return;
    if (begin) {
        g_frame_t0 = now_ns();
        return;
    }
    g_step_ns += now_ns() - g_frame_t0;
    if (++g_prof_frames % 120 == 0) {
        double n = 120.0;
        trace("glprof: step %.1f ms (bridge %.1f ms, %.0f calls), swap wait %.1f ms per frame",
              g_step_ns / n / 1e6, g_bridge_ns / n / 1e6, g_bridge_calls / n, g_swap_ns / n / 1e6);
        g_step_ns = g_bridge_ns = g_bridge_calls = g_swap_ns = 0;
    }
}

extern "C" void gl_bridge_swap(void)
{
    Lock l;
    send_msg(GB_OP_SWAP, NULL, 0, NULL, 0);
    out_flush();
    g_inflight++;
    /* The flush above is bridge time; the wait below is the host's. */
    uint64_t w0 = g_profile > 0 ? now_ns() : 0;
    while (g_inflight > g_max_inflight) {
        uint32_t v = 0;
        read_all(&v, 4);
        if (v != kAck) {
            errno = EPROTO;
            gone("unexpected reply while waiting for a frame");
        }
        g_inflight--;
    }
    if (g_profile > 0) {
        uint64_t w = now_ns() - w0;
        g_swap_ns += w;
        if (g_profile > 1)
            g_bridge_ns -= w;  /* the Lock destructor will count the wait as bridge time */
    }
    g_frames++;
    if (g_frames <= 3)
        trace("gl bridge: frame %ld sent", g_frames);
}

extern "C" void *port_gl_getproc(const char *name)
{
    if (!gl_bridge_enabled())
        return SDL_GL_GetProcAddress(name);
    if (!name)
        return NULL;
    for (int i = 0; kEntries[i].name; i++) {
        if (strcmp(kEntries[i].name, name) == 0)
            return kEntries[i].fn;
    }
    return NULL;
}
