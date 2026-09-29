#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "dns_server.h"
#include "bambu_mqtt.h"
#include "bambu_cmd.h"
#include "config_store.h"
#include "filament.h"
#include "ota.h"

static const char *TAG = "TOP_AMS_Pro";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char wifi_config_html_start[] asm("_binary_wifi_config_html_start");
extern const char wifi_config_html_end[]   asm("_binary_wifi_config_html_end");
extern const char printer_html_start[] asm("_binary_printer_html_start");
extern const char printer_html_end[]   asm("_binary_printer_html_end");
extern const char ota_html_start[] asm("_binary_ota_html_start");
extern const char ota_html_end[]   asm("_binary_ota_html_end");
extern const char monitor_html_start[] asm("_binary_monitor_html_start");
extern const char monitor_html_end[]   asm("_binary_monitor_html_end");

static SemaphoreHandle_t s_config_sem = NULL;
static SemaphoreHandle_t s_switch_sem = NULL;
static httpd_handle_t s_server = NULL;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;

static char s_pending_ssid[33] = {0};
static char s_pending_pass[65] = {0};
static bool s_time_synced = false;

/* ---------- 网页日志缓冲 ---------- */
#define LOG_BUF_LINES   100
#define LOG_LINE_SIZE   200

static char s_log_buf[LOG_BUF_LINES][LOG_LINE_SIZE];
static int  s_log_head = 0;
static int  s_log_count = 0;
static uint32_t s_log_total_written = 0;  /* 单调递增总序号 */
static SemaphoreHandle_t s_log_mutex = NULL;

static void log_push(const char *line)
{
    if (!s_log_mutex) return;
    if (xSemaphoreTake(s_log_mutex, 0) != pdTRUE) return;

    strncpy(s_log_buf[s_log_head], line, LOG_LINE_SIZE - 1);
    s_log_buf[s_log_head][LOG_LINE_SIZE - 1] = '\0';

    s_log_head = (s_log_head + 1) % LOG_BUF_LINES;
    if (s_log_count < LOG_BUF_LINES) s_log_count++;
    s_log_total_written++;

    xSemaphoreGive(s_log_mutex);
}


/* ---------- 自定义日志输出（真实时间）---------- */
static int custom_log_vprintf(const char *fmt, va_list args)
{
    char line[256];
    va_list args_copy;
    va_copy(args_copy, args);
    vsnprintf(line, sizeof(line), fmt, args_copy);
    va_end(args_copy);

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char time_buf[40];
    if (timeinfo.tm_year + 1900 < 2024) {
        snprintf(time_buf, sizeof(time_buf), "[%8lld]",
                 (long long)(esp_timer_get_time() / 1000));
    } else {
        snprintf(time_buf, sizeof(time_buf), "[%04d-%02d-%02d %02d:%02d:%02d]",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }

    printf("%s ", time_buf);
    int ret = vprintf(fmt, args);

    // 拼接：截断 line 到 150 字节，避免溢出
    char full_line[LOG_LINE_SIZE];
    snprintf(full_line, sizeof(full_line), "%.40s %.150s", time_buf, line);
    log_push(full_line);

    return ret;
}

static bool check_time_synced(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    return (timeinfo.tm_year + 1900 >= 2024);
}

/* ---------- 连通性检测重定向 ---------- */
static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.1.1/");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---------- 主界面 ---------- */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

/* ---------- WiFi 配置页 ---------- */
static esp_err_t wifi_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, wifi_config_html_start, wifi_config_html_end - wifi_config_html_start);
    return ESP_OK;
}

/* ---------- 打印机配置页 ---------- */
static esp_err_t printer_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, printer_html_start, printer_html_end - printer_html_start);
    return ESP_OK;
}

/* ---------- WiFi 扫描 ---------- */
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;
    if (ap_count == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }
    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));
    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        if (strlen((char *)ap_list[i].ssid) == 0) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(item, "auth", ap_list[i].authmode);
        cJSON_AddItemToArray(root, item);
    }
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    free(ap_list);
    return ESP_OK;
}

/* ---------- 提交 WiFi ---------- */
static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char buf[256];
    size_t remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }
    int ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    if (httpd_query_key_value(buf, "ssid", s_pending_ssid, sizeof(s_pending_ssid)) != ESP_OK ||
        httpd_query_key_value(buf, "password", s_pending_pass, sizeof(s_pending_pass)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid or password");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Received SSID: %s", s_pending_ssid);

    app_wifi_config_t wcfg = {0};
    strncpy(wcfg.ssid, s_pending_ssid, sizeof(wcfg.ssid) - 1);
    strncpy(wcfg.password, s_pending_pass, sizeof(wcfg.password) - 1);
    config_save_wifi(&wcfg);

    static char page[2048];
    snprintf(page, sizeof(page),
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>连接中...</title>"
        "<style>"
        "body{font-family:-apple-system,Arial;max-width:400px;margin:60px auto;padding:16px;text-align:center;}"
        ".spinner{border:4px solid #f3f3f3;border-top:4px solid #007bff;border-radius:50%%;"
        "width:40px;height:40px;animation:spin 1s linear infinite;margin:20px auto;}"
        "@keyframes spin{0%%{transform:rotate(0deg)}100%%{transform:rotate(360deg)}}"
        ".status{color:#666;font-size:15px;margin-top:16px;}"
        ".ok{color:#28a745;font-size:18px;font-weight:bold;}"
        ".err{color:#dc3545;}"
        "button{width:100%%;padding:14px;background:#007bff;color:#fff;border:none;"
        "border-radius:6px;font-size:16px;margin-top:24px;display:none;cursor:pointer;}"
        "</style></head><body>"
        "<h3>正在连接 WiFi</h3>"
        "<p style='color:#666;'>SSID: <b>%s</b></p>"
        "<div class='spinner' id='spinner'></div>"
        "<div class='status' id='status'>正在连接，请稍候...</div>"
        "<button id='backBtn' onclick=\"location.href='/'\">返回主界面</button>"
        "<script>"
        "let tries=0;"
        "function check(){"
        "  tries++;"
        "  fetch('/connect/status',{cache:'no-store'})"
        "    .then(r=>r.json())"
        "    .then(s=>{"
        "      if(s.connected){"
        "        document.getElementById('spinner').style.display='none';"
        "        document.getElementById('status').innerHTML='<span class=\"ok\">连接成功！</span>';"
        "        var target = s.ip ? ('http://' + s.ip + '/') : '/';"
        "        setTimeout(()=>location.href=target,1500);"
        "      } else if(tries>20){"
        "        document.getElementById('spinner').style.display='none';"
        "        document.getElementById('status').innerHTML='<span class=\"err\">连接超时，请检查密码</span>';"
        "        document.getElementById('backBtn').style.display='block';"
        "      } else {"
        "        setTimeout(check,1500);"
        "      }"
        "    })"
        "    .catch(()=>setTimeout(check,1500));"
        "}"
        "setTimeout(check,1500);"
        "</script></body></html>",
        s_pending_ssid);

    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    xSemaphoreGive(s_config_sem);
    return ESP_OK;
}

/* ---------- 连接状态查询 ---------- */
static esp_err_t connect_status_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t ap_info;
    bool connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);

    esp_netif_ip_info_t ip_info = {0};
    if (connected && s_sta_netif) {
        esp_netif_get_ip_info(s_sta_netif, &ip_info);
    }

    char out[128];
    snprintf(out, sizeof(out),
        "{\"connected\":%s,\"ip\":\"" IPSTR "\"}",
        connected ? "true" : "false",
        IP2STR(&ip_info.ip));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---------- 提交打印机 ---------- */
static esp_err_t bambu_post_handler(httpd_req_t *req)
{
    char buf[256];
    if (req->content_len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too large");
        return ESP_FAIL;
    }
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    bambu_config_t cfg = {0};
    if (httpd_query_key_value(buf, "ip",     cfg.printer_ip,  sizeof(cfg.printer_ip))  != ESP_OK ||
        httpd_query_key_value(buf, "serial", cfg.serial,      sizeof(cfg.serial))      != ESP_OK ||
        httpd_query_key_value(buf, "code",   cfg.access_code, sizeof(cfg.access_code)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing field");
        return ESP_FAIL;
    }

    config_save_bambu(&cfg);
    bambu_mqtt_init(&cfg);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t bambu_status_get_handler(httpd_req_t *req)
{
    char json[1536];
    bambu_mqtt_get_status_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---------- 打印机控制 API ---------- */
static esp_err_t bambu_cmd_post_handler(httpd_req_t *req)
{
    char buf[128];
    if (req->content_len >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too large");
        return ESP_FAIL;
    }
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char cmd[32] = {0};
    if (httpd_query_key_value(buf, "cmd", cmd, sizeof(cmd)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing cmd");
        return ESP_FAIL;
    }

    const char* payload = NULL;
    if      (strcmp(cmd, "pause")     == 0) payload = CMD_PRINT_PAUSE;
    else if (strcmp(cmd, "resume")    == 0) payload = CMD_PRINT_RESUME;
    else if (strcmp(cmd, "done")      == 0) payload = CMD_CLICK_DONE;
    else if (strcmp(cmd, "retry")     == 0) payload = CMD_CHICK_RESUME;
    else if (strcmp(cmd, "clean")     == 0) payload = CMD_ERROR_CLEAN;
    else if (strcmp(cmd, "status")    == 0) payload = CMD_GET_STATUS;
    else if (strcmp(cmd, "light_on")  == 0) payload = CMD_LED_ON;
    else if (strcmp(cmd, "light_off") == 0) payload = CMD_LED_OFF;
    else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown cmd");
        return ESP_FAIL;
    }

    bambu_send_cmd(payload);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---------- 返回已保存配置 ---------- */
static esp_err_t config_get_handler(httpd_req_t *req)
{
    char out[640];
    app_wifi_config_t wcfg = {0};
    bambu_config_t bcfg = {0};
    bool has_wifi = config_load_wifi(&wcfg);
    bool has_bambu = config_load_bambu(&bcfg);

    snprintf(out, sizeof(out),
        "{"
        "\"wifi_ssid\":\"%s\","
        "\"wifi_pass\":\"%s\","
        "\"bambu_ip\":\"%s\","
        "\"bambu_sn\":\"%s\","
        "\"bambu_code\":\"%s\","
        "\"has_wifi\":%s,"
        "\"has_bambu\":%s"
        "}",
        has_wifi ? wcfg.ssid : "",
        has_wifi ? wcfg.password : "",
        has_bambu ? bcfg.printer_ip : "",
        has_bambu ? bcfg.serial : "",
        has_bambu ? bcfg.access_code : "",
        has_wifi ? "true" : "false",
        has_bambu ? "true" : "false"
    );
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---------- 网络时间 API ---------- */
static esp_err_t time_get_handler(httpd_req_t *req)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char out[128];
    snprintf(out, sizeof(out),
        "{\"timestamp\":%lld,\"time\":\"%04d-%02d-%02d %02d:%02d:%02d\"}",
        (long long)now,
        timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---------- 耗材通道 API ---------- */
static esp_err_t filament_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "current", g_current_channel);
    cJSON_AddBoolToObject(root, "printer_sync", g_printer_sync);
    cJSON_AddNumberToObject(root, "slow_pulse", g_slow_feed_pulse_ms);
    cJSON_AddNumberToObject(root, "slow_gap", g_slow_feed_gap_ms);
    cJSON_AddNumberToObject(root, "buffer_gpio", g_buffer_switch_gpio);
    cJSON_AddNumberToObject(root, "buffer_feed", g_buffer_feed_ms);
    cJSON_AddNumberToObject(root, "buffer_cooldown", g_buffer_cooldown_ms);
    cJSON_AddNumberToObject(root, "feed_timeout", g_feed_timeout_ms);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < 8; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", g_channels[i].id);
        cJSON_AddStringToObject(item, "name", g_channels[i].name);
        cJSON_AddNumberToObject(item, "load_time", g_channels[i].load_time_ms);
        cJSON_AddNumberToObject(item, "uload_time", g_channels[i].uload_time_ms);
        cJSON_AddStringToObject(item, "material", g_channels[i].material);
        cJSON_AddStringToObject(item, "color", g_channels[i].color);   // 必须有这行
        cJSON_AddNumberToObject(item, "state", filament_get_state(g_channels[i].id));
        cJSON_AddBoolToObject(item, "reverse", g_channels[i].reverse);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "channels", arr);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t filament_time_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ch_str[8] = {0}, type[16] = {0}, time_str[16] = {0};
    if (httpd_query_key_value(buf, "ch", ch_str, sizeof(ch_str)) != ESP_OK ||
        httpd_query_key_value(buf, "type", type, sizeof(type)) != ESP_OK ||
        httpd_query_key_value(buf, "time", time_str, sizeof(time_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing field");
        return ESP_FAIL;
    }

    int ch = atoi(ch_str);
    int ms = atoi(time_str);

    if (strcmp(type, "load") == 0) filament_set_load_time(ch, ms);
    else if (strcmp(type, "uload") == 0) filament_set_uload_time(ch, ms);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t filament_action_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ch_str[8] = {0}, action[16] = {0};
    if (httpd_query_key_value(buf, "ch", ch_str, sizeof(ch_str)) != ESP_OK ||
        httpd_query_key_value(buf, "action", action, sizeof(action)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing field");
        return ESP_FAIL;
    }

    int ch = atoi(ch_str);

    if (strcmp(action, "forward") == 0) {
        filament_forward_async(ch);
    } else if (strcmp(action, "backward") == 0) {
        filament_backward_async(ch);
    } else if (strcmp(action, "load") == 0) {
        filament_load_async(ch);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---------- 注册所有路由 ---------- */
/* ---------- 耗材类型 API ---------- */
static esp_err_t filament_material_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ch_str[8] = {0}, mat[16] = {0};
    if (httpd_query_key_value(buf, "ch", ch_str, sizeof(ch_str)) != ESP_OK ||
        httpd_query_key_value(buf, "material", mat, sizeof(mat)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing field");
        return ESP_FAIL;
    }

    int ch = atoi(ch_str);
    filament_set_material(ch, mat);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---------- 打印机联动 API ---------- */

static esp_err_t filament_timeout_post_handler(httpd_req_t *req)
{
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char val_str[16] = {0};
    if (httpd_query_key_value(buf, "timeout", val_str, sizeof(val_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing timeout");
        return ESP_FAIL;
    }

    int timeout = atoi(val_str);
    filament_set_feed_timeout(timeout);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t filament_sync_post_handler(httpd_req_t *req)
{
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char sync_str[8] = {0};
    if (httpd_query_key_value(buf, "sync", sync_str, sizeof(sync_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing sync");
        return ESP_FAIL;
    }

    bool sync = (atoi(sync_str) != 0);
    filament_set_printer_sync(sync);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ota_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, ota_html_start, ota_html_end - ota_html_start);
    return ESP_OK;
}

/* ---------- 网页日志 API ---------- */
static esp_err_t logs_get_handler(httpd_req_t *req)
{
    char query[32] = {0};
    uint32_t since = 0;
    if (httpd_req_get_url_query_len(req) > 0) {
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char param[16];
            if (httpd_query_key_value(query, "since", param, sizeof(param)) == ESP_OK) {
                since = (uint32_t)strtoul(param, NULL, 10);
            }
        }
    }

    cJSON *arr = cJSON_CreateArray();
    uint32_t total = 0;

    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        total = s_log_total_written;

        /* 计算要返回的起始序号：
         * 如果 since 太旧（超过缓冲区容量），从最旧的开始
         * 否则从 since 开始
         */
        uint32_t oldest = (total > LOG_BUF_LINES) ? (total - LOG_BUF_LINES) : 0;
        uint32_t start = (since > oldest) ? since : oldest;

        /* 遍历从 start 到 total 的每一条 */
        for (uint32_t i = start; i < total; i++) {
            /* 计算在循环缓冲里的位置 */
            uint32_t idx = i % LOG_BUF_LINES;
            cJSON_AddItemToArray(arr, cJSON_CreateString(s_log_buf[idx]));
        }

        xSemaphoreGive(s_log_mutex);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "total", total);
    cJSON_AddItemToObject(root, "logs", arr);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ---------- 颜色设置 API ---------- */

/* ---------- 方向切换 API ---------- */
static esp_err_t filament_reverse_post_handler(httpd_req_t *req)
{
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ch_str[8] = {0}, rev_str[8] = {0};
    if (httpd_query_key_value(buf, "ch", ch_str, sizeof(ch_str)) != ESP_OK ||
        httpd_query_key_value(buf, "reverse", rev_str, sizeof(rev_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing field");
        return ESP_FAIL;
    }

    int ch = atoi(ch_str);
    bool rev = (atoi(rev_str) != 0);
    filament_set_reverse(ch, rev);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t filament_color_post_handler(httpd_req_t *req)
{
   char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ch_str[8] = {0}, color[32] = {0};
    if (httpd_query_key_value(buf, "ch", ch_str, sizeof(ch_str)) != ESP_OK ||
        httpd_query_key_value(buf, "color", color, sizeof(color)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing field");
        return ESP_FAIL;
    }

    // URL 解码：把 %23 替换回 #
    char decoded[16] = {0};
    int di = 0;
    for (int i = 0; color[i] && di < 15; ) {
        if (color[i] == '%' && color[i+1] && color[i+2]) {
            // 解析十六进制
            char hex[3] = { color[i+1], color[i+2], 0 };
            decoded[di++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else {
            decoded[di++] = color[i++];
        }
    }
    decoded[di] = '\0';

    int ch = atoi(ch_str);
    filament_set_color(ch, decoded);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---------- 缓慢进料参数 API ---------- */
static esp_err_t filament_slow_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char pulse_str[16] = {0}, gap_str[16] = {0};
    if (httpd_query_key_value(buf, "pulse", pulse_str, sizeof(pulse_str)) != ESP_OK ||
        httpd_query_key_value(buf, "gap", gap_str, sizeof(gap_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing field");
        return ESP_FAIL;
    }

    int pulse = atoi(pulse_str);
    int gap = atoi(gap_str);
    filament_set_slow_feed(pulse, gap);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---------- 缓慢进料测试 API ---------- */
static esp_err_t filament_slow_test_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ch_str[8] = {0}, pulse_str[16] = {0}, gap_str[16] = {0}, dur_str[16] = {0};
    if (httpd_query_key_value(buf, "ch", ch_str, sizeof(ch_str)) != ESP_OK ||
        httpd_query_key_value(buf, "pulse", pulse_str, sizeof(pulse_str)) != ESP_OK ||
        httpd_query_key_value(buf, "gap", gap_str, sizeof(gap_str)) != ESP_OK ||
        httpd_query_key_value(buf, "duration", dur_str, sizeof(dur_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing field");
        return ESP_FAIL;
    }

    int ch = atoi(ch_str);
    int pulse = atoi(pulse_str);
    int gap = atoi(gap_str);
    int duration = atoi(dur_str);

    filament_slow_test_async(ch, pulse, gap, duration);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t monitor_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, monitor_html_start, monitor_html_end - monitor_html_start);
    return ESP_OK;
}

/* ---------- 缓冲参数 API ---------- */
static esp_err_t filament_buffer_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char gpio_str[8] = {0}, feed_str[16] = {0}, cool_str[16] = {0};
    if (httpd_query_key_value(buf, "gpio", gpio_str, sizeof(gpio_str)) != ESP_OK ||
        httpd_query_key_value(buf, "feed", feed_str, sizeof(feed_str)) != ESP_OK ||
        httpd_query_key_value(buf, "cooldown", cool_str, sizeof(cool_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing field");
        return ESP_FAIL;
    }

    filament_set_buffer_switch(atoi(gpio_str));
    filament_set_buffer_feed(atoi(feed_str));
    filament_set_buffer_cooldown(atoi(cool_str));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


/* ---------- G-code 版换料接口 ---------- */
static esp_err_t filament_gcode_post_handler(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ch_str[8] = {0}, action[16] = {0};
    if (httpd_query_key_value(buf, "ch", ch_str, sizeof(ch_str)) != ESP_OK ||
        httpd_query_key_value(buf, "action", action, sizeof(action)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing field");
        return ESP_FAIL;
    }

    int ch = atoi(ch_str);

    if (strcmp(action, "forward") == 0) {
        filament_gcode_forward_async(ch);
    } else if (strcmp(action, "backward") == 0) {
        filament_gcode_backward_async(ch);
    } else if (strcmp(action, "load") == 0) {
        filament_gcode_load_async(ch);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown action");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void register_all_handlers(httpd_handle_t server)
{
    httpd_uri_t root = { .uri="/", .method=HTTP_GET, .handler=root_get_handler };
    httpd_uri_t wifi_page = { .uri="/wifi", .method=HTTP_GET, .handler=wifi_page_get_handler };
    httpd_uri_t printer_page = { .uri="/printer", .method=HTTP_GET, .handler=printer_page_get_handler };
    httpd_uri_t scan = { .uri="/scan", .method=HTTP_GET, .handler=scan_get_handler };
    httpd_uri_t connect = { .uri="/connect", .method=HTTP_POST, .handler=connect_post_handler };
    httpd_uri_t connect_status = { .uri="/connect/status", .method=HTTP_GET, .handler=connect_status_get_handler };
    httpd_uri_t bambu_post = { .uri="/bambu", .method=HTTP_POST, .handler=bambu_post_handler };
    httpd_uri_t bambu_status = { .uri="/bambu/status", .method=HTTP_GET, .handler=bambu_status_get_handler };
    httpd_uri_t bambu_cmd = { .uri="/bambu/cmd", .method=HTTP_POST, .handler=bambu_cmd_post_handler };
    httpd_uri_t config_get = { .uri="/config", .method=HTTP_GET, .handler=config_get_handler };
    httpd_uri_t time_get = { .uri="/time", .method=HTTP_GET, .handler=time_get_handler };
    httpd_uri_t filament_get = { .uri="/filament", .method=HTTP_GET, .handler=filament_get_handler };
    httpd_uri_t filament_time = { .uri="/filament/time", .method=HTTP_POST, .handler=filament_time_post_handler };
    httpd_uri_t filament_action = { .uri="/filament/action", .method=HTTP_POST, .handler=filament_action_post_handler };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &wifi_page);
    httpd_register_uri_handler(server, &printer_page);
    httpd_register_uri_handler(server, &scan);
    httpd_register_uri_handler(server, &connect);
    httpd_register_uri_handler(server, &connect_status);
    httpd_register_uri_handler(server, &bambu_post);
    httpd_register_uri_handler(server, &bambu_status);
    httpd_register_uri_handler(server, &bambu_cmd);
    httpd_register_uri_handler(server, &config_get);
    httpd_register_uri_handler(server, &time_get);
    httpd_register_uri_handler(server, &filament_get);
    httpd_register_uri_handler(server, &filament_time);
    httpd_register_uri_handler(server, &filament_action);
    httpd_uri_t filament_slow = { .uri="/filament/slow", .method=HTTP_POST, .handler=filament_slow_post_handler };
    httpd_register_uri_handler(server, &filament_slow);
    httpd_uri_t filament_buffer = { .uri="/filament/buffer", .method=HTTP_POST, .handler=filament_buffer_post_handler };
    httpd_register_uri_handler(server, &filament_buffer);
    httpd_uri_t filament_slow_test = { .uri="/filament/slow/test", .method=HTTP_POST, .handler=filament_slow_test_post_handler };
    httpd_register_uri_handler(server, &filament_slow_test);
    httpd_uri_t filament_color = { .uri="/filament/color", .method=HTTP_POST, .handler=filament_color_post_handler };
    httpd_register_uri_handler(server, &filament_color);
    httpd_uri_t logs_get = { .uri="/logs", .method=HTTP_GET, .handler=logs_get_handler };
    httpd_register_uri_handler(server, &logs_get);
    httpd_uri_t ota_page = { .uri="/ota", .method=HTTP_GET, .handler=ota_page_get_handler };
    httpd_uri_t ota_upload = { .uri="/ota/upload", .method=HTTP_POST, .handler=ota_upload_handler };
    httpd_register_uri_handler(server, &ota_page);
    httpd_uri_t monitor_page = { .uri="/monitor", .method=HTTP_GET, .handler=monitor_page_get_handler };
    httpd_register_uri_handler(server, &monitor_page);
    httpd_register_uri_handler(server, &ota_upload);
    httpd_uri_t filament_sync = { .uri="/filament/sync", .method=HTTP_POST, .handler=filament_sync_post_handler };
    httpd_uri_t filament_timeout = { .uri="/filament/timeout", .method=HTTP_POST, .handler=filament_timeout_post_handler };
    httpd_register_uri_handler(server, &filament_sync);
    httpd_register_uri_handler(server, &filament_timeout);
    httpd_uri_t filament_material = { .uri="/filament/material", .method=HTTP_POST, .handler=filament_material_post_handler };
    httpd_register_uri_handler(server, &filament_material);
    httpd_uri_t filament_reverse = { .uri="/filament/reverse", .method=HTTP_POST, .handler=filament_reverse_post_handler };
    httpd_register_uri_handler(server, &filament_reverse);

    httpd_uri_t filament_gcode = {
        .uri = "/filament/gcode",
        .method = HTTP_POST,
        .handler = filament_gcode_post_handler
    };
    httpd_register_uri_handler(server, &filament_gcode);


    const char *captive_urls[] = {
        "/hotspot-detect.html", "/library/test/success.html",
        "/generate_204", "/gen_204",
        "/connecttest.txt", "/ncsi.txt", "/redirect",
    };
    for (int i = 0; i < sizeof(captive_urls)/sizeof(captive_urls[0]); i++) {
        httpd_uri_t u = { .uri=captive_urls[i], .method=HTTP_GET, .handler=captive_redirect_handler };
        httpd_register_uri_handler(server, &u);
    }
    httpd_uri_t catch_all = { .uri="/*", .method=HTTP_GET, .handler=captive_redirect_handler };
    httpd_register_uri_handler(server, &catch_all);
}

/* ---------- 启动 HTTP 服务器 ---------- */
static void start_webserver(void)
{
    if (s_server) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.keep_alive_enable = false;   // 关键：关闭 keep-alive
    config.max_open_sockets = 7;        // 限制并发连接
    config.max_uri_handlers = 32;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务器启动失败");
        s_server = NULL;
        return;
    }
    register_all_handlers(s_server);
    ESP_LOGI(TAG, "HTTP 服务器已启动");
}

/* ---------- 启动 AP ---------- */
static void wifi_init_softap(void)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();

    /* 设置 AP IP 为 192.168.1.1 */
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip,      192, 168, 1, 1);
    IP4_ADDR(&ip_info.gw,      192, 168, 1, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ip_info);
    esp_netif_dhcps_start(s_ap_netif);

    assert(s_ap_netif);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "TOP AMS Pro",
            .ssid_len = strlen("TOP AMS Pro"),
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP: TOP AMS Pro / (无密码), http://192.168.1.1");
}

/* ---------- 连接 STA ---------- */
static void wifi_init_sta(void)
{
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, s_pending_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, s_pending_pass, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_connect());
}

/* ---------- 切换到纯 STA 的独立任务 ---------- */
static void switch_to_sta_task(void *arg)
{
    xSemaphoreTake(s_switch_sem, portMAX_DELAY);
    ESP_LOGI(TAG, "开始切换到纯 STA 模式...");

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP 服务器已停止");
    }

    dns_server_stop();
    ESP_LOGI(TAG, "DNS 服务器已停止");

    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
        ESP_LOGI(TAG, "AP netif 已销毁");
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "已切换到纯 STA 模式");

    vTaskDelay(pdMS_TO_TICKS(500));

    start_webserver();
    if (s_server) {
        ESP_LOGI(TAG, "HTTP 服务器已重启，监听 STA 网卡");
    } else {
        ESP_LOGE(TAG, "HTTP 服务器重启失败！");
    }

    vTaskDelete(NULL);
}

/* ---------- SNTP 时间同步任务（失败每分钟重试）---------- */
static void time_sync_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));

    while (!s_time_synced) {
        ESP_LOGI(TAG, "启动 SNTP 同步...");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "cn.ntp.org.cn");
        esp_sntp_setservername(2, "ntp.aliyun.com");
        esp_sntp_init();

        bool ok = false;
        for (int i = 0; i < 5; i++) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (check_time_synced()) {
                ok = true;
                break;
            }
        }

        esp_sntp_stop();

        if (ok) {
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);
            ESP_LOGI(TAG, "时间同步成功: %04d-%02d-%02d %02d:%02d:%02d",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            s_time_synced = true;
            break;
        }

        ESP_LOGW(TAG, "时间同步失败，60 秒后重试");
        vTaskDelay(pdMS_TO_TICKS(60000));
    }

    vTaskDelete(NULL);
}

/* ---------- 事件处理 ---------- */
static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA 断开，重连中...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;

        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "  联网成功！");
        ESP_LOGI(TAG, "  STA IP   : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "  子网掩码 : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "  网关     : " IPSTR, IP2STR(&event->ip_info.gw));
        ESP_LOGI(TAG, "  访问地址 : http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "========================================");

        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_APSTA) {
            xSemaphoreGive(s_switch_sem);
        }
    }
}

/* ---------- 配网任务 ---------- */
static void web_prov_task(void *arg)
{
    xSemaphoreTake(s_config_sem, portMAX_DELAY);
    ESP_LOGI(TAG, "收到配置，开始连接 STA...");
    wifi_init_sta();
    vTaskDelete(NULL);
}

/* ---------- 入口 ---------- */
/* ---------- socket 监控任务 ---------- */
static void socket_monitor_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));   /* 每 60 秒 */
        ESP_LOGI(TAG, "Free heap: %lu",
                 (unsigned long)esp_get_free_heap_size());
    }
}

void app_main(void)
{
      esp_log_set_vprintf(custom_log_vprintf);

    printf("=== before nvs init ===\n");
    fflush(stdout);
    esp_err_t ret = nvs_flash_init_partition("nvs");
    printf("=== nvs init ret=%d ===\n", ret);
    fflush(stdout);

    if (ret != ESP_OK) {
        printf("=== erasing nvs partition ===\n");
    fflush(stdout);
        nvs_flash_erase_partition("nvs");
        ret = nvs_flash_init_partition("nvs");
        printf("=== nvs re-init ret=%d ===\n", ret);
    fflush(stdout);
    }

printf("=== before netif init ===\n");
    fflush(stdout);
ESP_ERROR_CHECK(esp_netif_init());
printf("=== after netif init ===\n");
    fflush(stdout);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    s_log_mutex = xSemaphoreCreateMutex();
    assert(s_log_mutex);
    s_config_sem = xSemaphoreCreateBinary();
    assert(s_config_sem);
    s_switch_sem = xSemaphoreCreateBinary();
    assert(s_switch_sem);

    s_sta_netif = esp_netif_create_default_wifi_sta();
    assert(s_sta_netif);

    filament_init();
    filament_uload_start();
    filament_load_start();
    filament_bed_signal_start();
    filament_buffer_start();

    wifi_init_softap();
    start_webserver();
    dns_server_start("192.168.1.1");

    xTaskCreate(switch_to_sta_task, "switch_sta", 4096, NULL, 5, NULL);
    xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 3, NULL);

    app_wifi_config_t saved_wcfg = {0};
    if (config_load_wifi(&saved_wcfg)) {
        ESP_LOGI(TAG, "发现已保存 WiFi: %s，自动连接...", saved_wcfg.ssid);
        strncpy(s_pending_ssid, saved_wcfg.ssid, sizeof(s_pending_ssid) - 1);
        strncpy(s_pending_pass, saved_wcfg.password, sizeof(s_pending_pass) - 1);
        wifi_init_sta();
    } else {
        ESP_LOGI(TAG, "未找到已保存 WiFi，等待配网...");
    }

    bambu_config_t saved_bcfg = {0};
    if (config_load_bambu(&saved_bcfg)) {
        ESP_LOGI(TAG, "发现已保存打印机，自动连接...");
        bambu_mqtt_init(&saved_bcfg);
    }

    xTaskCreate(socket_monitor_task, "sock_mon", 2048, NULL, 1, NULL);
    xTaskCreate(web_prov_task, "web_prov", 4096, NULL, 5, NULL);
}