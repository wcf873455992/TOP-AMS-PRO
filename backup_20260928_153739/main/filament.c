#include "filament.h"
#include <string.h>
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

static const char* TAG = "filament";
static const char* NS  = "filament_cfg";

/* ============================================================
 * 状态码说明
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
    {1, 2,  3,  6000, 5000, "通道1", "PETG", "黑色"},
    {2, 10, 6,  6000, 5000, "通道2", "PETG", "白色"},
    {3, 5,  4,  6000, 5000, "通道3", "PETG", "红色"},
    {4, 8,  9,  6000, 5000, "通道4", "PETG", "蓝色"},
    {5, 0,  1,  6000, 5000, "通道5", "PETG", "绿色"},
    {6, -1, -1, 6000, 5000, "通道6", "PETG", "黄色"},
    {7, -1, -1, 6000, 5000, "通道7", "PETG", "灰色"},
    {8, -1, -1, 6000, 5000, "通道8", "PETG", "透明"},
};

int32_t g_current_channel = 0;
bool g_printer_sync = false;   /* false=被动模式, true=主动模式 */
int32_t g_slow_feed_pulse_ms = 30;
int32_t g_slow_feed_gap_ms = 600;
int32_t g_buffer_switch_gpio = -1;
int32_t g_buffer_feed_ms = 500;
int32_t g_buffer_cooldown_ms = 1000;
int32_t g_feed_timeout_ms = 180000;   /* 全速进料超时，默认 180 秒 */

static int32_t g_channel_state[8] = {0};

int32_t filament_get_state(int32_t ch)
{
    if (ch < 1 || ch > 8) return 0;
    return g_channel_state[ch - 1];
}

/* ============================================================
 * 配置
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
    ESP_LOGI(TAG, "换料模式切换: %s", sync ? "主动(开发者模式)" : "被动(只读状态)");
}

bool filament_get_printer_sync(void)
{
    return g_printer_sync;
}

void filament_set_slow_feed(int32_t pulse_ms, int32_t gap_ms)
{
    if (pulse_ms > 0 && pulse_ms < 500) g_slow_feed_pulse_ms = pulse_ms;
    if (gap_ms > 0 && gap_ms < 5000) g_slow_feed_gap_ms = gap_ms;
    filament_save_config();
}

int32_t filament_get_slow_feed_pulse(void) { return g_slow_feed_pulse_ms; }
int32_t filament_get_slow_feed_gap(void) { return g_slow_feed_gap_ms; }

void filament_set_buffer_switch(int32_t gpio) { g_buffer_switch_gpio = gpio; filament_save_config(); }
void filament_set_buffer_feed(int32_t ms) { if (ms > 0 && ms < 10000) g_buffer_feed_ms = ms; filament_save_config(); }
void filament_set_feed_timeout(int32_t ms)
{
    if (ms > 0 && ms <= 600000) {
        g_feed_timeout_ms = ms;
        filament_save_config();
    }
}

void filament_set_buffer_cooldown(int32_t ms) { if (ms >= 0 && ms < 60000) g_buffer_cooldown_ms = ms; filament_save_config(); }

void filament_save_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_i32(h, "current", g_current_channel);
    nvs_set_i32(h, "printer_sync", g_printer_sync ? 1 : 0);
    nvs_set_i32(h, "slow_pulse", g_slow_feed_pulse_ms);
    nvs_set_i32(h, "slow_gap", g_slow_feed_gap_ms);
    nvs_set_i32(h, "buf_gpio", g_buffer_switch_gpio);
    nvs_set_i32(h, "buf_feed", g_buffer_feed_ms);
    nvs_set_i32(h, "buf_cool", g_buffer_cooldown_ms);
    nvs_set_i32(h, "feed_timeout", g_feed_timeout_ms);

    for (int i = 0; i < 8; i++) {
        char key[16];
        snprintf(key, sizeof(key), "lt%d", i);
        nvs_set_i32(h, key, g_channels[i].load_time_ms);
        snprintf(key, sizeof(key), "ut%d", i);
        nvs_set_i32(h, key, g_channels[i].uload_time_ms);
        snprintf(key, sizeof(key), "mat%d", i);
        nvs_set_str(h, key, g_channels[i].material);
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

    if (nvs_get_i32(h, "slow_pulse", &val) == ESP_OK) g_slow_feed_pulse_ms = val;
    if (nvs_get_i32(h, "slow_gap", &val) == ESP_OK) g_slow_feed_gap_ms = val;
    if (nvs_get_i32(h, "buf_gpio", &val) == ESP_OK) g_buffer_switch_gpio = val;
    if (nvs_get_i32(h, "buf_feed", &val) == ESP_OK) g_buffer_feed_ms = val;
    if (nvs_get_i32(h, "buf_cool", &val) == ESP_OK) g_buffer_cooldown_ms = val;
    if (nvs_get_i32(h, "feed_timeout", &val) == ESP_OK) g_feed_timeout_ms = val;

    for (int i = 0; i < 8; i++) {
        char key[16];
        snprintf(key, sizeof(key), "lt%d", i);
        if (nvs_get_i32(h, key, &val) == ESP_OK) g_channels[i].load_time_ms = val;
        snprintf(key, sizeof(key), "ut%d", i);
        if (nvs_get_i32(h, key, &val) == ESP_OK) g_channels[i].uload_time_ms = val;
        snprintf(key, sizeof(key), "mat%d", i);
        size_t len = sizeof(g_channels[i].material);
        nvs_get_str(h, key, g_channels[i].material, &len);
    }

    nvs_close(h);

    if (g_current_channel >= 1 && g_current_channel <= 8) {
        g_channel_state[g_current_channel - 1] = 3;
        ESP_LOGI(TAG, "当前通道 %ld 状态设为使用中", (long)g_current_channel);
    }
}

/* ============================================================
 * GPIO 初始化
 * ============================================================ */

void filament_init(void)
{
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
    ESP_LOGI(TAG, "GPIO 已初始化，当前通道: %ld", (long)g_current_channel);
}

/* ============================================================
 * 物理进退料
 * ============================================================ */

void filament_forward(int32_t ch)
{
    if (ch < 1 || ch > 8) return;
    int idx = ch - 1;
    if (g_channels[idx].forward_gpio < 0) return;

    g_channel_state[idx] = 1;
    ESP_LOGI(TAG, "通道 %d 进料，时长 %d ms", ch, g_channels[idx].load_time_ms);
    gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(g_channels[idx].load_time_ms));
    gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
    g_channel_state[idx] = 0;
}

void filament_backward(int32_t ch)
{
    if (ch < 1 || ch > 8) return;
    int idx = ch - 1;
    if (g_channels[idx].backward_gpio < 0) return;

    g_channel_state[idx] = 2;
    ESP_LOGI(TAG, "通道 %d 退料，时长 %d ms", ch, g_channels[idx].uload_time_ms);
    gpio_set_level((gpio_num_t)g_channels[idx].backward_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(g_channels[idx].uload_time_ms));
    gpio_set_level((gpio_num_t)g_channels[idx].backward_gpio, 0);
    g_channel_state[idx] = 0;
}

/* ============================================================
 * 状态机枚举（被动/主动共用）
 * ============================================================ */

enum {
    ST_IDLE = 0,
    ST_ULOADING,      /* 直接退料：检测到 ams_status 258/259/260 就退当前通道 */
    ST_WAIT_ULOAD,
    ST_ULINE,
    ST_WAIT_LOAD,
    ST_FEED_FAST,
    ST_WAIT_FLUSH,
    ST_FEED_SLOW,
    ST_DONE,
};

/* ============================================================
 * 被动换料任务（未勾选联动）
 * 只读状态，物理执行，不发控制命令
 * ============================================================ */

static void filament_passive_task(void *arg)
{
    enum {
        P_IDLE,          /* 空闲，等 260 */
        P_ULOAD,         /* 物理退料 */
        P_WAIT_261,      /* 等 261 送耗材到挤出机 */
        P_FEED_FAST,     /* 全速进料，等 hw==3 */
        P_WAIT_FLUSH,    /* 等 263 冲刷 */
        P_FEED_SLOW,     /* 缓慢进料，等 768 */
        P_DONE,          /* 完成 */
    } state = P_IDLE;

    int target_ch = 0;
    int prev_ams = -1;
    int prev_hw  = -1;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(300));

        if (g_printer_sync) {
            if (state != P_IDLE) {
                ESP_LOGI(TAG, "[被动] 切到主动模式，重置状态机");
                state = P_IDLE;
            }
            prev_ams = -1;
            prev_hw  = -1;
            continue;
        }

        int ams = g_bambu_status.ams_status;
        int hw  = g_bambu_status.hw_switch_state;

        if (ams != prev_ams) {
            ESP_LOGI(TAG, "[被动] ams_status: %d -> %d", prev_ams, ams);
            prev_ams = ams;
        }
        if (hw != prev_hw) {
            ESP_LOGI(TAG, "[被动] hw_switch: %d -> %d", prev_hw, hw);
            prev_hw = hw;
        }

        switch (state) {

        /* ---------- 空闲：等 260 ---------- */
        case P_IDLE:
            if (ams == 260) {
                ESP_LOGI(TAG, "[被动] 检测到抽回耗材丝(260)，开始退料");
                target_ch = g_current_channel;
                state = P_ULOAD;
            }
            break;

        /* ---------- 物理退料 ---------- */
        case P_ULOAD:
            if (g_current_channel >= 1 && g_current_channel <= 8) {
                int idx = g_current_channel - 1;
                ESP_LOGI(TAG, "[被动] 退料通道=%ld bwd_gpio=%d uload_ms=%d",
                         (long)g_current_channel,
                         g_channels[idx].backward_gpio,
                         g_channels[idx].uload_time_ms);
                if (g_channels[idx].backward_gpio >= 0) {
                    filament_backward(g_current_channel);
                    ESP_LOGI(TAG, "[被动] 退料完成");
                } else {
                    ESP_LOGW(TAG, "[被动] 通道 %ld 退料 GPIO 未配置",
                             (long)g_current_channel);
                }
            } else {
                ESP_LOGW(TAG, "[被动] current=%ld 无效",
                         (long)g_current_channel);
            }
            state = P_WAIT_261;
            break;

        /* ---------- 等 261 送耗材到挤出机 ---------- */
        case P_WAIT_261:
            if (ams == 261) {
                ESP_LOGI(TAG, "[被动] 检测到 261 送耗材到挤出机，开始全速进料");
                state = P_FEED_FAST;
            } else if (ams == 0 || ams == -1) {
                ESP_LOGI(TAG, "[被动] ams 回到空闲，换料流程结束");
                state = P_IDLE;
            }
            break;

        /* ---------- 全速进料，等 hw==3 ---------- */
        case P_FEED_FAST: {
            int idx = target_ch - 1;
            if (idx < 0 || idx > 7 || g_channels[idx].forward_gpio < 0) {
                ESP_LOGW(TAG, "[被动] 通道 %d 送料 GPIO 无效，跳过", target_ch);
                state = P_WAIT_FLUSH;
                break;
            }

            ESP_LOGI(TAG, "[被动] 全速进料，等有料(hw==3)");
            uint32_t elapsed = 0;
            const uint32_t max_feed = g_feed_timeout_ms;
            const uint32_t step = 200;

            while (elapsed < max_feed) {
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
                vTaskDelay(pdMS_TO_TICKS(step));
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
                elapsed += step;

                if (g_bambu_status.hw_switch_state == 3) {
                    ESP_LOGI(TAG, "[被动] 检测到有料(hw==3)，停止全速进料");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (elapsed >= max_feed) ESP_LOGW(TAG, "[被动] 全速进料超时");
            state = P_WAIT_FLUSH;
            break;
        }

        /* ---------- 等 263 冲刷 ---------- */
        case P_WAIT_FLUSH:
            if (ams == 263) {
                ESP_LOGI(TAG, "[被动] 检测到 263 冲刷旧耗材，转缓慢进料");
                state = P_FEED_SLOW;
            } else if (ams == 768 || ams == 0) {
                ESP_LOGI(TAG, "[被动] 已冲刷完成，跳过缓慢进料");
                state = P_DONE;
            }
            break;

        /* ---------- 缓慢进料，等 768 ---------- */
        case P_FEED_SLOW: {
            int idx = target_ch - 1;
            if (idx < 0 || idx > 7 || g_channels[idx].forward_gpio < 0) {
                state = P_DONE;
                break;
            }

            ESP_LOGI(TAG, "[被动] 缓慢进料，等冲刷完成(768)");
            int timeout = 180;
            while (timeout-- > 0) {
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
                vTaskDelay(pdMS_TO_TICKS(g_slow_feed_pulse_ms));
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
                vTaskDelay(pdMS_TO_TICKS(g_slow_feed_gap_ms));

                if (g_bambu_status.ams_status == 768) {
                    ESP_LOGI(TAG, "[被动] 检测到 768 冲刷完成，停止进料");
                    break;
                }
                if (g_bambu_status.ams_status == 0) {
                    ESP_LOGI(TAG, "[被动] ams 回到 0，停止进料");
                    break;
                }
            }
            if (timeout <= 0) ESP_LOGW(TAG, "[被动] 等待冲刷完成超时");
            state = P_DONE;
            break;
        }

        /* ---------- 完成 ---------- */
        case P_DONE:
            ESP_LOGI(TAG, "[被动] 换料完成，通道 %d", target_ch);
            state = P_IDLE;
            break;
        }
    }
}





/* ============================================================
 * 主动换料任务（勾选联动）
 * 发 CMD_ULOAD / CMD_LOAD，需要仅局域网 + 开发者模式
 * ============================================================ */

static void filament_active_task(void *arg)
{
    int state = ST_IDLE;
    int target_ch = 0;
    int prev_bed = -1, prev_ams = -1, prev_hw = -1;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        /* 只在主动模式下工作 */
        if (!g_printer_sync) {
            if (state != ST_IDLE) {
                ESP_LOGI(TAG, "切换到被动模式，重置主动状态机");
                state = ST_IDLE;
            }
            continue;
        }

        int bed = g_bambu_status.bed_target_temper;
        int ams = g_bambu_status.ams_status;
        int hw = g_bambu_status.hw_switch_state;

        if (bed != prev_bed) { ESP_LOGI(TAG, "[主动] bed_target: %d -> %d", prev_bed, bed); prev_bed = bed; }
        if (ams != prev_ams) { ESP_LOGI(TAG, "[主动] ams_status: %d -> %d", prev_ams, ams); prev_ams = ams; }
        if (hw != prev_hw) { ESP_LOGI(TAG, "[主动] hw_switch: %d -> %d", prev_hw, hw); prev_hw = hw; }

        switch (state) {

        case ST_IDLE:
            if (bed >= 1 && bed <= 8) {
                target_ch = bed;
                ESP_LOGI(TAG, "[主动] 换料信号: 通道 %d", target_ch);
                if (target_ch == g_current_channel) {
                    ESP_LOGI(TAG, "[主动] 目标通道与当前通道相同，跳过");
                    break;
                }
                ESP_LOGI(TAG, "[主动] 发送 CMD_ULOAD");
                bambu_send_cmd(CMD_ULOAD);
                state = ST_WAIT_ULOAD;
            }
            break;

        case ST_WAIT_ULOAD:
            if (ams == 260) {
                ESP_LOGI(TAG, "[主动] 检测到抽回耗材丝，开始物理退线");
                state = ST_ULINE;
            } else if (bed == 0) {
                ESP_LOGI(TAG, "[主动] 换料取消");
                state = ST_IDLE;
            }
            break;

        case ST_ULINE:
            ESP_LOGI(TAG, "[主动] ST_ULINE: current=%ld target=%d",
                     (long)g_current_channel, target_ch);

            if (g_current_channel >= 1 && g_current_channel <= 8) {
                int idx = g_current_channel - 1;
                ESP_LOGI(TAG, "[主动] 退料通道=%ld bwd_gpio=%d uload_ms=%d",
                         (long)g_current_channel,
                         g_channels[idx].backward_gpio,
                         g_channels[idx].uload_time_ms);

                if (g_channels[idx].backward_gpio >= 0) {
                    filament_backward(g_current_channel);
                } else {
                    ESP_LOGW(TAG, "[主动] 通道 %ld 退料 GPIO 未配置（bwd=%d），跳过物理退料",
                             (long)g_current_channel, g_channels[idx].backward_gpio);
                }
            } else {
                ESP_LOGW(TAG, "[主动] g_current_channel=%ld 无效，无法退料！"
                              "请先在网页/配置里设置当前通道。",
                         (long)g_current_channel);
            }

            ESP_LOGI(TAG, "[主动] 退线完成");
            state = ST_WAIT_LOAD;
            break;

        case ST_WAIT_LOAD:
            if (hw == 2) {
                ESP_LOGI(TAG, "[主动] 打印机无料，切换通道到 %d", target_ch);
                g_current_channel = target_ch;
                g_channel_state[target_ch - 1] = 3;
                filament_save_config();
                ESP_LOGI(TAG, "[主动] 发送 CMD_LOAD");
                bambu_send_cmd(CMD_LOAD);
                state = ST_FEED_FAST;
            }
            break;

        case ST_FEED_FAST: {
            int idx = target_ch - 1;
            if (g_channels[idx].forward_gpio < 0) { state = ST_IDLE; break; }

            ESP_LOGI(TAG, "[主动] 全速进料，等有料");
            uint32_t elapsed = 0;
            const uint32_t max_feed = g_feed_timeout_ms;
            const uint32_t step = 200;

            while (elapsed < max_feed) {
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
                vTaskDelay(pdMS_TO_TICKS(step));
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
                elapsed += step;

                if (g_bambu_status.hw_switch_state == 3) {
                    ESP_LOGI(TAG, "[主动] 检测到有料，停止全速进料");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (elapsed >= max_feed) ESP_LOGW(TAG, "[主动] 全速进料超时");
            state = ST_WAIT_FLUSH;
            break;
        }

        case ST_WAIT_FLUSH:
            ESP_LOGI(TAG, "[主动] 等冲刷旧耗材");
            {
                int timeout = 180;
                bool skip = false;
                while (timeout-- > 0) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    if (g_bambu_status.ams_status == 263) {
                        ESP_LOGI(TAG, "[主动] 检测到冲刷旧耗材，转缓慢进料");
                        break;
                    }
                    if (g_bambu_status.ams_status == 768 ||
                        g_bambu_status.ams_status == 0) {
                        ESP_LOGI(TAG, "[主动] 打印机已冲刷完成，跳过缓慢进料");
                        skip = true;
                        break;
                    }
                }
                if (skip) { state = ST_DONE; break; }
            }
            state = ST_FEED_SLOW;
            break;

        case ST_FEED_SLOW: {
            int idx = target_ch - 1;
            if (g_channels[idx].forward_gpio < 0) { state = ST_IDLE; break; }

            ESP_LOGI(TAG, "[主动] 缓慢进料，等冲刷完成");
            int timeout = 180;
            while (timeout-- > 0) {
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
                vTaskDelay(pdMS_TO_TICKS(g_slow_feed_pulse_ms));
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
                vTaskDelay(pdMS_TO_TICKS(g_slow_feed_gap_ms));

                if (g_bambu_status.ams_status == 768) {
                    ESP_LOGI(TAG, "[主动] 检测到冲刷完成，停止进料");
                    break;
                }
                if (g_bambu_status.ams_status == 0) {
                    ESP_LOGI(TAG, "[主动] 检测到空闲，停止进料");
                    break;
                }
            }
            if (timeout <= 0) ESP_LOGW(TAG, "[主动] 等待冲刷完成超时");
            state = ST_DONE;
            break;
        }

        case ST_DONE:
            ESP_LOGI(TAG, "[主动] 换料完成，通道 %d", g_current_channel);
            state = ST_IDLE;
            break;
        }
    }
}

void filament_active_start(void)
{
    xTaskCreate(filament_active_task, "fil_active", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "主动换料任务已启动（发控制命令，需要开发者模式）");
}

/* ============================================================
 * 单通道异步进退料（手动按钮用）
 * ============================================================ */









/* ============================================================
 * 微动开关缓冲任务
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
 * 兼容旧接口
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

void filament_auto_load_start(void) {}

void filament_assist_feed_start(void) {}

void filament_slow_test_async(int32_t ch, int32_t pulse_ms, int32_t gap_ms, int32_t duration_sec) {}

/* 装载：先退料，再进料 */






/* ============================================================
 * 独立退料任务：ams==260 触发
 * ============================================================ */
static void filament_uload_task(void *arg)
{
    int prev_ams = -1;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        if (g_printer_sync) { prev_ams = -1; continue; }

        int ams = g_bambu_status.ams_status;
        if (ams == prev_ams) continue;
        prev_ams = ams;

        ESP_LOGI(TAG, "[退料] ams_status: %d", ams);

        if (ams == 260) {
            ESP_LOGI(TAG, "[退料] 检测到抽回耗材丝(260)，开始退料");
            if (g_current_channel >= 1 && g_current_channel <= 8) {
                int idx = g_current_channel - 1;
                ESP_LOGI(TAG, "[退料] 通道=%ld bwd_gpio=%d uload_ms=%d",
                         (long)g_current_channel,
                         g_channels[idx].backward_gpio,
                         g_channels[idx].uload_time_ms);
                if (g_channels[idx].backward_gpio >= 0) {
                    filament_backward(g_current_channel);
                    ESP_LOGI(TAG, "[退料] 完成");
                } else {
                    ESP_LOGW(TAG, "[退料] 通道 %ld GPIO 未配置",
                             (long)g_current_channel);
                }
            } else {
                ESP_LOGW(TAG, "[退料] current=%ld 无效",
                         (long)g_current_channel);
            }
        }
    }
}

/* ============================================================
 * 独立进料任务：ams==261 触发
 * ============================================================ */
static void filament_load_task(void *arg)
{
    enum {
        L_IDLE,
        L_FEED_FAST,
        L_WAIT_FLUSH,
        L_FEED_SLOW,
        L_DONE,
    } state = L_IDLE;

    int target_ch = 0;
    int prev_ams = -1;
    int prev_hw  = -1;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        if (g_printer_sync) {
            if (state != L_IDLE) {
                ESP_LOGI(TAG, "[进料] 切到主动模式，重置");
                state = L_IDLE;
            }
            prev_ams = -1;
            prev_hw  = -1;
            continue;
        }

        int ams = g_bambu_status.ams_status;
        int hw  = g_bambu_status.hw_switch_state;

        if (ams != prev_ams) {
            ESP_LOGI(TAG, "[进料] ams_status: %d -> %d (%s)",
                     prev_ams, ams, ams_status_desc(ams));
            prev_ams = ams;
        }
        if (hw != prev_hw) {
            ESP_LOGI(TAG, "[进料] hw_switch: %d -> %d (%s)",
                     prev_hw, hw, hw_switch_desc(hw));
            prev_hw = hw;
        }

        switch (state) {

        /* ---------- 空闲：等 261 ---------- */
        case L_IDLE:
            if (ams == 261) {
                ESP_LOGI(TAG, "[进料] 检测到 261 送耗材到挤出机，开始全速进料");

                int bed = g_bambu_status.bed_target_temper;
                if (bed >= 1 && bed <= 8) {
                    target_ch = bed;
                } else {
                    target_ch = g_current_channel;
                }
                if (target_ch < 1 || target_ch > 8) target_ch = 1;

                state = L_FEED_FAST;
            }
            break;

        /* ---------- 全速进料，等 hw==3 ---------- */
        case L_FEED_FAST: {
            int idx = target_ch - 1;
            if (g_channels[idx].forward_gpio < 0) {
                state = L_WAIT_FLUSH;
                break;
            }

            ESP_LOGI(TAG, "[进料] 全速进料，等有料(hw==3)");
            uint32_t elapsed = 0;
            const uint32_t max_feed = g_feed_timeout_ms;
            const uint32_t step = 200;

            while (elapsed < max_feed) {
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
                vTaskDelay(pdMS_TO_TICKS(step));
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
                elapsed += step;

                if (g_bambu_status.hw_switch_state == 3) {
                    ESP_LOGI(TAG, "[进料] 检测到有料(hw==3)，停止全速进料");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (elapsed >= max_feed) {
                ESP_LOGW(TAG, "[进料] 全速进料超时，停止");
                state = L_DONE;
                break;
            }
            state = L_WAIT_FLUSH;
            break;
        }

        /* ---------- 等 263 冲刷 ---------- */
        case L_WAIT_FLUSH:
            if (ams == 263) {
                ESP_LOGI(TAG, "[进料] 检测到 263 冲刷，转缓慢进料");
                state = L_FEED_SLOW;
            } else if (ams == 0) {
                ESP_LOGI(TAG, "[进料] ams 回到 0，跳过缓慢进料");
                state = L_DONE;
            }
            break;

        /* ---------- 缓慢进料，等 0 ---------- */
        case L_FEED_SLOW: {
            int idx = target_ch - 1;
            if (g_channels[idx].forward_gpio < 0) {
                state = L_DONE;
                break;
            }

            ESP_LOGI(TAG, "[进料] 缓慢进料，等空闲(0)");
            int timeout = 180;
            while (timeout-- > 0) {
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 1);
                vTaskDelay(pdMS_TO_TICKS(g_slow_feed_pulse_ms));
                gpio_set_level((gpio_num_t)g_channels[idx].forward_gpio, 0);
                vTaskDelay(pdMS_TO_TICKS(g_slow_feed_gap_ms));

                if (g_bambu_status.ams_status == 0) {
                    ESP_LOGI(TAG, "[进料] ams 回到 0，停止缓慢进料");
                    break;
                }
            }
            if (timeout <= 0) ESP_LOGW(TAG, "[进料] 等待空闲超时");
            state = L_DONE;
            break;
        }

        /* ---------- 完成 ---------- */
        case L_DONE:
            ESP_LOGI(TAG, "[进料] 完成，通道 %d", target_ch);
            state = L_IDLE;
            break;
        }
    }
}


/* ============================================================
 * 启动函数
 * ============================================================ */
void filament_uload_start(void)
{
    xTaskCreate(filament_uload_task, "fil_uload", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "退料任务已启动");
}

void filament_load_start(void)
{
    xTaskCreate(filament_load_task, "fil_load", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "进料任务已启动");
}


/* ============================================================
 * 手动按钮任务（进料 / 退料 / 装载）
 * 根据 g_printer_sync 联动
 * ============================================================ */

/* 进料：主动发 CMD_LOAD，被动只物理进料 */
static void filament_forward_manual_task(void *arg)
{
    int32_t ch = (int32_t)(intptr_t)arg;
    if (ch < 1 || ch > 8) { vTaskDelete(NULL); return; }

    if (g_printer_sync) {
        ESP_LOGI(TAG, "[进料-主动] 通道 %ld：发 ams_change_filament", (long)ch);
        bambu_send_change_load(ch);

        /* ===== 等 261 或 263 ===== */
        ESP_LOGI(TAG, "[进料-主动] 等 261 或 263...");
        bool got = false;
        int timeout = 360;
        int last_ams = -1;
        while (timeout-- > 0) {
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

        /* ===== 全速进料，等 hw==3 ===== */
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
            ESP_LOGI(TAG, "[进料] 通道 %ld 超时结束", (long)ch);
            vTaskDelete(NULL);
            return;
        }

        /* ===== 等 263 冲刷 ===== */
        ESP_LOGI(TAG, "[进料-主动] 等 263 冲刷...");
        bool got_263 = false;
        timeout = 360;
        last_ams = -1;
        while (timeout-- > 0) {
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

        /* ===== 缓慢进料，等 ams==0 ===== */
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
        filament_forward(ch);
    }

    g_current_channel = ch;
    g_channel_state[ch - 1] = 3;
    filament_save_config();

    ESP_LOGI(TAG, "[进料] 通道 %ld 完成", (long)ch);
    vTaskDelete(NULL);
}


/* 退料：主动发 CMD_ULOAD，被动只物理退料 */
static void filament_backward_manual_task(void *arg)
{
    int32_t ch = (int32_t)(intptr_t)arg;
    if (ch < 1 || ch > 8) { vTaskDelete(NULL); return; }

    if (g_printer_sync) {
        ESP_LOGI(TAG, "[退料-主动] 通道 %ld：发 CMD_ULOAD (ams_change_filament, slot_id 255)", (long)ch);
        bambu_send_cmd(CMD_ULOAD);

        /* 等 260 抽回耗材丝（3 分钟超时） */
        ESP_LOGI(TAG, "[退料-主动] 等打印机抽回耗材丝...");
        bool got_260 = false;
        int timeout = 360;   /* 3 分钟 */
        int last_ams = -1;
        while (timeout-- > 0) {
            int ams = g_bambu_status.ams_status;
            if (ams != last_ams) {
                ESP_LOGI(TAG, "[退料-主动] ams_status: %d -> %d (%s)",
                         last_ams, ams, ams_status_desc(ams));
                last_ams = ams;
            }
            if (ams == 260) {
                got_260 = true;
                break;
            }
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
    }

    filament_backward(ch);

    g_channel_state[ch - 1] = 0;

    /* 如果退的是当前通道，清掉 */
    if (g_current_channel == ch) {
        g_current_channel = 0;
        ESP_LOGI(TAG, "[退料] 已清空当前通道");
        filament_save_config();
    }

    ESP_LOGI(TAG, "[退料] 通道 %ld 完成", (long)ch);
    vTaskDelete(NULL);
}

/* 装载：退当前 + 进目标，主动模式发命令 */
static void filament_load_manual_task(void *arg)
{
    int32_t ch = (int32_t)(intptr_t)arg;
    if (ch < 1 || ch > 8) { vTaskDelete(NULL); return; }

    if (g_printer_sync) {
        /* ===== 主动模式：完整换料流程 ===== */

        /* 1. 发 CMD_ULOAD（退料） */
        ESP_LOGI(TAG, "[装载-主动] 目标通道 %ld：发 CMD_ULOAD", (long)ch);
        bambu_send_cmd(CMD_ULOAD);

        /* 2. 等 ams_status == 260 */
        ESP_LOGI(TAG, "[装载-主动] 等打印机抽回耗材丝(260)...");
        bool got_260 = false;
        int timeout = 360;
        int last_ams = -1;
        while (timeout-- > 0) {
            int ams = g_bambu_status.ams_status;
            if (ams != last_ams) {
                ESP_LOGI(TAG, "[装载-主动] ams_status: %d -> %d (%s)",
                         last_ams, ams, ams_status_desc(ams));
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

        /* 3. 物理退当前通道 */
        if (g_current_channel >= 1 && g_current_channel <= 8) {
            int idx0 = g_current_channel - 1;
            ESP_LOGI(TAG, "[装载-主动] 物理退通道 %ld (bwd_gpio=%d uload_ms=%d)",
                     (long)g_current_channel,
                     g_channels[idx0].backward_gpio,
                     g_channels[idx0].uload_time_ms);
            filament_backward(g_current_channel);
        }

        /* 4. 等 hw_switch_state == 2 */
        ESP_LOGI(TAG, "[装载-主动] 等打印机无料(hw==2)...");
        timeout = 360;
        while (timeout-- > 0) {
            if (g_bambu_status.hw_switch_state == 2) {
                ESP_LOGI(TAG, "[装载-主动] 打印机已无料");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        /* 5. 切通道 */
        if (g_current_channel >= 1 && g_current_channel <= 8 &&
            g_current_channel != ch) {
            g_channel_state[g_current_channel - 1] = 0;
            ESP_LOGI(TAG, "[装载] 清除通道 %ld 使用中状态",
                     (long)g_current_channel);
        }
        g_current_channel = ch;
        g_channel_state[ch - 1] = 3;
        filament_save_config();

        /* 6. 发 CMD_LOAD（进料） */
        ESP_LOGI(TAG, "[装载-主动] 发 CMD_LOAD，目标通道 %ld", (long)ch);
        bambu_send_cmd(CMD_LOAD);

        /* 7. 全速进料，等 hw == 3 */
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

        /* 8. 等 263 冲刷 */
        ESP_LOGI(TAG, "[装载-主动] 等 263 冲刷...");
        bool got_263 = false;
        timeout = 360;
        last_ams = -1;
        while (timeout-- > 0) {
            int ams = g_bambu_status.ams_status;
            if (ams != last_ams) {
                ESP_LOGI(TAG, "[装载-主动] ams_status: %d -> %d (%s)",
                         last_ams, ams, ams_status_desc(ams));
                last_ams = ams;
            }
            if (ams == 263) { got_263 = true; break; }
            if (ams == 768 || ams == 0) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        /* 9. 缓慢进料，等 768 */
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
        /* ===== 被动模式（保持原样） ===== */
        ESP_LOGI(TAG, "[装载-被动] 目标通道 %ld，先退当前通道 %ld",
                 (long)ch, (long)g_current_channel);

        if (g_current_channel >= 1 && g_current_channel <= 8) {
            filament_backward(g_current_channel);
        } else {
            ESP_LOGW(TAG, "[装载-被动] 当前通道 %ld 无效，跳过退料",
                     (long)g_current_channel);
        }

        vTaskDelay(pdMS_TO_TICKS(500));

        {
            int ams_now = g_bambu_status.ams_status;
            int hw_now  = g_bambu_status.hw_switch_state;
            ESP_LOGI(TAG, "[装载-被动] 再进料，通道 %ld (ams=%d %s, hw=%d %s)",
                     (long)ch, ams_now, ams_status_desc(ams_now),
                     hw_now, hw_switch_desc(hw_now));
        }
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

    vTaskDelete(NULL);
}


/* ============================================================
 * 异步启动函数
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
    xTaskCreate(filament_load_manual_task, "fil_load_manual", 4096,
                (void*)(intptr_t)ch, 5, NULL);
}

/* 同步版本（内部用） */
void filament_forward_sync(int32_t ch)
{
    if (ch < 1 || ch > 8) return;
    filament_forward(ch);
    g_current_channel = ch;
    g_channel_state[ch - 1] = 3;
    filament_save_config();
}

void filament_backward_sync(int32_t ch)
{
    if (ch < 1 || ch > 8) return;
    filament_backward(ch);
    g_channel_state[ch - 1] = 0;
}

void filament_load(int32_t ch)
{
    if (ch < 1 || ch > 8) return;

    if (g_printer_sync) {
        bambu_send_cmd(CMD_ULOAD);
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (g_current_channel >= 1 && g_current_channel <= 8) {
            filament_backward(g_current_channel);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        /* 清除旧通道的使用中状态 */
        if (g_current_channel >= 1 && g_current_channel <= 8 &&
            g_current_channel != ch) {
            g_channel_state[g_current_channel - 1] = 0;
            ESP_LOGI(TAG, "[装载] 清除通道 %ld 使用中状态",
                     (long)g_current_channel);
        }

        /* 设置新通道 */
        g_current_channel = ch;
        g_channel_state[ch - 1] = 3;
        filament_save_config();
        bambu_send_cmd(CMD_LOAD);
        vTaskDelay(pdMS_TO_TICKS(2000));
        filament_forward(ch);
    } else {
        if (g_current_channel >= 1 && g_current_channel <= 8) {
            filament_backward(g_current_channel);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        filament_forward(ch);
        /* 清除旧通道的使用中状态 */
        if (g_current_channel >= 1 && g_current_channel <= 8 &&
            g_current_channel != ch) {
            g_channel_state[g_current_channel - 1] = 0;
            ESP_LOGI(TAG, "[装载] 清除通道 %ld 使用中状态",
                     (long)g_current_channel);
        }

        /* 设置新通道 */
        g_current_channel = ch;
        g_channel_state[ch - 1] = 3;
        filament_save_config();
    }
}
