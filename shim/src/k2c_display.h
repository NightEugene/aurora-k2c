// Wayland-клиент: поверхность xdg_toplevel + презентация кадров через wl_shm.
#ifndef K2C_DISPLAY_H
#define K2C_DISPLAY_H

// Embedded Qt presentation: register before starting the game; no native window is created.
typedef void (*K2CFrameSink)(void *, const void *, int, int, int);
void k2c_display_set_host(int width, int height, K2CFrameSink sink, void *context);
void k2c_display_host_input(int action, float x, float y);

int  k2c_display_init(void);     // подключение, окно (fullscreen)
void k2c_display_dispatch(void); // обработка событий + flush
// Скопировать кадр (RGBA byte order) в shm-буфер и отправить на экран.
void k2c_display_present(const void *pixels, int width, int height, int stride_bytes);
// Только скопировать в промежуточный буфер (безопасно из чужих потоков, без wayland).
void k2c_display_stage(const void *pixels, int width, int height, int stride_bytes);
// Отправить staged-кадр на экран (звать из главного потока, возвращает 1 если был кадр).
int  k2c_display_present_staged(void);
int  k2c_display_screen_w(void);
int  k2c_display_screen_h(void);
// Single-contact Android actions: down=0, up=1, move=2, cancel=3.
typedef struct { int action; float x, y; } K2CInput;
int k2c_display_take_input(K2CInput *event);

#endif
