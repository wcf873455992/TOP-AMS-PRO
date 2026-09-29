#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TOP_AMS PRO 打包脚本
====================
输出到项目根目录的 "Top Ams Pro打包" 文件夹，包含：
  - bootloader_0x0.bin                   引导程序
  - partition-table_0x8000.bin           分区表
  - top_ams_pro_0x20000.bin              应用程序
  - TOP AMS PRO首次烧录_地址0.bin        合并固件（首次烧录用）
  - esp32烧录工具/                       乐鑫烧录工具
  - README.txt                           说明文件

用法:
  python pack.py            生成打包目录
  python pack.py --zip      同时生成 zip
"""

import os
import sys
import shutil
import subprocess
import argparse
import zipfile

# ============================================================
# 配置
# ============================================================

PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR   = os.path.join(PROJECT_DIR, "build")
PACK_DIR    = os.path.join(PROJECT_DIR, "Top Ams Pro打包")
TOOLS_SRC   = os.path.join(PROJECT_DIR, "tools")
TOOLS_DST   = os.path.join(PACK_DIR, "esp32烧录工具")

CHIP         = "esp32c3"
FLASH_MODE   = "dio"
FLASH_FREQ   = "80m"
FLASH_SIZE   = "4MB"

# 源文件路径, 烧录地址, 目标文件名（后缀带地址）
BINS = [
    ("bootloader/bootloader.bin",           "0x0",     "bootloader_0x0.bin"),
    ("partition_table/partition-table.bin", "0x8000",  "partition-table_0x8000.bin"),
    ("top_ams_pro.bin",                     "0x20000", "top_ams_pro_0x20000.bin"),
]

MERGED_NAME = "TOP AMS PRO首次烧录_地址0.bin"


# ============================================================
# 步骤 1：检查 build 目录
# ============================================================

def check_build():
    if not os.path.isdir(BUILD_DIR):
        print("[错误] 找不到 build 目录，请先执行 idf.py build")
        sys.exit(1)

    missing = []
    for rel, _, _ in BINS:
        p = os.path.join(BUILD_DIR, rel)
        if not os.path.exists(p):
            missing.append(rel)

    if missing:
        print("[错误] 缺少以下文件：")
        for m in missing:
            print(f"       {m}")
        print("       请先执行 idf.py build")
        sys.exit(1)

    print("[OK] build 目录检查通过")


# ============================================================
# 步骤 2：创建打包目录
# ============================================================

def make_pack_dir():
    if os.path.exists(PACK_DIR):
        print(f"[清理] 删除旧的 {PACK_DIR}")
        shutil.rmtree(PACK_DIR)
    os.makedirs(PACK_DIR)
    print(f"[OK] 创建 {PACK_DIR}")


# ============================================================
# 步骤 3：复制 bin 文件
# ============================================================

def copy_bins():
    for rel, _, dst in BINS:
        src = os.path.join(BUILD_DIR, rel)
        shutil.copy(src, os.path.join(PACK_DIR, dst))
        size = os.path.getsize(src)
        print(f"  [复制] {dst}  ({size/1024:.1f} KB)")


# ============================================================
# 步骤 4：合并固件
# ============================================================

def merge_bins():
    merged = os.path.join(PACK_DIR, MERGED_NAME)
    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", CHIP,
        "merge-bin",
        "-o", merged,
        "--flash-mode", FLASH_MODE,
        "--flash-freq", FLASH_FREQ,
        "--flash-size", FLASH_SIZE,
    ]
    for rel, addr, _ in BINS:
        cmd.extend([addr, os.path.join(BUILD_DIR, rel)])

    print("[合并] 生成首次烧录文件...")
    r = subprocess.run(cmd, capture_output=True, text=True)

    if r.returncode != 0 or not os.path.exists(merged):
        print("[警告] 合并失败，跳过")
        if r.stderr:
            print(r.stderr)
        print()
        print("       手动执行命令：")
        print("       " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
        return None

    size = os.path.getsize(merged)
    print(f"  [OK] {MERGED_NAME}  ({size/1024:.1f} KB)")
    return merged


# ============================================================
# 步骤 5：复制烧录工具
# ============================================================

def copy_tools():
    os.makedirs(TOOLS_DST, exist_ok=True)

    found = False
    if os.path.isdir(TOOLS_SRC):
        for f in os.listdir(TOOLS_SRC):
            src = os.path.join(TOOLS_SRC, f)
            if os.path.isfile(src):
                shutil.copy(src, os.path.join(TOOLS_DST, f))
                print(f"  [复制] esp32烧录工具/{f}")
                found = True
            elif os.path.isdir(src):
                dst_dir = os.path.join(TOOLS_DST, f)
                if os.path.exists(dst_dir):
                    shutil.rmtree(dst_dir)
                shutil.copytree(src, dst_dir)
                print(f"  [复制] esp32烧录工具/{f}/")
                found = True

    if not found:
        print("  [提示] tools/ 目录为空，未复制烧录工具")
        with open(os.path.join(TOOLS_DST, "工具下载说明.txt"),
                  "w", encoding="utf-8") as f:
            f.write("""ESP32 烧录工具下载说明
====================

乐鑫官方 Flash Download Tool:
https://www.espressif.com/zh-hans/support/download/other-tools

文件名: flash_download_tool_3.9.5.exe
下载后放入 tools/ 目录，重新运行 pack.py 即可。
""")
        print("  [生成] esp32烧录工具/工具下载说明.txt")

    return found


# ============================================================
# 步骤 6：生成 README.txt
# ============================================================

def write_readme():
    readme = '''TOP AMS PRO 固件包
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
'''
    with open(os.path.join(PACK_DIR, "README.txt"),
              "w", encoding="utf-8") as f:
        f.write(readme)
    print("  [生成] README.txt")


# ============================================================
# 步骤 7：打包 zip
# ============================================================

def make_zip():
    zip_path = os.path.join(PROJECT_DIR, "TOP_AMS_PRO_固件包.zip")
    if os.path.exists(zip_path):
        os.remove(zip_path)

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(PACK_DIR):
            for f in files:
                full = os.path.join(root, f)
                arc = os.path.relpath(full, PACK_DIR)
                z.write(full, arc)

    size = os.path.getsize(zip_path)
    print(f"[OK] 打包: {zip_path}  ({size/1024:.1f} KB)")


# ============================================================
# 主流程
# ============================================================

def main():
    parser = argparse.ArgumentParser(description="TOP_AMS PRO 打包脚本")
    parser.add_argument("--zip", action="store_true", help="同时生成 zip")
    args = parser.parse_args()

    print("=" * 44)
    print("  TOP_AMS PRO 打包脚本")
    print("=" * 44)
    print()

    check_build()
    make_pack_dir()

    print()
    print("[1/4] 复制 bin 文件...")
    copy_bins()

    print()
    print("[2/4] 合并固件...")
    merge_bins()

    print()
    print("[3/4] 复制烧录工具...")
    copy_tools()

    print()
    print("[4/4] 生成说明...")
    write_readme()

    if args.zip:
        print()
        print("[5/5] 打包 zip...")
        make_zip()
    else:
        print()
        print("[5/5] 跳过 zip（加 --zip 生成）")

    print()
    print("=" * 44)
    print(f"  完成！输出目录: {PACK_DIR}")
    print("=" * 44)
    print()
    print("目录结构：")
    for root, dirs, files in os.walk(PACK_DIR):
        level = root.replace(PACK_DIR, "").count(os.sep)
        indent = "  " * level
        name = os.path.basename(root) or root
        print(f"{indent}{name}/")
        for f in files:
            p = os.path.join(root, f)
            size = os.path.getsize(p)
            print(f"{indent}  {f}  ({size/1024:.1f} KB)")


if __name__ == "__main__":
    main()