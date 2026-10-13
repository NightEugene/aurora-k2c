#ifndef K2C_ANDROID_COMPAT_H
#define K2C_ANDROID_COMPAT_H

/* Enabled only when the device has no usable system libandroid. */
void k2c_android_fallback_enable(void);
void *k2c_android_fallback_symbol(const char *name);

#endif
