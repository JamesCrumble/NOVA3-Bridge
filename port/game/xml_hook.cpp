/*
 * Every XML document the engine parses, seen - and optionally edited - first.
 *
 * The quality profiles (GPUs.xml, GPU_n.xml, CPU_*.xml, MEM_*.xml and the
 * per-GPU custom files) live inside the encrypted .gla archives, so the only
 * place their text exists in the clear is the buffer handed to TinyXML. The
 * engine picks them by GL_RENDERER and gives a GPU it does not know - every
 * phone made after 2013 - the heaviest one, which under emulation is the
 * difference between playable and not.
 *
 *   <PREFIX>_XML_DUMP=dir        each parsed document is written to dir/NNN.xml
 *   <PREFIX>_XML_OVERRIDES=file  lines "old=>new": plain text replacements made
 *                                in every document before it is parsed
 *
 * TiXmlDocument::Parse takes only pointers and an enum, so no float ABI
 * concerns apply to the call through the hook.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <utility>
#include <vector>

#include "so_util.h"
#include "trace.h"
#include "port_env.h"

#include "nova3.h"

typedef const char *(*parse_fn)(void *self, const char *p, void *data, int encoding);

static ReentrantHook   g_hook;
static parse_fn        g_parse;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string     g_dump_dir;
static int             g_dumped;
static std::vector<std::pair<std::string, std::string>> g_overrides;

static void load_overrides(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        trace("xml: no overrides at %s", path);
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        char *arrow = strstr(line, "=>");
        if (!arrow || line[0] == '#')
            continue;
        *arrow = 0;
        g_overrides.push_back(std::make_pair(std::string(line), std::string(arrow + 2)));
        trace("xml: override '%s' => '%s'", line, arrow + 2);
    }
    fclose(f);
}

static void dump(const std::string &text, bool changed)
{
    if (g_dump_dir.empty() || g_dumped >= 2000)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%04d%s.xml", g_dump_dir.c_str(), g_dumped++, changed ? "_edited" : "");
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

/*
 * Applies the overrides to one document and dumps it. Returns a replacement
 * buffer (NUL-terminated, *len updated) or NULL when nothing changed. The
 * replacement is never freed: a parser may keep pointers into what it parsed,
 * and these documents are a few kB each, parsed once.
 */
static char *edit(const char *p, size_t *len)
{
    if (!p || (g_overrides.empty() && g_dump_dir.empty()))
        return NULL;
    std::string text(p, *len);
    bool changed = false;
    for (size_t i = 0; i < g_overrides.size(); i++) {
        const std::string &from = g_overrides[i].first, &to = g_overrides[i].second;
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
            changed = true;
        }
    }
    dump(text, changed);
    if (!changed)
        return NULL;
    char *copy = (char *)malloc(text.size() + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text.data(), text.size());
    copy[text.size()] = 0;
    *len = text.size();
    return copy;
}

static const char *parse_hook(void *self, const char *p, void *data, int encoding)
{
    pthread_mutex_lock(&g_lock);
    size_t len = p ? strlen(p) : 0;
    char *copy = edit(p, &len);
    rehook_unhook(&g_hook);
    const char *r = g_parse(self, copy ? copy : p, data, encoding);
    rehook_hook(&g_hook);
    pthread_mutex_unlock(&g_lock);
    /* The caller only tests the result for null. */
    return copy ? (r ? p : NULL) : r;
}

/* slim::XmlDocument::parse(const char *buffer, unsigned size) - SlimXml, the
 * parser the device profiles go through. */
typedef int (*slim_parse_fn)(void *self, const char *buf, unsigned size);

static ReentrantHook g_slim_hook;
static slim_parse_fn g_slim_parse;

static int slim_parse_hook(void *self, const char *buf, unsigned size)
{
    pthread_mutex_lock(&g_lock);
    size_t len = size;
    char *copy = edit(buf, &len);
    rehook_unhook(&g_slim_hook);
    int r = g_slim_parse(self, copy ? copy : buf, copy ? (unsigned)len : size);
    rehook_hook(&g_slim_hook);
    pthread_mutex_unlock(&g_lock);
    return r;
}

/*
 * pugi::xml_document::load_buffer_impl(void *contents, size_t size, unsigned options,
 *                                      xml_encoding, bool is_mutable, bool own)
 * pugixml, the parser DeviceOptions reads the quality profiles (GPU_n.xml, CPU_*.xml, the per-GPU
 * custom files) with. Every load_* entry point ends here. An edited document is handed over as
 * mutable and not owned: pugixml frees owned buffers with its own allocator, and the copy is ours.
 */
/*
 * It returns pugi::xml_parse_result, a 12-byte struct, so under the AAPCS the caller passes the
 * result's address in r0 and `this` moves to r1: the first parameter here is that hidden pointer.
 */
typedef void *(*pugi_load_fn)(void *result, void *self, void *contents, unsigned size,
                              unsigned options, int encoding, int is_mutable, int own);

static ReentrantHook g_pugi_hook;
static pugi_load_fn  g_pugi_load;

static void *pugi_load_hook(void *result, void *self, void *contents, unsigned size,
                            unsigned options, int encoding, int is_mutable, int own)
{
    pthread_mutex_lock(&g_lock);
    size_t len = size;
    char *copy = edit((const char *)contents, &len);
    rehook_unhook(&g_pugi_hook);
    void *r = copy ? g_pugi_load(result, self, copy, (unsigned)len, options, encoding, 1, 0)
                   : g_pugi_load(result, self, contents, size, options, encoding, is_mutable, own);
    rehook_hook(&g_pugi_hook);
    pthread_mutex_unlock(&g_lock);
    /* An owned original that was replaced is simply left alive, like the copy. */
    return r;
}

/* A function of the 1.0.7 library by its .symtab offset, if it starts with a push. */
static uintptr_t local_function(so_module *mod, const char *name, uintptr_t offset)
{
    uintptr_t addr = so_symbol(mod, name);
    if (addr)
        return addr;
    addr = mod->text_base + offset;
    /* push in the first two instructions (a pc-relative load may come first; the
     * hook restores the original words in place for each call). */
    const uint32_t *w = (const uint32_t *)addr;
    if ((w[0] & 0xFFFF0000u) != 0xE92D0000u && (w[1] & 0xFFFF0000u) != 0xE92D0000u) {
        trace("xml: no push at %s (+0x%lx: %08x), not this build - no hook", name,
              (unsigned long)offset, *(const uint32_t *)addr);
        return 0;
    }
    return addr;
}

void nova3_xml_hook_install(so_module *mod)
{
    const char *dir = port_getenv("XML_DUMP");
    const char *ovr = port_getenv("XML_OVERRIDES");
    if ((!dir || !*dir) && (!ovr || !*ovr))
        return;

    /* Local symbols: present in the 1.0.7 library's .symtab, not in its
     * exports, so so_symbol() cannot see them. The offsets are that build's
     * (sha1 ae97a442...); the prologue check keeps another build from being
     * patched in the middle of something else. */
    uintptr_t addr = local_function(mod, "_ZN13TiXmlDocument5ParseEPKcP16TiXmlParsingData13TiXmlEncoding", 0xa5b54c);
    uintptr_t slim = local_function(mod, "_ZN4slim11XmlDocument5parseEPKcj", 0x935820);
    uintptr_t pugi = local_function(mod, "_ZN4pugi12xml_document16load_buffer_implEPvjjNS_12xml_encodingEbb", 0x5ca154);
    if (dir && *dir) {
        g_dump_dir = dir;
        mkdir(dir, 0775);
        trace("xml: dumping parsed documents to %s", dir);
    }
    if (ovr && *ovr)
        load_overrides(ovr);
    if (addr) {
        g_parse = (parse_fn)addr;
        rehook_new(mod, &g_hook, addr, (uintptr_t)parse_hook);
        rehook_hook(&g_hook);
    }
    if (slim) {
        g_slim_parse = (slim_parse_fn)slim;
        rehook_new(mod, &g_slim_hook, slim, (uintptr_t)slim_parse_hook);
        rehook_hook(&g_slim_hook);
    }
    if (pugi) {
        g_pugi_load = (pugi_load_fn)pugi;
        rehook_new(mod, &g_pugi_hook, pugi, (uintptr_t)pugi_load_hook);
        rehook_hook(&g_pugi_hook);
    }
    trace("xml: hooks tinyxml=%d slimxml=%d pugixml=%d", addr != 0, slim != 0, pugi != 0);
}
