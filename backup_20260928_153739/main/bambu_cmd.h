#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern const char* CMD_PRINT_PAUSE;
extern const char* CMD_PRINT_RESUME;
extern const char* CMD_LOAD;
extern const char* CMD_ULOAD;
extern const char* CMD_CLICK_DONE;
extern const char* CMD_CHICK_RESUME;
extern const char* CMD_ERROR_CLEAN;
extern const char* CMD_GET_STATUS;
extern const char* CMD_LED_ON;
extern const char* CMD_LED_OFF;

char* bambu_cmd_run_gcode(const char* code);
void bambu_send_cmd(const char* payload);

#ifdef __cplusplus
}
#endif

extern const char* CMD_LOAD_AMS;
void bambu_send_m620_load(int ch);

extern const char* CMD_ChangeLOAD;

void bambu_send_change_load(int ch);
