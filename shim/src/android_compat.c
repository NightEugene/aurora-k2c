/* Minimal NDK event loop for devices without libandroid. No executable TLS:
 * glibc pthread keys keep the hybrid process's bionic TLS slots untouched. */
#include "android_compat.h"
#include <pthread.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#define MAX_FDS 64
#define FRAME_NS 16666667LL
typedef int (*FdCallback)(int, int, void *);
typedef void (*FrameCallback)(int64_t, void *);
struct Watch { int fd, ident, events; FdCallback callback; void *data; };
struct Frame { FrameCallback callback; void *data; int64_t due; struct Frame *next; };
struct Looper {
    pthread_mutex_t lock;
    int wake, options, refs, count;
    struct Watch watches[MAX_FDS];
    struct Frame *frames;
};
static pthread_key_t looper_key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static int enabled;

static int64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static void looper_acquire(struct Looper *l) { __atomic_add_fetch(&l->refs, 1, __ATOMIC_RELAXED); }
static void looper_release(struct Looper *l)
{
    if (__atomic_sub_fetch(&l->refs, 1, __ATOMIC_ACQ_REL)) return;
    while (l->frames) { struct Frame *f = l->frames; l->frames = f->next; free(f); }
    close(l->wake);
    pthread_mutex_destroy(&l->lock);
    free(l);
}
static void destroy_looper(void *l) { looper_release(l); }
static void make_key(void) { pthread_key_create(&looper_key, destroy_looper); }
static struct Looper *looper_for_thread(void)
{
    pthread_once(&key_once, make_key);
    return pthread_getspecific(looper_key);
}
static struct Looper *looper_prepare(int options)
{
    struct Looper *l = looper_for_thread();
    if (l) return l;
    l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (l->wake < 0) { free(l); return NULL; }
    pthread_mutex_init(&l->lock, NULL);
    l->options = options;
    l->refs = 1;
    if (pthread_setspecific(looper_key, l)) { looper_release(l); return NULL; }
    return l;
}
static void looper_wake(struct Looper *l)
{
    uint64_t one = 1;
    if (l) (void)write(l->wake, &one, sizeof one);
}
static int looper_remove_fd(struct Looper *l, int fd)
{
    int found = 0;
    pthread_mutex_lock(&l->lock);
    for (int i = 0; i < l->count; ++i) if (l->watches[i].fd == fd) {
        l->watches[i] = l->watches[--l->count]; found = 1; break;
    }
    pthread_mutex_unlock(&l->lock);
    if (found) looper_wake(l);
    return found;
}
static int looper_add_fd(struct Looper *l, int fd, int ident, int events, FdCallback cb, void *data)
{
    if (!l || fd < 0 || (!cb && (ident < 0 || !(l->options & 1)))) return -1;
    pthread_mutex_lock(&l->lock);
    int i;
    for (i = 0; i < l->count && l->watches[i].fd != fd; ++i) {}
    if (i == MAX_FDS) { pthread_mutex_unlock(&l->lock); return -1; }
    if (i == l->count) ++l->count;
    l->watches[i] = (struct Watch){fd, cb ? -2 : ident, events, cb, data};
    pthread_mutex_unlock(&l->lock);
    looper_wake(l);
    return 1;
}
static int looper_poll_once(int timeout, int *out_fd, int *out_events, void **out_data)
{
    if (out_fd) *out_fd = -1;
    if (out_events) *out_events = 0;
    if (out_data) *out_data = NULL;
    struct Looper *l = looper_for_thread();
    if (!l) return -3;
    struct Watch watches[MAX_FDS];
    struct pollfd fds[MAX_FDS + 1];
    fds[0] = (struct pollfd){l->wake, POLLIN, 0};
    pthread_mutex_lock(&l->lock);
    int count = l->count;
    memcpy(watches, l->watches, count * sizeof *watches);
    int64_t now = now_ns();
    for (struct Frame *f = l->frames; f; f = f->next) {
        int64_t ms = f->due <= now ? 0 : (f->due - now + 999999) / 1000000;
        int wait = ms > INT_MAX ? INT_MAX : (int)ms;
        if (timeout < 0 || wait < timeout) timeout = wait;
    }
    pthread_mutex_unlock(&l->lock);
    for (int i = 0; i < count; ++i) {
        short events = ((watches[i].events & 1) ? POLLIN : 0) |
                       ((watches[i].events & 2) ? POLLOUT : 0);
        fds[i + 1] = (struct pollfd){watches[i].fd, events, 0};
    }
    int result = poll(fds, count + 1, timeout);
    if (result < 0) return errno == EINTR ? -1 : -3;
    int status = result ? -1 : -4;
    if (fds[0].revents) { uint64_t value; (void)read(l->wake, &value, sizeof value); }
    for (int i = 0; i < count; ++i) if (fds[i + 1].revents) {
        int re = fds[i + 1].revents;
        int events = ((re & POLLIN) ? 1 : 0) | ((re & POLLOUT) ? 2 : 0) |
                     ((re & POLLERR) ? 4 : 0) | ((re & POLLHUP) ? 8 : 0) |
                     ((re & POLLNVAL) ? 16 : 0);
        if (watches[i].callback) {
            if (!watches[i].callback(watches[i].fd, events, watches[i].data))
                looper_remove_fd(l, watches[i].fd);
            status = -2;
        } else {
            if (out_fd) *out_fd = watches[i].fd;
            if (out_events) *out_events = events;
            if (out_data) *out_data = watches[i].data;
            return watches[i].ident;
        }
    }
    /* Detach before calling Unity: callbacks may post the next frame. */
    struct Frame *ready = NULL;
    now = now_ns();
    pthread_mutex_lock(&l->lock);
    struct Frame **cursor = &l->frames;
    while (*cursor) {
        struct Frame *f = *cursor;
        if (f->due <= now) { *cursor = f->next; f->next = ready; ready = f; }
        else cursor = &f->next;
    }
    pthread_mutex_unlock(&l->lock);
    while (ready) {
        struct Frame *f = ready; ready = f->next;
        f->callback(now, f->data); free(f); status = -2;
    }
    return status;
}
static int looper_poll_all(int timeout, int *fd, int *events, void **data)
{
    int64_t deadline = timeout > 0 ? now_ns() + (int64_t)timeout * 1000000 : 0;
    int result;
    do {
        result = looper_poll_once(timeout, fd, events, data);
        if (result != -2) return result;
        if (deadline) {
            int64_t remaining = deadline - now_ns();
            if (remaining <= 0) return -4;
            timeout = (int)((remaining + 999999) / 1000000);
        }
    } while (1);
}
static struct Looper *choreographer_instance(void) { return looper_prepare(0); }
static void choreographer_post_delayed(struct Looper *l, FrameCallback cb, void *data, int64_t delay_ms)
{
    if (!l || !cb) return;
    struct Frame *f = malloc(sizeof *f);
    if (!f) return;
    int64_t now = now_ns();
    if (delay_ms < 0) delay_ms = 0;
    /* aarch64's legacy callback long and callback64 int64_t have identical ABI. */
    *f = (struct Frame){cb, data, now + FRAME_NS + delay_ms * 1000000, NULL};
    pthread_mutex_lock(&l->lock);
    f->next = l->frames; l->frames = f;
    pthread_mutex_unlock(&l->lock);
    looper_wake(l);
}
static void choreographer_post(struct Looper *l, FrameCallback cb, void *data)
{ choreographer_post_delayed(l, cb, data, 0); }

/* This game uses touch input; absence of optional motion sensors is supported. */
static void *sensor_manager(void) { static int manager; return &manager; }
static int sensor_list(void *manager, const void ***list)
{ (void)manager; if (list) *list = NULL; return 0; }
static void *sensor_default(void *manager, int type) { (void)manager; (void)type; return NULL; }
static void *sensor_queue(void *manager, void *looper, int ident, FdCallback cb, void *data)
{ (void)manager; (void)looper; (void)ident; (void)cb; (void)data; return NULL; }
static int sensor_destroy_queue(void *manager, void *queue) { (void)manager; (void)queue; return 0; }
static int sensor_has_events(void *queue) { (void)queue; return 0; }
static long sensor_events(void *queue, void *events, size_t count)
{ (void)queue; (void)events; (void)count; return 0; }
static int sensor_enable(void *queue, const void *sensor) { (void)queue; (void)sensor; return -EINVAL; }
static int sensor_rate(void *queue, const void *sensor, int32_t rate)
{ (void)queue; (void)sensor; (void)rate; return -EINVAL; }
static int sensor_int(const void *sensor) { (void)sensor; return 0; }
static float sensor_resolution(const void *sensor) { (void)sensor; return 0; }
static const char *sensor_string(const void *sensor) { (void)sensor; return ""; }

static const struct { const char *name; void *function; } symbols[] = {
    {"ALooper_prepare", looper_prepare}, {"ALooper_forThread", looper_for_thread},
    {"ALooper_pollOnce", looper_poll_once}, {"ALooper_pollAll", looper_poll_all},
    {"ALooper_acquire", looper_acquire}, {"ALooper_release", looper_release},
    {"ALooper_wake", looper_wake}, {"ALooper_addFd", looper_add_fd},
    {"ALooper_removeFd", looper_remove_fd},
    {"AChoreographer_getInstance", choreographer_instance},
    {"AChoreographer_postFrameCallback", choreographer_post},
    {"AChoreographer_postFrameCallback64", choreographer_post},
    {"AChoreographer_postFrameCallbackDelayed", choreographer_post_delayed},
    {"AChoreographer_postFrameCallbackDelayed64", choreographer_post_delayed},
    {"ASensorManager_getInstance", sensor_manager},
    {"ASensorManager_getSensorList", sensor_list},
    {"ASensorManager_getDefaultSensor", sensor_default},
    {"ASensorManager_createEventQueue", sensor_queue},
    {"ASensorManager_destroyEventQueue", sensor_destroy_queue},
    {"ASensorEventQueue_hasEvents", sensor_has_events},
    {"ASensorEventQueue_getEvents", sensor_events},
    {"ASensorEventQueue_enableSensor", sensor_enable},
    {"ASensorEventQueue_disableSensor", sensor_enable},
    {"ASensorEventQueue_setEventRate", sensor_rate},
    {"ASensor_getType", sensor_int}, {"ASensor_getMinDelay", sensor_int},
    {"ASensor_getResolution", sensor_resolution},
    {"ASensor_getName", sensor_string}, {"ASensor_getVendor", sensor_string},
    {NULL, NULL}
};
void k2c_android_fallback_enable(void) { enabled = 1; }
void *k2c_android_fallback_symbol(const char *name)
{
    if (!enabled) return NULL;
    for (int i = 0; symbols[i].name; ++i)
        if (!strcmp(name, symbols[i].name)) return symbols[i].function;
    return NULL;
}
