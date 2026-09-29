#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 静态命令（无需参数） ---------- */
extern const char* CMD_PRINT_PAUSE;
extern const char* CMD_PRINT_RESUME;
extern const char* CMD_CLICK_DONE;
extern const char* CMD_CHICK_RESUME;
extern const char* CMD_ERROR_CLEAN;
extern const char* CMD_GET_STATUS;
extern const char* CMD_LED_ON;
extern const char* CMD_LED_OFF;

/* ---------- 动态命令构造 ---------- */

/**
 * 构造任意 G-code 发送 payload（malloc 返回，调用方 free）
 */
char* bambu_cmd_run_gcode(const char* code);

/**
 * 构造 ams_change_filament payload（malloc 返回，调用方 free）
 */
char* bambu_cmd_make_change_filament(int ams_id, int slot_id,
                                      int target, int curr_temp, int tar_temp);

/* ---------- 发送 ---------- */
void bambu_send_cmd(const char* payload);

/**
 * 发送 ams_change_filament（按通道自动换算 ams_id/slot_id）
 * @param ch      1-8
 * @param load    1=进料，0=退料
 */
void bambu_send_change_filament(int ch, int load);


/* ---------- G-code 版换料（M620/M621 系列） ---------- */
char* bambu_cmd_make_m620_load(int ch);    /* M620 S{n}A */
char* bambu_cmd_make_m620_uload(void);     /* M620 S65535 */
char* bambu_cmd_make_m621(int ch);         /* M621 S{n}A */

#ifdef __cplusplus
}
#endif
