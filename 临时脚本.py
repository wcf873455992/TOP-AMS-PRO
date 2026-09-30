#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
退料后不清 g_current_channel
"""

import os
import sys
import shutil
from datetime import datetime

TARGET = os.path.join("main", "filament.c")


def backup(path):
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    dst = f"{path}.bak_{ts}"
    shutil.copy(path, dst)
    print(f"[备份] {path} -> {dst}")


def main():
    if not os.path.isfile(TARGET):
        print(f"[ERROR] 找不到: {TARGET}")
        sys.exit(1)

    with open(TARGET, "r", encoding="utf-8", newline="") as f:
        text = f.read()

    nl = "\r\n" if "\r\n" in text else "\n"
    orig = text
    n = 0

    # ==========================================================
    # 1. filament_backward_manual_task 里清空 g_current_channel
    # ==========================================================
    OLD1 = (
        '    if (g_current_channel == ch) {\n'
        '        g_current_channel = 0;\n'
        '        ESP_LOGI(TAG, "[退料-主动] 已清空当前通道 %ld", (long)g_current_channel);\n'
        '        filament_save_config();\n'
        '    }'
    )
    OLD1_CRLF = OLD1.replace("\n", nl)

    NEW1 = (
        '    /* ★ 不清 g_current_channel，保留"上次用的通道" */\n'
        '    ESP_LOGI(TAG, "[退料] 通道 %ld 已退料，当前通道保留为 %ld",\n'
        '             (long)ch, (long)g_current_channel);\n'
        '    filament_save_config();'
    )
    NEW1_CRLF = NEW1.replace("\n", nl)

    if OLD1_CRLF in text:
        text = text.replace(OLD1_CRLF, NEW1_CRLF, 1)
        n += 1
        print("[OK]   backward_manual 不清 g_current_channel")
    elif OLD1 in text:
        text = text.replace(OLD1, NEW1, 1)
        n += 1
        print("[OK]   backward_manual 不清 g_current_channel（LF）")
    else:
        print("[WARN] backward_manual 锚点未找到")

    # ==========================================================
    # 2. filament_load_task 回退通道逻辑
    # ==========================================================
    OLD2 = (
        '                int sig = g_target_channel_signal;\n'
        '                if (sig < 1 || sig > 8) sig = g_bambu_status.bed_target_temper;\n'
        '                if (sig > 8) sig = g_current_channel;\n'
        '                target_ch = (sig >= 1 && sig <= 8) ? sig : g_current_channel;\n'
        '                if (target_ch < 1 || target_ch > 8) target_ch = 1;'
    )
    OLD2_CRLF = OLD2.replace("\n", nl)

    NEW2 = (
        '                int sig = g_target_channel_signal;\n'
        '                if (sig < 1 || sig > 8) sig = g_bambu_status.bed_target_temper;\n'
        '                if (sig > 8) sig = g_current_channel;\n'
        '\n'
        '                if (sig >= 1 && sig <= 8) {\n'
        '                    target_ch = sig;\n'
        '                } else if (g_current_channel >= 1 && g_current_channel <= 8) {\n'
        '                    target_ch = g_current_channel;   /* ★ 回退到当前通道 */\n'
        '                    ESP_LOGI(TAG, "[进料] 无信号，回退到当前通道 %d", target_ch);\n'
        '                } else {\n'
        '                    target_ch = 1;\n'
        '                }'
    )
    NEW2_CRLF = NEW2.replace("\n", nl)

    if OLD2_CRLF in text:
        text = text.replace(OLD2_CRLF, NEW2_CRLF, 1)
        n += 1
        print("[OK]   load_task 回退逻辑")
    elif OLD2 in text:
        text = text.replace(OLD2, NEW2, 1)
        n += 1
        print("[OK]   load_task 回退逻辑（LF）")
    else:
        print("[WARN] load_task 锚点未找到")

    if n == 0:
        print("[WARN] 无变化")
        return

    if text == orig:
        print("[SKIP] 无变化")
        return

    backup(TARGET)
    with open(TARGET, "w", encoding="utf-8", newline="") as f:
        f.write(text)

    print(f"[完成] {TARGET}  共修改 {n} 处")


if __name__ == "__main__":
    main()