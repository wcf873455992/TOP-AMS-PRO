#pragma once
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char printer_ip[16];
    char serial[32];
    char access_code[16];
    char topic_sub[64];
    char topic_pub[64];
} bambu_config_t;

typedef struct {
    char gcode_state[16];
    int  nozzle_temper;
    int  nozzle_target_temper;
    int  bed_temper;
    int  bed_target_temper;
    int  mc_percent;
    int  mc_remaining_time;
    int  layer_num;
    int  total_layer_num;
    int  print_error;
    int  hw_switch_state;
    bool connected;

    char subtask_name[64];
    char wifi_signal[16];
    int  ams_status;
    int  ams_exist;
    char rtsp_url[96];
    int  hms_count;
    int  print_type_is_local;
    char nozzle_type[16];
    char nozzle_diameter[8];
    char tray_type[16];
    char tray_color[16];

    bool chamber_light_on;
    bool work_light_on;

    int64_t last_update_ms;

    /* 工具头位置 */
    double toolhead_x;
    double toolhead_y;
    double toolhead_z;

    /* 换料前记录的真实热床温度 */
    int last_real_bed_target;

    /* extruder 信息 */
    int extruder_stat;
    int extruder_info;
    int extruder_snow;
    int extruder_star;
    int extruder_temp;

    /* 最近的 gcode_line */
    char last_gcode_line[128];
    uint32_t gcode_line_count;
} bambu_status_t;

extern bambu_status_t g_bambu_status;

void bambu_mqtt_init(const bambu_config_t *cfg);
void bambu_mqtt_request_status(void);
void bambu_mqtt_stop(void);
void bambu_mqtt_get_status_json(char *out, size_t out_len);
void bambu_mqtt_publish_raw(const char* payload);

#ifdef __cplusplus
}
#endif
