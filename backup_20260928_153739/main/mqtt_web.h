#pragma once
#include "bambu_mqtt.h"
#include "esp_http_server.h"

namespace mqtt_web {

// 全局实例（在 main.c 中定义）
extern bambu::BambuMqtt g_bambu;

// 处理 GET /printer/status，返回 JSON
esp_err_t status_handler(httpd_req_t* req) {
    const auto& s = g_bambu.status();
    if (!s.valid) {
        httpd_resp_set_type(req, "application/json");
        const char* err = R"({"connected":false})";
        httpd_resp_send(req, err, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char buf[512];
    snprintf(buf, sizeof(buf),
        R"({"connected":%s,"state":"%s","nozzle":%d,"nozzle_target":%d,)"
        R"("bed":%d,"bed_target":%d,"progress":%d,"remaining":%d,)"
        R"("layer":%d,"total_layer":%d,"error":%d,"hw_switch":%d})",
        g_bambu.is_connected() ? "true" : "false",
        s.gcode_state, s.nozzle_temper, s.nozzle_target_temper,
        s.bed_temper, s.bed_target_temper, s.mc_percent,
        s.mc_remaining_time, s.layer_num, s.total_layer_num,
        s.print_error, s.hw_switch_state);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// 处理 POST /printer/connect，接收 ip/serial/pass 并连接
esp_err_t connect_handler(httpd_req_t* req) {
    char buf[256];
    size_t remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too large");
        return ESP_FAIL;
    }
    int ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ip[32] = {0}, serial[32] = {0}, pass[16] = {0};
    if (httpd_query_key_value(buf, "ip", ip, sizeof(ip)) != ESP_OK ||
        httpd_query_key_value(buf, "serial", serial, sizeof(serial)) != ESP_OK ||
        httpd_query_key_value(buf, "pass", pass, sizeof(pass)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing fields");
        return ESP_FAIL;
    }

    g_bambu.start(ip, serial, pass);
    httpd_resp_send(req, "连接中，请稍后刷新状态", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

} // namespace mqtt_web
