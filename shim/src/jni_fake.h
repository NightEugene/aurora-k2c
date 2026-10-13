// Минимальные JNI-типы для фейкового JavaVM/JNIEnv (без NDK).
#ifndef JNI_FAKE_H
#define JNI_FAKE_H

#include <stdint.h>

typedef int32_t jint;
typedef int64_t jlong;
typedef int16_t jshort;
typedef int8_t jbyte;
typedef uint16_t jchar;
typedef uint8_t jboolean;
typedef float jfloat;
typedef double jdouble;
typedef jint jsize;

typedef void* jobject;
typedef void* jclass;
typedef void* jstring;
typedef void* jarray;
typedef void* jobjectArray;
typedef void* jbyteArray;
typedef void* jintArray;
typedef void* jbooleanArray;
typedef void* jcharArray;
typedef void* jshortArray;
typedef void* jlongArray;
typedef void* jfloatArray;
typedef void* jdoubleArray;
typedef void* jthrowable;
typedef void* jmethodID;
typedef void* jfieldID;
typedef void* jweak;

typedef union jvalue {
    jboolean z; jbyte b; jchar c; jshort s; jint i; jlong j;
    jfloat f; jdouble d; jobject l;
} jvalue;

typedef struct {
    char *name;
    char *signature;
    void *fnPtr;
} JNINativeMethod;

#define JNI_VERSION_1_1 0x00010001
#define JNI_VERSION_1_2 0x00010002
#define JNI_VERSION_1_4 0x00010004
#define JNI_VERSION_1_6 0x00010006

#define JNI_OK 0
#define JNI_ERR (-1)

#define JNI_FALSE 0
#define JNI_TRUE 1

// JNIEnv и JavaVM — указатели на указатели на таблицы функций.
typedef void* JNIEnv;
typedef void* JavaVM;

// Создать фейковые JavaVM/JNIEnv (glibc-куча, живут вечно).
JavaVM fake_jni_create_vm(void);
JNIEnv fake_jni_get_env(void);

// Включить подробный лог JNI-вызовов (по умолчанию вкл).
void fake_jni_set_verbose(int v);

// Найти native-метод, зарегистрированный через RegisterNatives.
// cls — имя в JNI-формате ("com/unity3d/player/UnityPlayer"), sig может быть NULL.
void *fake_jni_get_native(const char *cls, const char *name, const char *sig);

// Поиск native-метода по имени независимо от класса (для мод-библиотек).
void *fake_jni_find_native(const char *name, const char *sig);

// Доступ к содержимому фейк-строки и массива объектов.
const char *fake_jni_string_chars(jobject s);
jlong fake_jni_array_length(jobject arr);
jobject fake_jni_array_get(jobject arr, jlong idx);

// Создать фейковый jobject с именем (для Context/Surface и т.п.).
jobject fake_jni_new_object(const char *name);
jobject fake_jni_new_string(const char *s);
jobject fake_jni_new_motion_event(int action, float x, float y, jlong time_ms, jlong down_ms);
void fake_jni_release_motion_event(jobject event);

// Получить имя фейковой ссылки (для логов).
const char *fake_jni_ref_name(jobject obj);

#endif
