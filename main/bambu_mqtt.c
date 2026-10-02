#include "bambu_mqtt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_timer.h"

static const char *TAG = "bambu_mqtt";

bambu_status_t g_bambu_status = { .last_real_bed_target = 60 };

static esp_mqtt_client_handle_t s_client = NULL;
static bambu_config_t s_cfg = {0};
static double s_prev_toolhead_x;
static double s_prev_toolhead_y;
static double s_prev_toolhead_z;
static bool s_toolhead_position_valid;

#define TOOLHEAD_MOTION_WINDOW_MS 4000

static const char *PUSHALL_CMD =
    R"({"pushing":{"sequence_id":"0","command":"pushall"}})";

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!src) return;
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void parse_lights(cJSON *print)
{
    cJSON *lights = cJSON_GetObjectItem(print, "lights_report");
    if (!lights || !cJSON_IsArray(lights)) return;
    g_bambu_status.chamber_light_on = false;
    g_bambu_status.work_light_on = false;
    cJSON *light;
    cJSON_ArrayForEach(light, lights) {
        cJSON *node = cJSON_GetObjectItem(light, "node");
        cJSON *mode = cJSON_GetObjectItem(light, "mode");
        if (!cJSON_IsString(node) || !cJSON_IsString(mode)) continue;
        bool on = (strcmp(mode->valuestring, "on") == 0) ||
                  (strcmp(mode->valuestring, "flashing") == 0);
        if (strcmp(node->valuestring, "chamber_light") == 0) {
            g_bambu_status.chamber_light_on = on;
        } else if (strcmp(node->valuestring, "work_light") == 0) {
            g_bambu_status.work_light_on = on;
        }
    }
}

static void parse_tray(cJSON *print)
{
    cJSON *vir_slot = cJSON_GetObjectItem(print, "vir_slot");
    if (!vir_slot || !cJSON_IsArray(vir_slot)) return;
    cJSON *tray = cJSON_GetArrayItem(vir_slot, 0);
    if (!tray) return;
    cJSON *item;
    item = cJSON_GetObjectItem(tray, "tray_type");
    if (cJSON_IsString(item)) copy_str(g_bambu_status.tray_type,
                                        sizeof(g_bambu_status.tray_type),
                                        item->valuestring);
    item = cJSON_GetObjectItem(tray, "tray_color");
    if (cJSON_IsString(item)) copy_str(g_bambu_status.tray_color,
                                        sizeof(g_bambu_status.tray_color),
                                        item->valuestring);
}

static void parse_report(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "JSON 解析失败");
        return;
    }
    cJSON *print = cJSON_GetObjectItem(root, "print");
    if (!print) { cJSON_Delete(root); return; }

    cJSON *item;
        item = cJSON_GetObjectItem(print, "gcode_state");
    if (cJSON_IsString(item)) {
        static char prev_gcode_state[16] = "";
        if (strcmp(prev_gcode_state, item->valuestring) != 0) {
            ESP_LOGI(TAG, "打印机状态变化: %s -> %s",
                     prev_gcode_state[0] ? prev_gcode_state : "(空)",
                     item->valuestring);
            strncpy(prev_gcode_state, item->valuestring,
                    sizeof(prev_gcode_state) - 1);
            prev_gcode_state[sizeof(prev_gcode_state) - 1] = '\0';
        }
        copy_str(g_bambu_status.gcode_state,
                 sizeof(g_bambu_status.gcode_state),
                 item->valuestring);
    }
    #define GET_INT(field, target) \
        item = cJSON_GetObjectItem(print, field); \
        if (cJSON_IsNumber(item)) target = item->valueint;

    GET_INT("nozzle_temper",        g_bambu_status.nozzle_temper);
    GET_INT("nozzle_target_temper", g_bambu_status.nozzle_target_temper);
    GET_INT("bed_temper",           g_bambu_status.bed_temper);
    /* ★ 记录 bed_target_temper 变化（换料信号调试） */
    {
        static int prev_bed_target = -1;
        item = cJSON_GetObjectItem(print, "bed_target_temper");
        if (cJSON_IsNumber(item)) {
            int btt = item->valueint;
            if (btt != prev_bed_target) {
                ESP_LOGI(TAG, "热床目标温度变化: %d -> %d",
                         prev_bed_target, btt);
                prev_bed_target = btt;
            }
            g_bambu_status.bed_target_temper = btt;
        }
    }

    /* 记录换料前的真实热床温度（非 1-8 的值） */
    if (g_bambu_status.bed_target_temper > 8) {
        g_bambu_status.last_real_bed_target = g_bambu_status.bed_target_temper;
    }
    GET_INT("mc_percent",           g_bambu_status.mc_percent);
    GET_INT("mc_remaining_time",    g_bambu_status.mc_remaining_time);
    GET_INT("layer_num",            g_bambu_status.layer_num);
    GET_INT("total_layer_num",      g_bambu_status.total_layer_num);
    GET_INT("print_error",          g_bambu_status.print_error);
    GET_INT("hw_switch_state",      g_bambu_status.hw_switch_state);
    /* ---------- ams_status 变化检测 ---------- */
    {
        int ams_status_now = -1;
        item = cJSON_GetObjectItem(print, "ams_status");
        if (cJSON_IsNumber(item)) {
            ams_status_now = item->valueint;
            g_bambu_status.ams_status = ams_status_now;
        } else {
            /* ★ 字段缺失时保持旧值，不覆盖为 -1 */
            ams_status_now = g_bambu_status.ams_status;
        }

        static int last_ams_status = -1;
        if (ams_status_now != last_ams_status) {
            const char *ams_text = "未知";
            switch (ams_status_now) {
                case 0:   ams_text = "空闲"; break;
                case 258: ams_text = "加热喷嘴"; break;
                case 259: ams_text = "切断耗材丝"; break;
                case 260: ams_text = "抽回耗材丝"; break;
                case 262: ams_text = "咬入耗材丝"; break;
                case 263: ams_text = "冲刷旧耗材"; break;
                case 768: ams_text = "冲刷完成"; break;
                default:  ams_text = "未知"; break;
            }
            ESP_LOGI(TAG, "ams_status 变化: %d -> %d (%s)",
                     last_ams_status, ams_status_now, ams_text);
            last_ams_status = ams_status_now;
        }
    }
    #undef GET_INT

    item = cJSON_GetObjectItem(print, "subtask_name");
    if (cJSON_IsString(item)) copy_str(g_bambu_status.subtask_name,
                                        sizeof(g_bambu_status.subtask_name),
                                        item->valuestring);
    item = cJSON_GetObjectItem(print, "wifi_signal");
    if (cJSON_IsString(item)) copy_str(g_bambu_status.wifi_signal,
                                        sizeof(g_bambu_status.wifi_signal),
                                        item->valuestring);
    item = cJSON_GetObjectItem(print, "nozzle_type");
    if (cJSON_IsString(item)) copy_str(g_bambu_status.nozzle_type,
                                        sizeof(g_bambu_status.nozzle_type),
                                        item->valuestring);
    item = cJSON_GetObjectItem(print, "nozzle_diameter");
    if (cJSON_IsString(item)) copy_str(g_bambu_status.nozzle_diameter,
                                        sizeof(g_bambu_status.nozzle_diameter),
                                        item->valuestring);
    item = cJSON_GetObjectItem(print, "print_type");
    if (cJSON_IsString(item)) {
        g_bambu_status.print_type_is_local =
            (strcmp(item->valuestring, "local") == 0);
    }

    cJSON *ams = cJSON_GetObjectItem(print, "ams");
    if (ams) {
        cJSON *exist_bits = cJSON_GetObjectItem(ams, "ams_exist_bits");
        if (cJSON_IsString(exist_bits)) {
            g_bambu_status.ams_exist =
                (strcmp(exist_bits->valuestring, "0") != 0);
        }
    }

    cJSON *ipcam = cJSON_GetObjectItem(print, "ipcam");
    if (ipcam) {
        cJSON *url = cJSON_GetObjectItem(ipcam, "rtsp_url");
        if (cJSON_IsString(url)) copy_str(g_bambu_status.rtsp_url,
                                           sizeof(g_bambu_status.rtsp_url),
                                           url->valuestring);
    }

    cJSON *hms = cJSON_GetObjectItem(print, "hms");
    if (hms && cJSON_IsArray(hms)) {
        g_bambu_status.hms_count = cJSON_GetArraySize(hms);
    } else {
        g_bambu_status.hms_count = 0;
    }

    /* 解析 extruder 信息 */
    cJSON *device2 = cJSON_GetObjectItem(print, "device");
    if (device2) {
        cJSON *extruder = cJSON_GetObjectItem(device2, "extruder");
        if (extruder) {
            cJSON *info_arr = cJSON_GetObjectItem(extruder, "info");
            if (info_arr && cJSON_IsArray(info_arr) && cJSON_GetArraySize(info_arr) > 0) {
                cJSON *info0 = cJSON_GetArrayItem(info_arr, 0);
                if (info0) {
                    cJSON *item2;
                    item2 = cJSON_GetObjectItem(info0, "stat");
                    if (cJSON_IsNumber(item2)) g_bambu_status.extruder_stat = item2->valueint;
                    item2 = cJSON_GetObjectItem(info0, "info");
                    if (cJSON_IsNumber(item2)) g_bambu_status.extruder_info = item2->valueint;
                    item2 = cJSON_GetObjectItem(info0, "snow");
                    if (cJSON_IsNumber(item2)) g_bambu_status.extruder_snow = item2->valueint;
                    item2 = cJSON_GetObjectItem(info0, "star");
                    if (cJSON_IsNumber(item2)) g_bambu_status.extruder_star = item2->valueint;
                    item2 = cJSON_GetObjectItem(info0, "temp");
                    if (cJSON_IsNumber(item2)) g_bambu_status.extruder_temp = item2->valueint;
                }
            }
        }
    }

    parse_lights(print);
    parse_tray(print);

    /* 解析 toolhead 位置，并记录最近一次位移时间 */
    cJSON *device = cJSON_GetObjectItem(print, "device");
    if (device) {
        cJSON *toolhead = cJSON_GetObjectItem(device, "toolhead");
        if (toolhead) {
            cJSON *x = cJSON_GetObjectItem(toolhead, "pos_x");
            cJSON *y = cJSON_GetObjectItem(toolhead, "pos_y");
            cJSON *z = cJSON_GetObjectItem(toolhead, "pos_z");
            if (cJSON_IsNumber(x) && cJSON_IsNumber(y) && cJSON_IsNumber(z)) {
                double pos_x = x->valuedouble;
                double pos_y = y->valuedouble;
                double pos_z = z->valuedouble;
                if (s_toolhead_position_valid &&
                    (pos_x - s_prev_toolhead_x > 0.05 || s_prev_toolhead_x - pos_x > 0.05 ||
                     pos_y - s_prev_toolhead_y > 0.05 || s_prev_toolhead_y - pos_y > 0.05 ||
                     pos_z - s_prev_toolhead_z > 0.05 || s_prev_toolhead_z - pos_z > 0.05)) {
                    g_bambu_status.toolhead_last_move_ms = esp_timer_get_time() / 1000;
                }
                s_prev_toolhead_x = pos_x;
                s_prev_toolhead_y = pos_y;
                s_prev_toolhead_z = pos_z;
                s_toolhead_position_valid = true;
                g_bambu_status.toolhead_x = pos_x;
                g_bambu_status.toolhead_y = pos_y;
                g_bambu_status.toolhead_z = pos_z;
            }
        }
    }

    /* 解析 gcode_line */
    cJSON *gcode_line = cJSON_GetObjectItem(print, "gcode_line");
    if (gcode_line && cJSON_IsString(gcode_line)) {
        const char *line = gcode_line->valuestring;
        strncpy(g_bambu_status.last_gcode_line, line,
                sizeof(g_bambu_status.last_gcode_line) - 1);
        g_bambu_status.gcode_line_count++;

        /* 打印含 E 的挤出指令 */
        if (strstr(line, "G1") && strstr(line, "E")) {
            ESP_LOGI(TAG, "挤出指令: %s", line);
        }
    }

    g_bambu_status.last_update_ms = (int64_t)time(NULL) * 1000;

    cJSON_Delete(root);
}

static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT 已连接，订阅: %s", s_cfg.topic_sub);
        g_bambu_status.connected = true;
        esp_mqtt_client_subscribe(s_client, s_cfg.topic_sub, 1);
        esp_mqtt_client_publish(s_client, s_cfg.topic_pub, PUSHALL_CMD, 0, 0, 0);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT 断开");
        g_bambu_status.connected = false;
        break;

    case MQTT_EVENT_DATA: {
        static char *frag_buf = NULL;
        static size_t frag_size = 0;
        static size_t frag_used = 0;

        if (event->current_data_offset == 0) {
            size_t need = event->total_data_len + 1;
            if (need > frag_size) {
                if (frag_buf) free(frag_buf);
                frag_buf = malloc(need);
                frag_size = need;
            }
            frag_used = 0;
        }
        if (!frag_buf) break;

        if (frag_used + event->data_len < frag_size) {
            memcpy(frag_buf + frag_used, event->data, event->data_len);
            frag_used += event->data_len;
            frag_buf[frag_used] = '\0';
        }

        if (frag_used >= event->total_data_len) {
            if (frag_buf[0] == '{') {
                parse_report(frag_buf, frag_used);
            }
            frag_used = 0;
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT 错误");
        g_bambu_status.connected = false;
        break;

    default:
        break;
    }
}

void bambu_mqtt_init(const bambu_config_t *cfg)
{
    s_cfg = *cfg;

    snprintf(s_cfg.topic_sub, sizeof(s_cfg.topic_sub),
             "device/%s/report", cfg->serial);
    snprintf(s_cfg.topic_pub, sizeof(s_cfg.topic_pub),
             "device/%s/request", cfg->serial);

    char uri[64];
    snprintf(uri, sizeof(uri), "mqtts://%s:8883", cfg->printer_ip);

    esp_mqtt_client_config_t mqtt_cfg = {0};
    mqtt_cfg.broker.address.uri = uri;
    mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
    mqtt_cfg.broker.verification.certificate = NULL;
    mqtt_cfg.broker.verification.certificate_len = 0;

    mqtt_cfg.credentials.username = "bblp";
    mqtt_cfg.credentials.authentication.password = cfg->access_code;
    mqtt_cfg.buffer.size     = 8192;
    mqtt_cfg.buffer.out_size = 2048;
    mqtt_cfg.network.reconnect_timeout_ms = 5000;
    mqtt_cfg.task.priority = 5;
    mqtt_cfg.task.stack_size = 6144;

    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    ESP_LOGI(TAG, "MQTT 启动: %s, SN=%s", uri, cfg->serial);
}

void bambu_mqtt_publish_raw(const char* payload)
{
    if (!s_client || !g_bambu_status.connected || !payload) {
        ESP_LOGW(TAG, "MQTT 未连接，无法发送命令");
        return;
    }
    esp_mqtt_client_publish(s_client, s_cfg.topic_pub, payload, 0, 0, 0);
}

void bambu_mqtt_request_status(void)
{
    if (s_client && g_bambu_status.connected) {
        esp_mqtt_client_publish(s_client, s_cfg.topic_pub, PUSHALL_CMD, 0, 0, 0);
    }
}

void bambu_mqtt_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    g_bambu_status.connected = false;
}

bool bambu_mqtt_toolhead_moving(void)
{
    int64_t last_move_ms = g_bambu_status.toolhead_last_move_ms;
    int64_t now_ms = esp_timer_get_time() / 1000;
    return last_move_ms > 0 && now_ms >= last_move_ms &&
           now_ms - last_move_ms <= TOOLHEAD_MOTION_WINDOW_MS;
}

void bambu_mqtt_get_status_json(char *out, size_t out_len)
{
    time_t last_sec = g_bambu_status.last_update_ms / 1000;

    snprintf(out, out_len,
        "{"
        "\"connected\":%s,"
        "\"state\":\"%s\","
        "\"nozzle\":%d,"
        "\"nozzle_target\":%d,"
        "\"bed\":%d,"
        "\"bed_target\":%d,"
        "\"progress\":%d,"
        "\"remaining\":%d,"
        "\"layer\":%d,"
        "\"total_layer\":%d,"
        "\"error\":%d,"
        "\"hw_switch\":%d,"
        "\"subtask\":\"%s\","
        "\"wifi\":\"%s\","
        "\"ams_status\":%d,"
        "\"ams_exist\":%d,"
        "\"hms\":%d,"
        "\"nozzle_type\":\"%s\","
        "\"nozzle_diameter\":\"%s\","
        "\"tray_type\":\"%s\","
        "\"tray_color\":\"%s\","
        "\"chamber_light\":%s,"
        "\"work_light\":%s,"
        "\"rtsp\":\"%s\","
        "\"extruder_stat\":%d,"
        "\"extruder_info\":%d,"
        "\"extruder_snow\":%d,"
        "\"extruder_star\":%d,"
        "\"extruder_temp\":%d,"
        "\"toolhead_x\":%.2f,"
        "\"toolhead_y\":%.2f,"
        "\"toolhead_z\":%.2f,"
        "\"toolhead_moving\":%s,"
        "\"last_gcode_line\":\"%s\","
        "\"gcode_line_count\":%u,"
        "\"last_update\":%lld"
        "}",
        g_bambu_status.connected ? "true" : "false",
        g_bambu_status.gcode_state[0] ? g_bambu_status.gcode_state : "unknown",
        g_bambu_status.nozzle_temper,
        g_bambu_status.nozzle_target_temper,
        g_bambu_status.bed_temper,
        g_bambu_status.bed_target_temper,
        g_bambu_status.mc_percent,
        g_bambu_status.mc_remaining_time,
        g_bambu_status.layer_num,
        g_bambu_status.total_layer_num,
        g_bambu_status.print_error,
        g_bambu_status.hw_switch_state,
        g_bambu_status.subtask_name,
        g_bambu_status.wifi_signal,
        g_bambu_status.ams_status,
        g_bambu_status.ams_exist,
        g_bambu_status.hms_count,
        g_bambu_status.nozzle_type,
        g_bambu_status.nozzle_diameter,
        g_bambu_status.tray_type,
        g_bambu_status.tray_color,
        g_bambu_status.chamber_light_on ? "true" : "false",
        g_bambu_status.work_light_on ? "true" : "false",
        g_bambu_status.rtsp_url,
        g_bambu_status.extruder_stat,
        g_bambu_status.extruder_info,
        g_bambu_status.extruder_snow,
        g_bambu_status.extruder_star,
        g_bambu_status.extruder_temp,
        g_bambu_status.toolhead_x,
        g_bambu_status.toolhead_y,
        g_bambu_status.toolhead_z,
        bambu_mqtt_toolhead_moving() ? "true" : "false",
        g_bambu_status.last_gcode_line,
        (unsigned int)g_bambu_status.gcode_line_count,
        (long long)last_sec
    );
}
