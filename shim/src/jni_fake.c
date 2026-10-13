// Фейковый JavaVM/JNIEnv: таблицы функций, логирование вызовов,
// фейковые ссылки (классы/методы/строки/объекты/массивы) — достаточные,
// чтобы Unity на C side могла работать без ART.
#include "jni_fake.h"
#include "dex_unity_classes.h"
#include "k2c_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>

static int g_verbose;
void fake_jni_set_verbose(int v) { g_verbose = v; }

#define LOG(...) do { if (g_verbose) { fprintf(stderr, "[jni] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---- фейковые ссылки ------------------------------------------------------

enum { TAG_CLASS = 1, TAG_METHOD, TAG_FIELD, TAG_STRING, TAG_OBJECT, TAG_ARRAY, TAG_BUFFER };

typedef struct FakeRef {
    uint32_t tag;
    char *name;       // имя класса/метода/строки
    char *sig;        // сигнатура метода
    void *data;       // содержимое строки / элементы массива / буфер
    jlong length;     // длина строки/массива/буфера
    jint elem_size;   // для массивов
    jclass cls;       // класс объекта
    unsigned references;
    struct FakeRef *next_motion;
} FakeRef;

typedef struct { int action; float x, y; jlong time_ms, down_ms; } FakeMotion;
static FakeRef *g_motion_refs[64];
static pthread_mutex_t g_motion_mutex = PTHREAD_MUTEX_INITIALIZER;

static unsigned motion_bucket(void *obj) { return ((uintptr_t)obj >> 4) % 64; }
static FakeRef *find_motion_locked(void *obj)
{
    for (FakeRef *r = g_motion_refs[motion_bucket(obj)]; r; r = r->next_motion)
        if (r == obj) return r;
    return NULL;
}

static FakeRef *as_ref(void *p) { return (FakeRef *)p; }

static FakeRef *fake_new(uint32_t tag, const char *name, const char *sig)
{
    FakeRef *r = calloc(1, sizeof(FakeRef));
    r->tag = tag;
    r->references = 1;
    r->name = name ? strdup(name) : NULL;
    r->sig = sig ? strdup(sig) : NULL;
    return r;
}

static jobject game_file(const char *leaf)
{
    // Executable TLS overlaps Android's reserved TLS slots under libhybris.
    // Build the path on the stack; fake_new copies it into the Java reference.
    const char *root=getenv("K2C_GAME_DATA_ROOT");
    if(!root || !*root) root=".";
    char name[1100];
    snprintf(name,sizeof name,"java/io/File:%s/%s",root,leaf);
    return fake_new(TAG_OBJECT,name,NULL);
}

jobject fake_jni_new_motion_event(int action, float x, float y, jlong time_ms, jlong down_ms)
{
    FakeRef *r = fake_new(TAG_OBJECT, "android/view/MotionEvent", NULL);
    FakeMotion *motion = malloc(sizeof *motion);
    *motion = (FakeMotion){ action, x, y, time_ms, down_ms };
    r->data = motion;
    pthread_mutex_lock(&g_motion_mutex);
    unsigned bucket = motion_bucket(r);
    r->next_motion = g_motion_refs[bucket];
    g_motion_refs[bucket] = r;
    pthread_mutex_unlock(&g_motion_mutex);
    return r;
}

static FakeMotion *motion_data(void *obj)
{
    // Legacy JNI stubs may return sentinel handles rather than FakeRef pointers.
    // Only inspect objects registered by the MotionEvent factory.
    pthread_mutex_lock(&g_motion_mutex);
    FakeRef *r = find_motion_locked(obj);
    FakeMotion *motion = r ? r->data : NULL;
    pthread_mutex_unlock(&g_motion_mutex);
    return motion;
}

void fake_jni_release_motion_event(jobject obj)
{
    pthread_mutex_lock(&g_motion_mutex);
    FakeRef **link = &g_motion_refs[motion_bucket(obj)];
    while (*link && *link != obj) link = &(*link)->next_motion;
    FakeRef *r = *link;
    int destroy = r && --r->references == 0;
    if (destroy) *link = r->next_motion;
    pthread_mutex_unlock(&g_motion_mutex);
    if (destroy) {
        free(r->data); free(r->name); free(r->sig); free(r);
    }
}

static const char *ref_desc(void *ref, char *buf, size_t n)
{
    if (!ref) return "null";
    FakeRef *r = ref;
    snprintf(buf, n, "{tag=%d %s}", r->tag, r->name ? r->name : "?");
    return buf;
}

// ---- реализации -----------------------------------------------------------

static JavaVM g_vm = NULL;
static JNIEnv g_env = NULL;

static jint impl_GetVersion(JNIEnv env) { (void)env; return JNI_VERSION_1_6; }

static jclass impl_FindClass(JNIEnv env, const char *name)
{
    (void)env;
    LOG("FindClass(%s)", name);
    // Классов com/unity3d/player/*, которых нет в dex, настоящий Android не найдёт —
    // Unity опирается на это при выборе fallback-веток (NDK Choreographer и пр.).
    if (strncmp(name, "com/unity3d/player/", 19) == 0) {
        int found = 0;
        for (unsigned i = 0; i < sizeof(kDexUnityClasses)/sizeof(kDexUnityClasses[0]); i++)
            if (strcmp(name, kDexUnityClasses[i]) == 0) { found = 1; break; }
        if (!found) {
            fprintf(stderr, "[jni] FindClass(%s) -> NULL (нет в dex, как на реальном Android)\n", name);
            return NULL;
        }
    }
    return fake_new(TAG_CLASS, name, NULL);
}

static jmethodID impl_GetMethodID(JNIEnv env, jclass cls, const char *name, const char *sig)
{
    (void)env;
    char b[128];
    LOG("GetMethodID(%s, %s, %s)", ref_desc(cls, b, sizeof b), name, sig);
    return fake_new(TAG_METHOD, name, sig);
}

static jmethodID impl_GetStaticMethodID(JNIEnv env, jclass cls, const char *name, const char *sig)
{
    (void)env;
    char b[128];
    LOG("GetStaticMethodID(%s, %s, %s)", ref_desc(cls, b, sizeof b), name, sig);
    return fake_new(TAG_METHOD, name, sig);
}

static jfieldID impl_GetFieldID(JNIEnv env, jclass cls, const char *name, const char *sig)
{
    (void)env; (void)cls;
    return fake_new(TAG_FIELD, name, sig);
}

static jfieldID impl_GetStaticFieldID(JNIEnv env, jclass cls, const char *name, const char *sig)
{
    (void)env; (void)cls;
    return fake_new(TAG_FIELD, name, sig);
}

struct RegisteredNative {
    char *cls, *name, *sig;
    void *fn;
};
static struct RegisteredNative g_natives[512];
static int g_natives_count = 0;

void *fake_jni_get_native(const char *cls, const char *name, const char *sig)
{
    for (int i = 0; i < g_natives_count; i++) {
        struct RegisteredNative *r = &g_natives[i];
        if (strcmp(r->cls, cls) == 0 && strcmp(r->name, name) == 0 &&
            (!sig || strcmp(r->sig, sig) == 0))
            return r->fn;
    }
    return NULL;
}

// Поиск native-метода по имени независимо от класса (для мод-библиотек).
void *fake_jni_find_native(const char *name, const char *sig)
{
    for (int i = 0; i < g_natives_count; i++) {
        struct RegisteredNative *r = &g_natives[i];
        if (strcmp(r->name, name) == 0 && (!sig || strcmp(r->sig, sig) == 0))
            return r->fn;
    }
    return NULL;
}

// Доступ к содержимому фейк-строки и массива объектов (для опроса модов).
const char *fake_jni_string_chars(jobject s)
{
    FakeRef *r = (FakeRef *)s;
    return r && r->tag == TAG_STRING ? (const char *)r->data : NULL;
}

jlong fake_jni_array_length(jobject arr)
{
    FakeRef *r = (FakeRef *)arr;
    return r && r->tag == TAG_ARRAY ? r->length : 0;
}

jobject fake_jni_array_get(jobject arr, jlong idx)
{
    FakeRef *r = (FakeRef *)arr;
    if (!r || r->tag != TAG_ARRAY || idx < 0 || idx >= r->length) return NULL;
    return ((jobject *)r->data)[idx];
}

static jint impl_RegisterNatives(JNIEnv env, jclass cls, const JNINativeMethod *methods, jint n)
{
    (void)env;
    char b[128];
    LOG("=== RegisterNatives for class %s ===", ref_desc(cls, b, sizeof b));
    FakeRef *c = (FakeRef *)cls;
    for (jint i = 0; i < n; i++) {
        fprintf(stderr, "[jni]   native %s %s -> %p\n", methods[i].name, methods[i].signature, methods[i].fnPtr);
        if (g_natives_count < 512) {
            struct RegisteredNative *r = &g_natives[g_natives_count++];
            r->cls = strdup(c && c->name ? c->name : "?");
            r->name = strdup(methods[i].name);
            r->sig = strdup(methods[i].signature);
            r->fn = methods[i].fnPtr;
        }
    }
    return JNI_OK;
}

static jstring impl_NewStringUTF(JNIEnv env, const char *bytes)
{
    (void)env;
    if (!bytes) return NULL;
    FakeRef *r = fake_new(TAG_STRING, NULL, NULL);
    r->data = strdup(bytes);
    r->length = (jlong)strlen(bytes);
    return r;
}

static const char *impl_GetStringUTFChars(JNIEnv env, jstring str, jboolean *isCopy)
{
    (void)env;
    FakeRef *r = (FakeRef *)str;
    if (isCopy) *isCopy = JNI_TRUE;
    const char *d = (r && r->data) ? (const char *)r->data : "";
    char b[128];
    LOG("GetStringUTFChars(%s) -> \"%s\"", ref_desc(str, b, sizeof b), d);
    return d;
}

static void impl_ReleaseStringUTFChars(JNIEnv env, jstring str, const char *chars)
{
    (void)env; (void)str; (void)chars;
}

static jsize impl_GetStringUTFLength(JNIEnv env, jstring str)
{
    (void)env;
    FakeRef *r = (FakeRef *)str;
    return r ? (jsize)r->length : 0;
}

static jsize impl_GetStringLength(JNIEnv env, jstring str)
{
    (void)env;
    FakeRef *r = (FakeRef *)str;
    return r ? (jsize)r->length : 0;
}

static jobject impl_NewGlobalRef(JNIEnv env, jobject obj)
{
    (void)env;
    pthread_mutex_lock(&g_motion_mutex);
    FakeRef *r = find_motion_locked(obj);
    if (r) r->references++;
    pthread_mutex_unlock(&g_motion_mutex);
    return obj;
}
static void impl_DeleteGlobalRef(JNIEnv env, jobject obj) { (void)env; fake_jni_release_motion_event(obj); }
static void impl_DeleteLocalRef(JNIEnv env, jobject obj) { (void)env; fake_jni_release_motion_event(obj); }
static jboolean impl_IsSameObject(JNIEnv env, jobject a, jobject b) { (void)env; return a == b; }

static const char *ref_name(void *p);

static jobject new_object_wrap(JNIEnv env, jclass cls, FakeRef *arg0)
{
    (void)env;
    const char *cn = ref_name(cls);
    if (strncmp(cn, "class:", 6) == 0) cn += 6;
    FakeRef *o = fake_new(TAG_OBJECT, cn, NULL);
    // конструктор обёртки (Scanner(InputStream) и т.п.) — наследуем данные
    if (arg0 && arg0->data) {
        o->data = arg0->data;
        o->length = arg0->length;
    }
    return o;
}
static jobject impl_NewObject(JNIEnv env, jclass cls, jmethodID mid, ...)
{
    (void)mid;
    va_list ap;
    va_start(ap, mid);
    FakeRef *arg0 = as_ref(va_arg(ap, jobject));
    va_end(ap);
    return new_object_wrap(env, cls, arg0);
}
static jobject impl_NewObjectV(JNIEnv env, jclass cls, jmethodID mid, va_list ap)
{
    (void)mid;
    return new_object_wrap(env, cls, as_ref(va_arg(ap, jobject)));
}
static jobject impl_NewObjectA(JNIEnv env, jclass cls, jmethodID mid, const jvalue *args)
{
    (void)mid;
    return new_object_wrap(env, cls, args ? as_ref(args[0].l) : NULL);
}

static jclass impl_GetObjectClass(JNIEnv env, jobject obj)
{
    (void)env;
    FakeRef *r = (FakeRef *)obj;
    if (r && r->cls) return r->cls;
    char nm[256];
    snprintf(nm, sizeof nm, "class:%s", (r && r->name) ? r->name : "unknown");
    return fake_new(TAG_CLASS, nm, NULL);
}

static jclass impl_GetSuperclass(JNIEnv env, jclass cls) { (void)env; (void)cls; return fake_new(TAG_CLASS, "java/lang/Object", NULL); }

static jobject impl_AllocObject(JNIEnv env, jclass cls)
{
    (void)env;
    FakeRef *r = fake_new(TAG_OBJECT, "object", NULL);
    r->cls = cls;
    return r;
}

static jboolean impl_ExceptionCheck(JNIEnv env) { (void)env; return JNI_FALSE; }
static void impl_ExceptionClear(JNIEnv env) { (void)env; }
static jthrowable impl_ExceptionOccurred(JNIEnv env) { (void)env; return NULL; }
static void impl_ExceptionDescribe(JNIEnv env) { (void)env; fprintf(stderr, "[jni] ExceptionDescribe\n"); }
static jint impl_ThrowNew(JNIEnv env, jclass cls, const char *msg)
{
    (void)env; (void)cls;
    fprintf(stderr, "[jni] !!! ThrowNew: %s\n", msg ? msg : "?");
    return JNI_OK;
}
static jint impl_PushLocalFrame(JNIEnv env, jint cap) { (void)env; (void)cap; return JNI_OK; }
static jobject impl_PopLocalFrame(JNIEnv env, jobject res) { (void)env; return res; }
static jint impl_EnsureLocalCapacity(JNIEnv env, jint cap) { (void)env; (void)cap; return JNI_OK; }
static jint impl_MonitorEnter(JNIEnv env, jobject obj) { (void)env; (void)obj; return JNI_OK; }
static jint impl_MonitorExit(JNIEnv env, jobject obj) { (void)env; (void)obj; return JNI_OK; }
static jint impl_GetJavaVM(JNIEnv env, JavaVM *vm) { (void)env; *vm = g_vm; return JNI_OK; }
static jint impl_GetObjectRefType(JNIEnv env, jobject obj) { (void)env; (void)obj; return 1; }

// --- массивы ---------------------------------------------------------------

static FakeRef *array_new(jint elem_size, jsize len)
{
    FakeRef *r = fake_new(TAG_ARRAY, NULL, NULL);
    r->elem_size = elem_size;
    r->length = len;
    r->data = calloc((size_t)(len > 0 ? len : 1), (size_t)elem_size);
    return r;
}

static jsize impl_GetArrayLength(JNIEnv env, jarray arr)
{
    (void)env;
    FakeRef *r = (FakeRef *)arr;
    return r ? (jsize)r->length : 0;
}

static jobjectArray impl_NewObjectArray(JNIEnv env, jsize len, jclass cls, jobject init)
{
    (void)env; (void)init;
    FakeRef *r = array_new((jint)sizeof(jobject), len);
    r->cls = cls;
    return r;
}

static jobject impl_GetObjectArrayElement(JNIEnv env, jobjectArray arr, jsize idx)
{
    (void)env;
    FakeRef *r = (FakeRef *)arr;
    return ((jobject *)r->data)[idx];
}

static void impl_SetObjectArrayElement(JNIEnv env, jobjectArray arr, jsize idx, jobject val)
{
    (void)env;
    FakeRef *r = (FakeRef *)arr;
    ((jobject *)r->data)[idx] = val;
}

#define ARRAY_IMPLS(NAME, TYPE) \
    static j##TYPE##Array impl_New##NAME##Array(JNIEnv env, jsize len) { (void)env; return array_new((jint)sizeof(j##TYPE), len); } \
    static j##TYPE *impl_Get##NAME##ArrayElements(JNIEnv env, j##TYPE##Array arr, jboolean *isCopy) { \
        (void)env; if (isCopy) *isCopy = JNI_FALSE; return (j##TYPE *)((FakeRef *)arr)->data; } \
    static void impl_Release##NAME##ArrayElements(JNIEnv env, j##TYPE##Array arr, j##TYPE *elems, jint mode) { (void)env; (void)arr; (void)elems; (void)mode; } \
    static void impl_Get##NAME##ArrayRegion(JNIEnv env, j##TYPE##Array arr, jsize start, jsize len, j##TYPE *buf) { \
        (void)env; memcpy(buf, (char *)((FakeRef *)arr)->data + start * (jsize)sizeof(j##TYPE), (size_t)len * sizeof(j##TYPE)); } \
    static void impl_Set##NAME##ArrayRegion(JNIEnv env, j##TYPE##Array arr, jsize start, jsize len, const j##TYPE *buf) { \
        (void)env; memcpy((char *)((FakeRef *)arr)->data + start * (jsize)sizeof(j##TYPE), buf, (size_t)len * sizeof(j##TYPE)); }

ARRAY_IMPLS(Boolean, boolean)
ARRAY_IMPLS(Byte, byte)
ARRAY_IMPLS(Char, char)
ARRAY_IMPLS(Short, short)
ARRAY_IMPLS(Int, int)
ARRAY_IMPLS(Long, long)
ARRAY_IMPLS(Float, float)
ARRAY_IMPLS(Double, double)

// --- direct byte buffer ----------------------------------------------------

static jobject impl_NewDirectByteBuffer(JNIEnv env, void *addr, jlong cap)
{
    (void)env;
    FakeRef *r = fake_new(TAG_BUFFER, "directbuffer", NULL);
    r->data = addr;
    r->length = cap;
    return r;
}

static void *impl_GetDirectBufferAddress(JNIEnv env, jobject buf)
{
    (void)env;
    return ((FakeRef *)buf)->data;
}

static jlong impl_GetDirectBufferCapacity(JNIEnv env, jobject buf)
{
    (void)env;
    return ((FakeRef *)buf)->length;
}

// --- поля объектов ------------------------------------------------------------

static jobject impl_GetObjectField(JNIEnv env, jobject obj, jfieldID fid)
{
    (void)obj;
    FakeRef *f = as_ref(fid);
    const char *fname = f && f->name ? f->name : "";
    fprintf(stderr, "[jni] GetObjectField(%s) on %s\n", fname, ref_name(obj));
    if (strcmp(fname, "dataDir") == 0 || strcmp(fname, "deviceProtectedDataDir") == 0 ||
        strcmp(fname, "credentialProtectedDataDir") == 0) {
        /* Keep path storage off ELF TLS: Android uses TPIDR_EL0+0x28 as its stack guard. */
        char path[1024];
        const char *root = getenv("K2C_GAME_DATA_ROOT");
        snprintf(path, sizeof(path), "%s/data", root && *root ? root : ".");
        return impl_NewStringUTF(env, path);
    }
    if (strcmp(fname, "sourceDir") == 0 || strcmp(fname, "publicSourceDir") == 0)
        return impl_NewStringUTF(env, getenv("K2C_APK_PATH") ? getenv("K2C_APK_PATH") : "./base.apk");
    if (strcmp(fname, "nativeLibraryDir") == 0)
        return impl_NewStringUTF(env, getenv("K2C_LIB_ROOT") ? getenv("K2C_LIB_ROOT") : "./lib/arm64-v8a");
    if (strcmp(fname, "packageName") == 0 || strcmp(fname, "processName") == 0)
        return impl_NewStringUTF(env, "com.rawfury.kingdom2crowns");
    if (strcmp(fname, "className") == 0 || strcmp(fname, "name") == 0)
        return impl_NewStringUTF(env, "com.unity3d.player.UnityPlayerActivity");
    if (strcmp(fname, "versionName") == 0)
        return impl_NewStringUTF(env, "2.4.0");
    if (strcmp(fname, "MANUFACTURER") == 0) return impl_NewStringUTF(env, "F-Plus");
    if (strcmp(fname, "MODEL") == 0) return impl_NewStringUTF(env, "Fplus-MP-67A27");
    if (strcmp(fname, "DEVICE") == 0) return impl_NewStringUTF(env, "Fplus-MP-67A27");
    if (strcmp(fname, "BRAND") == 0) return impl_NewStringUTF(env, "F-Plus");
    if (strcmp(fname, "PRODUCT") == 0) return impl_NewStringUTF(env, "Fplus-MP-67A27");
    if (strcmp(fname, "HARDWARE") == 0) return impl_NewStringUTF(env, "mt6789");
    if (strcmp(fname, "BOARD") == 0) return impl_NewStringUTF(env, "mt6789");
    if (strcmp(fname, "RELEASE") == 0) return impl_NewStringUTF(env, "12");
    if (strcmp(fname, "SDK") == 0) return impl_NewStringUTF(env, "31");
    if (strcmp(fname, "FINGERPRINT") == 0)
        return impl_NewStringUTF(env, "F-Plus/Fplus-MP-67A27/Fplus-MP-67A27:12/SP1A.210812.016/1789922063:user/release-keys");
    if (strcmp(fname, "SUPPORTED_ABIS") == 0)
        return impl_NewStringUTF(env, "arm64-v8a");
    static const struct { const char *field, *val; } svc[] = {
        {"DISPLAY_SERVICE","display"},{"WINDOW_SERVICE","window"},{"AUDIO_SERVICE","audio"},
        {"SENSOR_SERVICE","sensor"},{"VIBRATOR_SERVICE","vibrator"},{"POWER_SERVICE","power"},
        {"ACTIVITY_SERVICE","activity"},{"NOTIFICATION_SERVICE","notification"},
        {"INPUT_METHOD_SERVICE","input_method"},{"CLIPBOARD_SERVICE","clipboard"},
        {"LAYOUT_INFLATER_SERVICE","layout_inflater"},{"KEYGUARD_SERVICE","keyguard"},
        {"UI_MODE_SERVICE","uimode"},{"CONNECTIVITY_SERVICE","connectivity"},
        {"WIFI_SERVICE","wifi"},{"TELEPHONY_SERVICE","phone"},{"LOCATION_SERVICE","location"},
        {"MEDIA_ROUTER_SERVICE","media_router"},
    };
    for (unsigned i = 0; i < sizeof(svc)/sizeof(svc[0]); i++)
        if (strcmp(fname, svc[i].field) == 0)
            return impl_NewStringUTF(env, svc[i].val);
    if (strcmp(fname, "MEDIA_MOUNTED") == 0)
        return impl_NewStringUTF(env, "mounted");
    return fake_new(TAG_OBJECT, "field", NULL);
}

static jint impl_GetIntField(JNIEnv env, jobject obj, jfieldID fid)
{
    (void)env; (void)obj;
    FakeRef *f = as_ref(fid);
    const char *fname = f && f->name ? f->name : "";
    if (strncmp(fname, "jfield:", 7) == 0) fname += 7;
    if (strcmp(fname, "targetSdkVersion") == 0) return 31;
    if (strcmp(fname, "minSdkVersion") == 0) return 24;
    if (strcmp(fname, "SDK_INT") == 0) return 31;
    if (strcmp(fname, "uid") == 0) return 1000;
    if (strcmp(fname, "width") == 0 || strcmp(fname, "widthPixels") == 0) return k2c_display_screen_w();
    if (strcmp(fname, "height") == 0 || strcmp(fname, "heightPixels") == 0) return k2c_display_screen_h();
    if (strcmp(fname, "densityDpi") == 0) return 320;
    return 0;
}

static jfloat impl_GetFloatField(JNIEnv env, jobject obj, jfieldID fid)
{
    (void)env; (void)obj;
    FakeRef *f = as_ref(fid);
    const char *fname = f && f->name ? f->name : "";
    if (strcmp(fname, "density") == 0) return 2.0f;
    if (strcmp(fname, "scaledDensity") == 0) return 2.0f;
    if (strcmp(fname, "xdpi") == 0) return 320.0f;
    if (strcmp(fname, "ydpi") == 0) return 320.0f;
    return 0.0f;
}

static jlong impl_GetLongField(JNIEnv env, jobject obj, jfieldID fid)
{
    (void)env; (void)obj; (void)fid;
    return 0;
}

static jboolean impl_GetBooleanField(JNIEnv env, jobject obj, jfieldID fid)
{
    (void)env; (void)obj; (void)fid;
    return JNI_FALSE;
}

static jobject impl_GetStaticObjectField(JNIEnv env, jclass cls, jfieldID fid)
{
    (void)cls;
    return impl_GetObjectField(env, NULL, fid);
}

static jint impl_GetStaticIntField(JNIEnv env, jclass cls, jfieldID fid)
{
    (void)cls;
    return impl_GetIntField(env, NULL, fid);
}

static jboolean impl_GetStaticBooleanField(JNIEnv env, jclass cls, jfieldID fid)
{
    (void)env; (void)cls; (void)fid;
    return JNI_FALSE;
}

static jlong impl_GetStaticLongField(JNIEnv env, jclass cls, jfieldID fid)
{
    (void)env; (void)cls; (void)fid;
    return 0;
}

static void impl_SetIntField(JNIEnv env, jobject obj, jfieldID fid, jint v)
{
    (void)env; (void)obj; (void)fid; (void)v;
}
static void impl_SetLongField(JNIEnv env, jobject obj, jfieldID fid, jlong v)
{
    (void)env; (void)obj; (void)fid; (void)v;
}
static void impl_SetBooleanField(JNIEnv env, jobject obj, jfieldID fid, jboolean v)
{
    (void)env; (void)obj; (void)fid; (void)v;
}
static void impl_SetObjectField(JNIEnv env, jobject obj, jfieldID fid, jobject v)
{
    (void)env; (void)obj; (void)fid; (void)v;
}

static jboolean impl_IsInstanceOf(JNIEnv env, jobject obj, jclass cls)
{
    (void)env;
    FakeRef *c = cls;
    if (motion_data(obj) && c && c->name) {
        const char *name = c->name;
        if (strncmp(name, "class:", 6) == 0) name += 6;
        return strcmp(name, "android/view/MotionEvent") == 0 ||
               strcmp(name, "android.view.MotionEvent") == 0 ||
               strcmp(name, "android/view/InputEvent") == 0 ||
               strcmp(name, "android.view.InputEvent") == 0 ||
               strcmp(name, "java/lang/Object") == 0 || strcmp(name, "java.lang.Object") == 0;
    }
    return JNI_TRUE;
}

// --- вызовы методов: диспетчер эмуляции фреймворка ---------------------------

#include <stdarg.h>

static jclass impl_GetObjectClass(JNIEnv env, jobject obj);

// FromReflectedMethod/ToReflectedMethod и Field-варианты: наши FakeRef
// одновременно играют роль jobject и jmethodID/jfieldID — возвращаем как есть.
static jobject impl_FromReflected(JNIEnv env, jobject obj)
{
    (void)env;
    return obj;
}

static int ref_valid(void *p)
{
    if (!p || (uintptr_t)p < 0x10000) return 0;
    FakeRef *r = as_ref(p);
    return r->tag >= TAG_CLASS && r->tag <= TAG_BUFFER;
}

static const char *ref_name(void *p)
{
    if (!ref_valid(p)) return "invalid-ref";
    FakeRef *r = as_ref(p);
    return r->name ? r->name : "?";
}

// Эмуляция известных методов фреймворка. Возвращает NULL, если метод неизвестен —
// тогда вызывающий код вернёт дефолтный фейк.
static jobject framework_call_object(JNIEnv env, void *target, FakeRef *m, jvalue *args)
{
    const char *tname = ref_name(target);
    const char *meth = m ? m->name : "?";
    const char *sig = m ? m->sig : "";

    // ReflectionHelper.getMethodID/getFieldID/getConstructorID(Class, name, sig, static)
    // — запоминаем имя метода/поля в имени фейк-объекта, чтобы потом отвечать на invoke/get*.
    if ((strcmp(meth, "getMethodID") == 0 || strcmp(meth, "getFieldID") == 0 ||
         strcmp(meth, "getConstructorID") == 0) && args) {
        FakeRef *a1 = ref_valid(args[1].l) ? as_ref(args[1].l) : NULL;
        const char *mn = (a1 && a1->tag == TAG_STRING) ? (char *)a1->data : NULL;
        if (mn) {
            char buf[192];
            snprintf(buf, sizeof buf, "%s:%s", meth[3] == 'F' ? "jfield" : "jmeth", mn);
            return fake_new(TAG_OBJECT, buf, NULL);
        }
    }
    if (strcmp(meth, "getClassLoader") == 0)
        return fake_new(TAG_OBJECT, "java/lang/ClassLoader", NULL);
    if (strcmp(meth, "open") == 0 && args) {
        // AssetManager.open(name) → читаем файл из assets
        FakeRef *a0 = as_ref(args[0].l);
        const char *fname = (a0 && a0->tag == TAG_STRING) ? (char *)a0->data : NULL;
        if (fname) {
            const char *root = getenv("K2C_ASSETS_ROOT");
            if (!root) root = "./assets";
            char path[768];
            snprintf(path, sizeof path, "%s/%s", root, fname);
            FILE *f = fopen(path, "rb");
            fprintf(stderr, "[jni] framework: AssetManager.open(%s) -> %s\n", fname, f ? "ok" : "MISS");
            char nm[300];
            snprintf(nm, sizeof nm, "InputStream:%s", fname);
            FakeRef *o = fake_new(TAG_OBJECT, nm, NULL);
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                char *buf = malloc((size_t)sz + 1);
                if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) sz = 0;
                buf[sz] = 0;
                fclose(f);
                o->data = buf;
                o->length = sz;
            }
            return o;
        }
    }
    if (strcmp(meth, "next") == 0 || strcmp(meth, "nextLine") == 0) {
        FakeRef *t = as_ref(target);
        return impl_NewStringUTF(env, t && t->data ? (char *)t->data : "");
    }
    if (strcmp(meth, "hasNext") == 0)
        return target;
    if (strcmp(meth, "close") == 0)
        return target;
    if (strcmp(meth, "useDelimiter") == 0 || strcmp(meth, "useLocale") == 0 ||
        strcmp(meth, "useRadix") == 0 || strcmp(meth, "skip") == 0)
        return target;
    if (strcmp(meth, "getAssets") == 0) {
        fprintf(stderr, "[jni] framework: %s.getAssets()\n", tname);
        return fake_new(TAG_OBJECT, "android/content/res/AssetManager", NULL);
    }
    if (strcmp(meth, "getPackageName") == 0)
        return impl_NewStringUTF(env, "com.rawfury.kingdom2crowns");
    if (strcmp(meth, "getPackageCodePath") == 0)
        return impl_NewStringUTF(env, getenv("K2C_APK_PATH") ? getenv("K2C_APK_PATH") : "./base.apk");
    if (strcmp(meth, "getPackageResourcePath") == 0)
        return impl_NewStringUTF(env, getenv("K2C_DATA_ROOT") ? getenv("K2C_DATA_ROOT") : ".");
    if (strcmp(meth, "getOpPackageName") == 0)
        return impl_NewStringUTF(env, "com.rawfury.kingdom2crowns");
    if (strcmp(meth, "getPackageManager") == 0)
        return fake_new(TAG_OBJECT, "android/content/pm/PackageManager", NULL);
    if (strcmp(meth, "getApplicationInfo") == 0)
        return fake_new(TAG_OBJECT, "android/content/pm/ApplicationInfo", NULL);
    if (strcmp(meth, "getFilesDir") == 0)
        return game_file("files");
    if (strcmp(meth, "getCacheDir") == 0 || strcmp(meth, "getExternalCacheDir") == 0)
        return game_file("cache");
    if (strcmp(meth, "getDataDir") == 0)
        return game_file("data");
    if (strcmp(meth, "getObbDir") == 0)
        return game_file("obb");
    if (strcmp(meth, "getExternalFilesDir") == 0)
        return game_file("external");
    if (strcmp(meth, "getSystemService") == 0) {
        FakeRef *arg0 = args ? as_ref(args[0].l) : NULL;
        const char *svc = (arg0 && arg0->tag == TAG_STRING) ? (char *)arg0->data : "?";
        fprintf(stderr, "[jni] framework: getSystemService(%s)\n", svc);
        char nm[128];
        snprintf(nm, sizeof nm, "systemService:%s", svc);
        return fake_new(TAG_OBJECT, nm, NULL);
    }
    if (strcmp(meth, "getWindow") == 0)
        return fake_new(TAG_OBJECT, "android/view/Window", NULL);
    if (strcmp(meth, "getWindowManager") == 0)
        return fake_new(TAG_OBJECT, "android/view/WindowManager", NULL);
    if (strcmp(meth, "getContentResolver") == 0)
        return fake_new(TAG_OBJECT, "android/content/ContentResolver", NULL);
    if (strcmp(meth, "getResources") == 0)
        return fake_new(TAG_OBJECT, "android/content/res/Resources", NULL);
    if (strcmp(meth, "getMainLooper") == 0)
        return fake_new(TAG_OBJECT, "android/os/Looper", NULL);
    if (strcmp(meth, "getApplication") == 0)
        return fake_new(TAG_OBJECT, "android/app/Application", NULL);
    if (strcmp(meth, "getApplicationContext") == 0)
        return target;
    if (strcmp(meth, "getAbsolutePath") == 0) {
        // java/io/File → путь из имени объекта
        const char *colon = strchr(tname, ':');
        return impl_NewStringUTF(env, colon ? colon + 1 : tname);
    }
    if (strcmp(meth, "getPath") == 0) {
        const char *colon = strchr(tname, ':');
        return impl_NewStringUTF(env, colon ? colon + 1 : tname);
    }
    if (strcmp(meth, "findLibrary") == 0 && args) {
        // ClassLoader.findLibrary("unity") → полный путь к .so
        FakeRef *a0 = as_ref(args[0].l);
        const char *lib = (a0 && a0->tag == TAG_STRING && a0->data) ? (char *)a0->data : "";
        char path[512];
        snprintf(path, sizeof path, "%s/lib%s.so", getenv("K2C_LIB_ROOT") ? getenv("K2C_LIB_ROOT") : "./lib/arm64-v8a", lib);
        FILE *f = fopen(path, "rb");
        fprintf(stderr, "[jni] framework: findLibrary(%s) -> %s\n", lib, f ? path : "(null)");
        if (f) { fclose(f); return impl_NewStringUTF(env, path); }
        return NULL;
    }
    if (strcmp(meth, "getName") == 0 && strncmp(tname, "class:", 6) == 0)
        return impl_NewStringUTF(env, tname + 6);
    if (strcmp(meth, "append") == 0)
        return target;
    if (strcmp(meth, "toString") == 0)
        return impl_NewStringUTF(env, "");
    if (strcmp(meth, "trim") == 0)
        return target;
    if (strcmp(meth, "getClass") == 0)
        return (jobject)impl_GetObjectClass(env, target);
    (void)sig;
    return NULL;
}

static jint framework_call_int(JNIEnv env, void *target, FakeRef *m, jvalue *args)
{
    (void)env; (void)args;
    const char *meth = m ? m->name : "?";
    const char *tname = ref_name(target);
    FakeMotion *motion = motion_data(target);
    if (motion) {
        if (strcmp(meth, "getAction") == 0 || strcmp(meth, "getActionMasked") == 0) return motion->action;
        if (strcmp(meth, "getPointerCount") == 0) return 1;
        if (strcmp(meth, "getSource") == 0) return 0x1002;
        if (strcmp(meth, "getToolType") == 0) return 1;
        return 0; // pointer id/index, history, buttons, device id, meta state
    }
    if (strcmp(meth, "getDisplayId") == 0) return 0;
    if (strcmp(meth, "getRotation") == 0) return 0;
    // Field.getInt(...) на объекте из ReflectionHelper.getFieldID — имя поля в имени фейка
    if (strncmp(tname, "jfield:", 7) == 0) {
        const char *fn = tname + 7;
        if (strcmp(fn, "SDK_INT") == 0) return 31;
        if (strcmp(fn, "targetSdkVersion") == 0) return 31;
        return 0;
    }
    // FromReflectedField → GetIntField/CallIntMethod с mid="jfield:<имя>"
    if (strncmp(meth, "jfield:", 7) == 0) {
        const char *fn = meth + 7;
        if (strcmp(fn, "SDK_INT") == 0) return 31;
        if (strcmp(fn, "targetSdkVersion") == 0) return 31;
        return 0;
    }
    if (strcmp(meth, "getWidth") == 0) return k2c_display_screen_w();
    if (strcmp(meth, "getHeight") == 0) return k2c_display_screen_h();
    if (strcmp(meth, "describeContents") == 0) return 0;
    if (strcmp(meth, "checkSelfPermission") == 0) return 0;
    if (strcmp(meth, "length") == 0) {
        FakeRef *r = as_ref(target);
        return r && r->data ? (jint)strlen(r->data) : 0;
    }
    (void)tname;
    return 0;
}

static jboolean framework_call_boolean(JNIEnv env, void *target, FakeRef *m, jvalue *args)
{
    (void)env;
    const char *meth = m ? m->name : "?";
    if (strcmp(meth, "equals") == 0) {
        // String.equals: сравниваем содержимое фейк-строк
        FakeRef *t = ref_valid(target) ? as_ref(target) : NULL;
        FakeRef *a = (args && ref_valid(args[0].l)) ? as_ref(args[0].l) : NULL;
        if (t && t->tag == TAG_STRING && a && a->tag == TAG_STRING && t->data && a->data)
            return strcmp((char *)t->data, (char *)a->data) == 0 ? JNI_TRUE : JNI_FALSE;
        return target == (void *)a;
    }
    if (strcmp(meth, "hasSystemFeature") == 0) return JNI_FALSE;
    if (strcmp(meth, "exists") == 0) return JNI_TRUE;
    if (strcmp(meth, "mkdirs") == 0) return JNI_TRUE;
    if (strcmp(meth, "canRead") == 0) return JNI_TRUE;
    if (strcmp(meth, "isDirectory") == 0) return JNI_FALSE;
    if (strcmp(meth, "isEmpty") == 0) return JNI_FALSE;
    return JNI_FALSE;
}

static jlong framework_call_long(JNIEnv env, void *target, FakeRef *m, jvalue *args)
{
    (void)env; (void)args;
    FakeMotion *motion = motion_data(target);
    if (motion && m && m->name) {
        if (strcmp(m->name, "getEventTime") == 0) return motion->time_ms;
        if (strcmp(m->name, "getDownTime") == 0) return motion->down_ms;
    }
    return 0;
}

#define CALL_PROLOG() \
    FakeRef *m = as_ref(mid); \
    const char *meth = m && m->name ? m->name : "?"; \
    if (g_verbose) fprintf(stderr, "[jni-call] %s.%s %s\n", ref_name(target), meth, m && m->sig ? m->sig : "")

// object
static jobject call_object_common(JNIEnv env, void *target, jmethodID mid, jvalue *args)
{
    CALL_PROLOG();
    if (args && (strcmp(meth, "setMessage") == 0 || strcmp(meth, "setTitle") == 0)) {
        FakeRef *a0 = ref_valid(args[0].l) ? as_ref(args[0].l) : NULL;
        if (a0 && a0->data)
            fprintf(stderr, "[dialog] %s: %s\n", meth, (char *)a0->data);
    }
    jobject r = framework_call_object(env, target, m, args);
    if (r) return r;
    return fake_new(TAG_OBJECT, "ret", NULL);
}
static jobject impl_CallObjectMethod(JNIEnv env, jobject target, jmethodID mid, ...)
{
    va_list ap; va_start(ap, mid);
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    va_end(ap);
    return call_object_common(env, target, mid, args);
}
static jobject impl_CallObjectMethodV(JNIEnv env, jobject target, jmethodID mid, va_list ap)
{
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    return call_object_common(env, target, mid, args);
}
static jobject impl_CallObjectMethodA(JNIEnv env, jobject target, jmethodID mid, const jvalue *args)
{
    return call_object_common(env, target, mid, (jvalue *)args);
}

// int
static jint call_int_common(JNIEnv env, void *target, jmethodID mid, jvalue *args)
{
    CALL_PROLOG();
    return framework_call_int(env, target, m, args);
}
static jint impl_CallIntMethod(JNIEnv env, jobject target, jmethodID mid, ...)
{
    va_list ap; va_start(ap, mid);
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    va_end(ap);
    return call_int_common(env, target, mid, args);
}
static jint impl_CallIntMethodV(JNIEnv env, jobject target, jmethodID mid, va_list ap)
{
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    return call_int_common(env, target, mid, args);
}
static jint impl_CallIntMethodA(JNIEnv env, jobject target, jmethodID mid, const jvalue *args)
{
    return call_int_common(env, target, mid, (jvalue *)args);
}

// long
static jlong call_long_common(JNIEnv env, void *target, jmethodID mid, jvalue *args)
{
    CALL_PROLOG();
    return framework_call_long(env, target, m, args);
}
static jlong impl_CallLongMethod(JNIEnv env, jobject target, jmethodID mid, ...)
{
    va_list ap; va_start(ap, mid);
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    va_end(ap);
    return call_long_common(env, target, mid, args);
}
static jlong impl_CallLongMethodV(JNIEnv env, jobject target, jmethodID mid, va_list ap)
{
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    return call_long_common(env, target, mid, args);
}
static jlong impl_CallLongMethodA(JNIEnv env, jobject target, jmethodID mid, const jvalue *args)
{
    return call_long_common(env, target, mid, (jvalue *)args);
}

// boolean
static jboolean call_boolean_common(JNIEnv env, void *target, jmethodID mid, jvalue *args)
{
    CALL_PROLOG();
    return framework_call_boolean(env, target, m, args);
}
static jboolean impl_CallBooleanMethod(JNIEnv env, jobject target, jmethodID mid, ...)
{
    va_list ap; va_start(ap, mid);
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    va_end(ap);
    return call_boolean_common(env, target, mid, args);
}
static jboolean impl_CallBooleanMethodV(JNIEnv env, jobject target, jmethodID mid, va_list ap)
{
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    return call_boolean_common(env, target, mid, args);
}
static jboolean impl_CallBooleanMethodA(JNIEnv env, jobject target, jmethodID mid, const jvalue *args)
{
    return call_boolean_common(env, target, mid, (jvalue *)args);
}

// void
static void call_void_common(JNIEnv env, void *target, jmethodID mid, jvalue *args)
{
    CALL_PROLOG();
    (void)env; (void)args;
}
static void impl_CallVoidMethod(JNIEnv env, jobject target, jmethodID mid, ...)
{
    va_list ap; va_start(ap, mid);
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    va_end(ap);
    call_void_common(env, target, mid, args);
}
static void impl_CallVoidMethodV(JNIEnv env, jobject target, jmethodID mid, va_list ap)
{
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    call_void_common(env, target, mid, args);
}
static void impl_CallVoidMethodA(JNIEnv env, jobject target, jmethodID mid, const jvalue *args)
{
    call_void_common(env, target, mid, (jvalue *)args);
}

// --- static-вызовы ----------------------------------------------------------

static jobject call_static_object_common(JNIEnv env, jclass cls, jmethodID mid, jvalue *args)
{
    FakeRef *m = as_ref(mid);
    const char *meth = m && m->name ? m->name : "?";
    if (g_verbose) fprintf(stderr, "[jni-call] static %s.%s %s\n", ref_name(cls), meth, m && m->sig ? m->sig : "");
    if (strcmp(meth, "obtain") == 0 && args && motion_data(args[0].l)) {
        FakeMotion *src = motion_data(args[0].l);
        return fake_jni_new_motion_event(src->action, src->x, src->y, src->time_ms, src->down_ms);
    }
    // Class.forName("a.b.C") → фейковый jclass
    if (strcmp(meth, "forName") == 0 && args) {
        FakeRef *a0 = as_ref(args[0].l);
        if (a0 && a0->tag == TAG_STRING) {
            char nm[256];
            snprintf(nm, sizeof nm, "class:%s", (char *)a0->data);
            return fake_new(TAG_CLASS, nm, NULL);
        }
    }
    if (strcmp(meth, "valueOf") == 0)
        return fake_new(TAG_OBJECT, "java/lang/Long", NULL);
    if (strcmp(meth, "getExternalStorageState") == 0)
        return impl_NewStringUTF(env, "mounted");
    if (strcmp(meth, "getExternalStorageDirectory") == 0)
        return game_file("external");
    if (strcmp(meth, "getDataDirectory") == 0)
        return game_file("data");
    jobject r = framework_call_object(env, cls, m, args);
    if (r) return r;
    return fake_new(TAG_OBJECT, "ret", NULL);
}
static jobject impl_CallStaticObjectMethod(JNIEnv env, jclass cls, jmethodID mid, ...)
{
    va_list ap; va_start(ap, mid);
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    va_end(ap);
    return call_static_object_common(env, cls, mid, args);
}
static jobject impl_CallStaticObjectMethodV(JNIEnv env, jclass cls, jmethodID mid, va_list ap)
{
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    return call_static_object_common(env, cls, mid, args);
}
static jobject impl_CallStaticObjectMethodA(JNIEnv env, jclass cls, jmethodID mid, const jvalue *args)
{
    return call_static_object_common(env, cls, mid, (jvalue *)args);
}

static jint call_static_int_common(JNIEnv env, jclass cls, jmethodID mid, jvalue *args)
{
    FakeRef *m = as_ref(mid);
    const char *meth = m && m->name ? m->name : "?";
    if (g_verbose) fprintf(stderr, "[jni-call] static %s.%s %s\n", ref_name(cls), meth, m && m->sig ? m->sig : "");
    return framework_call_int(env, cls, m, args);
}
static jint impl_CallStaticIntMethod(JNIEnv env, jclass cls, jmethodID mid, ...)
{
    va_list ap; va_start(ap, mid);
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    va_end(ap);
    return call_static_int_common(env, cls, mid, args);
}
static jint impl_CallStaticIntMethodV(JNIEnv env, jclass cls, jmethodID mid, va_list ap)
{
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    return call_static_int_common(env, cls, mid, args);
}
static jint impl_CallStaticIntMethodA(JNIEnv env, jclass cls, jmethodID mid, const jvalue *args)
{
    return call_static_int_common(env, cls, mid, (jvalue *)args);
}

static jlong impl_CallStaticLongMethod(JNIEnv env, jclass cls, jmethodID mid, ...)
{
    FakeRef *m = as_ref(mid);
    if (g_verbose) fprintf(stderr, "[jni-call] static %s.%s\n", ref_name(cls), m && m->name ? m->name : "?");
    return 0;
}
static jlong impl_CallStaticLongMethodV(JNIEnv env, jclass cls, jmethodID mid, va_list ap)
{
    (void)ap;
    return impl_CallStaticLongMethod(env, cls, mid);
}
static jlong impl_CallStaticLongMethodA(JNIEnv env, jclass cls, jmethodID mid, const jvalue *args)
{
    (void)args;
    return impl_CallStaticLongMethod(env, cls, mid);
}

static jboolean call_static_boolean_common(JNIEnv env, jclass cls, jmethodID mid, jvalue *args)
{
    FakeRef *m = as_ref(mid);
    const char *meth = m && m->name ? m->name : "?";
    if (g_verbose) fprintf(stderr, "[jni-call] static %s.%s %s\n", ref_name(cls), meth, m && m->sig ? m->sig : "");
    return framework_call_boolean(env, cls, m, args);
}
static jboolean impl_CallStaticBooleanMethod(JNIEnv env, jclass cls, jmethodID mid, ...)
{
    va_list ap; va_start(ap, mid);
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    va_end(ap);
    return call_static_boolean_common(env, cls, mid, args);
}
static jboolean impl_CallStaticBooleanMethodV(JNIEnv env, jclass cls, jmethodID mid, va_list ap)
{
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    return call_static_boolean_common(env, cls, mid, args);
}
static jboolean impl_CallStaticBooleanMethodA(JNIEnv env, jclass cls, jmethodID mid, const jvalue *args)
{
    return call_static_boolean_common(env, cls, mid, (jvalue *)args);
}

static void call_static_void_common(JNIEnv env, jclass cls, jmethodID mid, jvalue *args)
{
    FakeRef *m = as_ref(mid);
    const char *meth = m && m->name ? m->name : "?";
    if (g_verbose) fprintf(stderr, "[jni-call] static %s.%s %s\n", ref_name(cls), meth, m && m->sig ? m->sig : "");
    (void)env; (void)args;
}
static void impl_CallStaticVoidMethod(JNIEnv env, jclass cls, jmethodID mid, ...)
{
    va_list ap; va_start(ap, mid);
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    va_end(ap);
    call_static_void_common(env, cls, mid, args);
}
static void impl_CallStaticVoidMethodV(JNIEnv env, jclass cls, jmethodID mid, va_list ap)
{
    jvalue args[8]; for (int i = 0; i < 8; i++) args[i].l = va_arg(ap, jobject);
    call_static_void_common(env, cls, mid, args);
}
static void impl_CallStaticVoidMethodA(JNIEnv env, jclass cls, jmethodID mid, const jvalue *args)
{
    call_static_void_common(env, cls, mid, (jvalue *)args);
}

// float/double/byte/short/char — лог + дефолт
static jfloat call_float_common(JNIEnv env, void *target, jmethodID mid)
{
    CALL_PROLOG();
    const char *meth2 = m && m->name ? m->name : "?";
    FakeMotion *motion = motion_data(target);
    if (motion) {
        if (strcmp(meth2, "getX") == 0 || strcmp(meth2, "getRawX") == 0) return motion->x;
        if (strcmp(meth2, "getY") == 0 || strcmp(meth2, "getRawY") == 0) return motion->y;
        if (strcmp(meth2, "getPressure") == 0) return motion->action == 1 || motion->action == 3 ? 0.f : 1.f;
        if (strcmp(meth2, "getSize") == 0 || strcmp(meth2, "getXPrecision") == 0 ||
            strcmp(meth2, "getYPrecision") == 0 || strcmp(meth2, "getTouchMajor") == 0 ||
            strcmp(meth2, "getTouchMinor") == 0) return 1.f;
        return 0.f;
    }
    if (strcmp(meth2, "getRefreshRate") == 0) return 60.f;
    (void)env;
    return 0.f;
}
static jfloat impl_CallFloatMethod(JNIEnv env, jobject target, jmethodID mid, ...) { return call_float_common(env, target, mid); }
static jfloat impl_CallFloatMethodV(JNIEnv env, jobject target, jmethodID mid, va_list ap) { (void)ap; return call_float_common(env, target, mid); }
static jfloat impl_CallFloatMethodA(JNIEnv env, jobject target, jmethodID mid, const jvalue *args) { (void)args; return call_float_common(env, target, mid); }
static jdouble impl_CallDoubleMethodV(JNIEnv env, jobject target, jmethodID mid, va_list ap) { (void)ap; return call_float_common(env, target, mid); }
static jdouble impl_CallDoubleMethodA(JNIEnv env, jobject target, jmethodID mid, const jvalue *args) { (void)args; return call_float_common(env, target, mid); }
static jdouble impl_CallDoubleMethod(JNIEnv env, jobject target, jmethodID mid, ...) { CALL_PROLOG(); return 0.0; }
static jbyte impl_CallByteMethod(JNIEnv env, jobject target, jmethodID mid, ...) { CALL_PROLOG(); return 0; }
static jshort impl_CallShortMethod(JNIEnv env, jobject target, jmethodID mid, ...) { CALL_PROLOG(); return 0; }
static jchar impl_CallCharMethod(JNIEnv env, jobject target, jmethodID mid, ...) { CALL_PROLOG(); return 0; }

// --- generic-заглушка для всего остального ----------------------------------

static void stub_generic(void)
{
    fprintf(stderr, "[jni] !!! вызван незареализованный JNI-стаб\n");
}

// ---- таблицы ----------------------------------------------------------------

// Имена слотов JNIEnv (после 4 reserved) — для отладки таблицы.
static const char *env_slot_names[] = {
    "GetVersion","DefineClass","FindClass","FromReflectedMethod","FromReflectedField",
    "ToReflectedMethod","GetSuperclass","IsAssignableFrom","ToReflectedField","Throw",
    "ThrowNew","ExceptionOccurred","ExceptionDescribe","ExceptionClear","FatalError",
    "PushLocalFrame","PopLocalFrame","NewGlobalRef","DeleteGlobalRef","DeleteLocalRef",
    "IsSameObject","NewLocalRef","EnsureLocalCapacity","AllocObject","NewObject",
    "NewObjectV","NewObjectA","GetObjectClass","IsInstanceOf","GetMethodID",
    "CallObjectMethod","CallObjectMethodV","CallObjectMethodA","CallBooleanMethod","CallBooleanMethodV",
    "CallBooleanMethodA","CallByteMethod","CallByteMethodV","CallByteMethodA","CallCharMethod",
    "CallCharMethodV","CallCharMethodA","CallShortMethod","CallShortMethodV","CallShortMethodA",
    "CallIntMethod","CallIntMethodV","CallIntMethodA","CallLongMethod","CallLongMethodV",
    "CallLongMethodA","CallFloatMethod","CallFloatMethodV","CallFloatMethodA","CallDoubleMethod",
    "CallDoubleMethodV","CallDoubleMethodA","CallVoidMethod","CallVoidMethodV","CallVoidMethodA",
    "CallNonvirtualObjectMethod","CallNonvirtualObjectMethodV","CallNonvirtualObjectMethodA",
    "CallNonvirtualBooleanMethod","CallNonvirtualBooleanMethodV","CallNonvirtualBooleanMethodA",
    "CallNonvirtualByteMethod","CallNonvirtualByteMethodV","CallNonvirtualByteMethodA",
    "CallNonvirtualCharMethod","CallNonvirtualCharMethodV","CallNonvirtualCharMethodA",
    "CallNonvirtualShortMethod","CallNonvirtualShortMethodV","CallNonvirtualShortMethodA",
    "CallNonvirtualIntMethod","CallNonvirtualIntMethodV","CallNonvirtualIntMethodA",
    "CallNonvirtualLongMethod","CallNonvirtualLongMethodV","CallNonvirtualLongMethodA",
    "CallNonvirtualFloatMethod","CallNonvirtualFloatMethodV","CallNonvirtualFloatMethodA",
    "CallNonvirtualDoubleMethod","CallNonvirtualDoubleMethodV","CallNonvirtualDoubleMethodA",
    "CallNonvirtualVoidMethod","CallNonvirtualVoidMethodV","CallNonvirtualVoidMethodA",
    "GetFieldID","GetObjectField","GetBooleanField","GetByteField","GetCharField",
    "GetShortField","GetIntField","GetLongField","GetFloatField","GetDoubleField",
    "SetObjectField","SetBooleanField","SetByteField","SetCharField","SetShortField",
    "SetIntField","SetLongField","SetFloatField","SetDoubleField","GetStaticMethodID",
    "CallStaticObjectMethod","CallStaticObjectMethodV","CallStaticObjectMethodA","CallStaticBooleanMethod","CallStaticBooleanMethodV",
    "CallStaticBooleanMethodA","CallStaticByteMethod","CallStaticByteMethodV","CallStaticByteMethodA","CallStaticCharMethod",
    "CallStaticCharMethodV","CallStaticCharMethodA","CallStaticShortMethod","CallStaticShortMethodV","CallStaticShortMethodA",
    "CallStaticIntMethod","CallStaticIntMethodV","CallStaticIntMethodA","CallStaticLongMethod","CallStaticLongMethodV",
    "CallStaticLongMethodA","CallStaticFloatMethod","CallStaticFloatMethodV","CallStaticFloatMethodA","CallStaticDoubleMethod",
    "CallStaticDoubleMethodV","CallStaticDoubleMethodA","CallStaticVoidMethod","CallStaticVoidMethodV","CallStaticVoidMethodA",
    "GetStaticFieldID","GetStaticObjectField","GetStaticBooleanField","GetStaticByteField","GetStaticCharField",
    "GetStaticShortField","GetStaticIntField","GetStaticLongField","GetStaticFloatField","GetStaticDoubleField",
    "SetStaticObjectField","SetStaticBooleanField","SetStaticByteField","SetStaticCharField","SetStaticShortField",
    "SetStaticIntField","SetStaticLongField","SetStaticFloatField","SetStaticDoubleField","NewString",
    "GetStringLength","GetStringChars","ReleaseStringChars","NewStringUTF","GetStringUTFLength",
    "GetStringUTFChars","ReleaseStringUTFChars","GetArrayLength","NewObjectArray","GetObjectArrayElement",
    "SetObjectArrayElement","NewBooleanArray","NewByteArray","NewCharArray","NewShortArray",
    "NewIntArray","NewLongArray","NewFloatArray","NewDoubleArray","GetBooleanArrayElements",
    "GetByteArrayElements","GetCharArrayElements","GetShortArrayElements","GetIntArrayElements","GetLongArrayElements",
    "GetFloatArrayElements","GetDoubleArrayElements","ReleaseBooleanArrayElements","ReleaseByteArrayElements","ReleaseCharArrayElements",
    "ReleaseShortArrayElements","ReleaseIntArrayElements","ReleaseLongArrayElements","ReleaseFloatArrayElements","ReleaseDoubleArrayElements",
    "GetBooleanArrayRegion","GetByteArrayRegion","GetCharArrayRegion","GetShortArrayRegion","GetIntArrayRegion",
    "GetLongArrayRegion","GetFloatArrayRegion","GetDoubleArrayRegion","SetBooleanArrayRegion","SetByteArrayRegion",
    "SetCharArrayRegion","SetShortArrayRegion","SetIntArrayRegion","SetLongArrayRegion","SetFloatArrayRegion",
    "SetDoubleArrayRegion","RegisterNatives","UnregisterNatives","MonitorEnter","MonitorExit",
    "GetJavaVM","GetStringRegion","GetStringUTFRegion","GetPrimitiveArrayCritical","ReleasePrimitiveArrayCritical",
    "GetStringCritical","ReleaseStringCritical","NewWeakGlobalRef","DeleteWeakGlobalRef","ExceptionCheck",
    "NewDirectByteBuffer","GetDirectBufferAddress","GetDirectBufferCapacity","GetObjectRefType",
};

#define ENV_SLOTS (4 + (int)(sizeof(env_slot_names) / sizeof(env_slot_names[0])))

static void **g_env_table = NULL;
static void *g_env_holder = NULL;

// Размещаем реализацию в слоте по имени (безопасно против ошибок порядка).
static void env_set(const char *name, void *fn)
{
    for (int i = 0; i < (int)(sizeof(env_slot_names) / sizeof(env_slot_names[0])); i++) {
        if (strcmp(env_slot_names[i], name) == 0) {
            g_env_table[4 + i] = fn;
            return;
        }
    }
    fprintf(stderr, "[jni] env_set: слот %s не найден!\n", name);
}

JNIEnv fake_jni_get_env(void)
{
    if (g_env) return g_env;
    g_env_table = malloc((size_t)ENV_SLOTS * sizeof(void *));
    for (int i = 0; i < ENV_SLOTS; i++)
        g_env_table[i] = stub_generic;
    g_env_table[0] = g_env_table[1] = g_env_table[2] = g_env_table[3] = NULL;

    env_set("GetVersion", impl_GetVersion);
    env_set("FindClass", impl_FindClass);
    env_set("FromReflectedMethod", impl_FromReflected);
    env_set("FromReflectedField", impl_FromReflected);
    env_set("ToReflectedMethod", impl_FromReflected);
    env_set("ToReflectedField", impl_FromReflected);
    env_set("GetSuperclass", impl_GetSuperclass);
    env_set("ThrowNew", impl_ThrowNew);
    env_set("ExceptionOccurred", impl_ExceptionOccurred);
    env_set("ExceptionDescribe", impl_ExceptionDescribe);
    env_set("ExceptionClear", impl_ExceptionClear);
    env_set("ExceptionCheck", impl_ExceptionCheck);
    env_set("PushLocalFrame", impl_PushLocalFrame);
    env_set("PopLocalFrame", impl_PopLocalFrame);
    env_set("NewGlobalRef", impl_NewGlobalRef);
    env_set("DeleteGlobalRef", impl_DeleteGlobalRef);
    env_set("DeleteLocalRef", impl_DeleteLocalRef);
    env_set("IsSameObject", impl_IsSameObject);
    env_set("NewLocalRef", impl_NewGlobalRef);
    env_set("EnsureLocalCapacity", impl_EnsureLocalCapacity);
    env_set("AllocObject", impl_AllocObject);
    env_set("NewObject", impl_NewObject);
    env_set("NewObjectV", impl_NewObjectV);
    env_set("NewObjectA", impl_NewObjectA);
    env_set("GetObjectClass", impl_GetObjectClass);
    env_set("GetMethodID", impl_GetMethodID);
    env_set("GetStaticMethodID", impl_GetStaticMethodID);
    env_set("GetFieldID", impl_GetFieldID);
    env_set("GetObjectField", impl_GetObjectField);
    env_set("GetIntField", impl_GetIntField);
    env_set("GetFloatField", impl_GetFloatField);
    env_set("GetLongField", impl_GetLongField);
    env_set("GetBooleanField", impl_GetBooleanField);
    env_set("GetStaticObjectField", impl_GetStaticObjectField);
    env_set("GetStaticIntField", impl_GetStaticIntField);
    env_set("GetStaticBooleanField", impl_GetStaticBooleanField);
    env_set("GetStaticLongField", impl_GetStaticLongField);
    env_set("SetIntField", impl_SetIntField);
    env_set("SetLongField", impl_SetLongField);
    env_set("SetBooleanField", impl_SetBooleanField);
    env_set("SetObjectField", impl_SetObjectField);
    env_set("IsInstanceOf", impl_IsInstanceOf);
    env_set("GetStaticFieldID", impl_GetStaticFieldID);
    env_set("CallVoidMethod", impl_CallVoidMethod);
    env_set("CallVoidMethodV", impl_CallVoidMethodV);
    env_set("CallVoidMethodA", impl_CallVoidMethodA);
    env_set("CallObjectMethodV", impl_CallObjectMethodV);
    env_set("CallObjectMethodA", impl_CallObjectMethodA);
    env_set("CallIntMethodV", impl_CallIntMethodV);
    env_set("CallIntMethodA", impl_CallIntMethodA);
    env_set("CallLongMethodV", impl_CallLongMethodV);
    env_set("CallLongMethodA", impl_CallLongMethodA);
    env_set("CallBooleanMethodV", impl_CallBooleanMethodV);
    env_set("CallBooleanMethodA", impl_CallBooleanMethodA);
    env_set("CallFloatMethod", impl_CallFloatMethod);
    env_set("CallFloatMethodV", impl_CallFloatMethodV);
    env_set("CallFloatMethodA", impl_CallFloatMethodA);
    env_set("CallDoubleMethodV", impl_CallDoubleMethodV);
    env_set("CallDoubleMethodA", impl_CallDoubleMethodA);
    env_set("CallDoubleMethod", impl_CallDoubleMethod);
    env_set("CallByteMethod", impl_CallByteMethod);
    env_set("CallShortMethod", impl_CallShortMethod);
    env_set("CallCharMethod", impl_CallCharMethod);
    env_set("CallStaticVoidMethodV", impl_CallStaticVoidMethodV);
    env_set("CallStaticVoidMethodA", impl_CallStaticVoidMethodA);
    env_set("CallStaticObjectMethodV", impl_CallStaticObjectMethodV);
    env_set("CallStaticObjectMethodA", impl_CallStaticObjectMethodA);
    env_set("CallStaticIntMethodV", impl_CallStaticIntMethodV);
    env_set("CallStaticIntMethodA", impl_CallStaticIntMethodA);
    env_set("CallStaticLongMethod", impl_CallStaticLongMethod);
    env_set("CallStaticLongMethodV", impl_CallStaticLongMethodV);
    env_set("CallStaticLongMethodA", impl_CallStaticLongMethodA);
    env_set("CallStaticBooleanMethodV", impl_CallStaticBooleanMethodV);
    env_set("CallStaticBooleanMethodA", impl_CallStaticBooleanMethodA);
    env_set("CallIntMethod", impl_CallIntMethod);
    env_set("CallLongMethod", impl_CallLongMethod);
    env_set("CallBooleanMethod", impl_CallBooleanMethod);
    env_set("CallObjectMethod", impl_CallObjectMethod);
    env_set("CallStaticVoidMethod", impl_CallStaticVoidMethod);
    env_set("CallStaticIntMethod", impl_CallStaticIntMethod);
    env_set("CallStaticObjectMethod", impl_CallStaticObjectMethod);
    env_set("CallStaticBooleanMethod", impl_CallStaticBooleanMethod);
    env_set("NewStringUTF", impl_NewStringUTF);
    env_set("GetStringUTFChars", impl_GetStringUTFChars);
    env_set("ReleaseStringUTFChars", impl_ReleaseStringUTFChars);
    env_set("GetStringUTFLength", impl_GetStringUTFLength);
    env_set("GetStringLength", impl_GetStringLength);
    env_set("GetArrayLength", impl_GetArrayLength);
    env_set("NewObjectArray", impl_NewObjectArray);
    env_set("GetObjectArrayElement", impl_GetObjectArrayElement);
    env_set("SetObjectArrayElement", impl_SetObjectArrayElement);
    env_set("NewBooleanArray", impl_NewBooleanArray);
    env_set("NewByteArray", impl_NewByteArray);
    env_set("NewCharArray", impl_NewCharArray);
    env_set("NewShortArray", impl_NewShortArray);
    env_set("NewIntArray", impl_NewIntArray);
    env_set("NewLongArray", impl_NewLongArray);
    env_set("NewFloatArray", impl_NewFloatArray);
    env_set("NewDoubleArray", impl_NewDoubleArray);
    env_set("GetBooleanArrayElements", impl_GetBooleanArrayElements);
    env_set("GetByteArrayElements", impl_GetByteArrayElements);
    env_set("GetCharArrayElements", impl_GetCharArrayElements);
    env_set("GetShortArrayElements", impl_GetShortArrayElements);
    env_set("GetIntArrayElements", impl_GetIntArrayElements);
    env_set("GetLongArrayElements", impl_GetLongArrayElements);
    env_set("GetFloatArrayElements", impl_GetFloatArrayElements);
    env_set("GetDoubleArrayElements", impl_GetDoubleArrayElements);
    env_set("ReleaseBooleanArrayElements", impl_ReleaseBooleanArrayElements);
    env_set("ReleaseByteArrayElements", impl_ReleaseByteArrayElements);
    env_set("ReleaseCharArrayElements", impl_ReleaseCharArrayElements);
    env_set("ReleaseShortArrayElements", impl_ReleaseShortArrayElements);
    env_set("ReleaseIntArrayElements", impl_ReleaseIntArrayElements);
    env_set("ReleaseLongArrayElements", impl_ReleaseLongArrayElements);
    env_set("ReleaseFloatArrayElements", impl_ReleaseFloatArrayElements);
    env_set("ReleaseDoubleArrayElements", impl_ReleaseDoubleArrayElements);
    env_set("GetBooleanArrayRegion", impl_GetBooleanArrayRegion);
    env_set("GetByteArrayRegion", impl_GetByteArrayRegion);
    env_set("GetCharArrayRegion", impl_GetCharArrayRegion);
    env_set("GetShortArrayRegion", impl_GetShortArrayRegion);
    env_set("GetIntArrayRegion", impl_GetIntArrayRegion);
    env_set("GetLongArrayRegion", impl_GetLongArrayRegion);
    env_set("GetFloatArrayRegion", impl_GetFloatArrayRegion);
    env_set("GetDoubleArrayRegion", impl_GetDoubleArrayRegion);
    env_set("SetBooleanArrayRegion", impl_SetBooleanArrayRegion);
    env_set("SetByteArrayRegion", impl_SetByteArrayRegion);
    env_set("SetCharArrayRegion", impl_SetCharArrayRegion);
    env_set("SetShortArrayRegion", impl_SetShortArrayRegion);
    env_set("SetIntArrayRegion", impl_SetIntArrayRegion);
    env_set("SetLongArrayRegion", impl_SetLongArrayRegion);
    env_set("SetFloatArrayRegion", impl_SetFloatArrayRegion);
    env_set("SetDoubleArrayRegion", impl_SetDoubleArrayRegion);
    env_set("RegisterNatives", impl_RegisterNatives);
    env_set("UnregisterNatives", impl_RegisterNatives);
    env_set("MonitorEnter", impl_MonitorEnter);
    env_set("MonitorExit", impl_MonitorExit);
    env_set("GetJavaVM", impl_GetJavaVM);
    env_set("NewDirectByteBuffer", impl_NewDirectByteBuffer);
    env_set("GetDirectBufferAddress", impl_GetDirectBufferAddress);
    env_set("GetDirectBufferCapacity", impl_GetDirectBufferCapacity);
    env_set("GetObjectRefType", impl_GetObjectRefType);

    g_env_holder = malloc(sizeof(void *));
    *(void ***)g_env_holder = g_env_table;
    g_env = g_env_holder;
    return g_env;
}

// ---- JavaVM ---------------------------------------------------------------

static jint vm_DestroyJavaVM(JavaVM vm) { (void)vm; return JNI_OK; }
static jint vm_AttachCurrentThread(JavaVM vm, void **penv, void *args)
{
    (void)vm; (void)args;
    *penv = fake_jni_get_env();
    return JNI_OK;
}
static jint vm_DetachCurrentThread(JavaVM vm) { (void)vm; return JNI_OK; }
static jint vm_GetEnv(JavaVM vm, void **penv, jint version)
{
    (void)vm; (void)version;
    *penv = fake_jni_get_env();
    return JNI_OK;
}
static jint vm_AttachCurrentThreadAsDaemon(JavaVM vm, void **penv, void *args)
{
    return vm_AttachCurrentThread(vm, penv, args);
}

JavaVM fake_jni_create_vm(void)
{
    if (g_vm) return g_vm;
    fake_jni_get_env();
    void **table = malloc(8 * sizeof(void *));
    table[0] = table[1] = table[2] = NULL;
    table[3] = vm_DestroyJavaVM;
    table[4] = vm_AttachCurrentThread;
    table[5] = vm_DetachCurrentThread;
    table[6] = vm_GetEnv;
    table[7] = vm_AttachCurrentThreadAsDaemon;
    void *holder = malloc(sizeof(void *));
    *(void ***)holder = table;
    g_vm = holder;
    return g_vm;
}

jobject fake_jni_new_object(const char *name)
{
    return fake_new(TAG_OBJECT, name, NULL);
}

jobject fake_jni_new_string(const char *s)
{
    FakeRef *r = fake_new(TAG_STRING, NULL, NULL);
    r->data = strdup(s ? s : "");
    r->length = (jlong)strlen((const char *)r->data);
    return r;
}

const char *fake_jni_ref_name(jobject obj)
{
    FakeRef *r = (FakeRef *)obj;
    return (r && r->name) ? r->name : "?";
}
