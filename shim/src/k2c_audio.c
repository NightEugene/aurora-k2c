// AAudio output callback bridge to the host PulseAudio simple API.
#include "k2c_audio.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { OK=0, INVALID=-898, STATE_ERROR=-895, UNAVAILABLE=-889, TIMEOUT=-885 };
enum { OPEN=2, STARTED=4, STOPPED=10, CLOSED=12 };
typedef int32_t (*DataCallback)(void *, void *, void *, int32_t);
typedef void (*ErrorCallback)(void *, void *, int32_t);
typedef struct {
    int32_t rate, channels, format, frames, capacity, direction, device, session;
    DataCallback callback;
    void *user;
    ErrorCallback error_callback;
    void *error_user;
} Builder;
typedef struct {
    Builder config;
    void *pulse;
    pthread_t thread;
    int joinable;
    atomic_int running, state;
    int32_t buffer_size;
    atomic_llong frames_written;
} Stream;

// Stable public libpulse ABI. Load the glibc library, never the Android linker.
typedef struct { int format; uint32_t rate; uint8_t channels; } SampleSpec;
typedef struct { uint32_t maxlength, tlength, prebuf, minreq, fragsize; } BufferAttr;
static void *(*pulse_new)(const char *, const char *, int, const char *, const char *,
                         const SampleSpec *, const void *, const BufferAttr *, int *);
static int (*pulse_write)(void *, const void *, size_t, int *);
static void (*pulse_free)(void *);
static const char *(*pulse_error)(int);
static pthread_once_t pulse_once = PTHREAD_ONCE_INIT;
static void load_pulse(void)
{
    void *lib = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
    void *base = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
    if (lib) {
        pulse_new = dlsym(lib, "pa_simple_new");
        pulse_write = dlsym(lib, "pa_simple_write");
        pulse_free = dlsym(lib, "pa_simple_free");
    }
    if (base) pulse_error = dlsym(base, "pa_strerror");
}
static const char *error_text(int error)
{ return pulse_error ? pulse_error(error) : "PulseAudio unavailable"; }

static int32_t create_builder(Builder **out)
{
    if (!out) return INVALID;
    *out = calloc(1, sizeof **out);
    if (!*out) return UNAVAILABLE;
    **out = (Builder){ .rate=48000, .channels=2, .format=2, .frames=256, .capacity=4096 };
    fprintf(stderr, "[audio] AAudio builder created\n");
    return OK;
}
static int32_t delete_builder(Builder *b) { free(b); return OK; }
#define SETTER(name, field) static void name(Builder *b, int32_t value) { if (b && value) b->field=value; }
SETTER(set_rate, rate)
SETTER(set_channels, channels)
SETTER(set_format, format)
SETTER(set_frames, frames)
SETTER(set_capacity, capacity)
SETTER(set_device, device)
SETTER(set_session, session)
static void set_direction(Builder *b, int32_t value) { if (b) b->direction=value; }
static void set_option(Builder *b, int32_t value) { (void)b; (void)value; }
static void set_callback(Builder *b, DataCallback cb, void *user)
{ if (b) { b->callback=cb; b->user=user; } }
static void set_error_callback(Builder *b, ErrorCallback cb, void *user)
{ if (b) { b->error_callback=cb; b->error_user=user; } }

static int32_t open_stream(Builder *b, Stream **out)
{
    if (!b || !out) return INVALID;
    *out=NULL;
    if (b->direction || !b->callback || b->rate<8000 || b->rate>192000 ||
        b->channels<1 || b->channels>8 || (b->format!=1 && b->format!=2) ||
        b->frames<1 || b->frames>8192 || b->capacity<b->frames) return INVALID;
    pthread_once(&pulse_once, load_pulse);
    if (!pulse_new || !pulse_write || !pulse_free) return UNAVAILABLE;
    Stream *s=calloc(1, sizeof *s);
    if (!s) return UNAVAILABLE;
    s->config=*b;
    s->buffer_size=b->frames*2;
    if (s->buffer_size>b->capacity) s->buffer_size=b->capacity;
    atomic_init(&s->running, 0); atomic_init(&s->state, OPEN);
    atomic_init(&s->frames_written, 0);
    uint32_t bytes=(uint32_t)b->frames*b->channels*(b->format==2 ? 4 : 2);
    SampleSpec spec={b->format==2 ? 5 : 3, (uint32_t)b->rate, (uint8_t)b->channels};
    BufferAttr attr={UINT32_MAX, bytes*2, 0, bytes, UINT32_MAX};
    int error=0;
    s->pulse=pulse_new(NULL, "Kingdom Two Crowns", 1, NULL, "Game audio", &spec, NULL, &attr, &error);
    if (!s->pulse) {
        fprintf(stderr, "[audio] open failed: %s\n", error_text(error));
        free(s); return UNAVAILABLE;
    }
    *out=s;
    fprintf(stderr, "[audio] opened %d Hz, %d channels, format=%d, callback=%d frames\n",
            b->rate,b->channels,b->format,b->frames);
    return OK;
}

static void *audio_thread(void *arg)
{
    Stream *s=arg;
    Builder *b=&s->config;
    size_t samples=(size_t)b->frames*b->channels;
    size_t bytes=samples*(b->format==2 ? 4 : 2);
    void *data=calloc(1,bytes);
    if (!data) { atomic_store(&s->running,0); atomic_store(&s->state,STOPPED); return NULL; }
    float peak=0;
    long long next_log=b->rate;
    while (atomic_load(&s->running)) {
        memset(data,0,bytes);
        if (b->callback(s,b->user,data,b->frames)!=0) break;
        for (size_t i=0;i<samples;i++) {
            float value=b->format==2 ? ((float *)data)[i] : ((int16_t *)data)[i]/32768.f;
            if (value<0) value=-value;
            if (value>peak) peak=value;
        }
        int error=0;
        if (pulse_write(s->pulse,data,bytes,&error)<0) {
            fprintf(stderr,"[audio] write failed: %s\n",error_text(error));
            if (b->error_callback) b->error_callback(s,b->error_user,UNAVAILABLE);
            break;
        }
        long long frames=atomic_fetch_add(&s->frames_written,b->frames)+b->frames;
        if (frames>=next_log) {
            fprintf(stderr,"[audio] played frames=%lld peak=%.4f\n",frames,peak);
            next_log=frames+(long long)b->rate*10; peak=0;
        }
    }
    free(data);
    atomic_store(&s->running,0); atomic_store(&s->state,STOPPED);
    return NULL;
}
static int32_t stop_stream(Stream *s)
{
    if (!s) return INVALID;
    if (s->joinable && pthread_equal(pthread_self(),s->thread)) return STATE_ERROR;
    atomic_store(&s->running,0);
    if (s->joinable) { pthread_join(s->thread,NULL); s->joinable=0; }
    atomic_store(&s->state,STOPPED);
    return OK;
}
static int32_t start_stream(Stream *s)
{
    if (!s) return INVALID;
    if (atomic_load(&s->running)) return OK;
    if (s->joinable) { pthread_join(s->thread,NULL); s->joinable=0; }
    atomic_store(&s->running,1); atomic_store(&s->state,STARTED);
    if (pthread_create(&s->thread,NULL,audio_thread,s)) {
        atomic_store(&s->running,0); atomic_store(&s->state,STOPPED); return UNAVAILABLE;
    }
    s->joinable=1; return OK;
}
static int32_t close_stream(Stream *s)
{
    if (!s) return INVALID;
    int32_t result=stop_stream(s);
    if (result) return result;
    atomic_store(&s->state,CLOSED);
    pulse_free(s->pulse); free(s); return OK;
}
static int32_t wait_state(Stream *s, int32_t input, int32_t *next, int64_t timeout)
{
    if (!s || !next) return INVALID;
    struct timespec start,now,delay={0,1000000};
    clock_gettime(CLOCK_MONOTONIC,&start);
    while ((*next=atomic_load(&s->state))==input) {
        clock_gettime(CLOCK_MONOTONIC,&now);
        int64_t elapsed=(now.tv_sec-start.tv_sec)*1000000000LL+now.tv_nsec-start.tv_nsec;
        if (timeout<=0 || elapsed>=timeout) return TIMEOUT;
        nanosleep(&delay,NULL);
    }
    return OK;
}
#define GETTER(name, value) static int32_t name(Stream *s) { return s ? (value) : INVALID; }
GETTER(get_capacity,s->config.capacity)
GETTER(get_buffer_size,s->buffer_size)
GETTER(get_burst,s->config.frames)
GETTER(get_device,s->config.device)
GETTER(get_session,s->config.session)
GETTER(get_rate,s->config.rate)
GETTER(get_channels,s->config.channels)
GETTER(get_format,s->config.format)
GETTER(get_state,atomic_load(&s->state))
GETTER(get_xruns,0)
static int is_mmap(Stream *s) { (void)s; return 0; }
static int32_t set_buffer_size(Stream *s,int32_t size)
{
    if (!s || size<=0) return INVALID;
    s->buffer_size=size>s->config.capacity ? s->config.capacity : size;
    return s->buffer_size;
}
void *k2c_audio_hook(const char *symbol)
{
    static const struct { const char *name; void *fn; } hooks[]={
        {"AAudio_createStreamBuilder",create_builder},
        {"AAudioStreamBuilder_delete",delete_builder},
        {"AAudioStreamBuilder_openStream",open_stream},
        {"AAudioStreamBuilder_setSampleRate",set_rate},
        {"AAudioStreamBuilder_setChannelCount",set_channels},
        {"AAudioStreamBuilder_setFormat",set_format},
        {"AAudioStreamBuilder_setFramesPerDataCallback",set_frames},
        {"AAudioStreamBuilder_setBufferCapacityInFrames",set_capacity},
        {"AAudioStreamBuilder_setDeviceId",set_device},
        {"AAudioStreamBuilder_setSessionId",set_session},
        {"AAudioStreamBuilder_setDirection",set_direction},
        {"AAudioStreamBuilder_setPerformanceMode",set_option},
        {"AAudioStreamBuilder_setSharingMode",set_option},
        {"AAudioStreamBuilder_setDataCallback",set_callback},
        {"AAudioStreamBuilder_setErrorCallback",set_error_callback},
        {"AAudioStream_close",close_stream},
        {"AAudioStream_requestStart",start_stream},
        {"AAudioStream_requestStop",stop_stream},
        {"AAudioStream_waitForStateChange",wait_state},
        {"AAudioStream_getBufferCapacityInFrames",get_capacity},
        {"AAudioStream_getBufferSizeInFrames",get_buffer_size},
        {"AAudioStream_setBufferSizeInFrames",set_buffer_size},
        {"AAudioStream_getFramesPerBurst",get_burst},
        {"AAudioStream_getDeviceId",get_device},
        {"AAudioStream_getSessionId",get_session},
        {"AAudioStream_getSampleRate",get_rate},
        {"AAudioStream_getChannelCount",get_channels},
        {"AAudioStream_getFormat",get_format},
        {"AAudioStream_getState",get_state},
        {"AAudioStream_getXRunCount",get_xruns},
        {"AAudioStream_isMMapUsed",is_mmap},
        {NULL,NULL}
    };
    for (unsigned i=0;hooks[i].name;i++)
        if (!strcmp(symbol,hooks[i].name)) return hooks[i].fn;
    return NULL;
}
