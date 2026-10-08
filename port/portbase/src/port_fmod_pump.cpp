/*
 * The audio pump: the Java thread libfmodex.so is waiting for.
 *
 * Real Racing 3 plays everything through FMOD Ex, and FMOD Ex on Android does
 * not own its own playback thread. Its AudioTrack output only mixes when
 * somebody calls it, and on a phone that somebody is Java:
 * org.fmod.FMODAudioDevice runs a thread that asks the native side for the
 * format, hands it a direct ByteBuffer to fill, and writes the result to an
 * android.media.AudioTrack. The game's own AudioStreamManager owns that object.
 *
 * A loader with no JVM never runs any of it. FMOD came up, the engine loaded
 * its banks, the mixer was ready - and nothing ever asked it for a single
 * sample. The port reached the driving tutorial in complete silence with no
 * error anywhere in the log, because from FMOD's point of view nothing had
 * gone wrong. It was still waiting to be asked.
 *
 * So this file is that thread, in C++. libfmodex.so exports exactly the two
 * entry points the Java class calls, and nothing else about it is Java-shaped:
 *
 *   jint fmodGetInfo(JNIEnv*, jobject, jint what)
 *   jint fmodProcess(JNIEnv*, jobject, jobject directByteBuffer)
 *
 * Both were read out of the donor's disassembly rather than guessed, because
 * the wrong buffer size here is not a build error - it is noise, or a stutter
 * that reads like a performance problem. What the disassembly settled:
 *
 *   - fmodGetInfo ignores env and thiz entirely; only the third argument is
 *     used. It answers a five-entry jump table and returns -1 for anything
 *     else, and for every code while the output is not yet initialised:
 *
 *         0  sample rate
 *         1  DSP buffer length, in FRAMES
 *         2  number of buffers
 *         3  the constant 1
 *         4  channel count
 *
 *   - fmodProcess calls exactly one JNIEnv function, GetDirectBufferAddress,
 *     and writes bufferLength * channels samples of interleaved PCM16 there.
 *     It never asks the buffer how big it is, so the buffer we hand over must
 *     be at least that large - FMOD will write that much regardless.
 *
 * The -1 is what makes the start-up order a non-problem: the pump can be
 * started before the game has created its FMOD system, and simply waits until
 * the answers become real. That is also this file's diagnostic value. If the
 * config never arrives, FMOD did not choose the AudioTrack output at all, and
 * the log says so instead of leaving another silent run to interpret.
 */
#include <atomic>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include <SDL2/SDL.h>

#include "jni.h"
#include "platform.h"
#include "so_util.h"
#include "trace.h"
#include "port_env.h"

#include "port_fmod_pump.h"

namespace {

/* FMOD's own codes for fmodGetInfo, named after what the disassembly showed
 * each branch reading out of the system object. */
enum {
    INFO_SAMPLE_RATE   = 0,
    INFO_BUFFER_FRAMES = 1,
    INFO_BUFFER_COUNT  = 2,
    INFO_CHANNELS      = 4,
};

typedef jint (ABI_ATTR *GetInfoFn)(JNIEnv *, jobject, jint);
typedef jint (ABI_ATTR *ProcessFn)(JNIEnv *, jobject, jobject);

GetInfoFn g_get_info = NULL;
ProcessFn g_process  = NULL;

JNIEnv           *g_env    = NULL;
pthread_t         g_thread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_started{false};

/*
 * Signal metrics, so a run that produced no sound can be told apart from a run
 * that produced sound nobody could hear.
 *
 * On the console those are different problems with different fixes - a mixer
 * that was never asked versus an ALSA endpoint that went nowhere - and they
 * look identical from the outside. They also cannot be told apart in the
 * emulator at all, which has no speaker: there, this is the only evidence the
 * pump works.
 *
 * Reported on the first few blocks and then at powers of two, which keeps a
 * long session bounded while still saying something early.
 */
void report_pcm(const int16_t *samples, int count)
{
    static unsigned long block   = 0;
    static bool          audible = false;

    unsigned int nonzero = 0;
    unsigned int peak    = 0;

    for (int i = 0; i < count; i++) {
        int sample = samples[i];
        unsigned int magnitude = sample < 0 ? (unsigned int)(-(int64_t)sample)
                                            : (unsigned int)sample;
        if (magnitude)
            nonzero++;
        if (magnitude > peak)
            peak = magnitude;
    }

    block++;
    bool first_sound = nonzero && !audible;
    audible = audible || nonzero;

    if (block <= 4 || first_sound || (block & (block - 1)) == 0) {
        trace("fmod pump: block=%lu samples=%d nonzero=%u peak=%u%s",
              block, count, nonzero, peak,
              first_sound ? "  <- first audible block" : "");
    }
}

/*
 * Opening the output.
 *
 * The plain default is right almost always. The retry exists because some
 * PortMaster frontends still hold the speaker PCM for a moment after launching
 * a port, and an unused HDMI endpoint answers instantly - so taking the first
 * device that opens can hand the whole soundtrack to a cable nobody has
 * plugged in.
 */
SDL_AudioDeviceID open_output(SDL_AudioSpec *want, SDL_AudioSpec *have)
{
    const char *forced = port_getenv("AUDIODEV");
    if (forced && *forced) {
        SDL_AudioDeviceID device = SDL_OpenAudioDevice(forced, 0, want, have, 0);
        if (!device)
            warning("fmod pump: " PORT_ENV_PREFIX "_AUDIODEV=%s failed: %s\n",
                    forced, SDL_GetError());
        return device;
    }

    SDL_AudioDeviceID device = SDL_OpenAudioDevice(NULL, 0, want, have, 0);
    if (device)
        return device;

    char first_error[256];
    snprintf(first_error, sizeof(first_error), "%s", SDL_GetError());

    int count = SDL_GetNumAudioDevices(0);
    for (int pass = 0; pass < 2 && !device; pass++) {
        for (int i = 0; i < count && !device; i++) {
            const char *name = SDL_GetAudioDeviceName(i, 0);
            if (!name || !*name)
                continue;
            if (pass == 0 && strcasestr(name, "hdmi"))
                continue;

            int attempts = pass == 0 ? 5 : 1;
            for (int attempt = 0; attempt < attempts; attempt++) {
                device = SDL_OpenAudioDevice(name, 0, want, have, 0);
                if (device)
                    break;
                const char *err = SDL_GetError();
                if (!err || !strcasestr(err, "busy") || attempt + 1 >= attempts)
                    break;
                SDL_Delay(400);
            }
            if (device)
                trace("fmod pump: default output failed (%s); opened \"%s\" on "
                      "pass %d", first_error, name, pass + 1);
        }
    }

    if (!device)
        warning("fmod pump: no output accepted %d Hz / %d ch: %s "
                "(%d device(s) enumerated) - the mixer will still run, but "
                "nothing can be heard.\n",
                want->freq, (int)want->channels, first_error, count);
    return device;
}

/* Wait for the game to bring FMOD up. Returns false if it never does. */
bool await_output(int *rate, int *frames, int *channels)
{
    /* Generous, because this waits out both loading screens on a cold SD card.
     * It is a giving-up point, not a timing assumption. */
    const int kTimeoutMs = 120000;
    const int kStepMs    = 50;

    for (int waited = 0; waited < kTimeoutMs; waited += kStepMs) {
        jint r = g_get_info(g_env, NULL, INFO_SAMPLE_RATE);
        if (r > 0) {
            *rate     = (int)r;
            *frames   = (int)g_get_info(g_env, NULL, INFO_BUFFER_FRAMES);
            *channels = (int)g_get_info(g_env, NULL, INFO_CHANNELS);
            trace("fmod pump: FMOD is up after %d ms - %d Hz, %d ch, %d frames "
                  "per block, %d buffers",
                  waited, *rate, *channels, *frames,
                  (int)g_get_info(g_env, NULL, INFO_BUFFER_COUNT));
            return *frames > 0 && *channels > 0;
        }

        if (waited && waited % 10000 == 0)
            trace("fmod pump: still waiting for FMOD's output (%d s)",
                  waited / 1000);

        SDL_Delay(kStepMs);
    }

    /*
     * This is a finding, not a timeout to retry. fmodGetInfo answers -1 only
     * while the AudioTrack output object is null, so two minutes of -1 means
     * FMOD initialised some other output - or none - and this pump is the
     * wrong instrument for whatever it chose. Say that plainly: the last time
     * this port went quiet, the absence of any message at all is what made it
     * take a day to find.
     */
    warning("fmod pump: FMOD never initialised its AudioTrack output "
            "(fmodGetInfo still answers -1 after 120 s). The game is not "
            "waiting for this pump - it selected a different output, and the "
            "silence has another cause.\n");
    return false;
}

void *pump_main(void *)
{
    int rate = 0, frames = 0, channels = 0;
    if (!await_output(&rate, &frames, &channels))
        return NULL;

    /* FMOD writes frames*channels samples per call and never asks how much
     * room it has, so this buffer is sized from its answer, not ours. */
    const int    samples_per_block = frames * channels;
    const size_t bytes_per_block   = (size_t)samples_per_block * sizeof(int16_t);

    int16_t *pcm = (int16_t *)calloc(1, bytes_per_block);
    if (!pcm) {
        warning("fmod pump: out of memory for a %zu byte mix buffer\n",
                bytes_per_block);
        return NULL;
    }

    /*
     * One ByteBuffer for the whole run. NewDirectByteBuffer allocates an object
     * per call and this loader has no garbage collector, so making one per
     * block would leak at the mixer's rate - a few hundred objects a second.
     */
    jobject buffer = g_env->NewDirectByteBuffer(pcm, (jlong)bytes_per_block);
    if (!buffer) {
        warning("fmod pump: could not wrap the mix buffer for FMOD\n");
        free(pcm);
        return NULL;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    SDL_zero(have);
    want.freq     = rate;
    want.format   = AUDIO_S16SYS;
    want.channels = (Uint8)channels;
    want.samples  = (Uint16)frames;
    want.callback = NULL;              /* queue-driven, like the rest of the port */

    SDL_AudioDeviceID device = open_output(&want, &have);
    if (device) {
        trace("fmod pump: output open - %d Hz, %d ch, %d frame period",
              have.freq, (int)have.channels, (int)have.samples);
        SDL_PauseAudioDevice(device, 0);
    }

    const unsigned int bytes_per_frame =
        device ? (SDL_AUDIO_BITSIZE(have.format) / 8) * have.channels
               : (unsigned int)(sizeof(int16_t) * channels);
    const unsigned int high_water = have.samples ? have.samples * bytes_per_frame * 3
                                                 : 0;
    /* Real-time pacing for the no-device case, so the mixer still advances at
     * the rate the game expects. FMOD's AudioTrack output is a state machine
     * driven by these calls; letting it free-run or stall is worse than
     * silence, because game logic waits on sound events. */
    const Uint32 block_ms = (Uint32)((frames * 1000.0) / (rate ? rate : 44100));

    trace("fmod pump: running%s", device ? "" : " with no output device");

    while (g_running.load()) {
        if (g_process(g_env, NULL, buffer) != 0)
            break;

        report_pcm(pcm, samples_per_block);

        if (!device) {
            SDL_Delay(block_ms ? block_ms : 1);
            continue;
        }

        if (SDL_QueueAudio(device, pcm, (Uint32)bytes_per_block) != 0)
            warning("fmod pump: SDL_QueueAudio failed: %s\n", SDL_GetError());

        /* Keep a few periods queued rather than draining to zero: waiting for
         * empty between blocks leaves an audible gap at every block boundary. */
        while (g_running.load() && high_water &&
               SDL_GetQueuedAudioSize(device) > high_water)
            SDL_Delay(1);
    }

    trace("fmod pump: stopped");

    if (device)
        SDL_CloseAudioDevice(device);
    free(pcm);
    return NULL;
}

} // namespace

extern "C" int port_fmod_prefers_audiotrack(void)
{
    const char *choice = port_getenv("FMOD_OUTPUT");
    return choice && strcasecmp(choice, "audiotrack") == 0;
}

void port_fmod_pump_start(JNIEnv *env)
{
    if (g_started.exchange(true))
        return;

    /* Not an opt-out but a consequence: on the OpenSL path FMOD never creates
     * the AudioTrack output, so there is nothing here to drive and waiting for
     * it would only fill the log with a problem that is not one. */
    if (!port_fmod_prefers_audiotrack()) {
        trace("fmod pump: not needed - FMOD is on its OpenSL output "
              "(set " PORT_ENV_PREFIX "_FMOD_OUTPUT=audiotrack to use this pump instead)");
        return;
    }

    /* so_symbol(NULL, ...) searches every loaded module, which is what this
     * needs: libfmodex.so arrives on its own as a DT_NEEDED of the game and
     * nothing here ever holds a handle to it. */
    g_get_info = (GetInfoFn)so_symbol(NULL,
        "Java_org_fmod_FMODAudioDevice_fmodGetInfo");
    g_process  = (ProcessFn)so_symbol(NULL,
        "Java_org_fmod_FMODAudioDevice_fmodProcess");

    if (!g_get_info || !g_process) {
        warning("fmod pump: libfmodex.so does not export the FMODAudioDevice "
                "entry points (getInfo=%p process=%p); this build drives its "
                "audio some other way.\n",
                (void *)g_get_info, (void *)g_process);
        return;
    }

    /*
     * The main thread's JNIEnv is reused deliberately. fmodProcess touches the
     * environment for exactly one call, GetDirectBufferAddress, which this
     * loader answers out of the object with no per-thread state - so attaching
     * a second environment would buy nothing and add a lifetime to manage.
     */
    g_env = env;
    g_running.store(true);

    if (pthread_create(&g_thread, NULL, pump_main, NULL) != 0) {
        warning("fmod pump: could not start the mixer thread\n");
        g_running.store(false);
        return;
    }

    trace("fmod pump: started, waiting for FMOD to initialise its output");
}

void port_fmod_pump_stop(void)
{
    if (!g_running.exchange(false))
        return;
    pthread_join(g_thread, NULL);
}
