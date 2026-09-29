#include "bambu_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "bambu_mqtt.h"
#include "esp_timer.h"

static const char* TAG = "bambu_cmd";

/* ---------- 静态命令 ---------- */
const char* CMD_PRINT_PAUSE =
    R"({"print":{"command":"pause","sequence_id":"1"}})";

const char* CMD_PRINT_RESUME =
    R"({"print":{"command":"resume","sequence_id":"2"}})";

const char* CMD_CLICK_DONE =
    R"({"print":{"command":"ams_control","param":"done","sequence_id":"1"},"user_id":"1"})";

const char* CMD_CHICK_RESUME =
    R"({"print":{"command":"ams_control","param":"resume","sequence_id":"20030"},"user_id":"1"})";

const char* CMD_ERROR_CLEAN =
    R"({"print":{"command":"clean_print_error","sequence_id":"1"},"user_id":"1"})";

const char* CMD_GET_STATUS =
    R"({"pushing":{"sequence_id":"0","command":"pushall"}})";

const char* CMD_LED_ON =
    R"({"system":{"sequence_id":"312","command":"ledctrl","led_node":"chamber_light","led_mode":"on","led_on_time":500,"led_off_time":500,"loop_times":1,"interval_time":1000}})";

const char* CMD_LED_OFF =
    R"({"system":{"sequence_id":"313","command":"ledctrl","led_node":"chamber_light","led_mode":"off","led_on_time":500,"led_off_time":500,"loop_times":1,"interval_time":1000}})";

/* ---------- 动态构造 ---------- */

char* bambu_cmd_run_gcode(const char* code)
{
    if (!code) return NULL;
    size_t len = strlen(code);
    int need_nl = (len == 0 || code[len - 1] != '\n');
    const char* prefix = R"({"print":{"command":"gcode_line","param" : ")";
    const char* suffix = R"(", "sequence_id" : "0"}})";
    const char* nl = need_nl ? "\\n" : "";
    size_t total = strlen(prefix) + len + strlen(nl) + strlen(suffix) + 1;
    char* buf = malloc(total);
    if (!buf) return NULL;
    snprintf(buf, total, "%s%s%s%s", prefix, code, nl, suffix);
    return buf;
}

char* bambu_cmd_make_change_filament(int ams_id, int slot_id,
                                      int target, int curr_temp, int tar_temp)
{
    char* buf = malloc(320);
    if (!buf) return NULL;

    int seq = (int)(esp_timer_get_time() / 1000) % 100000;

    snprintf(buf, 320,
        "{\"print\":{\"command\":\"ams_change_filament\","
        "\"sequence_id\":\"%d\","
        "\"ams_id\":%d,\"slot_id\":%d,\"target\":%d,"
        "\"curr_temp\":%d,\"tar_temp\":%d,\"user_id\":\"1\"}}",
        seq, ams_id, slot_id, target, curr_temp, tar_temp);
    return buf;
}

/* ---------- 发送 ---------- */

void bambu_send_cmd(const char* payload)
{
    if (!payload) return;
    bambu_mqtt_publish_raw(payload);
    ESP_LOGI(TAG, "发送命令: %s", payload);
}

void bambu_send_change_filament(int ch, int load)
{
    if (ch < 1 || ch > 8) return;

    int ams_id  = (ch - 1) / 4;
    int slot_id = (ch - 1) % 4;

    int target;
    if (load) {
        target  = 254;
    } else {
        slot_id = 255;
        target  = 255;
    }

    int curr_temp = g_bambu_status.nozzle_temper;
    if (curr_temp <= 0) curr_temp = 245;
    int tar_temp = 245;

    char* payload = bambu_cmd_make_change_filament(
        ams_id, slot_id, target, curr_temp, tar_temp);
    if (!payload) {
        ESP_LOGE(TAG, "构造 payload 失败");
        return;
    }

    bambu_mqtt_publish_raw(payload);
    ESP_LOGI(TAG, "发送 ams_change_filament: ch=%d ams=%d slot=%d target=%d %s",
             ch, ams_id, slot_id, target, load ? "进料" : "退料");
    free(payload);
}
