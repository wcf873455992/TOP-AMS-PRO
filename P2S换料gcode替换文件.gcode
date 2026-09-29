;======== P2S filament_change gcode (TOP AMS Pro) ==========
;===== 2026/05/15 =====

;===== 1. 到擦嘴位 =====
G150.3
M400

;===== 2. 通知 ESP32 换料通道（next_filament_id 0-based）=====
{if next_filament_id == 0}
M140 S1
{endif}
{if next_filament_id == 1}
M140 S2
{endif}
{if next_filament_id == 2}
M140 S3
{endif}
{if next_filament_id == 3}
M140 S4
{endif}
{if next_filament_id == 4}
M140 S5
{endif}
{if next_filament_id == 5}
M140 S6
{endif}
{if next_filament_id == 6}
M140 S7
{endif}
{if next_filament_id == 7}
M140 S8
{endif}
G4 S2                                          ; 等 2 秒让 ESP32 捕获
M140 S[bed_temperature_initial_layer_single]   ; 恢复热床

;===== 3. 暂停等用户确认（Bambu 弹窗）=====
;M400 U1;暂停，主动模式打开，被动模式屏蔽

;===== 4. AMS 换料流程 =====
M620 S[next_filament_id]A
M204 S9000
{if toolchange_count > 1 && (z_hop_types[current_filament_id] == 0 || z_hop_types[current_filament_id] == 3)}
G17
G2 Z{z_after_toolchange + 0.4} I0.86 J0.86 P1 F10000 ; spiral lift a little from second lift
{endif}

;nozzle_change_gcode

G1 Z{max_layer_z + 3.0} F1200

M400
M400 U1
M106 P1 S0

{if toolchange_count == 2}
; get travel path for change filament
;M620.1 X[travel_point_1_x] Y[travel_point_1_y] F21000 P0
;M620.1 X[travel_point_2_x] Y[travel_point_2_y] F21000 P1
;M620.1 X[travel_point_3_x] Y[travel_point_3_y] F21000 P2
{endif}

{if ((filament_type[current_filament_id] == "PLA") || (filament_type[current_filament_id] == "PLA-CF") || (filament_type[current_filament_id] == "PETG")) && (nozzle_diameter_at_nozzle_id[current_nozzle_id] == 0.2)}
M620.10 A0 F74.8347 L[flush_length] H{nozzle_diameter_at_nozzle_id[current_nozzle_id]} T{flush_temperatures[current_filament_id]} P[old_filament_temp] S1
{else}
M620.10 A0 F{flush_volumetric_speeds[current_filament_id]/2.4053*60} L[flush_length] H{nozzle_diameter_at_nozzle_id[current_nozzle_id]} T{flush_temperatures[current_filament_id]} P[old_filament_temp] S1
{endif}

{if ((filament_type[next_filament_id] == "PLA") || (filament_type[next_filament_id] == "PLA-CF") || (filament_type[next_filament_id] == "PETG")) && (nozzle_diameter_at_nozzle_id[next_nozzle_id] == 0.2)}
M620.10 A1 F74.8347 L[flush_length] H{nozzle_diameter_at_nozzle_id[next_nozzle_id]} T{flush_temperatures[next_filament_id]} P[new_filament_temp] S1
{else}
M620.10 A1 F{flush_volumetric_speeds[next_filament_id]/2.4053*60} L[flush_length] H{nozzle_diameter_at_nozzle_id[next_nozzle_id]} T{flush_temperatures[next_filament_id]} P[new_filament_temp] S1
{endif}

M620.15 C{new_filament_temp - filament_cooling_before_tower[next_filament_id]}

{if long_retraction_when_cut}
M620.11 P1 L0 I[current_filament_id] E-{retraction_distance_when_cut} F{max((flush_volumetric_speeds[current_filament_id]/2.4053*60), 200)}
{else}
M620.11 P0 L0 I[current_filament_id] E0
{endif}

M620.11 K0 I[current_filament_id] R0

T[next_filament_id]

;deretract
{if filament_type[next_filament_id] == "TPU"}
{else}
{if filament_type[next_filament_id] == "PA"}
;VG1 E1 F{max(new_filament_e_feedrate, 200)}
;VG1 E1 F{max(new_filament_e_feedrate/2, 100)}
{else}
;VG1 E4 F{max(new_filament_e_feedrate, 200)}
;VG1 E4 F{max(new_filament_e_feedrate/2, 100)}
{endif}
{endif}

; VFLUSH_START
{if flush_length>41.5}
;VG1 E41.5 F{min(old_filament_e_feedrate,new_filament_e_feedrate)}
;VG1 E{flush_length-41.5} F{new_filament_e_feedrate}
{else}
;VG1 E{flush_length} F{min(old_filament_e_feedrate,new_filament_e_feedrate)}
{endif}
SYNC T{ceil(flush_length / 80) * 5}
; VFLUSH_END

M1002 set_filament_type:{filament_type[next_filament_id]}

M400
M83
{if next_filament_id < 255}
M620.10 R{retract_length_toolchange[filament_map[next_filament_id]-1]}
M628 S0
;VM109 S[new_filament_temp]

M629
M400
M983.3 F{filament_max_volumetric_speed[next_filament_id]/2.4} A0.4 R{retract_length_toolchange[filament_map[next_filament_id]-1]}
M400
G1 Y247 F30000
G1 Y217 F18000

G1 Z{max_layer_z + 3.0} F3000
{if layer_z <= (initial_layer_print_height + 0.001)}
M204 S[initial_layer_acceleration]
{else}
M204 S[travel_acceleration]
{endif}

{else}
G1 X[x_after_toolchange] Y[y_after_toolchange] Z[z_after_toolchange] F12000
{endif}

M621 S[next_filament_id]A

M622.1 S0 ;for prev version, default skip
M1002 judge_flag powerloss_resume_flag
M622 J1
M983.3 F{filament_max_volumetric_speed[next_filament_id]/2.4} A0.4 R{retract_length_toolchange[filament_map[next_filament_id]-1]}
M400
G1 Y247 F30000
G1 Y217 F18000
G1 Z{max_layer_z + 3.0} F3000
{if layer_z <= (initial_layer_print_height + 0.001)}
M204 S[initial_layer_acceleration]
{else}
M204 S[travel_acceleration]
{endif}
M1002 set_flag powerloss_resume_flag=0
M623

{if (filament_type[next_filament_id] == "PLA") ||  (filament_type[next_filament_id] == "PETG")
 ||  (filament_type[next_filament_id] == "PLA-CF")  ||  (filament_type[next_filament_id] == "PETG-CF")}
M1015.4 S1 K1 H{nozzle_diameter_at_nozzle_id[next_nozzle_id]} ;enable E air printing detect
{else}
M1015.4 S0 K0 H{nozzle_diameter_at_nozzle_id[next_nozzle_id]} ;disable E air printing detect
{endif}

M620.6 I[next_filament_id] W1 ;enable ams air printing detect

G1 Y256 F18000

{if (overall_chamber_temperature < 40)}
{if (layer_num + 1 <= close_additional_fan_first_x_layers[next_filament_id])}
    {if (min_vitrification_temperature <= 50)}
        M106 P2 S{first_x_layer_fan_speed[next_filament_id]*255.0/100.0 };set first x_layer aux fan
    {endif}
{elsif (layer_num + 1 < additional_fan_full_speed_layer[next_filament_id] && additional_fan_full_speed_layer[next_filament_id] > close_additional_fan_first_x_layers[next_filament_id])}
    {if (min_vitrification_temperature <= 50)}
        M106 P2 S{(first_x_layer_fan_speed[next_filament_id] + (additional_cooling_fan_speed[next_filament_id] - first_x_layer_fan_speed[next_filament_id]) * (layer_num + 1 - close_additional_fan_first_x_layers[next_filament_id]) / max(additional_fan_full_speed_layer[next_filament_id] - close_additional_fan_first_x_layers[next_filament_id], 1)) * 255.0/100.0}
    {endif}
{else}
    {if (min_vitrification_temperature <= 50)}
        {if (nozzle_diameter_at_nozzle_id[current_nozzle_id] == 0.2)}
            M142 P1 R30 S40 U{max_additional_fan/100.0} V1.0 O45; set PLA/TPU ND0.2 chamber autocooling
        {else}
            M142 P1 R30 S40 U{max_additional_fan/100.0} V1.0 O45; set PLA/TPU ND0.4 chamber autocooling
        {endif}
    {endif}
{endif}
{endif}

M622.1 S0
M1002 judge_flag ventobox_replace_aux1_fan_flag
M622 J0
{if (layer_num + 1 <= close_additional_fan_first_x_layers[next_filament_id])}
    M106 P10 S{first_x_layer_fan_speed[next_filament_id]*255.0/100.0 };set first x_layer left aux fan
{elsif (layer_num + 1 < additional_fan_full_speed_layer[next_filament_id] && additional_fan_full_speed_layer[next_filament_id] > close_additional_fan_first_x_layers[next_filament_id])}
    M106 P10 S{(first_x_layer_fan_speed[next_filament_id] + (additional_cooling_fan_speed[next_filament_id] - first_x_layer_fan_speed[next_filament_id]) * (layer_num + 1 - close_additional_fan_first_x_layers[next_filament_id]) / max(additional_fan_full_speed_layer[next_filament_id] - close_additional_fan_first_x_layers[next_filament_id], 1)) * 255.0/100.0}
{else}
    M106 P10 S{additional_cooling_fan_speed[next_filament_id]*255.0/100.0};set left aux fan
{endif}
M623

;not set fan changing filament