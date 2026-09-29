#pragma once
#include <stdbool.h>
#include "bambu_mqtt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    char password[65];
} app_wifi_config_t;

void config_store_init(void);
bool config_load_wifi(app_wifi_config_t *out);
void config_save_wifi(const app_wifi_config_t *cfg);
bool config_has_wifi(void);
bool config_load_bambu(bambu_config_t *out);
void config_save_bambu(const bambu_config_t *cfg);
bool config_has_bambu(void);

#ifdef __cplusplus
}
#endif
