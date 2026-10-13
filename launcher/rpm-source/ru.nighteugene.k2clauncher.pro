TEMPLATE = app
TARGET = ru.nighteugene.k2clauncher
QT += core gui qml quick dbus
CONFIG += c++11 auroraapp
QMAKE_CXXFLAGS += -fPIC
QMAKE_LFLAGS += -Wl,--export-dynamic
SOURCES += main.cpp hybris_bridge.c
SOURCES += native/m3.c native/jni_fake.c native/hooks.c native/k2c_audio.c \
           native/k2c_display.c native/k2c_window.c native/xdg_shell_min.c native/android_compat.c
# This dependency anchor is loaded by Android's linker, so it must not link
# glibc or contain compiler-generated TLS/startup code.
android_anchor.target = libandroid.so
android_anchor.depends = $$PWD/native/android_compat_library.c
android_anchor.commands = $(CC) -shared -nostdlib -fPIC -fno-stack-protector -Wl,-soname,libandroid.so -Wl,--hash-style=both $$PWD/native/android_compat_library.c -o libandroid.so
QMAKE_EXTRA_TARGETS += android_anchor
PRE_TARGETDEPS += libandroid.so
QMAKE_CLEAN += libandroid.so
DEFINES += K2C_ENTRY_POINT=k2c_game_main
DEFINES += hybris_dlopen=k2c_hybris_dlopen hybris_dlsym=k2c_hybris_dlsym \
           hybris_dlerror=k2c_hybris_dlerror hybris_set_hook_callback=k2c_hybris_set_hook_callback \
           hybris_create_namespace=k2c_hybris_create_namespace hybris_dlopen_ext=k2c_hybris_dlopen_ext
QMAKE_CFLAGS += -fvisibility=default
RESOURCES += launcher.qrc
contains(CONFIG, builtin) {
    DEFINES += K2C_BUILTIN_GAME
    RESOURCES = game.qrc
    TARGET = ru.nighteugene.kingdom
    DISTFILES += ru.nighteugene.kingdom.desktop Game.qml
} else {
    DISTFILES += ru.nighteugene.k2clauncher.desktop Launcher.qml
}
AURORAAPP_ICONS = 86x86 108x108 128x128 172x172
LIBS += -lz -ldl -lpthread -lwayland-client
