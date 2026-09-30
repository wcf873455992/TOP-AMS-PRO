#include "filament.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bambu_mqtt.h"
#include "bambu_cmd.h"

/* ============================================================
 * 全局换料互斥锁 + 忙标志
 * 保护：GPIO 驱动、g_current_channel、g_target_channel_signal、
 *       以及 ams 状态机（退料/进料/G-code/主动手动任务）
 * ============================================================ */
#include "freertos/semphr.h"

static SemaphoreHandle_t s_filament_mutex = NULL;
static volatile bool     s_filament_busy  = false;

/* 最近一次已处理的 ams_status，防止 L_DONE 后重复进入 */
static volatile int      s_last_handled_ams = -1;

static void filament_mutex_init(void)
{
    if (!s_filament_mutex) {
        s_filament_mutex = xSemaphoreCreateMutex();
        configASSERT(s_filament_mutex);
    }
}

static bool filament_lock(uint32_t timeout_ms)
{
    if (!s_filament_mutex) return true;
    return xSemaphoreTake(s_filament_mutex,
                          pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void filament_unlock(void)
{
    if (s_filament_mutex) xSemaphoreGive(s_filament_mutex);
}

static const char* TAG = "filament";
static const char* NS  = "filament_cfg";

/* 退料进行中标志（多任务共享） */
static volatile bool g_uload_in_progress = false;

/* 热床信号通道（独立任务记录，避免错过短暂的 M140 S1~S8） */
static volatile int g_target_channel_signal = 0;

/* ============================================================
 * 按材料返回目标喷嘴温度
 * ============================================================ */
static int filament_target_temp_by_channel(int ch)
{
    if (ch < 1 || ch > 8) return 255;
    const char *mat = g_channels[ch - 1].material;
    if (!mat) return 255;
    if (strcmp(mat, "PLA") == 0)  return 220;
    if (strcmp(mat, "PETG") == 0) return 270;   /* 官方换料温度 */
    if (strcmp(mat, "ABS") == 0)  return 260;
    if (strcmp(mat, "TPU") == 0)  return 230;
    if (strcmp(mat, "PC") == 0)   return 270;
    if (strcmp(mat, "PA") == 0)   return 280;
    return 255;
}

/* ============================================================
 * 官方换料序列（复刻 plate_9.gcode）
 * 让固件自己按 M620.10 的 T<temp> 加热，不再主动发 M104
 * ============================================================ */

static void send_line(const char *code)
{
    char *p = bambu_cmd_run_gcode(code);
    if (!p) {
        ESP_LOGE(TAG, "构造 G-code 失败: %s", code);
        return;
    }
    bambu_send_cmd(p);
    free(p);
    /* 给固件一点处理时间，避免命令堆积 */
    vTaskDelay(pdMS_TO_TICKS(150));
}

/* 进料：M620 SnA → M620.10 ×2 → T{n} */
static void send_official_load_seq(int ch, int tar_temp)
{
    int slot = ch - 1;
    char buf[128];

    ESP_LOGI(TAG, "[官方] === 进料序列 ch=%d slot=%d temp=%d ===",
             ch, slot, tar_temp);

    /* 1. 请求换料 */
    snprintf(buf, sizeof(buf), "M620 S%dA", slot);
    send_line(buf);

    /* 2. 冲刷温度（旧料 / 新料） */
    snprintf(buf, sizeof(buf),
             "M620.10 A0 F299 L278 H0.4 T%d P%d S1", tar_temp, tar_temp);
    send_line(buf);

    snprintf(buf, sizeof(buf),
             "M620.10 A1 F299 L278 H0.4 T%d P%d S1", tar_temp, tar_temp);
    send_line(buf);

    /* 3. 冷却温度（比目标低 15°C，参考官方 C245/T270） */
    snprintf(buf, sizeof(buf), "M620.15 C%d", tar_temp > 15 ? tar_temp - 15 : tar_temp);
    send_line(buf);

    /* 4. 回抽参数 */
    send_line("M620.11 P0 L0 I0 E0");
    send_line("M620.11 K0 I0 R0");

    /* 5. ★★★ 切工具号（触发实际动作） */
    snprintf(buf, sizeof(buf), "T%d", slot);
    send_line(buf);

    /* 6. 等待运动完成 */
    send_line("M400");

    /* 7. 冲刷回抽 */
    send_line("M620.10 R2");

    /* 8. 关闭 AMS 主动送料，交给物理进给 */
    send_line("M628 S0");

    /* 9. 冲刷完成 */
    send_line("M629");

    /* 10. 动态补偿 */
    send_line("M983.3 F5 A0.4 R2");

    ESP_LOGI(TAG, "[官方] 进料序列发送完毕，等待 261/263");
}

/* 换料结束通知 */
static void send_official_m621(int ch)
{
    int slot = ch - 1;
    char buf[64];

    /* ★ 主动挤出 5mm 确保料到达喷嘴 */
    ESP_LOGI(TAG, "[官方] 主动挤出 5mm 检测通路");
    send_line("M83");
    send_line("G1 E5 F300");
    send_line("M400");
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* 发 M621 */
    snprintf(buf, sizeof(buf), "M621 S%dA", slot);
    send_line(buf);

    /* ★ 关键：M621 后立即发 M104，让固件认为"换料是流程的一部分" */
    int tar_temp = filament_target_temp_by_channel(ch);
    snprintf(buf, sizeof(buf), "M104 S%d", tar_temp);
    ESP_LOGI(TAG, "[官方] 换料后补 M104 S%d，避免弹窗", tar_temp);
    send_line(buf);
    send_line("M400");

    /* ★ 走擦嘴位，模拟官方后续动作 */
    send_line("G150.3");
    send_line("M400");

    ESP_LOGI(TAG, "[官方] M621 + M104 + G150.3 发送完毕");
}

/* 退料：M620 S65535 → T65535 → M621 S65535 */
static void send_official_unload_seq(void)
{
    ESP_LOGI(TAG, "[官方] === 退料序列 ===");
    send_line("M620 S65535");
    send_line("T65535");
    send_line("G150.1 F8000");
    ESP_LOGI(TAG, "[官方] 退料序列发送完毕，等待 260");
}

static void send_official_m621_unload(void)
{
    /* 退料后也补 M104 + M400，避免固件进入独立检测模式 */
    send_line("M621 S65535");
    send_line("M104 S0");        /* 退料后关加热 */
    send_line("M400");
    send_line("G150.3");
    send_line("M400");
}


/* ============================================================
 * ams_status 读取辅助
 * 有些 report 包不含 ams_status 字段，parse_report 会保留旧值或 -1
 * 这里 -1 时返回上一次有效值，避免打断状态机
 * ============================================================ */
static int s_last_valid_ams = 0;
static int read_ams_status(void)
{
    int v = g_bambu_status.ams_status;
    if (v < 0) return s_last_valid_ams;
    s_last_valid_ams = v;
    return v;
}

/* 原子地判断 ams 是否是新值 */
static bool ams_is_new(int ams)
{
    if (ams == s_last_handled_ams) return false;
    s_last_handled_ams = ams;
    return true;
}



/* ============================================================
 * 状态码描述
 * ============================================================ */
static const char* ams_status_desc(int s)
{
    switch (s) {
    case -1:  return "未知";
    case 0:   return "空闲";
    case 258: return "加热喷嘴";
    case 259: return "切断耗材丝";
    case 260: return "抽回耗材丝";
    case 261: return "送耗材到挤出机";
    case 262: return "咬入耗材丝";
    case 263: return "冲刷旧耗材";
    case 768: return "冲刷完成";
    default:  return "其他";
    }
}

static const char* hw_switch_desc(int s)
{
    switch (s) {
    case 0: return "未知";
    case 1: return "咬入中";
    case 2: return "无料";
    case 3: return "有料";
    default: return "其他";
    }
}

/* ============================================================
 * 全局变量
 * ============================================================ */
filament_channel_t g_channels[8] = {
    {1, 2,  3,  6000, 5000, "通道1", "PETG", "#000000", false,},
    {2, 10, 6,  6000, 5000, "通道2", "PETG", "#ffffff", false,},
    {3, 5,  4,  6000, 5000, "通道3", "PETG", "#ff0000", false,},
    {4, 8,  9,  6000, 5000, "通道4", "PETG", "#0000ff", false,},
    {5, -1, -1, 6000, 5000, "通道5", "PETG", "#00ff00", false,},
    {6, -1, -1, 6000, 5000, "通道6", "PETG", "#ffff00", false,},
    {7, -1, -1, 6000, 5000, "通道7", "PETG", "#808080", false,},
    {8, -1, -1, 6000, 5000, "通道8", "PETG", "#ffffff", false,},};

int32_t g_current_channel = 0;
bool    g_printer_sync = false;
int32_t g_slow_feed_pulse_ms = 30;
int32_t g_slow_feed_gap_ms   = 600;
int32_t g_buffer_switch_gpio = -1;
int32_t g_buffer_feed_ms     = 500;
int32_t g_buffer_cooldown_ms = 1000;
int32_t g_feed_timeout_ms    = 180000;
int32_t g_uload_wait_timeout_ms = 15000;   /* 退料等待超时，默认 15 秒 */

static int32_t g_channel_state[8] = {0};

int32_t filament_get_state(int32_t ch)
{
    if (ch < 1 || ch > 8) return 0;
    return g_channel_state[ch - 1];
}

/* ============================================================
 * 配置存取
 * ============================================================ */
void filament_set_material(int32_t ch, const char *mat)
{
    if (ch < 1 || ch > 8 || !mat) return;
    strncpy(g_channels[ch - 1].material, mat, sizeof(g_channels[ch - 1].material) - 1);
    g_channels[ch - 1].material[sizeof(g_channels[ch - 1].material) - 1] = '\0';
    filament_save_config();
}

void filament_set_color(int32_t ch, const char *color)
{
    if (ch < 1 || ch > 8 || !color) return;
    strncpy(g_channels[ch - 1].color, color, sizeof(g_channels[ch - 1].color) - 1);
    g_channels[ch - 1].color[sizeof(g_channels[ch - 1].color) - 1] = '\0';
    filament_save_config();
}

/* ============================================================
 * 方向切换
 * ============================================================ */
void filament_set_reverse(int32_t ch, bool reverse)
{
    if (ch < 1 || ch > 8) return;
    g_channels[ch - 1].reverse = reverse;
    filament_save_config();
    ESP_LOGI(TAG, "通道 %ld 方向: %s", (long)ch, reverse ? "反转" : "正常");
}

bool filament_get_reverse(int32_t ch)
{
    if (ch < 1 || ch > 8) return false;
    return g_channels[ch - 1].reverse;
}



const char* filament_get_color(int32_t ch)
{
    if (ch < 1 || ch > 8) return "";
    return g_channels[ch - 1].color;
}

const char* filament_get_material(int32_t ch)
{
    if (ch < 1 || ch > 8) return "";
    return g_channels[ch - 1].material;
}

void filament_set_printer_sync(bool sync)
{
    g_printer_sync = sync;
    filament_save_config();
    ESP_LOGI(TAG, "换料模式: %s", sync ? "主动" : "被动");
}

bool filament_get_printer_sync(void) { return g_printer_sync; }

void filament_set_slow_feed(int32_t pulse_ms, int32_t gap_ms)
{
    if (pulse_ms > 0 && pulse_ms < 500) g_slow_feed_pulse_ms = pulse_ms;
    if (gap_ms > 0 && gap_ms < 5000)    g_slow_feed_gap_ms   = gap_ms;
    filament_save_config();
}

int32_t filament_get_slow_feed_pulse(void) { return g_slow_feed_pulse_ms; }
int32_t filament_get_slow_feed_gap(void)   { return g_slow_feed_gap_ms; }

void filament_set_feed_timeout(int32_t ms)
{
    if (ms > 0 && ms <= 600000) {
        g_feed_timeout_ms = ms;
        filament_save_config();
    }
}

void filament_set_uload_wait_timeout(int32_t ms)
{
    if (ms >= 1000 && ms <= 120000) {
        g_uload_wait_timeout_ms = ms;
        filament_save_config();
        ESP_LOGI(TAG, "退料等待超时: %ld ms", (long)ms);
    }
}

int32_t filament_get_uload_wait_timeout(void)
{
    return g_uload_wait_timeout_ms;
}

void filament_set_buffer_switch(int32_t gpio) { g_buffer_switch_gpio = gpio; filament_save_config(); }
void filament_set_buffer_feed(int32_t ms)     { if (ms > 0 && ms < 10000) g_buffer_feed_ms = ms; filament_save_config(); }
void filament_set_buffer_cooldown(int32_t ms) { if (ms >= 0 && ms < 60000) g_buffer_cooldown_ms = ms; filament_save_config(); }

void filament_save_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_i32(h, "current",       g_current_channel);
    nvs_set_i32(h, "printer_sync",  g_printer_sync ? 1 : 0);
    nvs_set_i32(h, "slow_pulse",    g_slow_feed_pulse_ms);
    nvs_set_i32(h, "slow_gap",      g_slow_feed_gap_ms);
    nvs_set_i32(h, "buf_gpio",      g_buffer_switch_gpio);
    nvs_set_i32(h, "buf_feed",      g_buffer_feed_ms);
    nvs_set_i32(h, "buf_cool",      g_buffer_cooldown_ms);
    nvs_set_i32(h, "feed_timeout",  g_feed_timeout_ms);
    nvs_set_i32(h, "uload_wait",    g_uload_wait_timeout_ms);

    for (int i = 0; i < 8; i++) {
        char key[16];
        snprintf(key, sizeof(key), "lt%d", i);
        nvs_set_i32(h, key, g_channels[i].load_time_ms);
        snprintf(key, sizeof(key), "ut%d", i);
        nvs_set_i32(h, key, g_channels[i].uload_time_ms);
        snprintf(key, sizeof(key), "mat%d", i);
        nvs_set_str(h, key, g_channels[i].material);
        snprintf(key, sizeof(key), "col%d", i);
        nvs_set_str(h, key, g_channels[i].color);
        snprintf(key, sizeof(key), "rev%d", i);
        nvs_set_i32(h, key, g_channels[i].reverse ? 1 : 0);
    }

    nvs_commit(h);
    nvs_close(h);
}

void filament_load_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;

    int32_t val;
    int32_t cur = 0;
    if (nvs_get_i32(h, "current", &cur) == ESP_OK) g_current_channel = cur;

    int32_t sync = 0;
    if (nvs_get_i32(h, "printer_sync", &sync) == ESP_OK) g_printer_sync = (sync != 0);

    if (nvs_get_i32(h, "slow_pulse",   &val) == ESP_OK) g_slow_feed_pulse_ms = val;
    if (nvs_get_i32(h, "slow_gap",     &val) == ESP_OK) g_slow_feed_gap_ms   = val;
    if (nvs_get_i32(h, "buf_gpio",     &val) == ESP_OK) g_buffer_switch_gpio = val;
    if (nvs_get_i32(h, "buf_feed",     &val) == ESP_OK) g_buffer_feed_ms     = val;
    if (nvs_get_i32(h, "buf_cool",     &val) == ESP_OK) g_buffer_cooldown_ms = val;
    if (nvs_get_i32(h, "feed_timeout", &val) == ESP_OK) g_feed_timeout_ms    = val;
    if (nvs_get_i32(h, "uload_wait",   &val) == ESP_OK) g_uload_wait_timeout_ms = val;

    for (int i = 0; i < 8; i++) {
        char key[16];
        snprintf(key, sizeof(key), "lt%d", i);
        if (nvs_get_i32(h, key, &val) == ESP_OK) g_channels[i].load_time_ms = val;
        snprintf(key, sizeof(key), "ut%d", i);
        if (nvs_get_i32(h, key, &val) == ESP_OK) g_channels[i].uload_time_ms = val;
        snprintf(key, sizeof(key), "mat%d", i);
        size_t len = sizeof(g_channels[i].material);
        nvs_get_str(h, key, g_channels[i].material, &len);
        snprintf(key, sizeof(key), "col%d", i);
        len = sizeof(g_channels[i].color);
        nvs_get_str(h, key, g_channels[i].color, &len);
        snprintf(key, sizeof(key), "rev%d", i);
        if (nvs_get_i32(h, key, &val) == ESP_OK) {
            g_channels[i].reverse = (val != 0);
        }
    }

    nvs_close(h);

    if (g_current_channel >= 1 && g_current_channel <= 8) {
        g_channel_state[g_current_channel - 1] = 3;
        ESP_LOGI(TAG, "当前通道 %ld 状态: 使用中", (long)g_current_channel);
    }
}

/* ============================================================
 * GPIO 初始化
 * ============================================================ */
void filament_init(void)
{
    filament_mutex_init();

    for (int i = 0; i < 8; i++) {
        int fwd = g_channels[i].forward_gpio;
        int bwd = g_channels[i].backward_gpio;

        if (fwd >= 0 && fwd <= 10) {
            gpio_reset_pin((gpio_num_t)fwd);
            gpio_set_direction((gpio_num_t)fwd, GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)fwd, 0);
        }
        if (bwd >= 0 && bwd <= 10) {
            gpio_reset_pin((gpio_num_t)bwd);
            gpio_set_direction((gpio_num_t)bwd, GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)bwd, 0);
        }
    }

    if (g_buffer_switch_gpio >= 0 && g_buffer_switch_gpio <= 10) {
        gpio_reset_pin((gpio_num_t)g_buffer_switch_gpio);
        gpio_set_direction((gpio_num_t)g_buffer_switch_gpio, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)g_buffer_switch_gpio, GPIO_PULLUP_ONLY);
    }

    filament_load_config();
    ESP_LOGI(TAG, "GPIO 初始化完成，当前通道: %ld", (long)g_current_channel);
}

/* ============================================================
 * 物理进退料（阻塞）
 * ============================================================ */
void filament_forward(int32_t ch)
{
    if (ch < 1 || ch > 8) return;
    int idx = ch - 1;

    /* ★ 根据 reverse 选实际 GPIO */
    int gpio = g_channels[idx].reverse
             ? g_channels[idx].backward_gpio
             : g_channels[idx].forward_gpio;

    if (gpio < 0) return;

    filament_lock(portMAX_DELAY);

    g_channel_state[idx] = 1;
    ESP_LOGI(TAG, "通道 %d 进料 %d ms (GPIO=%d%s)",
             ch, g_channels[idx].load_time_ms, gpio,
             g_channels[idx].reverse ? " 反转" : "");
    gpio_set_level((gpio_num_t)gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(g_channels[idx].load_time_ms));
    gpio_set_level((gpio_num_t)gpio, 0);
    g_channel_state[idx] = 0;

    filament_unlock();
}

void filament_backward(int32_t ch)
{
    if (ch < 1 || ch > 8) return;
    int idx = ch - 1;

    /* ★ 根据 reverse 选实际 GPIO */
    int gpio = g_channels[idx].reverse
             ? g_channels[idx].forward_gpio
             : g_channels[idx].backward_gpio;

    if (gpio < 0) return;

    filament_lock(portMAX_DELAY);

    g_channel_state[idx] = 2;
    ESP_LOGI(TAG, "通道 %d 退料 %d ms (GPIO=%d%s)",
             ch, g_channels[idx].uload_time_ms, gpio,
             g_channels[idx].reverse ? " 反转" : "");
    gpio_set_level((gpio_num_t)gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(g_channels[idx].uload_time_ms));
    gpio_set_level((gpio_num_t)gpio, 0);
    g_channel_state[idx] = 0;

    filament_unlock();
}

/* ============================================================
 * 后台任务：退料（监听 ams=260，被动模式）
 * ============================================================ */
static void filament_uload_task(void *arg)
{
    int prev_ams = -1;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        int ams = read_ams_status();
        if (ams == prev_ams) continue;
        prev_ams = ams;

        if (ams != 260) continue;

        /* 被动模式下，如果手动任务正在跑，让手动任务处理 */
        if (!g_printer_sync && s_filament_busy) {
            continue;
        }

        ESP_LOGI(TAG, "[退料-被动] ams_status: %d (%s)", ams, ams_status_desc(ams));


        /* ★ 尝试拿锁，拿不到说明别的任务在动，跳过 */
        if (!filament_lock(1000)) {
            ESP_LOGD(TAG, "[退料] 换料忙，跳过本次 260");
            continue;
        }
        if (s_filament_busy) {
            ESP_LOGD(TAG, "[退料] 已有换料任务在跑，跳过");
            filament_unlock();
            continue;
        }
        s_filament_busy = true;
        filament_unlock();

        ESP_LOGI(TAG, "[退料-被动] 物理退通道 %ld (bwd_gpio=%d uload_ms=%d)",
                 (long)g_current_channel,
                 g_channels[g_current_channel - 1].backward_gpio,
                 g_channels[g_current_channel - 1].uload_time_ms);
        g_uload_in_progress = true;

        if (g_current_channel >= 1 && g_current_channel <= 8) {
            int idx = g_current_channel - 1;
            if (g_channels[idx].backward_gpio >= 0) {
                filament_backward(g_current_channel);
                ESP_LOGI(TAG, "[退料-被动] 物理退料完成");
            } else {
                ESP_LOGW(TAG, "[退料] 通道 %ld GPIO 未配置",
                         (long)g_current_channel);
            }
        } else {
            ESP_LOGW(TAG, "[退料] current=%ld 无效", (long)g_current_channel);
        }

        g_uload_in_progress = false;

        filament_lock(portMAX_DELAY);
        s_filament_busy = false;
        filament_unlock();
    }
}

void filament_uload_start(void)
{
    xTaskCreate(filament_uload_task, "fil_uload", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "退料监听任务已启动");
}

/* ============================================================
 * 后台任务：进料（监听 ams=261，被动模式）
 * ============================================================ */
static void filament_load_task(void *arg)
{
    enum { L_IDLE, L_FEED_FAST, L_WAIT_FLUSH, L_FEED_SLOW, L_DONE } state = L_IDLE;
    int target_ch = 0;
    int prev_ams = -1;
    int prev_hw  = -1;

      while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (g_printer_sync && s_filament_busy) {
            if (state != L_IDLE) state = L_IDLE;
            prev_ams = -1;
            prev_hw  = -1;
            continue;
        }

        /* 退料进行中，冻结进料任务 */
        if (g_uload_in_progress) {
            if (state != L_IDLE) {
                ESP_LOGI(TAG, "[进料] 退料进行中，暂停进料状态机");
                state = L_IDLE;
            }
            prev_ams = -1;
            prev_hw  = -1;
            continue;
        }

        int ams = g_bambu_status.ams_status;
        int hw  = g_bambu_status.hw_switch_state;

        if (ams != prev_ams) {
            if (ams == 261 || ams == 263 || ams == 0 || ams == 768) {
                int log_ch = (g_target_channel_signal >= 1 && g_target_channel_signal <= 8)

                             ? g_target_channel_signal : g_current_channel;

                ESP_LOGI(TAG, "[进料-被动] [ch=%d] ams_status: %d -> %d (%s)",

                         log_ch, prev_ams, ams, ams_status_desc(ams));
            }
            prev_ams = ams;
        }
        if (hw != prev_hw) {
            if (hw == 2 || hw == 3) {
                int log_ch = (g_target_channel_signal >= 1 && g_target_channel_signal <= 8)

                             ? g_target_channel_signal : g_current_channel;

                ESP_LOGI(TAG, "[进料-被动] [ch=%d] hw_switch: %d -> %d (%s)",

                         log_ch, prev_hw, hw, hw_switch_desc(hw));
            }
            prev_hw = hw;
        }

        switch (state) {
        case L_IDLE:
            /* ★ 用独立任务记录的信号（避免信号过早消失） */
            if (ams == 261 || ams == 263) {
                /* 主动模式：忽略 263，只等 261 */
                if (g_printer_sync && ams == 263) {
                    break;
                }
                /* ★ 同一个 ams 只处理一次，避免 L_DONE 后重复进入 */
                if (!ams_is_new(ams)) {
                    break;
                }

                /* ★ 如果退料任务还在跑，等它完成或超时 */
                if (!g_printer_sync && (g_uload_in_progress || s_filament_busy)) {
                    int waited = 0;
                    int limit = g_uload_wait_timeout_ms;
                    ESP_LOGI(TAG, "[进料] 等待退料完成，最多 %d ms", limit);
                    while ((g_uload_in_progress || s_filament_busy) && waited < limit) {
                        vTaskDelay(pdMS_TO_TICKS(200));
                        waited += 200;
                    }
                    if (g_uload_in_progress || s_filament_busy) {
                        ESP_LOGW(TAG, "[进料] 等退料完成超时（%d ms），继续主动送料", limit);
                    } else {
                        ESP_LOGI(TAG, "[进料] 退料已完成，继续主动送料");
                    }
                }

                if (!g_printer_sync) {
                    if (!filament_lock(1000)) {
                        ESP_LOGW(TAG, "[进料] 换料忙，跳过本次 %d", ams);
                        break;
                    }
                    s_filament_busy = true;
                    filament_unlock();
                }

                int sig = g_target_channel_signal;
                if (sig < 1 || sig > 8) sig = g_bambu_status.bed_target_temper;
                if (sig > 8) sig = g_current_channel;

                if (sig >= 1 && sig <= 8) {
                    target_ch = sig;
                } else if (g_current_channel >= 1 && g_current_channel <= 8) {
                    target_ch = g_current_channel;   /* ★ 回退到当前通道 */
                    ESP_LOGI(TAG, "[进料] 无信号，回退到当前通道 %d", target_ch);
                } else {
                    target_ch = 1;
                }

                ESP_LOGI(TAG, "[进料-被动] 检测到 %d，目标通道 %d（信号=%d）",
                         ams, target_ch, sig);


                /* ★ 逻辑切换当前通道 */
                if (target_ch != g_current_channel) {
                    if (g_current_channel >= 1 && g_current_channel <= 8) {
                        g_channel_state[g_current_channel - 1] = 0;
                    }
                    g_current_channel = target_ch;
                    g_channel_state[target_ch - 1] = 3;
                    filament_save_config();
                    ESP_LOGI(TAG, "[进料-被动] 逻辑切换到通道 %d", target_ch);
                }
                if (ams == 261) {
                    state = L_FEED_FAST;
                } else {
                    /* 263：固件跳过 261（hw 已有料），直接缓慢进料 */
                    state = L_FEED_SLOW;
                }

                /* 用完清零，等下次换料记录新信号 */
                g_target_channel_signal = 0;
            }
            break;

        case L_FEED_FAST: {
            int idx = target_ch - 1;
            if (g_channels[idx].forward_gpio < 0) { state = L_WAIT_FLUSH; break; }

            ESP_LOGI(TAG, "[进料-被动] [ch=%d] 全速进料，等 hw==3", target_ch);
            uint32_t elapsed = 0;
            const uint32_t max_feed = g_feed_timeout_ms;
            const uint32_t step = 200;

            while (elapsed < max_feed) {
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
                vTaskDelay(pdMS_TO_TICKS(step));
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
                elapsed += step;
                if (g_bambu_status.hw_switch_state == 3) {
                    ESP_LOGI(TAG, "[进料-被动] [ch=%d] 有料，停止全速", target_ch);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (elapsed >= max_feed) {
                ESP_LOGW(TAG, "[进料-被动] [ch=%d] 全速超时");
                state = L_DONE;
                break;
            }
            state = L_WAIT_FLUSH;
            break;
        }

        case L_WAIT_FLUSH:
            if (ams == 262) {
                ESP_LOGI(TAG, "[进料-被动] [ch=%d] 262，缓慢送料 2 秒", target_ch);
                s_last_handled_ams = 262;
                state = L_FEED_SLOW;
            } else if (ams == 263) {
                ESP_LOGI(TAG, "[进料-被动] [ch=%d] 263，转缓慢", target_ch);
                s_last_handled_ams = 263;   /* ★ 标记 263 已处理 */
                state = L_FEED_SLOW;
            } else if (ams == 0) {
                state = L_DONE;
            }
            break;

        case L_FEED_SLOW: {
            int idx = target_ch - 1;
            if (g_channels[idx].forward_gpio < 0) { state = L_DONE; break; }

            ESP_LOGI(TAG, "[进料-被动] [ch=%d] 缓慢进料，等 0/768", target_ch);
            /* 262 进来时只送 2 秒，其他 90 秒 */
            bool is_262 = (s_last_handled_ams == 262);
            int64_t slow_start = esp_timer_get_time() / 1000;
            int64_t slow_limit_ms = is_262 ? 2000 : 90000;
            while (1) {
                int64_t now = esp_timer_get_time() / 1000;
                if (now - slow_start >= slow_limit_ms) {
                    if (is_262) ESP_LOGI(TAG, "[进料-被动] 262 缓慢送料 2 秒完成");
                    else ESP_LOGW(TAG, "[进料-被动] [ch=%d] 缓慢超时", target_ch);
                    break;
                }
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
                vTaskDelay(pdMS_TO_TICKS(g_slow_feed_pulse_ms));
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
                vTaskDelay(pdMS_TO_TICKS(g_slow_feed_gap_ms));

                int a = g_bambu_status.ams_status;
                if (a == 0 || a == 768) {
                    ESP_LOGI(TAG, "[进料-被动] [ch=%d] ams=%d，停止缓慢进料",
                             target_ch, a);
                    break;
                }
            }
            state = L_DONE;
            break;
        }

        case L_DONE:
            ESP_LOGI(TAG, "[进料-被动] [ch=%d] 完成", target_ch);
            /* ★ 兜底更新当前通道 */
            if (target_ch >= 1 && target_ch <= 8) {
                if (g_current_channel >= 1 && g_current_channel <= 8 &&
                    g_current_channel != target_ch) {
                    g_channel_state[g_current_channel - 1] = 0;
                }
                g_current_channel = target_ch;
                g_channel_state[target_ch - 1] = 3;
                filament_save_config();
            }
            /* ★ 重置已处理 ams，等下次换料 */
            s_last_handled_ams = -1;
            state = L_IDLE;
            filament_lock(portMAX_DELAY);
            s_filament_busy = false;
            filament_unlock();
            break;
        }
    }
}



/* ============================================================
 * 热床信号记录任务
 * 独立 100ms 轮询，记录 bed_target_temper 1~8 信号
 * 避免固件到 261/263 时信号已经消失（G4 S2 只有 2 秒）
 * ============================================================ */
static void bed_signal_task(void *arg)
{
    (void)arg;
    int prev_sig = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        int bed = g_bambu_status.bed_target_temper;

        if (bed >= 1 && bed <= 8) {
            if (bed != prev_sig) {
                ESP_LOGI(TAG, "[信号] 记录通道 = %d（当前通道 = %d）",
                     bed, (int)g_current_channel);
                g_target_channel_signal = bed;
                prev_sig = bed;
            }
        } else {
            /* 热床恢复（>8 或 0），保留信号供换料流程使用 */
            prev_sig = 0;
        }
    }
}

void filament_bed_signal_start(void)
{
    xTaskCreate(bed_signal_task, "bed_signal", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "热床信号任务已启动");
}

void filament_load_start(void)
{
    xTaskCreate(filament_load_task, "fil_load", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "进料监听任务已启动");
}

/* ============================================================
 * 手动进料（主动/被动）
 * ============================================================ */
static void filament_forward_manual_task(void *arg)
{
    int32_t ch = (int32_t)(intptr_t)arg;    if (ch < 1 || ch > 8) { vTaskDelete(NULL); return; }
    bool started_active = g_printer_sync;

    if (!filament_lock(2000)) {
        ESP_LOGW(TAG, "[进料-手动] 换料忙，放弃");
        vTaskDelete(NULL); return;
    }
    if (s_filament_busy) {
        ESP_LOGW(TAG, "[进料-手动] 已有换料任务在跑，放弃");
        filament_unlock();
        vTaskDelete(NULL); return;
    }
    s_filament_busy = true;
    filament_unlock();

    if (g_printer_sync) {
        ESP_LOGI(TAG, "[进料-主动] 通道 %ld：发 ams_change_filament", (long)ch);
        bambu_send_change_filament(ch, 1);

        ESP_LOGI(TAG, "[进料-主动] 等 261 或 263...");
        bool got = false;
        int timeout = 360;
        int last_ams = -1;
        while (timeout-- > 0) {
            /* ★ 切到被动模式，立即中止 */
            if (!g_printer_sync) {
                ESP_LOGW(TAG, "[手动] 切到被动模式，中止");
                filament_lock(portMAX_DELAY);
                s_filament_busy = false;
                filament_unlock();
                vTaskDelete(NULL);
                return;
            }
            int ams = g_bambu_status.ams_status;
            if (ams != last_ams) {
                ESP_LOGI(TAG, "[进料-主动] ams_status: %d -> %d (%s)",
                         last_ams, ams, ams_status_desc(ams));
                last_ams = ams;
            }
            if (ams == 261 || ams == 263) { got = true; break; }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (!got) {
            ESP_LOGW(TAG, "[进料-主动] 等待超时，中止");
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "[进料-主动] 检测到 %d，开始全速进料", last_ams);

        int idx = ch - 1;
        if (g_channels[idx].forward_gpio < 0) {
            ESP_LOGW(TAG, "[进料-主动] 通道 %ld 送料 GPIO 未配置", (long)ch);
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGI(TAG, "[进料-主动] 全速进料，等有料(hw==3)");
        uint32_t elapsed = 0;
        const uint32_t max_feed = g_feed_timeout_ms;
        const uint32_t step = 200;
        bool got_hw3 = false;

        while (elapsed < max_feed) {
            if (g_bambu_status.hw_switch_state == 3) {
                ESP_LOGI(TAG, "[进料-主动] 检测到有料(hw==3)，停止进料");
                got_hw3 = true;
                break;
            }
            gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
            vTaskDelay(pdMS_TO_TICKS(step));
            gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
            elapsed += step;
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (!got_hw3) {
            ESP_LOGW(TAG, "[进料-主动] 全速进料超时（%lu ms），停止",
                     (unsigned long)max_feed);
            g_current_channel = ch;
            g_channel_state[ch - 1] = 3;
            filament_save_config();
            ESP_LOGI(TAG, "[进料-主动] 通道 %ld 超时结束", (long)ch);
            vTaskDelete(NULL);
            return;
        }
                ESP_LOGI(TAG, "[进料-主动] 等 263 冲刷...");
        bool got_263 = false;
        timeout = 360;
        last_ams = -1;
        while (timeout-- > 0) {
            /* ★ 切到被动模式，立即中止 */
            if (!g_printer_sync) {
                ESP_LOGW(TAG, "[手动] 切到被动模式，中止");
                filament_lock(portMAX_DELAY);
                s_filament_busy = false;
                filament_unlock();
                vTaskDelete(NULL);
                return;
            }
            int ams = g_bambu_status.ams_status;
            if (ams != last_ams) {
                ESP_LOGI(TAG, "[进料-主动] ams_status: %d -> %d (%s)",
                         last_ams, ams, ams_status_desc(ams));
                last_ams = ams;
            }
            if (ams == 263) { got_263 = true; break; }
            if (ams == 0 || ams == 768) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (got_263) {
            ESP_LOGI(TAG, "[进料-主动] 缓慢进料，等空闲(0)");
            int timeout2 = 180;
            while (timeout2-- > 0) {
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
                vTaskDelay(pdMS_TO_TICKS(g_slow_feed_pulse_ms));
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
                vTaskDelay(pdMS_TO_TICKS(g_slow_feed_gap_ms));
                if (g_bambu_status.ams_status == 0) {
                    ESP_LOGI(TAG, "[进料-主动] ams 回到 0，停止缓慢进料");
                    break;
                }
            }
            if (timeout2 <= 0) ESP_LOGW(TAG, "[进料-主动] 缓慢进料超时");
        }
    } else {
        ESP_LOGI(TAG, "[进料-被动] 通道 %ld：物理进料", (long)ch);
        /* ★ 进料前先切换通道 */
        if (g_current_channel >= 1 && g_current_channel <= 8 &&
            g_current_channel != ch) {
            g_channel_state[g_current_channel - 1] = 0;
            ESP_LOGI(TAG, "[切通道] 清除通道 %ld 使用中状态",
                     (long)g_current_channel);
        }
        g_current_channel = ch;
        g_channel_state[ch - 1] = 1;
        filament_save_config();
        filament_forward(ch);
    }

    if (g_current_channel >= 1 && g_current_channel <= 8 &&
        g_current_channel != ch) {
        g_channel_state[g_current_channel - 1] = 0;
    }
    if (g_current_channel >= 1 && g_current_channel <= 8 &&
        g_current_channel != ch) {
        g_channel_state[g_current_channel - 1] = 0;
        ESP_LOGI(TAG, "[切通道] 清除通道 %ld 使用中状态",
                 (long)g_current_channel);
    }
    g_current_channel = ch;
    g_channel_state[ch - 1] = 3;
    filament_save_config();

    if (started_active) {
        ESP_LOGI(TAG, "[进料-主动] 通道 %ld 完成", (long)ch);
    } else {
        ESP_LOGI(TAG, "[进料-被动] 通道 %ld 完成", (long)ch);
    }

    filament_lock(portMAX_DELAY);
    s_filament_busy = false;
    filament_unlock();
    vTaskDelete(NULL);
}

/* ============================================================
 * 手动退料
 * ============================================================ */
static void filament_backward_manual_task(void *arg)
{
    int32_t ch = (int32_t)(intptr_t)arg;    if (ch < 1 || ch > 8) { vTaskDelete(NULL); return; }
    bool started_active = g_printer_sync;

    if (!filament_lock(2000)) {
        ESP_LOGW(TAG, "[退料-手动] 换料忙，放弃");
        vTaskDelete(NULL); return;
    }
    if (s_filament_busy) {
        ESP_LOGW(TAG, "[退料-手动] 已有换料任务在跑，放弃");
        filament_unlock();
        vTaskDelete(NULL); return;
    }
    s_filament_busy = true;
    filament_unlock();

    if (g_printer_sync) {
        ESP_LOGI(TAG, "[退料-主动] 通道 %ld：发 CMD_ULOAD", (long)ch);
bambu_send_change_filament(ch, 0);

        ESP_LOGI(TAG, "[退料-主动] 等打印机抽回耗材丝...");
        bool got_260 = false;
        int timeout = 360;
        int last_ams = -1;
        while (timeout-- > 0) {
            /* ★ 切到被动模式，立即中止 */
            if (!g_printer_sync) {
                ESP_LOGW(TAG, "[手动] 切到被动模式，中止");
                filament_lock(portMAX_DELAY);
                s_filament_busy = false;
                filament_unlock();
                vTaskDelete(NULL);
                return;
            }
            int ams = g_bambu_status.ams_status;
            if (ams != last_ams) {
                ESP_LOGI(TAG, "[退料-主动] ams_status: %d -> %d (%s)",
                         last_ams, ams, ams_status_desc(ams));
                last_ams = ams;
            }
            if (ams == 260) { got_260 = true; break; }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (!got_260) {
            ESP_LOGW(TAG, "[退料-主动] 等待 260 超时，跳过物理退料");
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "[退料-主动] 检测到 260，开始物理退料");
    } else {
        ESP_LOGI(TAG, "[退料-被动] 通道 %ld：物理退料", (long)ch);
        /* ★ 退料前先切换通道 */
        if (g_current_channel >= 1 && g_current_channel <= 8 &&
            g_current_channel != ch) {
            g_channel_state[g_current_channel - 1] = 0;
            ESP_LOGI(TAG, "[切通道] 清除通道 %ld 使用中状态",
                     (long)g_current_channel);
        }
        g_current_channel = ch;
        g_channel_state[ch - 1] = 2;
        filament_save_config();
    }

    filament_backward(ch);
    g_channel_state[ch - 1] = 0;

    /* ★ 不清 g_current_channel，保留"上次用的通道" */
    ESP_LOGI(TAG, "[退料] 通道 %ld 已退料，当前通道保留为 %ld",
             (long)ch, (long)g_current_channel);
    filament_save_config();

    if (started_active) {
        ESP_LOGI(TAG, "[退料-主动] 通道 %ld 完成", (long)ch);
    } else {
        ESP_LOGI(TAG, "[退料-被动] 通道 %ld 完成", (long)ch);
    }

    filament_lock(portMAX_DELAY);
    s_filament_busy = false;
    filament_unlock();
    vTaskDelete(NULL);
}

/* ============================================================
 * 手动装载（退当前 + 进目标）
 * ============================================================ */
static void filament_load_manual_task(void *arg)
{
    int32_t ch = (int32_t)(intptr_t)arg;    if (ch < 1 || ch > 8) { vTaskDelete(NULL); return; }

    bool started_active = g_printer_sync;
    /* ★ 记录启动时的模式（结尾日志用） */    if (!filament_lock(2000)) {
        ESP_LOGW(TAG, "[装载-手动] 换料忙，放弃");
        vTaskDelete(NULL); return;
    }
    if (s_filament_busy) {
        ESP_LOGW(TAG, "[装载-手动] 已有换料任务在跑，放弃");
        filament_unlock();
        vTaskDelete(NULL); return;
    }
    s_filament_busy = true;
    filament_unlock();

    if (g_printer_sync) {
        /* 退料：只退"当前使用中的通道"，而不是目标通道 ch */
        int old_ch = g_current_channel;
        if (old_ch < 1 || old_ch > 8) {
            ESP_LOGW(TAG, "[装载-主动] 当前无使用中通道，跳过退料，直接进料");
            /* 跳到进料部分：把 ch 设为目标通道，模拟已"清空旧料"状态 */
            goto ACTIVE_LOAD_ONLY;
        }

        /* ★ 通道相同，跳过换料，直接 resume（冗余设计） */
        if (old_ch == ch) {
            ESP_LOGI(TAG, "[装载-主动] 通道相同 (%d)，跳过换料", ch);
            goto SKIP_SWAP_RESUME;
        }

        ESP_LOGI(TAG, "[装载-主动] 当前使用通道 %d -> 目标通道 %ld：先退 %d",
                 old_ch, (long)ch, old_ch);
bambu_send_change_filament(old_ch, 0);

        ESP_LOGI(TAG, "[装载-主动] 等打印机抽回耗材丝(260)...");
        bool got_260 = false;
        int timeout = 360;
        int last_ams = -1;
        while (timeout-- > 0) {
            /* ★ 切到被动模式，立即中止 */
            if (!g_printer_sync) {
                ESP_LOGW(TAG, "[手动] 切到被动模式，中止");
                filament_lock(portMAX_DELAY);
                s_filament_busy = false;
                filament_unlock();
                vTaskDelete(NULL);
                return;
            }
            int ams = g_bambu_status.ams_status;
            if (ams != last_ams) {

                if (ams == 260 || ams == 261 || ams == 263 || ams == 768) {

                    ESP_LOGI(TAG, "[装载-主动] ams_status: %d -> %d (%s)",

                             last_ams, ams, ams_status_desc(ams));

                }

                last_ams = ams;

            }
            if (ams == 260) { got_260 = true; break; }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (!got_260) {
            ESP_LOGW(TAG, "[装载-主动] 等待 260 超时，中止");
            vTaskDelete(NULL);
            return;
        }

        if (g_current_channel >= 1 && g_current_channel <= 8) {
            int idx0 = g_current_channel - 1;
            ESP_LOGI(TAG, "[装载-主动] 物理退通道 %d (bwd_gpio=%d uload_ms=%d)",
                     old_ch,
                     g_channels[idx0].backward_gpio,
                     g_channels[idx0].uload_time_ms);
            filament_backward(g_current_channel);
        }

        ESP_LOGI(TAG, "[装载-主动] 等打印机无料(hw==2)...");
        timeout = 360;
        while (timeout-- > 0) {
            if (g_bambu_status.hw_switch_state == 2) {
                ESP_LOGI(TAG, "[装载-主动] 打印机已无料");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (old_ch >= 1 && old_ch <= 8 && old_ch != ch) {
            g_channel_state[old_ch - 1] = 0;
            ESP_LOGI(TAG, "[装载] 清除通道 %d 使用中状态", old_ch);
        }
        g_current_channel= ch;
        g_channel_state[ch - 1] = 3;
        filament_save_config();

        ACTIVE_LOAD_ONLY:
        /* 若从退料跳过进来，仍需要把当前通道切到目标 */
        if (g_current_channel != ch) {
            if (g_current_channel >= 1 && g_current_channel <= 8) {
                g_channel_state[g_current_channel - 1] = 0;
            }
            g_current_channel = ch;
            g_channel_state[ch - 1] = 3;
            filament_save_config();
        }
        ESP_LOGI(TAG, "[装载-主动] 发 CMD_LOAD，目标通道 %ld", (long)ch);
        bambu_send_change_filament(ch, 1);

        int idx = ch - 1;
        if (g_channels[idx].forward_gpio < 0) {
            ESP_LOGW(TAG, "[装载-主动] 通道 %ld 送料 GPIO 未配置", (long)ch);
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGI(TAG, "[装载-主动] 全速进料，等有料(hw==3)");
        uint32_t elapsed = 0;
        const uint32_t max_feed = g_feed_timeout_ms;
        const uint32_t step = 200;
        bool got_hw3 = false;

        while (elapsed < max_feed) {
            if (g_bambu_status.hw_switch_state == 3) {
                ESP_LOGI(TAG, "[装载-主动] 检测到有料(hw==3)，停止进料");
                got_hw3 = true;
                break;
            }
            gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
            vTaskDelay(pdMS_TO_TICKS(step));
            gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
            elapsed += step;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!got_hw3) {
            ESP_LOGW(TAG, "[装载-主动] 全速进料超时（%lu ms），停止",
                     (unsigned long)max_feed);
            vTaskDelete(NULL);
            return;
        }
                ESP_LOGI(TAG, "[装载-主动] 等 263 冲刷...");
        bool got_263 = false;
        timeout = 360;
        last_ams = -1;
        while (timeout-- > 0) {
            /* ★ 切到被动模式，立即中止 */
            if (!g_printer_sync) {
                ESP_LOGW(TAG, "[手动] 切到被动模式，中止");
                filament_lock(portMAX_DELAY);
                s_filament_busy = false;
                filament_unlock();
                vTaskDelete(NULL);
                return;
            }
            int ams = g_bambu_status.ams_status;
            if (ams != last_ams) {

                if (ams == 260 || ams == 261 || ams == 263 || ams == 768) {

                    ESP_LOGI(TAG, "[装载-主动] ams_status: %d -> %d (%s)",

                             last_ams, ams, ams_status_desc(ams));

                }

                last_ams = ams;

            }
            if (ams == 263) { got_263 = true; break; }
            if (ams == 768 || ams == 0) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (got_263) {
            ESP_LOGI(TAG, "[装载-主动] 缓慢进料，等冲刷完成(768)");
            int timeout2 = 180;
            while (timeout2-- > 0) {
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
                vTaskDelay(pdMS_TO_TICKS(g_slow_feed_pulse_ms));
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
                vTaskDelay(pdMS_TO_TICKS(g_slow_feed_gap_ms));
                if (g_bambu_status.ams_status == 768) {
                    ESP_LOGI(TAG, "[装载-主动] 检测到 768 冲刷完成，停止");
                    break;
                }
                if (g_bambu_status.ams_status == 0) {
                    ESP_LOGI(TAG, "[装载-主动] ams 回到 0，停止");
                    break;
                }
            }
            if (timeout2 <= 0) ESP_LOGW(TAG, "[装载-主动] 等待冲刷完成超时");
        }

        ESP_LOGI(TAG, "[装载-主动] 完成");
    } else {
        ESP_LOGI(TAG, "[装载-被动] 目标通道 %ld，先退当前通道 %ld",
                 (long)ch, (long)g_current_channel);

        if (g_current_channel >= 1 && g_current_channel <= 8) {
            filament_backward(g_current_channel);
        } else {
            ESP_LOGW(TAG, "[装载-被动] 当前通道 %ld 无效，跳过退料",
                     (long)g_current_channel);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
        filament_forward(ch);

        if (g_current_channel >= 1 && g_current_channel <= 8 &&
            g_current_channel != ch) {
            g_channel_state[g_current_channel - 1] = 0;
            ESP_LOGI(TAG, "[装载] 清除通道 %ld 使用中状态",
                     (long)g_current_channel);
        }

        g_current_channel = ch;
        g_channel_state[ch - 1] = 3;
        filament_save_config();

        ESP_LOGI(TAG, "[装载-被动] 完成");
    }

    SKIP_SWAP_RESUME:
    /* ★ 主动模式：换料完成，等 1 秒让打印机稳定，然后自动 resume */
    if (started_active) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (strcmp(g_bambu_status.gcode_state, "PAUSE") == 0) {
            ESP_LOGI(TAG, "[主动] 换料完成，自动恢复打印");
            bambu_send_cmd(CMD_PRINT_RESUME);
        }
    }

    filament_lock(portMAX_DELAY);
    s_filament_busy = false;
    filament_unlock();
    vTaskDelete(NULL);
}

/* ============================================================
 * 异步启动接口
 * ============================================================ */
void filament_forward_async(int32_t ch)
{
    if (ch < 1 || ch > 8) return;
    xTaskCreate(filament_forward_manual_task, "fil_fwd", 4096,
                (void*)(intptr_t)ch, 5, NULL);
}

void filament_backward_async(int32_t ch)
{
    if (ch < 1 || ch > 8) return;
    xTaskCreate(filament_backward_manual_task, "fil_bwd", 4096,
                (void*)(intptr_t)ch, 5, NULL);
}

void filament_load_async(int32_t ch)
{
    if (ch < 1 || ch > 8) return;
    xTaskCreate(filament_load_manual_task, "fil_load_m", 4096,
                (void*)(intptr_t)ch, 5, NULL);
}

/* ============================================================
 * 兼容旧接口（同步版）
 * ============================================================ */
void filament_set_load_time(int32_t ch, int32_t ms)
{
    if (ch < 1 || ch > 8) return;
    g_channels[ch - 1].load_time_ms = ms;
    filament_save_config();
}

void filament_set_uload_time(int32_t ch, int32_t ms)
{
    if (ch < 1 || ch > 8) return;
    g_channels[ch - 1].uload_time_ms = ms;
    filament_save_config();
}

/* ============================================================
 * 缓冲送料（微动开关）
 * ============================================================ */
static void filament_buffer_task(void *arg)
{
    int64_t last_feed_time = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));

        if (g_buffer_switch_gpio < 0) continue;
        if (strcmp(g_bambu_status.gcode_state, "RUNNING") != 0) continue;
        if (g_current_channel < 1 || g_current_channel > 8) continue;

        int idx = g_current_channel - 1;
        if (g_channels[idx].forward_gpio < 0) continue;

        int level = gpio_get_level((gpio_num_t)g_buffer_switch_gpio);
        if (level != 0) continue;

        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_feed_time < g_buffer_cooldown_ms) continue;

        ESP_LOGI(TAG, "微动触发，送料 %ld ms", (long)g_buffer_feed_ms);
        g_channel_state[idx] = 1;
        gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(g_buffer_feed_ms));
        gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
        g_channel_state[idx] = 0;

        last_feed_time = esp_timer_get_time() / 1000;
    }
}

void filament_buffer_start(void)
{
    xTaskCreate(filament_buffer_task, "fil_buf", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "缓冲送料任务已启动");
}

/* ============================================================
 * 自动换料监听任务（主动模式）
 * 检测到 PAUSE + 换料信号 → 启动换料
 * ============================================================ */
static void auto_swap_monitor_task(void *arg)
{
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!g_printer_sync) continue;

        /* 检测到 PAUSE + 信号 */
        if (strcmp(g_bambu_status.gcode_state, "PAUSE") == 0 &&
            g_target_channel_signal >= 1 && g_target_channel_signal <= 8 &&
            !s_filament_busy) {

            int target = g_target_channel_signal;
            ESP_LOGI(TAG, "[主动] 检测到换料请求：通道 %d", target);

            g_target_channel_signal = 0;
            filament_load_async(target);
        }
    }
}

void filament_auto_swap_start(void)
{
    xTaskCreate(auto_swap_monitor_task, "auto_swap", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "自动换料监听任务已启动");
}




/* ============================================================
 * 缓慢进料测试（网页"测试"按钮用）
 * ============================================================ */
typedef struct {
    int32_t ch;
    int32_t pulse_ms;
    int32_t gap_ms;
    int32_t duration_sec;
} slow_test_arg_t;

static void filament_slow_test_task(void *arg)
{
    slow_test_arg_t *a = (slow_test_arg_t *)arg;
    if (!a) { vTaskDelete(NULL); return; }

    int ch = a->ch;
    int pulse = a->pulse_ms;
    int gap   = a->gap_ms;
    int dur   = a->duration_sec;
    free(a);

    if (ch < 1 || ch > 8) { vTaskDelete(NULL); return; }
    int idx = ch - 1;
    if (g_channels[idx].forward_gpio < 0) {
        ESP_LOGW(TAG, "[slow_test] ch=%d 未配置 GPIO", ch);
        vTaskDelete(NULL);
        return;
    }

    if (pulse <= 0) pulse = 30;
    if (gap   <  0) gap   = 600;
    if (dur   <= 0) dur   = 3;

    ESP_LOGI(TAG, "[slow_test] ch=%d pulse=%d gap=%d dur=%ds",
             ch, pulse, gap, dur);

    int64_t end_ms = (esp_timer_get_time() / 1000) + (int64_t)dur * 1000;
    while (esp_timer_get_time() / 1000 < end_ms) {
        g_channel_state[idx] = 1;
        gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(pulse));
        gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
        g_channel_state[idx] = 0;
        vTaskDelay(pdMS_TO_TICKS(gap));
    }

    ESP_LOGI(TAG, "[slow_test] ch=%d 完成", ch);
    vTaskDelete(NULL);
}

void filament_slow_test_async(int32_t ch, int32_t pulse_ms,
                              int32_t gap_ms, int32_t duration_sec)
{
    slow_test_arg_t *a = malloc(sizeof(slow_test_arg_t));
    if (!a) return;
    a->ch = ch;
    a->pulse_ms = pulse_ms;
    a->gap_ms = gap_ms;
    a->duration_sec = duration_sec;
    xTaskCreate(filament_slow_test_task, "fil_slow_test", 3072,
                a, 5, NULL);
}


/* ============================================================
 * G-code 版手动换料任务（M620/M621 系列）
 * ============================================================ */

static void filament_gcode_forward_task(void *arg)
{
    int32_t ch = (int32_t)(intptr_t)arg;    if (ch < 1 || ch > 8) { vTaskDelete(NULL); return; }
    bool started_active = g_printer_sync;
    int idx = ch - 1;

    if (!filament_lock(2000)) {
        ESP_LOGW(TAG, "[G进料] 换料忙，放弃");
        vTaskDelete(NULL); return;
    }
    if (s_filament_busy) {
        ESP_LOGW(TAG, "[G进料] 已有换料任务在跑，放弃");
        filament_unlock();
        vTaskDelete(NULL); return;
    }
    s_filament_busy = true;
    filament_unlock();

    int tar_temp = filament_target_temp_by_channel(ch);

    /* 发送官方换料序列 */
    send_official_load_seq(ch, tar_temp);

    /* 等 261 或 263 */
    ESP_LOGI(TAG, "[G进料] 等 261/263...");
    bool got = false;
    int timeout = 360, last_ams = -1;
    while (timeout-- > 0) {
        int ams = read_ams_status();
        if (ams != last_ams) {
            ESP_LOGI(TAG, "[G进料] ams: %d -> %d (%s)",
                     last_ams, ams, ams_status_desc(ams));
            last_ams = ams;
        }
        if (ams == 261 || ams == 263) { got = true; break; }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!got) { ESP_LOGW(TAG, "[G进料] 等待超时"); vTaskDelete(NULL); return; }

    if (g_channels[idx].forward_gpio < 0) {
        ESP_LOGW(TAG, "[G进料] ch=%ld GPIO 未配置", (long)ch);
        vTaskDelete(NULL); return;
    }

    ESP_LOGI(TAG, "[G进料] 全速进料，等 hw==3");
    uint32_t elapsed = 0;
    const uint32_t max_feed = g_feed_timeout_ms;
    const uint32_t step = 200;
    bool got_hw3 = false;
    while (elapsed < max_feed) {
        if (g_bambu_status.hw_switch_state == 3) { got_hw3 = true; break; }
        gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(step));
        gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
        elapsed += step;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!got_hw3) ESP_LOGW(TAG, "[G进料] 全速进料超时");

    timeout = 360; last_ams = -1;
    bool got_263 = false;
    while (timeout-- > 0) {
        int ams = read_ams_status();
        if (ams != last_ams) {
            ESP_LOGI(TAG, "[G进料] ams: %d -> %d (%s)",
                     last_ams, ams, ams_status_desc(ams));
            last_ams = ams;
        }
        if (ams == 263) { got_263 = true; break; }
        if (ams == 0 || ams == 768) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (got_263) {
        ESP_LOGI(TAG, "[G进料] 缓慢进料，等 0");
        int t2 = 180;
        while (t2-- > 0) {
            gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
            vTaskDelay(pdMS_TO_TICKS(g_slow_feed_pulse_ms));
            gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
            vTaskDelay(pdMS_TO_TICKS(g_slow_feed_gap_ms));
            if (read_ams_status() == 0) break;
        }
    }

    ESP_LOGI(TAG, "[G进料] 发送 M621 S%dA", (int)(ch - 1));
    send_official_m621(ch);

    if (g_current_channel >= 1 && g_current_channel <= 8 &&
        g_current_channel != ch) {
        g_channel_state[g_current_channel - 1] = 0;
    }
    if (g_current_channel >= 1 && g_current_channel <= 8 &&
        g_current_channel != ch) {
        g_channel_state[g_current_channel - 1] = 0;
        ESP_LOGI(TAG, "[切通道] 清除通道 %ld 使用中状态",
                 (long)g_current_channel);
    }
    g_current_channel = ch;
    g_channel_state[ch - 1] = 3;
    filament_save_config();
    ESP_LOGI(TAG, "[G进料] ch=%ld 完成", (long)ch);

    filament_lock(portMAX_DELAY);
    s_filament_busy = false;
    filament_unlock();
    vTaskDelete(NULL);
}

static void filament_gcode_backward_task(void *arg)
{
    (void)arg;

    if (!filament_lock(2000)) {
        ESP_LOGW(TAG, "[G退料] 换料忙，放弃");
        vTaskDelete(NULL); return;
    }
    if (s_filament_busy) {
        ESP_LOGW(TAG, "[G退料] 已有换料任务在跑，放弃");
        filament_unlock();
        vTaskDelete(NULL); return;
    }
    s_filament_busy = true;
    filament_unlock();

    /* 退料：官方序列 M620 S65535 → T65535 */
    send_official_unload_seq();

    ESP_LOGI(TAG, "[G退料] 等 260...");
    bool got_260 = false;
    int timeout = 360, last_ams = -1;
    while (timeout-- > 0) {
        int ams = read_ams_status();
        if (ams != last_ams) {
            ESP_LOGI(TAG, "[G退料] ams: %d -> %d (%s)",
                     last_ams, ams, ams_status_desc(ams));
            last_ams = ams;
        }
        if (ams == 260) { got_260 = true; break; }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!got_260) { ESP_LOGW(TAG, "[G退料] 超时"); vTaskDelete(NULL); return; }

    if (g_current_channel >= 1 && g_current_channel <= 8) {
        ESP_LOGI(TAG, "[G退料] 物理退通道 %ld", (long)g_current_channel);
        filament_backward(g_current_channel);
    }

    ESP_LOGI(TAG, "[G退料] 发送 M621 S65535");
    send_official_m621_unload();

    int old = g_current_channel;
    g_current_channel = 0;
    if (old >= 1 && old <= 8) g_channel_state[old - 1] = 0;
    filament_save_config();
    ESP_LOGI(TAG, "[G退料] 完成");

    filament_lock(portMAX_DELAY);
    s_filament_busy = false;
    filament_unlock();
    vTaskDelete(NULL);
}

static void filament_gcode_load_task(void *arg)
{
    int32_t ch = (int32_t)(intptr_t)arg;    if (ch < 1 || ch > 8) { vTaskDelete(NULL); return; }
    bool started_active = g_printer_sync;

    if (!filament_lock(2000)) {
        ESP_LOGW(TAG, "[G装载] 换料忙，放弃");
        vTaskDelete(NULL); return;
    }
    if (s_filament_busy) {
        ESP_LOGW(TAG, "[G装载] 已有换料任务在跑，放弃");
        filament_unlock();
        vTaskDelete(NULL); return;
    }
    s_filament_busy = true;
    filament_unlock();

    /* ========== 第一步：退当前料 ========== */
    ESP_LOGI(TAG, "[G装载] 第一步：退当前料");
    send_official_unload_seq();

    ESP_LOGI(TAG, "[G装载] 等 260...");
    bool got_260 = false;
    int timeout = 360, last_ams = -1;
    while (timeout-- > 0) {
        int ams = read_ams_status();
        if (ams != last_ams) {
            ESP_LOGI(TAG, "[G装载] ams: %d -> %d (%s)",
                     last_ams, ams, ams_status_desc(ams));
            last_ams = ams;
        }
        if (ams == 260) { got_260 = true; break; }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!got_260) { ESP_LOGW(TAG, "[G装载] 等 260 超时"); vTaskDelete(NULL); return; }

    if (g_current_channel >= 1 && g_current_channel <= 8) {
        ESP_LOGI(TAG, "[G装载] 物理退通道 %ld", (long)g_current_channel);
        filament_backward(g_current_channel);
    }

    ESP_LOGI(TAG, "[G装载] 发 M621 S65535");
    send_official_m621_unload();

    if (g_current_channel >= 1 && g_current_channel <= 8 &&
        g_current_channel != ch) {
        g_channel_state[g_current_channel - 1] = 0;
    }

    /* ===== 等待打印机完全空闲（关键！避免 07FF-8012）===== */
    ESP_LOGI(TAG, "[G装载] 等 ams 回 0（空闲）...");
    int idle_timeout = 120;   /* 最多 60 秒 */
    int last_idle_ams = -999;
    while (idle_timeout-- > 0) {
        int ams = read_ams_status();
        if (ams != last_idle_ams) {
            ESP_LOGI(TAG, "[G装载] ams: %d -> %d (%s)",
                     last_idle_ams, ams, ams_status_desc(ams));
            last_idle_ams = ams;
        }
        if (ams == 0) {
            ESP_LOGI(TAG, "[G装载] ams 已空闲，继续第二步");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (idle_timeout <= 0) {
        ESP_LOGW(TAG, "[G装载] 等空闲超时，仍继续");
    }
    /* 再给固件 2 秒重建 AMS 映射表 */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* ========== 第二步：进目标料 ========== */
    int tar_temp = filament_target_temp_by_channel(ch);
    ESP_LOGI(TAG, "[G装载] 第二步：进目标料 ch=%ld temp=%d", (long)ch, tar_temp);
    send_official_load_seq(ch, tar_temp);

    if (g_current_channel >= 1 && g_current_channel <= 8 &&
        g_current_channel != ch) {
        g_channel_state[g_current_channel - 1] = 0;
    }
    if (g_current_channel >= 1 && g_current_channel <= 8 &&
        g_current_channel != ch) {
        g_channel_state[g_current_channel - 1] = 0;
        ESP_LOGI(TAG, "[切通道] 清除通道 %ld 使用中状态",
                 (long)g_current_channel);
    }
    g_current_channel = ch;
    g_channel_state[ch - 1] = 3;
    filament_save_config();

    ESP_LOGI(TAG, "[G装载] 等 261/263...");
    bool got = false;
    timeout = 360; last_ams = -1;
    while (timeout-- > 0) {
        int ams = read_ams_status();
        if (ams != last_ams) {
            ESP_LOGI(TAG, "[G装载] ams: %d -> %d (%s)",
                     last_ams, ams, ams_status_desc(ams));
            last_ams = ams;
        }
        if (ams == 261 || ams == 263) { got = true; break; }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!got) { ESP_LOGW(TAG, "[G装载] 等 261/263 超时"); vTaskDelete(NULL); return; }

    int idx = ch - 1;
    if (g_channels[idx].forward_gpio < 0) {
        ESP_LOGW(TAG, "[G装载] ch=%ld GPIO 未配置", (long)ch);
        vTaskDelete(NULL); return;
    }

    ESP_LOGI(TAG, "[G装载] 全速进料，等 hw==3");
    uint32_t elapsed = 0;
    const uint32_t max_feed = g_feed_timeout_ms;
    const uint32_t step = 200;
    bool got_hw3 = false;
    while (elapsed < max_feed) {
        if (g_bambu_status.hw_switch_state == 3) { got_hw3 = true; break; }
        gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(step));
        gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
        elapsed += step;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!got_hw3) ESP_LOGW(TAG, "[G装载] 全速进料超时");

    timeout = 360; last_ams = -1;
    bool got_263 = false;
    while (timeout-- > 0) {
        int ams = read_ams_status();
        if (ams != last_ams) {
            ESP_LOGI(TAG, "[G装载] ams: %d -> %d (%s)",
                     last_ams, ams, ams_status_desc(ams));
            last_ams = ams;
        }
        if (ams == 263) { got_263 = true; break; }
        if (ams == 0 || ams == 768) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (got_263) {
        ESP_LOGI(TAG, "[G装载] 缓慢进料，等 0");
        int t2 = 180;
        while (t2-- > 0) {
            gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
            vTaskDelay(pdMS_TO_TICKS(g_slow_feed_pulse_ms));
            gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
            vTaskDelay(pdMS_TO_TICKS(g_slow_feed_gap_ms));
            if (read_ams_status() == 0) break;
        }
    }

    ESP_LOGI(TAG, "[G装载] 发 M621 S%dA", (int)(ch - 1));
    send_official_m621(ch);

    ESP_LOGI(TAG, "[G装载] ch=%ld 完成", (long)ch);

    filament_lock(portMAX_DELAY);
    s_filament_busy = false;
    filament_unlock();
    vTaskDelete(NULL);
}

void filament_gcode_forward_async(int32_t ch)
{
    if (ch < 1 || ch > 8) return;
    xTaskCreate(filament_gcode_forward_task, "g_fwd", 4096,
                (void*)(intptr_t)ch, 5, NULL);
}

void filament_gcode_backward_async(int32_t ch)
{
    (void)ch;
    xTaskCreate(filament_gcode_backward_task, "g_bwd", 4096, NULL, 5, NULL);
}

void filament_gcode_load_async(int32_t ch)
{
    if (ch < 1 || ch > 8) return;
    xTaskCreate(filament_gcode_load_task, "g_load", 4096,
                (void*)(intptr_t)ch, 5, NULL);
}
