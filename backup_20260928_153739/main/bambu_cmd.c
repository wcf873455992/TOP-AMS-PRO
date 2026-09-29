#include "bambu_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "bambu_mqtt.h"
#include "esp_timer.h"

static const char* TAG = "bambu_cmd";

const char* CMD_PRINT_PAUSE =
    R"({"print":{"command":"pause","sequence_id":"1"}})";

const char* CMD_PRINT_RESUME =
    R"({"print":{"command":"resume","sequence_id":"2"}})";

const char* CMD_LOAD =
    R"({"print":{"command":"ams_change_filament","sequence_id":"10","ams_id":0,"slot_id":0,"target":254,"curr_temp":245,"tar_temp":245,"user_id":"1"}})";

/* 装载用：ams_change_filament */
const char* CMD_ChangeLOAD =
    R"({"print":{"command":"ams_change_filament","sequence_id":"13","ams_id":0,"slot_id":0,"target":254,"curr_temp":245,"tar_temp":245,"user_id":"1"}})";

/* 装载用：ams_change_filament */
const char* CMD_LOAD_AMS =
    R"({"print":{"command":"ams_change_filament","sequence_id":"12","ams_id":0,"slot_id":0,"target":254,"curr_temp":245,"tar_temp":245,"user_id":"1"}})";

const char* CMD_ULOAD =
    R"({"print":{"command":"ams_change_filament","sequence_id":"11","ams_id":0,"slot_id":255,"target":255,"curr_temp":245,"tar_temp":245,"user_id":"1"}})";

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

void bambu_send_cmd(const char* payload)
{
    if (!payload) return;
    bambu_mqtt_publish_raw(payload);
    ESP_LOGI(TAG, "发送命令: %s", payload);
}

/* 动态发 M620 S{n}A（进料用） */
void bambu_send_m620_load(int ch)
{
    char param[128];
    snprintf(param, sizeof(param), "M620 S%dA\\nT%d\\nM621 S%dA\\n", ch, ch, ch);

    char json[256];
    snprintf(json, sizeof(json),
             "{\"print\":{\"command\":\"gcode_line\",\"sequence_id\":\"%d\",\"param\":\"%s\"}}",
             (int)(esp_timer_get_time() / 1000) % 10000,
             param);

    bambu_mqtt_publish_raw(json);
    ESP_LOGI("bambu_cmd", "发送 M620 S%dA", ch);
}

/* 动态发 ams_change_filament（进料用，按通道设置 slot_id） */
void bambu_send_change_load(int ch)
{
    int ams_id = (ch - 1) / 4;
    int slot_id = (ch - 1) % 4;

    char json[256];
    snprintf(json, sizeof(json),
             "{\"print\":{\"command\":\"ams_change_filament\",\"sequence_id\":\"10\","
             "\"ams_id\":%d,\"slot_id\":%d,\"target\":254,"
             "\"curr_temp\":245,\"tar_temp\":245,\"user_id\":\"1\"}}",
             ams_id, slot_id);

    bambu_mqtt_publish_raw(json);
    ESP_LOGI(TAG, "发送 ams_change_filament: ams_id=%d slot_id=%d", ams_id, slot_id);
}
