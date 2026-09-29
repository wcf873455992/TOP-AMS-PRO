#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t id;
    int32_t forward_gpio;
    int32_t backward_gpio;
    int32_t load_time_ms;
    int32_t uload_time_ms;
    char    name[24];
    char    material[16];
    char    color[16];
    bool    reverse;      /* 方向反转：交换进料/退料 GPIO */
} filament_channel_t;

extern filament_channel_t g_channels[8];
extern int32_t g_current_channel;
extern bool    g_printer_sync;
extern int32_t g_slow_feed_pulse_ms;
extern int32_t g_slow_feed_gap_ms;
extern int32_t g_feed_timeout_ms;
extern int32_t g_uload_wait_timeout_ms;
extern int32_t g_buffer_switch_gpio;
extern int32_t g_buffer_feed_ms;
extern int32_t g_buffer_cooldown_ms;

void filament_init(void);
void filament_save_config(void);
void filament_load_config(void);

void filament_forward(int32_t ch);
void filament_backward(int32_t ch);

void filament_forward_async(int32_t ch);
void filament_backward_async(int32_t ch);
void filament_load_async(int32_t ch);

void filament_set_load_time(int32_t ch, int32_t ms);
void filament_set_uload_time(int32_t ch, int32_t ms);
void filament_set_material(int32_t ch, const char *mat);
const char* filament_get_material(int32_t ch);
void filament_set_color(int32_t ch, const char *color);
void filament_set_reverse(int32_t ch, bool reverse);
bool filament_get_reverse(int32_t ch);
const char* filament_get_color(int32_t ch);
int32_t filament_get_state(int32_t ch);

void filament_set_printer_sync(bool sync);
bool filament_get_printer_sync(void);

void filament_set_slow_feed(int32_t pulse_ms, int32_t gap_ms);
int32_t filament_get_slow_feed_pulse(void);
int32_t filament_get_slow_feed_gap(void);
void filament_set_feed_timeout(int32_t ms);
void filament_set_uload_wait_timeout(int32_t ms);
int32_t filament_get_uload_wait_timeout(void);

void filament_set_buffer_switch(int32_t gpio);
void filament_set_buffer_feed(int32_t ms);
void filament_set_buffer_cooldown(int32_t ms);

void filament_uload_start(void);
void filament_load_start(void);
void filament_buffer_start(void);

void filament_slow_test_async(int32_t ch, int32_t pulse_ms, int32_t gap_ms, int32_t duration_sec);


/* ---------- G-code 版手动换料（M620/M621） ---------- */
void filament_gcode_forward_async(int32_t ch);
void filament_gcode_backward_async(int32_t ch);
void filament_gcode_load_async(int32_t ch);

#ifdef __cplusplus
}
#endif

void filament_bed_signal_start(void);
