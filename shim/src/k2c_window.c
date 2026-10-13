// ANativeWindow (AOSP 12 layout) поверх hybris-gralloc; презентация через k2c_display.
#include "k2c_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/syscall.h>

// Лог без glibc-stdio/TLS — безопасно вызывать из bionic-потоков.
static void xlog(const char *msg)
{
    if (!getenv("K2C_VERBOSE")) return;
    size_t n = 0;
    while (msg[n]) n++;
    syscall(SYS_write, 2, msg, n);
    syscall(SYS_write, 2, "\n", 1);
}

// ---- AHardwareBuffer из bionic-мира (настоящий allocator-сервис) --------------

extern void *hybris_dlopen(const char *filename, int flag);
extern void *hybris_dlsym(void *handle, const char *symbol);

typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;

typedef int (*AHB_allocate_fn)(const AHardwareBuffer_Desc *desc, void **outBuffer);
typedef void (*AHB_release_fn)(void *buffer);
typedef void (*AHB_describe_fn)(const void *buffer, AHardwareBuffer_Desc *outDesc);
typedef int (*AHB_lock_fn)(void *buffer, uint64_t usage, int32_t fence,
                           const void *rect, void **outVirtualAddress);
typedef int (*AHB_unlock_fn)(void *buffer, int32_t *fence);
typedef const void *(*AHB_getNativeHandle_fn)(void *buffer);

static AHB_allocate_fn AHB_allocate;
static AHB_release_fn AHB_release;
static AHB_describe_fn AHB_describe;
static AHB_lock_fn AHB_lock;
static AHB_unlock_fn AHB_unlock;
static AHB_getNativeHandle_fn AHB_getNativeHandle;
static void (*g_gl_get_int)(unsigned, int *);

static int ahb_init_once(void)
{
    static int done = 0;
    if (done) return AHB_allocate != NULL;
    void *la = hybris_dlopen("/usr/libexec/droid-hybris/system/lib64/libandroid.so", 2 | 0x2);
    if (!la) la = hybris_dlopen("/system/lib64/libandroid.so", 2 | 0x2);
    // Recent device adaptations export AHardwareBuffer from libnativewindow.
    if (!la) la = hybris_dlopen("/usr/libexec/droid-hybris/system/lib64/libnativewindow.so", 2 | 0x2);
    if (!la) la = hybris_dlopen("/system/lib64/libnativewindow.so", 2 | 0x2);
    if (la) {
        AHB_allocate = (AHB_allocate_fn)hybris_dlsym(la, "AHardwareBuffer_allocate");
        AHB_release = (AHB_release_fn)hybris_dlsym(la, "AHardwareBuffer_release");
        AHB_describe = (AHB_describe_fn)hybris_dlsym(la, "AHardwareBuffer_describe");
        AHB_lock = (AHB_lock_fn)hybris_dlsym(la, "AHardwareBuffer_lock");
        AHB_unlock = (AHB_unlock_fn)hybris_dlsym(la, "AHardwareBuffer_unlock");
        AHB_getNativeHandle = (AHB_getNativeHandle_fn)hybris_dlsym(la, "AHardwareBuffer_getNativeHandle");
    }
    if (!AHB_release || !AHB_describe || !AHB_lock || !AHB_unlock || !AHB_getNativeHandle)
        AHB_allocate = NULL;
    xlog(AHB_allocate ? "[window] AHardwareBuffer API найден" : "[window] AHardwareBuffer API НЕ найден");
    done = 1;
    return AHB_allocate != NULL;
}

// ---- hybris gralloc (glibc-мир) ---------------------------------------------

typedef const void *buffer_handle_t;
extern void hybris_gralloc_initialize(int framebuffer);
extern int hybris_gralloc_allocate(int width, int height, int format,
                                   int usage, buffer_handle_t *handle, uint32_t *stride);
extern int hybris_gralloc_release(buffer_handle_t handle, int was_allocated);
extern int hybris_gralloc_lock(buffer_handle_t handle, int usage, int l, int t,
                               int w, int h, void **vaddr);
extern int hybris_gralloc_unlock(buffer_handle_t handle);

#define HAL_PIXEL_FORMAT_RGBA_8888 1
#define HAL_PIXEL_FORMAT_BGRA_8888 5

#define GRALLOC_USAGE_SW_READ_OFTEN  0x03
#define GRALLOC_USAGE_SW_WRITE_OFTEN 0x30
#define GRALLOC_USAGE_HW_TEXTURE     0x100
#define GRALLOC_USAGE_HW_RENDER      0x200

// ---- AOSP-структуры ----------------------------------------------------------

struct android_native_base_t {
    int magic;
    int version;
    void *reserved[4];
    void (*incRef)(struct android_native_base_t *base);
    void (*decRef)(struct android_native_base_t *base);
};

#define MAGIC(a,b,c,d) (((unsigned)(a)<<24)|((unsigned)(b)<<16)|((unsigned)(c)<<8)|(unsigned)(d))
#define ANDROID_NATIVE_WINDOW_MAGIC MAGIC('_','w','n','d')
#define ANDROID_NATIVE_BUFFER_MAGIC MAGIC('_','b','f','r')

typedef struct ANativeWindowBuffer {
    struct android_native_base_t common;
    int width, height, stride, format;
    int usage_deprecated;
    uintptr_t layerCount;
    void *reserved[1];
    buffer_handle_t handle;
    uint64_t usage;
    void *reserved_proc[8];
} ANativeWindowBuffer_t;

struct ANativeWindow {
    struct android_native_base_t common;
    const uint32_t flags;
    const int minSwapInterval;
    const int maxSwapInterval;
    const float xdpi;
    const float ydpi;
    intptr_t oem[4];
    int (*setSwapInterval)(struct ANativeWindow *window, int interval);
    int (*dequeueBuffer_DEPRECATED)(struct ANativeWindow *window, ANativeWindowBuffer_t **buffer);
    int (*lockBuffer_DEPRECATED)(struct ANativeWindow *window, ANativeWindowBuffer_t *buffer);
    int (*queueBuffer_DEPRECATED)(struct ANativeWindow *window, ANativeWindowBuffer_t *buffer);
    int (*query)(const struct ANativeWindow *window, int what, int *value);
    int (*perform)(struct ANativeWindow *window, int operation, ...);
    int (*cancelBuffer_DEPRECATED)(struct ANativeWindow *window, ANativeWindowBuffer_t *buffer);
    int (*dequeueBuffer)(struct ANativeWindow *window, ANativeWindowBuffer_t **buffer, int *fenceFd);
    int (*queueBuffer)(struct ANativeWindow *window, ANativeWindowBuffer_t *buffer, int fenceFd);
    int (*cancelBuffer)(struct ANativeWindow *window, ANativeWindowBuffer_t *buffer, int fenceFd);
};

// ---- состояние окна -----------------------------------------------------------

#define MAX_BUFS 6

typedef struct {
    ANativeWindowBuffer_t buf;
    void *ahb;      // AHardwareBuffer* если выделен через bionic-мир
    int allocated;
    int dequeued;
} K2CSlot;

typedef struct {
    struct ANativeWindow win;
    int width, height, format;
    uint64_t usage;
    int buffer_count;
    K2CSlot slots[MAX_BUFS];
    unsigned frames;
} K2CWindow;

static K2CWindow g_kwin;
static int g_gralloc_ready;

static void base_incdec(struct android_native_base_t *base) { (void)base; }

static void gralloc_init_once(void)
{
    if (g_gralloc_ready) return;
    hybris_gralloc_initialize(0);
    g_gralloc_ready = 1;
    xlog("[window] hybris_gralloc_initialize done");
}

static int slot_alloc(K2CSlot *s, int w, int h, int format, uint64_t usage)
{
    if (!ahb_init_once())
        return -1;
    if (s->allocated) {
        if (s->ahb) AHB_release(s->ahb);
        s->ahb = NULL;
        memset(&s->buf, 0, sizeof s->buf);
        s->allocated = 0;
    }
    AHardwareBuffer_Desc d;
    memset(&d, 0, sizeof d);
    d.width = (uint32_t)w;
    d.height = (uint32_t)h;
    d.layers = 1;
    d.format = (uint32_t)format;
    d.usage = usage | GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN |
              GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER;
    void *ahb = NULL;
    int r = AHB_allocate(&d, &ahb);
    if (r != 0 || !ahb) {
        xlog("[window] AHardwareBuffer_allocate failed");
        return -1;
    }
    AHardwareBuffer_Desc out;
    memset(&out, 0, sizeof out);
    AHB_describe(ahb, &out);
    buffer_handle_t handle = AHB_getNativeHandle ? AHB_getNativeHandle(ahb)
                                                 : *(buffer_handle_t *)ahb;
    {
        const int *nh = (const int *)handle;
        fprintf(stderr, "[window] handle=%p version=%d numFds=%d numInts=%d stride=%u\n",
                (void *)handle, nh[0], nh[1], nh[2], out.stride);
    }
    s->buf.common.magic = ANDROID_NATIVE_BUFFER_MAGIC;
    s->buf.common.version = sizeof(ANativeWindowBuffer_t);
    s->buf.common.incRef = base_incdec;
    s->buf.common.decRef = base_incdec;
    s->buf.width = w;
    s->buf.height = h;
    s->buf.stride = (int)out.stride;
    s->buf.format = format;
    s->buf.usage_deprecated = (int)usage;
    s->buf.layerCount = 1;
    s->buf.handle = handle;
    s->buf.usage = usage;
    s->ahb = ahb;
    s->allocated = 1;
    xlog("[window] буфер выделен через AHardwareBuffer");
    return 0;
}

static int win_setSwapInterval(struct ANativeWindow *window, int interval)
{
    (void)window; (void)interval;
    return 0;
}

static int win_dequeueBuffer(struct ANativeWindow *window, ANativeWindowBuffer_t **buffer, int *fenceFd)
{
    K2CWindow *k = (K2CWindow *)window;
    xlog("[window] dequeueBuffer вхождение (count=%d)");
    for (int i = 0; i < k->buffer_count; i++) {
        K2CSlot *s = &k->slots[i];
        if (s->dequeued) continue;
        if (!s->allocated || s->buf.width != k->width || s->buf.height != k->height ||
            s->buf.format != k->format || s->buf.usage != k->usage) {
            if (slot_alloc(s, k->width, k->height, k->format, k->usage) != 0)
                return -12; // ENOMEM
        }
        s->dequeued = 1;
        *buffer = &s->buf;
        *fenceFd = -1;
        xlog("[window] dequeueBuffer -> slot %d (stride=%d)");
        return 0;
    }
    xlog("[window] dequeue: нет свободных буферов");
    return -16; // EBUSY
}

static int win_queueBuffer(struct ANativeWindow *window, ANativeWindowBuffer_t *buffer, int fenceFd)
{
    K2CWindow *k = (K2CWindow *)window;
    if (!g_gl_get_int) {
        void *gles = hybris_dlopen("/usr/libexec/droid-hybris/system/lib64/libGLESv2.so", 2);
        g_gl_get_int = gles ? hybris_dlsym(gles, "glGetIntegerv") : NULL;
    }
    int viewport[4] = {0, 0, buffer->width, buffer->height}, framebuffer = -1;
    if (g_gl_get_int) {
        g_gl_get_int(0x0ba2, viewport);
        g_gl_get_int(0x8ca6, &framebuffer);
        if (k->frames == 30 && getenv("K2C_VERBOSE")) {
            int scissor[4] = {0};
            g_gl_get_int(0x0c10, scissor);
            fprintf(stderr, "[window] buffer=%dx%d viewport=%d,%d %dx%d scissor=%d,%d %dx%d fbo=%d\n",
                    buffer->width, buffer->height, viewport[0], viewport[1], viewport[2], viewport[3],
                    scissor[0], scissor[1], scissor[2], scissor[3], framebuffer);
        }
    }
    xlog("[window] queueBuffer %dx%d stride=%d");
    if (fenceFd >= 0) close(fenceFd);
    if (!getenv("K2C_NO_LOCK")) {
        K2CSlot *bs = NULL;
        for (int i = 0; i < k->buffer_count; i++)
            if (&k->slots[i].buf == buffer) { bs = &k->slots[i]; break; }
        void *vaddr = NULL;
        int r = -1;
        if (bs && bs->ahb)
            r = AHB_lock(bs->ahb, GRALLOC_USAGE_SW_READ_OFTEN, -1, NULL, &vaddr);
        if (r == 0 && vaddr) {
            int x = 0, y = 0, w = buffer->width, h = buffer->height;
            if (framebuffer == 0 && viewport[0] >= 0 && viewport[1] >= 0 &&
                viewport[2] > 0 && viewport[3] > 0 &&
                viewport[0] + viewport[2] <= buffer->width &&
                viewport[1] + viewport[3] <= buffer->height) {
                x = viewport[0];
                y = buffer->height - viewport[1] - viewport[3];
                w = viewport[2];
                h = viewport[3];
            }
            k2c_display_stage((const char *)vaddr + ((size_t)y * buffer->stride + x) * 4,
                              w, h, buffer->stride * 4);
            AHB_unlock(bs->ahb, NULL);
        } else {
            xlog("[window] AHB_lock failed");
        }
    }
    for (int i = 0; i < k->buffer_count; i++)
        if (&k->slots[i].buf == buffer) { k->slots[i].dequeued = 0; break; }
    k->frames++;
    if (k->frames == 1 || k->frames % 300 == 0)
        xlog("[window] кадр #%u");
    return 0;
}

static int win_cancelBuffer(struct ANativeWindow *window, ANativeWindowBuffer_t *buffer, int fenceFd)
{
    K2CWindow *k = (K2CWindow *)window;
    if (fenceFd >= 0) close(fenceFd);
    for (int i = 0; i < k->buffer_count; i++)
        if (&k->slots[i].buf == buffer) { k->slots[i].dequeued = 0; break; }
    return 0;
}

static int win_dequeueBuffer_DEPRECATED(struct ANativeWindow *window, ANativeWindowBuffer_t **buffer)
{
    int fd;
    return win_dequeueBuffer(window, buffer, &fd);
}
static int win_queueBuffer_DEPRECATED(struct ANativeWindow *window, ANativeWindowBuffer_t *buffer)
{
    return win_queueBuffer(window, buffer, -1);
}
static int win_cancelBuffer_DEPRECATED(struct ANativeWindow *window, ANativeWindowBuffer_t *buffer)
{
    return win_cancelBuffer(window, buffer, -1);
}
static int win_lockBuffer_DEPRECATED(struct ANativeWindow *window, ANativeWindowBuffer_t *buffer)
{
    (void)window; (void)buffer;
    return 0;
}

static int win_query(const struct ANativeWindow *window, int what, int *value)
{
    const K2CWindow *k = (const K2CWindow *)window;
    switch (what) {
    case 0:  *value = k->width; break;            // WIDTH
    case 1:  *value = k->height; break;           // HEIGHT
    case 2:  *value = k->format; break;           // FORMAT
    case 3:  *value = 1; break;                   // MIN_UNDEQUEUED_BUFFERS
    case 4:  *value = 0; break;                   // QUEUED_TO_COMPOSER
    case 5:  *value = k->buffer_count; break;     // NUM_FREE_BUFFERS (приблизительно)
    case 6:  *value = k->width; break;            // DEFAULT_WIDTH
    case 7:  *value = k->height; break;           // DEFAULT_HEIGHT
    case 8:  *value = 0; break;                   // TRANSFORM_HINT
    case 9:  *value = 0; break;                   // CONSUMER_RUNNING_BEHIND
    case 10: *value = (int)k->usage; break;       // CONSUMER_USAGE_BITS
    case 11: *value = 0; break;                   // STICKY_TRANSFORM
    case 12: *value = 0; break;                   // DEFAULT_DATASPACE
    case 13: *value = 0; break;                   // BUFFER_AGE
    case 17: *value = (int)k->usage; break;   // op 17 (пишем только 4 байта)
    default:
        xlog("[window] query what=%d");
        *value = 0;
        break;
    }
    return 0;
}

static int win_perform(struct ANativeWindow *window, int operation, ...)
{
    K2CWindow *k = (K2CWindow *)window;
    va_list ap;
    va_start(ap, operation);
    switch (operation) {
    case 0: // SET_USAGE
        k->usage = va_arg(ap, uint32_t);
        xlog("[window] perform SET_USAGE 0x%llx");
        break;
    case 1: // CONNECT (deprecated)
    case 2: // DISCONNECT (deprecated)
        break;
    case 4: { // SET_BUFFER_COUNT
        int n = va_arg(ap, int);
        if (n < 2) n = 2;
        if (n > MAX_BUFS) n = MAX_BUFS;
        k->buffer_count = n;
        xlog("[window] perform SET_BUFFER_COUNT %d");
        break;
    }
    case 8: { // SET_BUFFERS_DIMENSIONS
        int w = va_arg(ap, int);
        int h = va_arg(ap, int);
        if (w > 0 && h > 0) { k->width = w; k->height = h; }
        xlog("[window] perform SET_BUFFERS_DIMENSIONS %dx%d");
        break;
    }
    case 9: { // SET_BUFFERS_FORMAT
        k->format = va_arg(ap, int);
        xlog("[window] perform SET_BUFFERS_FORMAT %d");
        break;
    }
    case 10: // SET_SCALING_MODE
        (void)va_arg(ap, int);
        break;
    case 13: // API_CONNECT
        (void)va_arg(ap, int);
        break;
    case 30: // SET_USAGE64 / ALLOCATE_BUFFERS (вариации)
        k->usage = va_arg(ap, uint64_t);
        xlog("[window] perform op=30 usage64 0x%llx");
        break;
    default:
        xlog("[window] perform op=%d (игнор)");
        break;
    }
    va_end(ap);
    return 0;
}

void *k2c_window_get(void)
{
    static int initialized = 0;
    if (!initialized) {
        K2CWindow *k = &g_kwin;
        memset(k, 0, sizeof *k);
        k->win.common.magic = ANDROID_NATIVE_WINDOW_MAGIC;
        k->win.common.version = sizeof(struct ANativeWindow);
        k->win.common.incRef = base_incdec;
        k->win.common.decRef = base_incdec;
        k->win.setSwapInterval = win_setSwapInterval;
        k->win.dequeueBuffer_DEPRECATED = win_dequeueBuffer_DEPRECATED;
        k->win.lockBuffer_DEPRECATED = win_lockBuffer_DEPRECATED;
        k->win.queueBuffer_DEPRECATED = win_queueBuffer_DEPRECATED;
        k->win.query = win_query;
        k->win.perform = win_perform;
        k->win.cancelBuffer_DEPRECATED = win_cancelBuffer_DEPRECATED;
        k->win.dequeueBuffer = win_dequeueBuffer;
        k->win.queueBuffer = win_queueBuffer;
        k->win.cancelBuffer = win_cancelBuffer;
        k->width = k2c_display_screen_w();
        k->height = k2c_display_screen_h();
        k->format = HAL_PIXEL_FORMAT_RGBA_8888;
        k->usage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER;
        k->buffer_count = 3;
        initialized = 1;
        xlog("[window] ANativeWindow создано %dx%d");
    }
    return &g_kwin.win;
}
