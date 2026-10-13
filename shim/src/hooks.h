// Хуки libhybris: наши glibc-реализации android-API для bionic-мира.
// Регистрируются через hybris_set_hook_callback.
#ifndef K2C_HOOKS_H
#define K2C_HOOKS_H

// Вызывать ДО загрузки игровых библиотек.
void hooks_install(void);

// Путь к распакованным assets APK.
void hooks_set_assets_root(const char *path);

#endif
