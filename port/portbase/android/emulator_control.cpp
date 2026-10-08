/*
 * File-backed control channel for the local qemu emulator.
 *
 * A host process appends one command per line to <control>/commands. The
 * loader consumes at most one line per game frame, which guarantees that a
 * down/up pair spans frames even if the driver writes both immediately.
 * touch lines are the exception: see touch_line().
 *
 * Supported commands:
 *   touch ACTION X Y ID       engine touchEvent (1 down, 2 move, 0 up)
 *   key CODE down|up          engine OnKeyDown/OnKeyUp with a raw keycode
 *   pad 0|1                   a PowerA pad disconnected / connected
 *   cursor X Y
 *   click down|up
 *   button NAME down|up
 *   stick left|right X Y
 *   screenshot TOKEN
 *   quit
 *
 * Screenshots are captured from the real default framebuffer and written as
 * RGBA PNG files under <control>/screenshots/. No game asset crosses this
 * interface; it only exposes pixels already rendered by the user's own copy.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <zlib.h>

#include "khronos/glad.h"
#include "trace.h"
#include "gl_bridge.h"

#include "emulator_control.h"
#include "input_bridge.h"
#include "../src/port_env.h"

static std::string g_control_dir;
static std::string g_command_path;
static FILE *g_commands = NULL;
static std::string g_screenshot_token;
static bool g_enabled = false;

static uintptr_t find_gles1_function(const char *name)
{
    return (uintptr_t)SDL_GL_GetProcAddress(name);
}

static bool safe_token(const char *token)
{
    if (!token || !*token)
        return false;
    size_t n = strlen(token);
    if (n > 80)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return true;
}

static void write_status(long frame, const char *state,
                         const char *screenshot = NULL)
{
    if (!g_enabled)
        return;

    std::string temp = g_control_dir + "/status.json.tmp";
    std::string final = g_control_dir + "/status.json";
    FILE *out = fopen(temp.c_str(), "w");
    if (!out)
        return;
    fprintf(out, "{\"state\":\"%s\",\"frame\":%ld,\"menu\":%d", state, frame,
            android_input_in_menu() ? 1 : 0);
    if (screenshot)
        fprintf(out, ",\"screenshot\":\"%s\"", screenshot);
    fprintf(out, "}\n");
    fclose(out);
    rename(temp.c_str(), final.c_str());
}

void emulator_control_init(void)
{
    const char *dir = port_getenv("CONTROL_DIR");
    if (!dir || !*dir)
        return;

    g_control_dir = dir;
    g_command_path = g_control_dir + "/commands";
    mkdir(g_control_dir.c_str(), 0775);
    std::string shots = g_control_dir + "/screenshots";
    mkdir(shots.c_str(), 0775);

    FILE *create = fopen(g_command_path.c_str(), "a");
    if (create)
        fclose(create);
    g_commands = fopen(g_command_path.c_str(), "r");
    if (!g_commands) {
        trace("emulator control disabled: cannot open %s: %s",
              g_command_path.c_str(), strerror(errno));
        return;
    }

    g_enabled = true;
    write_status(0, "ready");
    trace("emulator control ready at %s", g_control_dir.c_str());
}

/*
 * Touch lines. Unlike the rest they are not one per frame: a finger moving at
 * the panel's rate would fall further behind every frame. What still has to
 * span frames is a press and release of the same finger, so a second
 * down/up for a pointer already pressed or released this frame waits.
 * Returns false when the line has to wait for the next frame.
 */
static bool touch_line(const char *line, unsigned *changed)
{
    int a = 0, x = 0, y = 0, id = 0;
    if (sscanf(line, "%*s %d %d %d %d", &a, &x, &y, &id) != 4)
        return true;
    unsigned bit = 1u << (id & 31);
    if (a != 2) {
        if (*changed & bit)
            return false;
        *changed |= bit;
    }
    android_input_inject_touch(a, x, y, id);
    return true;
}

bool emulator_control_tick(long frame)
{
    if (!g_enabled || !g_commands)
        return true;

    char line[256];
    unsigned touched = 0;
    for (int n = 0; ; n++) {
        long at = ftell(g_commands);
        if (n >= 64 || !fgets(line, sizeof(line), g_commands)) {
            clearerr(g_commands);
            if (frame % 10 == 0)
                write_status(frame, "running");
            return true;
        }
        if (strncmp(line, "touch ", 6) != 0) {
            /* Anything else ends the frame's batch, as before; it is handled
             * below if it is the first line, otherwise next frame. */
            if (n > 0) {
                fseek(g_commands, at, SEEK_SET);
                if (frame % 10 == 0)
                    write_status(frame, "running");
                return true;
            }
            break;
        }
        if (!touch_line(line, &touched)) {
            fseek(g_commands, at, SEEK_SET);
            return true;
        }
    }

    char action[32] = {};
    if (sscanf(line, "%31s", action) != 1)
        return true;

    if (!strcmp(action, "key")) {
        int code = 0;
        char state[16] = {};
        if (sscanf(line, "%*s %d %15s", &code, state) == 2)
            android_input_inject_key(code, !strcmp(state, "down"));
    } else if (!strcmp(action, "pad")) {
        int on = 0;
        if (sscanf(line, "%*s %d", &on) == 1)
            android_input_set_pad(on != 0);
    } else if (!strcmp(action, "cursor")) {
        float x = 0, y = 0;
        if (sscanf(line, "%*s %f %f", &x, &y) == 2) {
            android_input_cursor_set(x, y);
            trace("emulator: cursor %.1f %.1f", (double)x, (double)y);
        }
    } else if (!strcmp(action, "click")) {
        char state[16] = {};
        if (sscanf(line, "%*s %15s", state) == 1)
            android_input_cursor_press(!strcmp(state, "down"));
    } else if (!strcmp(action, "button")) {
        char name[32] = {}, state[16] = {};
        if (sscanf(line, "%*s %31s %15s", name, state) == 2) {
            bool down = !strcmp(state, "down");
            if (!android_input_inject_control(name, down))
                trace("emulator: unknown control '%s'", name);
        }
    } else if (!strcmp(action, "stick")) {
        char name[16] = {};
        float x = 0.0f, y = 0.0f;
        if (sscanf(line, "%*s %15s %f %f", name, &x, &y) == 3) {
            if (android_input_inject_stick(name, x, y))
                trace("emulator: stick %s %.3f %.3f",
                      name, (double)x, (double)y);
            else
                trace("emulator: unknown stick '%s'", name);
        }
    } else if (!strcmp(action, "screenshot")) {
        char token[96] = {};
        if (sscanf(line, "%*s %95s", token) == 1 && safe_token(token))
            g_screenshot_token = token;
    } else if (!strcmp(action, "quit")) {
        trace("emulator: quit requested");
        return false;
    } else {
        trace("emulator: unknown command '%s'", action);
    }

    write_status(frame, "running");
    return true;
}

static void put_u32(FILE *out, uint32_t value)
{
    unsigned char bytes[4] = {
        (unsigned char)(value >> 24),
        (unsigned char)(value >> 16),
        (unsigned char)(value >> 8),
        (unsigned char)value,
    };
    fwrite(bytes, 1, sizeof(bytes), out);
}

static bool png_chunk(FILE *out, const char type[4],
                      const unsigned char *data, size_t size)
{
    if (size > 0xffffffffu)
        return false;
    put_u32(out, (uint32_t)size);
    fwrite(type, 1, 4, out);
    if (size)
        fwrite(data, 1, size, out);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *)type, 4);
    if (size)
        crc = crc32(crc, data, (uInt)size);
    put_u32(out, (uint32_t)crc);
    return ferror(out) == 0;
}

static bool write_png(const char *path, int width, int height,
                      const unsigned char *rgba)
{
    size_t stride = 1 + (size_t)width * 4;
    std::vector<unsigned char> raw(stride * (size_t)height);
    for (int y = 0; y < height; y++) {
        unsigned char *dst = raw.data() + (size_t)y * stride;
        const unsigned char *src =
            rgba + (size_t)(height - 1 - y) * (size_t)width * 4;
        dst[0] = 0;
        memcpy(dst + 1, src, (size_t)width * 4);
    }

    uLongf compressed_size = compressBound((uLong)raw.size());
    std::vector<unsigned char> compressed(compressed_size);
    if (compress2(compressed.data(), &compressed_size, raw.data(),
                  (uLong)raw.size(), Z_BEST_SPEED) != Z_OK)
        return false;
    compressed.resize(compressed_size);

    FILE *out = fopen(path, "wb");
    if (!out)
        return false;
    static const unsigned char signature[8] =
        {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(signature, 1, sizeof(signature), out);

    unsigned char ihdr[13] = {
        (unsigned char)(width >> 24), (unsigned char)(width >> 16),
        (unsigned char)(width >> 8), (unsigned char)width,
        (unsigned char)(height >> 24), (unsigned char)(height >> 16),
        (unsigned char)(height >> 8), (unsigned char)height,
        8, 6, 0, 0, 0,
    };
    bool ok = png_chunk(out, "IHDR", ihdr, sizeof(ihdr)) &&
              png_chunk(out, "IDAT", compressed.data(), compressed.size()) &&
              png_chunk(out, "IEND", NULL, 0);
    ok = ok && fclose(out) == 0;
    return ok;
}

/* Reads the default framebuffer, bottom row first, as tightly packed RGBA. */
static bool capture_framebuffer(int width, int height, unsigned char *dst)
{
    using ReadPixels = void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                                void *);
    using GetIntegerv = void (*)(GLenum, GLint *);
    using BindFramebuffer = void (*)(GLenum, GLuint);
    using PixelStorei = void (*)(GLenum, GLint);
    static ReadPixels read_pixels =
        (ReadPixels)find_gles1_function("glReadPixels");
    static GetIntegerv get_integer =
        (GetIntegerv)find_gles1_function("glGetIntegerv");
    static BindFramebuffer bind_framebuffer =
        (BindFramebuffer)find_gles1_function("glBindFramebufferOES");
    static PixelStorei pixel_store =
        (PixelStorei)find_gles1_function("glPixelStorei");

    if (!read_pixels || !get_integer || !bind_framebuffer || !pixel_store)
        return false;

    GLint previous_fb = 0, previous_pack = 4;
    get_integer(GL_FRAMEBUFFER_BINDING, &previous_fb);
    get_integer(GL_PACK_ALIGNMENT, &previous_pack);
    bind_framebuffer(GL_FRAMEBUFFER, 0);
    pixel_store(GL_PACK_ALIGNMENT, 1);
    read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    pixel_store(GL_PACK_ALIGNMENT, previous_pack);
    bind_framebuffer(GL_FRAMEBUFFER, (GLuint)previous_fb);
    return true;
}

/*
 * <control>/frame.bin, for a host that draws the game itself (the Android
 * wrapper). Little-endian header, then width*height RGBA, bottom row first:
 *   0  "NVF1"   4 width   8 height   12 seq_begin   16 seq_end   32 pixels
 * seq_begin is stored before the pixels and seq_end after, so a reader that
 * sees both equal around its copy has a whole frame.
 */
static const size_t kFrameHeader = 32;
static unsigned char *g_frame_map = NULL;
static size_t g_frame_map_size = 0;
static uint32_t g_frame_seq = 0;

static void export_frame(int width, int height, const unsigned char *pixels)
{
    size_t need = kFrameHeader + (size_t)width * (size_t)height * 4;
    if (!g_frame_map || g_frame_map_size != need) {
        if (g_frame_map)
            munmap(g_frame_map, g_frame_map_size);
        g_frame_map = NULL;
        std::string path = g_control_dir + "/frame.bin";
        int fd = open(path.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd < 0 || ftruncate(fd, (off_t)need) != 0) {
            trace("frame export disabled: %s: %s", path.c_str(), strerror(errno));
            if (fd >= 0)
                close(fd);
            g_frame_map_size = 0;
            return;
        }
        void *map = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (map == MAP_FAILED) {
            trace("frame export disabled: mmap: %s", strerror(errno));
            g_frame_map_size = 0;
            return;
        }
        g_frame_map = (unsigned char *)map;
        g_frame_map_size = need;
        memcpy(g_frame_map, "NVF1", 4);
        ((uint32_t *)g_frame_map)[1] = (uint32_t)width;
        ((uint32_t *)g_frame_map)[2] = (uint32_t)height;
        trace("frame export: %dx%d -> %s", width, height, path.c_str());
    }

    volatile uint32_t *hdr = (volatile uint32_t *)g_frame_map;
    uint32_t seq = ++g_frame_seq;
    hdr[3] = seq;
    __sync_synchronize();
    memcpy(g_frame_map + kFrameHeader, pixels, need - kFrameHeader);
    __sync_synchronize();
    hdr[4] = seq;
}

void emulator_control_after_draw(long frame, int width, int height)
{
    if (!g_enabled || width <= 0 || height <= 0)
        return;

    static const bool export_on = port_getenv_long("FRAME_EXPORT", 0) != 0;
    if (!export_on && g_screenshot_token.empty())
        return;

    size_t size = (size_t)width * (size_t)height * 4;
    std::vector<unsigned char> pixels(size);
    if (!capture_framebuffer(width, height, pixels.data())) {
        trace("emulator screenshot failed: GLES1 functions unresolved");
        g_screenshot_token.clear();
        return;
    }

    if (export_on)
        export_frame(width, height, pixels.data());
    if (g_screenshot_token.empty())
        return;

    std::string relative = "screenshots/" + g_screenshot_token + ".png";
    std::string final = g_control_dir + "/" + relative;
    std::string temp = final + ".tmp";
    bool ok = write_png(temp.c_str(), width, height, pixels.data()) &&
              rename(temp.c_str(), final.c_str()) == 0;
    if (ok) {
        trace("emulator: screenshot %s at frame %ld",
              relative.c_str(), frame);
        write_status(frame, "running", relative.c_str());
    } else {
        trace("emulator screenshot failed: %s", strerror(errno));
        unlink(temp.c_str());
    }
    g_screenshot_token.clear();
}

void emulator_control_shutdown(long frame)
{
    if (!g_enabled)
        return;
    write_status(frame, "stopped");
    if (g_commands)
        fclose(g_commands);
    g_commands = NULL;
    g_enabled = false;
}
