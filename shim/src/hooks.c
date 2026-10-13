// Хуки: __android_log, AAssetManager, ANativeWindow — glibc-реализации,
// подставляемые в bionic-мир при релокации символов.
#include "hooks.h"
#include "android_compat.h"
#include "k2c_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

extern void hybris_set_hook_callback(void *(*callback)(const char *sym, const char *requester));

/* libhybris stores the native attribute pointer in the Android attribute.
 * FMOD requests 96 KiB, below Aurora's native minimum on this device. */
static int hook_pthread_attr_setstacksize(void *android_attr, size_t stack)
{
    pthread_attr_t *attr = (pthread_attr_t *)*(uintptr_t *)android_attr;
    long minimum = sysconf(_SC_THREAD_STACK_MIN);
    if (stack && minimum > 0 && stack < (size_t)minimum) {
        fprintf(stderr, "[threads] raising Android stack %zu to native minimum %ld\n",
                stack, minimum);
        stack = (size_t)minimum;
    }
    return pthread_attr_setstacksize(attr, stack);
}

// ---- __android_log ---------------------------------------------------------

static int hook_android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    (void)prio;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[unity:%s] ", tag ? tag : "?");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}

static int hook_android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    (void)prio;
    fprintf(stderr, "[unity:%s] ", tag ? tag : "?");
    vfprintf(stderr, fmt ? fmt : "", ap);
    fputc('\n', stderr);
    return 0;
}

static int hook_android_log_write(int prio, const char *tag, const char *text)
{
    (void)prio;
    fprintf(stderr, "[unity:%s] %s\n", tag ? tag : "?", text ? text : "");
    return 0;
}

// ---- AAssetManager ----------------------------------------------------------

static char g_assets_root[512] = "./assets";
void hooks_set_assets_root(const char *path)
{
    snprintf(g_assets_root, sizeof g_assets_root, "%s", path);
}

struct AAsset {
    FILE *f;
    long size;
    void *map;   // mmap для getBuffer
};

struct AAssetDir {
    DIR *d;
};

struct AAssetManager { int dummy; };
static struct AAssetManager g_asset_mgr;

static void *hook_AAssetManager_open(struct AAssetManager *mgr, const char *filename, int mode)
{
    (void)mgr; (void)mode;
    char path[768];
    snprintf(path, sizeof path, "%s/%s", g_assets_root, filename);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[assets] MISS %s\n", filename);
        return NULL;
    }
    struct AAsset *a = calloc(1, sizeof *a);
    a->f = f;
    fseek(f, 0, SEEK_END);
    a->size = ftell(f);
    fseek(f, 0, SEEK_SET);
    a->map = mmap(NULL, (size_t)a->size, PROT_READ, MAP_PRIVATE, fileno(f), 0);
    if (a->map == MAP_FAILED) a->map = NULL;
    fprintf(stderr, "[assets] open %s (%ld bytes)\n", filename, a->size);
    return a;
}

static int hook_AAsset_read(struct AAsset *a, void *buf, size_t count)
{
    return (int)fread(buf, 1, count, a->f);
}

static long hook_AAsset_seek(struct AAsset *a, long offset, int whence)
{
    if (fseek(a->f, offset, whence) != 0) return -1;
    return ftell(a->f);
}

static long hook_AAsset_seek64(struct AAsset *a, long long offset, int whence)
{
    return hook_AAsset_seek(a, (long)offset, whence);
}

static void hook_AAsset_close(struct AAsset *a)
{
    if (!a) return;
    if (a->map) munmap(a->map, (size_t)a->size);
    fclose(a->f);
    free(a);
}

static long hook_AAsset_getLength(struct AAsset *a) { return a->size; }
static long long hook_AAsset_getLength64(struct AAsset *a) { return a->size; }
static long hook_AAsset_getRemainingLength(struct AAsset *a) { return a->size - ftell(a->f); }
static long long hook_AAsset_getRemainingLength64(struct AAsset *a) { return a->size - ftell(a->f); }
static const void *hook_AAsset_getBuffer(struct AAsset *a) { return a->map; }
static int hook_AAsset_isAllocated(struct AAsset *a) { (void)a; return 0; }
static int hook_AAsset_openFileDescriptor(struct AAsset *a, long *outStart, long *outLength)
{
    if (!a || !outStart || !outLength) return -1;
    int fd = dup(fileno(a->f));
    if (fd < 0) return -1;
    *outStart = 0;
    *outLength = a->size;
    fprintf(stderr, "[assets] audio/file descriptor fd=%d length=%ld\n", fd, a->size);
    return fd;
}

static void *hook_AAssetManager_openDir(struct AAssetManager *mgr, const char *dirName)
{
    (void)mgr;
    char path[768];
    snprintf(path, sizeof path, "%s/%s", g_assets_root, dirName);
    DIR *d = opendir(path);
    if (!d) return NULL;
    struct AAssetDir *ad = calloc(1, sizeof *ad);
    ad->d = d;
    return ad;
}

static const char *hook_AAssetDir_getNextFileName(struct AAssetDir *ad)
{
    struct dirent *e;
    while ((e = readdir(ad->d)) != NULL) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            return e->d_name;
    }
    return NULL;
}

static void hook_AAssetDir_rewind(struct AAssetDir *ad) { rewinddir(ad->d); }
static void hook_AAssetDir_close(struct AAssetDir *ad) { closedir(ad->d); free(ad); }

static struct AAssetManager *hook_AAssetManager_fromJava(void *env, void *obj)
{
    (void)env; (void)obj;
    fprintf(stderr, "[assets] AAssetManager_fromJava\n");
    return &g_asset_mgr;
}

// ---- ANativeWindow — реальная реализация из k2c_window.c ----------------------

extern void *k2c_window_get(void);
extern int k2c_display_screen_w(void);
extern int k2c_display_screen_h(void);

struct ANativeWindow;

static void *hook_ANativeWindow_fromSurface(void *env, void *surface)
{
    (void)env; (void)surface;
    void *w = k2c_window_get();
    fprintf(stderr, "[window] ANativeWindow_fromSurface -> %p\n", w);
    return w;
}

static void hook_ANativeWindow_acquire(struct ANativeWindow *w) { (void)w; }
static void hook_ANativeWindow_release(struct ANativeWindow *w) { (void)w; }
static int hook_ANativeWindow_getWidth(struct ANativeWindow *w) { (void)w; return k2c_display_screen_w(); }
static int hook_ANativeWindow_getHeight(struct ANativeWindow *w) { (void)w; return k2c_display_screen_h(); }
static int hook_ANativeWindow_getFormat(struct ANativeWindow *w) { (void)w; return 1; }
static int hook_ANativeWindow_setBuffersGeometry(struct ANativeWindow *w, int width, int height, int format)
{
    (void)w;
    fprintf(stderr, "[window] setBuffersGeometry %dx%d fmt=%d\n", width, height, format);
    return 0;
}

// ---- Vulkan: запрещаем, пусть Unity уходит на GLES ---------------------------

extern void *hybris_dlopen(const char *filename, int flag);
extern void *hybris_dlsym(void *handle, const char *symbol);

static int hook_vkCreateInstance(const void *pCreateInfo, const void *pAllocator, void *pInstance);

typedef void *(*vkGetInstanceProcAddr_real_fn)(void *instance, const char *name);
static vkGetInstanceProcAddr_real_fn real_vkGetInstanceProcAddr;

static void *hook_vkGetInstanceProcAddr(void *instance, const char *name)
{
    if (!real_vkGetInstanceProcAddr) {
        void *h = hybris_dlopen("/system/lib64/libvulkan.so", 2 | 0x2);
        if (h) real_vkGetInstanceProcAddr = (vkGetInstanceProcAddr_real_fn)hybris_dlsym(h, "vkGetInstanceProcAddr");
    }
    if (name && strcmp(name, "vkCreateInstance") == 0) {
        fprintf(stderr, "[vk] vkGetInstanceProcAddr(vkCreateInstance) -> наш стаб (запрет Vulkan)\n");
        return hook_vkCreateInstance;
    }
    if (real_vkGetInstanceProcAddr)
        return real_vkGetInstanceProcAddr(instance, name);
    return NULL;
}

static int hook_vkCreateInstance(const void *pCreateInfo, const void *pAllocator, void *pInstance)
{
    (void)pCreateInfo; (void)pAllocator; (void)pInstance;
    fprintf(stderr, "[vk] vkCreateInstance -> VK_ERROR_INITIALIZATION_FAILED (принудительно)\n");
    return -3;
}
static int hook_vkEnumeratePhysicalDevices(void *instance, uint32_t *count, void *devices)
{
    (void)instance; (void)devices;
    if (count) *count = 0;
    fprintf(stderr, "[vk] vkEnumeratePhysicalDevices -> 0 устройств (принудительно)\n");
    return -3;
}

// OpenSL is unavailable; AAudio is handled by the host PulseAudio bridge.
static long hook_slCreateEngine(void *pEngine, unsigned numOptions, const void *pEngineOptions,
                                unsigned numInterfaces, const void *pInterfaceIds, const void *pInterfaceRequired)
{
    (void)pEngine; (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
    (void)pInterfaceIds; (void)pInterfaceRequired;
    return 9; // SL_RESULT_RESOURCE_ERROR
}

// В гибридном процессе слот __stack_chk_guard (tpidr_el0+0x28) на glibc-потоках
// используется libhybris под bionic-TLS и меняется посреди функций Unity,
// поэтому bionic stack protector срабатывает ложно (сами кадры не повреждены —
// проверено дампом стека). Гасим ложные abort'ы.
static void hook_stack_chk_fail(void)
{
    static int reported;
    if (!reported) {
        reported = 1;
        fprintf(stderr, "[hooks] __stack_chk_fail проигнорирован (ложный guard)\n");
    }
}

// ---- диспетчер ----------------------------------------------------------------

static const struct { const char *sym; void *fn; } g_hooks[] = {
    { "__android_log_print", hook_android_log_print },
    { "__android_log_write", hook_android_log_write },
    { "__android_log_vprint", hook_android_log_vprint },

    { "AAssetManager_open", hook_AAssetManager_open },
    { "AAssetManager_openDir", hook_AAssetManager_openDir },
    { "AAssetManager_fromJava", hook_AAssetManager_fromJava },
    { "AAsset_read", hook_AAsset_read },
    { "AAsset_seek", hook_AAsset_seek },
    { "AAsset_seek64", hook_AAsset_seek64 },
    { "AAsset_close", hook_AAsset_close },
    { "AAsset_getLength", hook_AAsset_getLength },
    { "AAsset_getLength64", hook_AAsset_getLength64 },
    { "AAsset_getRemainingLength", hook_AAsset_getRemainingLength },
    { "AAsset_getRemainingLength64", hook_AAsset_getRemainingLength64 },
    { "AAsset_getBuffer", hook_AAsset_getBuffer },
    { "AAsset_isAllocated", hook_AAsset_isAllocated },
    { "AAsset_openFileDescriptor", hook_AAsset_openFileDescriptor },
    { "AAsset_openFileDescriptor64", hook_AAsset_openFileDescriptor },
    { "AAssetDir_getNextFileName", hook_AAssetDir_getNextFileName },
    { "AAssetDir_rewind", hook_AAssetDir_rewind },
    { "AAssetDir_close", hook_AAssetDir_close },

    { "ANativeWindow_fromSurface", hook_ANativeWindow_fromSurface },
    { "ANativeWindow_acquire", hook_ANativeWindow_acquire },
    { "ANativeWindow_release", hook_ANativeWindow_release },
    { "ANativeWindow_getWidth", hook_ANativeWindow_getWidth },
    { "ANativeWindow_getHeight", hook_ANativeWindow_getHeight },
    { "ANativeWindow_getFormat", hook_ANativeWindow_getFormat },
    { "ANativeWindow_setBuffersGeometry", hook_ANativeWindow_setBuffersGeometry },

    // ALooper/AChoreographer use system libandroid when available, otherwise
    // android_compat provides native frame callbacks through the hook dispatcher.

    { "vkCreateInstance", hook_vkCreateInstance },
    { "vkEnumeratePhysicalDevices", hook_vkEnumeratePhysicalDevices },
    { "vkGetInstanceProcAddr", hook_vkGetInstanceProcAddr },

    { "slCreateEngine", hook_slCreateEngine },

    { NULL, NULL },
};

static void *hook_dispatcher(const char *sym, const char *requester)
{
    void *android = k2c_android_fallback_symbol(sym);
    if (android) return android;
    if (strcmp(sym, "pthread_attr_setstacksize") == 0)
        return hook_pthread_attr_setstacksize;
    if (strcmp(sym, "__stack_chk_fail") == 0)
        return hook_stack_chk_fail;
    void *audio = k2c_audio_hook(sym);
    if (audio) return audio;
    for (int i = 0; g_hooks[i].sym; i++)
        if (strcmp(g_hooks[i].sym, sym) == 0)
            return g_hooks[i].fn;
    return NULL;
}

void hooks_install(void)
{
    hybris_set_hook_callback(hook_dispatcher);
}
