#include "config_store.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "config_store";
static const char *NS = "app_cfg";

void config_store_init(void) {}

bool config_load_wifi(app_wifi_config_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(out->ssid);
    if (nvs_get_str(h, "wifi_ssid", out->ssid, &len) != ESP_OK) {
        nvs_close(h); return false;
    }
    len = sizeof(out->password);
    if (nvs_get_str(h, "wifi_pass", out->password, &len) != ESP_OK) {
        out->password[0] = '\0';
    }
    nvs_close(h);
    return out->ssid[0] != '\0';
}

void config_save_wifi(const app_wifi_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "wifi_ssid", cfg->ssid);
    nvs_set_str(h, "wifi_pass", cfg->password);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi 已保存: %s", cfg->ssid);
}

bool config_has_wifi(void)
{
    app_wifi_config_t tmp;
    return config_load_wifi(&tmp);
}

bool config_load_bambu(bambu_config_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(out->printer_ip);
    if (nvs_get_str(h, "bambu_ip", out->printer_ip, &len) != ESP_OK) {
        nvs_close(h); return false;
    }
    len = sizeof(out->serial);
    if (nvs_get_str(h, "bambu_sn", out->serial, &len) != ESP_OK) out->serial[0] = '\0';
    len = sizeof(out->access_code);
    if (nvs_get_str(h, "bambu_code", out->access_code, &len) != ESP_OK) out->access_code[0] = '\0';
    nvs_close(h);
    return out->printer_ip[0] != '\0';
}

void config_save_bambu(const bambu_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "bambu_ip",   cfg->printer_ip);
    nvs_set_str(h, "bambu_sn",   cfg->serial);
    nvs_set_str(h, "bambu_code", cfg->access_code);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "打印机已保存: %s / %s", cfg->printer_ip, cfg->serial);
}

bool config_has_bambu(void)
{
    bambu_config_t tmp;
    return config_load_bambu(&tmp);
}
