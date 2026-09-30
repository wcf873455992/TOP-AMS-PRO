TOP AMS PRO 固件包
====================

文件清单（文件名后缀 _地址 表示烧录地址）
----------------------------------------
bootloader_0x0.bin                   引导程序，烧录到 0x0
partition-table_0x8000.bin           分区表，烧录到 0x8000
top_ams_pro_0x20000.bin              应用程序，烧录到 0x20000
TOP AMS PRO首次烧录_地址0.bin        合并固件，烧录到 0x0
esp32烧录工具/                       ESP32 专用烧录工具
README.txt                           本文件

⚠️ 首次烧录（必须按顺序）
--------------------------
第 1 步：擦除 Flash（必须）
  方法一：用 Flash Download Tool
    打开工具 → 点 ERASE 按钮（见下方图2编号6）

  方法二：命令行
    esptool.py --chip esp32c3 --port COM3 erase_flash

  原因: 板子原有分区表可能与新固件不一致，
        NVS 里可能存有旧配置，导致启动异常。


第 2 步：打开 Flash Download Tool
  【图1】启动设置
    ┌─────────────────────────────┐
    │  ChipType:  ESP32-C3   ← ①  │
    │  WorkMode:  Develop         │
    │  LoadMode:  UART            │
    │              [ OK ]    ← ②  │
    └─────────────────────────────┘
  ① ChipType 选 ESP32-C3
  ② 点 OK


第 3 步：选择固件并烧录
  【图2】烧录界面
   ① 文件路径: 点击 ... 选择 TOP AMS PRO首次烧录_地址0.bin
   ② 地址:     填 0（合并固件烧到 0x0）
   ③ 复选框:   勾选该行最左侧的复选框
   ④ COM口:    选择你的串口（如 COM19）
   ⑤ 波特率:   选 921600
   ⑥ 擦除:     必须点 ERASE 擦除（首次烧录必须执行）
   ⑦ START:    点 START 开始烧录

  烧录完成后，串口应看到:
    Loaded app from partition at offset 0x20000


⚠️ 为什么首次烧录必须先擦除？
--------------------------
- 板子原有分区表可能与新固件不一致
- NVS 里可能存有旧配置，导致启动异常
- 擦除后设备恢复出厂状态，最干净
- 擦除会清除：固件、分区表、NVS、SPIFFS
- eFuse（MAC 地址）不会被擦除
- 不擦除可能导致: 启动失败、反复重启、配网异常


手动烧录（用分离的 bin）
------------------------
如果不用合并固件，可以分别烧 3 个文件:

  bootloader_0x0.bin          → 0x0
  partition-table_0x8000.bin  → 0x8000
  top_ams_pro_0x20000.bin     → 0x20000

在 Flash Download Tool 里填 3 行:
  第1行: bootloader_0x0.bin         地址 0x0
  第2行: partition-table_0x8000.bin 地址 0x8000
  第3行: top_ams_pro_0x20000.bin    地址 0x20000

或命令行:
  esptool.py --chip esp32c3 --port COM3 write_flash ^
    --flash_mode dio --flash_freq 80m --flash_size 4MB ^
    0x0 bootloader_0x0.bin ^
    0x8000 partition-table_0x8000.bin ^
    0x20000 top_ams_pro_0x20000.bin


后续升级（OTA）
---------------
设备联网后，浏览器访问:
  http://<设备IP>/ota.html
上传 top_ams_pro_0x20000.bin 即可。
不需要擦除，不需要串口。


分区表
------
nvs        0x9000     0x6000
otadata    0xf000     0x2000
phy_init   0x11000    0x1000
factory    0x20000    0x180000
ota_0      0x1a0000   0x180000


注意事项
--------
- 首次烧录必须：先擦除 → 再烧合并固件
- 首次烧录不擦除可能导致启动失败
- 后续升级只上传 top_ams_pro_0x20000.bin
- 不要混用
- Flash Download Tool 版本: V3.9.9_R1
