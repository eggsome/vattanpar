#include "sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h>

/* Minimal ALSA playback interface resolved with dlopen at runtime, so the
 * game builds without libasound2-dev. The enum values are stable ALSA ABI.
 * On PipeWire/PulseAudio desktops the "default" PCM routes through them. */
typedef struct snd_pcm snd_pcm_t;
#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_FORMAT_S16_LE 2
#define SND_PCM_ACCESS_RW_INTERLEAVED 3

static int  (*p_open)(snd_pcm_t **, const char *, int, int);
static int  (*p_set_params)(snd_pcm_t *, int, int, unsigned, unsigned,
                            int, unsigned);
static long (*p_writei)(snd_pcm_t *, const void *, unsigned long);
static int  (*p_recover)(snd_pcm_t *, int, int);
static int  (*p_close)(snd_pcm_t *);

#define RATE 48000
#define MAX_VOICES 8
#define PERIOD 256

static int16_t *clips[SND_COUNT];
static size_t clip_len[SND_COUNT];
static struct { int clip; size_t pos; bool active; } voices[MAX_VOICES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static snd_pcm_t *pcm;
static bool running;
static bool ready;

static uint32_t rd32(const uint8_t *b)
{
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

static bool load_wav(const char *path, int idx)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "sound: cannot open %s\n", path);
        return false;
    }
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
        goto bad;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8)
            goto bad;
        uint32_t sz = rd32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[16];
            if (sz < 16 || fread(fmt, 1, 16, f) != 16)
                goto bad;
            unsigned audio = fmt[0] | fmt[1] << 8;
            unsigned channels = fmt[2] | fmt[3] << 8;
            uint32_t rate = rd32(fmt + 4);
            unsigned bits = fmt[14] | fmt[15] << 8;
            if (audio != 1 || channels != 1 || rate != RATE || bits != 16) {
                fprintf(stderr, "sound: %s must be PCM 16-bit mono %d Hz\n",
                        path, RATE);
                fclose(f);
                return false;
            }
            if (sz > 16)
                fseek(f, sz - 16 + (sz & 1), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            clips[idx] = malloc(sz);
            if (!clips[idx] || fread(clips[idx], 1, sz, f) != sz)
                goto bad;
            clip_len[idx] = sz / 2;
            fclose(f);
            return true;
        } else {
            fseek(f, sz + (sz & 1), SEEK_CUR);
        }
    }
bad:
    fprintf(stderr, "sound: bad wav file %s\n", path);
    fclose(f);
    return false;
}

static void *audio_main(void *arg)
{
    (void)arg;
    int16_t buf[PERIOD];
    while (running) {
        memset(buf, 0, sizeof buf);
        pthread_mutex_lock(&lock);
        for (int v = 0; v < MAX_VOICES; v++) {
            if (!voices[v].active) continue;
            const int16_t *d = clips[voices[v].clip];
            size_t len = clip_len[voices[v].clip];
            for (int i = 0; i < PERIOD && voices[v].pos < len;
                 i++, voices[v].pos++) {
                int s = buf[i] + d[voices[v].pos];
                buf[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : (int16_t)s;
            }
            if (voices[v].pos >= len)
                voices[v].active = false;
        }
        pthread_mutex_unlock(&lock);
        long r = p_writei(pcm, buf, PERIOD); /* blocks; paces this thread */
        if (r < 0)
            p_recover(pcm, (int)r, 1);
    }
    return NULL;
}

bool sound_init(void)
{
    void *lib = dlopen("libasound.so.2", RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "sound: libasound.so.2 not found, audio disabled\n");
        return false;
    }
    p_open = dlsym(lib, "snd_pcm_open");
    p_set_params = dlsym(lib, "snd_pcm_set_params");
    p_writei = dlsym(lib, "snd_pcm_writei");
    p_recover = dlsym(lib, "snd_pcm_recover");
    p_close = dlsym(lib, "snd_pcm_close");
    if (!p_open || !p_set_params || !p_writei || !p_recover || !p_close) {
        fprintf(stderr, "sound: incomplete libasound, audio disabled\n");
        return false;
    }
    if (!load_wav("assets/jump.wav", SND_JUMP) ||
        !load_wav("assets/fall.wav", SND_FALL))
        return false;
    if (p_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "sound: cannot open PCM, audio disabled\n");
        return false;
    }
    if (p_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                     1, RATE, 1, 50000) < 0) {
        fprintf(stderr, "sound: cannot set PCM params, audio disabled\n");
        p_close(pcm);
        return false;
    }
    running = true;
    if (pthread_create(&thread, NULL, audio_main, NULL) != 0) {
        p_close(pcm);
        return false;
    }
    ready = true;
    return true;
}

void sound_play(int clip)
{
    if (!ready)
        return;
    pthread_mutex_lock(&lock);
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!voices[v].active) {
            voices[v].clip = clip;
            voices[v].pos = 0;
            voices[v].active = true;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
}

void sound_shutdown(void)
{
    if (!ready)
        return;
    running = false;
    pthread_join(thread, NULL);
    p_close(pcm);
    ready = false;
}
