#include "k2c_app.h"
// M3-harness: загрузка игры, JNI-мост, вызов initJni — старт Unity.
// Модель потоков как на Android: главный = UI-thread (прокачка ALooper +
// wayland), worker = UnityMain (JNI, initJni, nativeRender).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <elf.h>
#include <limits.h>
#include <ctype.h>
#include "jni_fake.h"
#include "hooks.h"
#include "android_compat.h"
#include "k2c_display.h"

extern void *hybris_dlopen(const char *filename, int flag);
extern void *hybris_dlsym(void *handle, const char *symbol);
extern char *hybris_dlerror(void);
extern void *hybris_create_namespace(const char *, const char *, const char *, uint64_t, const char *, void *);
extern void *hybris_dlopen_ext(const char *, int, const void *);

static const char *HYBRIS_SYSTEM_PATH = "/usr/libexec/droid-hybris/system/lib64";

static void *(*ALooper_prepare_p)(int);
static int (*ALooper_pollAll_p)(int, int *, int *, void **);
static int g_worker_done;
static int g_worker_failed;
static void *g_worker_tpidr;
static void *g_game_namespace;

static void *load_game_lib(const char *lib)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", getenv("K2C_LIB_ROOT") ? getenv("K2C_LIB_ROOT") : "./lib/arm64-v8a", lib);
    struct {
        uint64_t flags;
        void *reserved_addr;
        size_t reserved_size;
        int relro_fd, library_fd;
        int64_t library_fd_offset;
        void *library_namespace;
    } ext = { .flags = 0x200, .library_namespace = g_game_namespace };
    void *h = hybris_dlopen_ext(path, RTLD_NOW | RTLD_GLOBAL, &ext);
    fprintf(stderr, "[m3] dlopen(%s) = %p %s\n", lib, h, h ? "" : hybris_dlerror());
    return h;
}

typedef jint (*JNI_OnLoad_fn)(JavaVM vm, void *reserved);

// ---- vsync-тикер -------------------------------------------------------------
// На реальном Android кадровый счётчик движка (bias+0x1607958) тикает
// Choreographer-объектом при каждом vsync. У нас vsync-источника нет — тикаем
// сами через внутреннюю функцию Unity (bias+0x809f64: counter++ + broadcast).

static uintptr_t find_libunity_bias(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t bias = 0;
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "libunity.so") && strstr(line, "r-xp")) {
            uintptr_t start, end;
            unsigned long off;
            char path[384];
            if (sscanf(line, "%lx-%lx %*4s %lx %*s %*s %383s",
                       &start, &end, &off, path) != 4) continue;
            FILE *elf = fopen(path, "rb");
            if (!elf) continue;
            Elf64_Ehdr eh;
            if (fread(&eh, sizeof eh, 1, elf) != 1 ||
                memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
                eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_machine != EM_AARCH64 ||
                eh.e_phentsize != sizeof(Elf64_Phdr) ||
                fseek(elf, (long)eh.e_phoff, SEEK_SET) != 0) {
                fclose(elf);
                continue;
            }
            for (unsigned i = 0; i < eh.e_phnum; i++) {
                Elf64_Phdr ph;
                if (fread(&ph, sizeof ph, 1, elf) != 1) break;
                if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X) ||
                    off > ph.p_offset || ph.p_offset - off >= (unsigned long)sysconf(_SC_PAGESIZE))
                    continue;
                // ELF virtual addresses differ from file offsets (0x4000 in this APK).
                bias = start - (ph.p_vaddr - (ph.p_offset - off));
                break;
            }
            fclose(elf);
            if (!bias) continue;
            break;
        }
    }
    fclose(f);
    return bias;
}

static void *vsync_ticker(void *arg)
{
    (void)arg;
    uintptr_t bias = 0;
    for (int i = 0; i < 600 && !bias; i++) { bias = find_libunity_bias(); usleep(50000); }
    if (!bias) { fprintf(stderr, "[ticker] libunity не найден\n"); return NULL; }
    // This offset belongs to the libunity.so shipped in the current APK.
    // Let Unity use its relocated pthread calls: its Android mutex/condition
    // objects cannot be passed directly to glibc pthread functions.
    const uint32_t *code = (const uint32_t *)(bias + 0x809f64);
    if (code[0] != 0xf81e0ffe || code[1] != 0xa9014ff4 ||
        code[2] != 0xd0006ff4 || code[3] != 0x91240294) {
        fprintf(stderr, "[ticker] неподдерживаемая версия libunity, тикер отключён\n");
        return NULL;
    }
    void (*tick)(long) = (void (*)(long))(bias + 0x809f64);
    fprintf(stderr, "[ticker] bias=0x%lx, старт тиков 60 Гц\n", bias);
    struct timespec ts;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long t = ts.tv_sec * 1000000000L + ts.tv_nsec;
        tick(t);
        usleep(16666);
    }
    return NULL;
}

static void *unity_worker_run(void *arg)
{
    (void)arg;
    __asm__ __volatile__("mrs %0, tpidr_el0" : "=r"(g_worker_tpidr));
    if (ALooper_prepare_p) {
        void *looper = ALooper_prepare_p(0);
        fprintf(stderr, "[m3] worker ALooper_prepare -> %p\n", looper);
    }
    fake_jni_set_verbose(getenv("K2C_JNI_TRACE") && strcmp(getenv("K2C_JNI_TRACE"),"1")==0);
    JavaVM vm = fake_jni_create_vm();
    JNIEnv env = fake_jni_get_env();

    void *il2cpp = load_game_lib("libil2cpp.so");
    void *unity = load_game_lib("libunity.so");
    void *mainlib = load_game_lib("libmain.so");
    if (!il2cpp || !unity || !mainlib) {
        g_worker_failed = 1;
        fprintf(stderr, "[m3] не все библиотеки загрузились\n");
        return NULL;
    }

    // На этом потоке после bionic-загрузок tpidr_el0 оказывается смещённым
    // libhybris в область стека; слот tpidr+0x28 (bionic stack guard)
    // попадает в чужие кадры и «меняется» посреди функций Unity — bionic
    // stack protector срабатывает ложно. Возвращаем исходный glibc tpidr.
    {
        void *tp_now;
        __asm__ __volatile__("mrs %0, tpidr_el0" : "=r"(tp_now));
        fprintf(stderr, "[m3] tpidr worker: entry=%p now=%p guard=%p\n",
                g_worker_tpidr, tp_now, ((void **)tp_now)[5]);
        if (g_worker_tpidr && tp_now != g_worker_tpidr) {
            __asm__ __volatile__("msr tpidr_el0, %0" :: "r"(g_worker_tpidr));
            fprintf(stderr, "[m3] tpidr восстановлен, guard=%p\n", ((void **)g_worker_tpidr)[5]);
        }
    }

    JNI_OnLoad_fn onload_unity = (JNI_OnLoad_fn)hybris_dlsym(unity, "JNI_OnLoad");
    JNI_OnLoad_fn onload_main = (JNI_OnLoad_fn)hybris_dlsym(mainlib, "JNI_OnLoad");

    fprintf(stderr, "[m3] unity JNI_OnLoad -> 0x%x\n", onload_unity ? onload_unity(vm, NULL) : -1);
    fprintf(stderr, "[m3] main JNI_OnLoad -> 0x%x\n", onload_main ? onload_main(vm, NULL) : -1);

    // Мод PDALIFE: грузим его библиотеки и вызываем их JNI_OnLoad —
    // нативный init читает PDALIFE.conf из assets и ставит A64-хуки на
    // il2cpp-функции (разблокировка DLC и пр.). Меню мода (Android overlay)
    // не создаём — нужны только патчи.
    void *pdaprot = load_game_lib("libpdaprotect.so");
    void *pdalife = load_game_lib("libPDALIFE.so");
    fprintf(stderr, "[m3] pdaprotect=%p pdalife=%p\n", pdaprot, pdalife);
    if (pdalife) {
        JNI_OnLoad_fn onload_pda = (JNI_OnLoad_fn)hybris_dlsym(pdalife, "JNI_OnLoad");
        jint rc = onload_pda ? onload_pda(vm, NULL) : -1;
        fprintf(stderr, "[m3] PDALIFE JNI_OnLoad -> 0x%x\n", rc);

        // Меню мода (Android overlay) у нас не живёт — опрашиваем список фич
        // и включаем переключаемые напрямую через native Changes().
        typedef jobject (*list_fn)(JNIEnv);
        typedef void (*changes_fn)(JNIEnv, jobject, jint, jstring, jint, jlong, jboolean, jstring);
        list_fn getFeatures = (list_fn)fake_jni_find_native("GetFeatureList", NULL);
        changes_fn changes = (changes_fn)fake_jni_find_native("Changes", NULL);
        fprintf(stderr, "[mods] GetFeatureList=%p Changes=%p\n", (void *)getFeatures, (void *)changes);
        if (getFeatures) {
            jobject ctx = fake_jni_new_object("android/app/Activity");
            jobject arr = getFeatures(env);
            jlong n = fake_jni_array_length(arr);
            fprintf(stderr, "[mods] GetFeatureList: %ld фич\n", (long)n);
            for (jlong i = 0; i < n; i++) {
                const char *f = fake_jni_string_chars(fake_jni_array_get(arr, i));
                fprintf(stderr, "[mods]   %ld: %s\n", (long)i, f ? f : "?");
                if (!changes || !f) continue;
                jint cat = 0;
                const char *p = f;
                // формат LGL: "cat_Toggle_Имя" — категория необязательна
                if (isdigit((unsigned char)*p)) {
                    cat = (jint)strtol(p, (char **)&p, 10);
                    if (*p == '_') p++;
                }
                jboolean on = JNI_FALSE;
                jlong val = 0;
                if (strncmp(p, "Toggle_", 7) == 0) { p += 7; on = JNI_TRUE; }
                else if (strncmp(p, "CheckBox_", 9) == 0) { p += 9; on = JNI_TRUE; }
                else if (strncmp(p, "ButtonOnOff_", 12) == 0) { p += 12; on = JNI_TRUE; }
                else continue; // SeekBar/Spinner/Button/Category не трогаем
                if (getenv("K2C_MODS_OFF")) continue;
                fprintf(stderr, "[mods] Changes(cat=%d, \"%s\", on=1)\n", (int)cat, p);
                changes(env, ctx, cat, fake_jni_new_string(p), 0, val, on, fake_jni_new_string(""));
            }
        }
    }

    // Эмулируем конструктор UnityPlayer: initJni(context, contextType)
    typedef void (*initJni_fn)(JNIEnv, jobject, jobject, jint);
    initJni_fn initJni = (initJni_fn)fake_jni_get_native(
        "com/unity3d/player/UnityPlayer", "initJni", "(Landroid/content/Context;I)V");
    fprintf(stderr, "[m3] initJni = %p\n", (void *)initJni);
    if (!initJni) { g_worker_failed = 1; return NULL; }

    jobject unityPlayer = fake_jni_new_object("UnityPlayer");
    jobject context = fake_jni_new_object("android/app/Activity");
    fprintf(stderr, "[m3] === вызов initJni ===\n");
    initJni(env, unityPlayer, context, 0); // 0 = ActivityOrService
    fprintf(stderr, "[m3] initJni вернулся\n");

    // surfaceCreated → nativeRecreateGfxState(0, surface)
    typedef void (*recreate_fn)(JNIEnv, jobject, jint, jobject);
    recreate_fn recreate = (recreate_fn)fake_jni_get_native(
        "com/unity3d/player/UnityPlayerForActivityOrService",
        "nativeRecreateGfxState", "(ILandroid/view/Surface;)V");
    jobject surface = fake_jni_new_object("android/view/Surface");
    fprintf(stderr, "[m3] === nativeRecreateGfxState %p ===\n", (void *)recreate);
    if (recreate) recreate(env, unityPlayer, 0, surface);

    typedef void (*surfchg_fn)(JNIEnv, jobject);
    surfchg_fn surfchg = (surfchg_fn)fake_jni_get_native(
        "com/unity3d/player/UnityPlayerForActivityOrService", "nativeSendSurfaceChangedEvent", "()V");
    fprintf(stderr, "[m3] === nativeSendSurfaceChangedEvent %p ===\n", (void *)surfchg);
    if (surfchg) surfchg(env, unityPlayer);

    typedef void (*focus_fn)(JNIEnv, jobject, jboolean);
    focus_fn focus = (focus_fn)fake_jni_get_native(
        "com/unity3d/player/UnityPlayerForActivityOrService", "nativeFocusChanged", "(Z)V");
    fprintf(stderr, "[m3] === nativeFocusChanged %p ===\n", (void *)focus);
    if (focus) focus(env, unityPlayer, 1);

    typedef void (*resume_fn)(JNIEnv, jobject);
    resume_fn resume = (resume_fn)fake_jni_get_native(
        "com/unity3d/player/UnityPlayerForActivityOrService", "nativeResume", "()V");
    fprintf(stderr, "[m3] === nativeResume %p ===\n", (void *)resume);
    if (resume) resume(env, unityPlayer);

    pthread_t ticker;
    if (pthread_create(&ticker, NULL, vsync_ticker, NULL) == 0)
        pthread_detach(ticker);

    typedef jboolean (*render_fn)(JNIEnv, jobject);
    render_fn render = (render_fn)fake_jni_get_native(
        "com/unity3d/player/UnityPlayerForActivityOrService", "nativeRender", "()Z");
    fprintf(stderr, "[m3] === nativeRender loop %p ===\n", (void *)render);
    typedef jboolean (*inject_fn)(JNIEnv, jobject, jobject);
    inject_fn inject = (inject_fn)fake_jni_get_native(
        "com/unity3d/player/UnityPlayer", "nativeInjectEvent", "(Landroid/view/InputEvent;)Z");
    if (!inject) inject = (inject_fn)fake_jni_get_native(
        "com/unity3d/player/UnityPlayerForActivityOrService", "nativeInjectEvent", "(Landroid/view/InputEvent;)Z");
    fprintf(stderr, "[input] nativeInjectEvent = %p\n", (void *)inject);
    if (render) {
        unsigned long limit = 0;
        const char *max_frames = getenv("K2C_MAX_FRAMES");
        if (max_frames) limit = strtoul(max_frames, NULL, 10);
        struct timespec next_frame,report_time;
        clock_gettime(CLOCK_MONOTONIC,&next_frame);
        report_time=next_frame;
        unsigned long report_frame=0;
        for (unsigned long i = 0; !limit || i < limit; i++) {
            K2CInput event;
            static jlong down_ms;
            while (inject && k2c_display_take_input(&event)) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                jlong ms = (jlong)now.tv_sec * 1000 + now.tv_nsec / 1000000;
                if (event.action == 0) down_ms = ms;
                jobject motion = fake_jni_new_motion_event(event.action, event.x, event.y, ms, down_ms);
                jboolean accepted = inject(env, unityPlayer, motion);
                fake_jni_release_motion_event(motion);
                if (event.action != 2)
                    fprintf(stderr, "[input] action=%d x=%.1f y=%.1f accepted=%d\n",
                            event.action, event.x, event.y, accepted);
            }
            jboolean r = render(env, unityPlayer);
            if (!r) { fprintf(stderr, "[m3] nativeRender вернул false на кадре %lu\n", i); break; }
            if (i == 0 || (i + 1) % 300 == 0)
                fprintf(stderr, "[m3] rendered frames: %lu\n", i + 1);
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC,&now);
            long long elapsed=(now.tv_sec-report_time.tv_sec)*1000000000LL+now.tv_nsec-report_time.tv_nsec;
            if(elapsed>=5000000000LL) {
                fprintf(stderr,"[performance] Unity %.1f fps\n",(i+1-report_frame)*1e9/elapsed);
                report_time=now;report_frame=i+1;
            }
            // Unity already waits for vsync; sleep only the remainder of the frame budget.
            next_frame.tv_nsec+=16666667;
            if(next_frame.tv_nsec>=1000000000){++next_frame.tv_sec;next_frame.tv_nsec-=1000000000;}
            if(now.tv_sec<next_frame.tv_sec || (now.tv_sec==next_frame.tv_sec && now.tv_nsec<next_frame.tv_nsec))
                clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&next_frame,NULL);
            else next_frame=now;
        }
    }

    typedef jboolean (*pause_fn)(JNIEnv, jobject);
    pause_fn pause = (pause_fn)fake_jni_get_native(
        "com/unity3d/player/UnityPlayerForActivityOrService", "nativePause", "()Z");
    if (pause) pause(env, unityPlayer);

    fprintf(stderr, "[m3] worker done\n");
    return NULL;
}

static void *unity_worker(void *arg)
{
    unity_worker_run(arg);
    __atomic_store_n(&g_worker_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

#ifndef K2C_ENTRY_POINT
#define K2C_ENTRY_POINT main
#endif
int K2C_ENTRY_POINT(void)
{
    setenv("PULSE_PROP_media.role","x-maemo",1);
    setvbuf(stdout, NULL, _IONBF, 0);
    char hybris_paths[1024];
    snprintf(hybris_paths, sizeof hybris_paths, "%s:%s", HYBRIS_SYSTEM_PATH, getenv("K2C_LIB_ROOT") ? getenv("K2C_LIB_ROOT") : "./lib/arm64-v8a");
    setenv("HYBRIS_LD_LIBRARY_PATH", hybris_paths, 1);
    setenv("HYBRIS_ANDROID_SDK_VERSION", "29", 0);
    if (!getenv("WAYLAND_DISPLAY")) setenv("WAYLAND_DISPLAY", "/run/display/wayland-0", 0);
    if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/run/display", 0);
    hooks_install();
    const char *assets_root = getenv("K2C_ASSETS_ROOT");
    hooks_set_assets_root(assets_root && *assets_root ? assets_root : "./assets");

    // Prefer the device's bionic API; fall back to the host event loop.
    void *la = hybris_dlopen("/usr/libexec/droid-hybris/system/lib64/libandroid.so", 2 | 0x2);
    if (!la) la = hybris_dlopen("/system/lib64/libandroid.so", 2 | 0x2);
    if (la) {
        ALooper_prepare_p = hybris_dlsym(la, "ALooper_prepare");
        ALooper_pollAll_p = hybris_dlsym(la, "ALooper_pollAll");
        if (ALooper_prepare_p) {
            void *looper = ALooper_prepare_p(0);
            fprintf(stderr, "[m3] ALooper_prepare -> %p\n", looper);
        }
    } else {
        fprintf(stderr, "[m3] system libandroid unavailable; using host NDK event loop\n");
        k2c_android_fallback_enable();
        ALooper_prepare_p = k2c_android_fallback_symbol("ALooper_prepare");
        ALooper_pollAll_p = k2c_android_fallback_symbol("ALooper_pollAll");
        ALooper_prepare_p(0);
    }

    // A package path resolves into /opt/app, outside Android's default namespace.
    // Share already loaded system libraries, and permit this game's library directory.
    char library_root[PATH_MAX], namespace_paths[PATH_MAX + 1024], permitted_paths[PATH_MAX + 1024];
    const char *root = getenv("K2C_LIB_ROOT");
    if (!realpath(root && *root ? root : "./lib/arm64-v8a", library_root)) {
        fprintf(stderr, "[m3] каталог библиотек игры недоступен\n");
        return 1;
    }
    snprintf(namespace_paths, sizeof namespace_paths, "%s:%s:/system/lib64", library_root, HYBRIS_SYSTEM_PATH);
    // Sailjail binds /usr/share paths while the Android loader resolves package
    // files to /opt/app. Permit both names, scoped to this application's package.
    snprintf(permitted_paths, sizeof permitted_paths, "%s:/opt/app/" K2C_APP_ID "", library_root);
    g_game_namespace = hybris_create_namespace("k2c-game", namespace_paths,
                                               namespace_paths, 3, permitted_paths, NULL);
    if (!g_game_namespace) {
        fprintf(stderr, "[m3] не удалось создать пространство библиотек: %s\n", hybris_dlerror());
        return 1;
    }
    fprintf(stderr, "[m3] game namespace ready: %s\n", library_root);

    if (k2c_display_init() != 0) {
        fprintf(stderr, "[m3] wayland недоступен, продолжаю вслепую\n");
    }

    pthread_t worker;
    if (pthread_create(&worker, NULL, unity_worker, NULL) != 0) {
        fprintf(stderr, "[m3] не удалось создать worker\n");
        return 1;
    }

    // Главный поток = аналог UI-thread: только прокачка лупера и wayland.
    while (!__atomic_load_n(&g_worker_done, __ATOMIC_ACQUIRE)) {
        if (ALooper_pollAll_p) ALooper_pollAll_p(20, NULL, NULL, NULL);
        else usleep(20000);
        k2c_display_dispatch();
        k2c_display_present_staged();
    }
    pthread_join(worker, NULL);
    return g_worker_failed ? 1 : 0;
}
