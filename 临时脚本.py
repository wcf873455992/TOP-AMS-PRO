#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
quiet_filament_log.py  (增强版)
方案三：只关闭 filament.c 中周期性/刷屏的调试日志。
规则 1（5秒[监控]）使用正则匹配，容错缩进/空行差异。
"""

import sys
import os
import re
import shutil

BAK_SUFFIX = ".bak"


def restore(path: str) -> None:
    bak = path + BAK_SUFFIX
    if not os.path.exists(bak):
        print(f"[!] 找不到备份文件: {bak}")
        sys.exit(1)
    shutil.copyfile(bak, path)
    print(f"[+] 已从 {bak} 还原 {path}")


# ============ 正则规则（可容错缩进/空行） ============
# 规则 1: 删除 auto_swap_monitor_task 中每 5 秒的 [监控] 日志块
RE_MONITOR = re.compile(
    r'[ \t]*/\*[^\n]*调试日志[^\n]*\*/\s*'          # 注释行（可选）
    r'\{[^{}]*?'                                     # 外层 {
    r'static\s+int\s+dbg_cnt\s*=\s*0\s*;[^{}]*?'    # static int dbg_cnt = 0;
    r'dbg_cnt\s*\+\+\s*%\s*5\s*==\s*0[^{}]*?'       # dbg_cnt++ % 5 == 0
    r'ESP_LOGI\([^;]*?\[监控\][^;]*?\)\s*;[^{}]*?'   # ESP_LOGI(... [监控] ...);
    r'\}[ \t]*\n',
    re.DOTALL,
)


# ============ 精确文本规则 ============
RULES = [
    (
        "关闭热床信号记录日志",
        '''            if (bed != prev_sig) {
                ESP_LOGI(TAG, "[信号] 记录通道 = %d（当前通道 = %d）",
                     bed, (int)g_current_channel);
                g_target_channel_signal = bed;
                prev_sig = bed;
            }
''',
        '''            if (bed != prev_sig) {
                g_target_channel_signal = bed;
                prev_sig = bed;
            }
''',
    ),
    (
        "关闭退料任务 ams_status 变化日志",
        '''        if (ams != prev_ams) continue;
        prev_ams = ams;

        if (ams != 260) continue;

        /* 被动模式下，如果手动任务正在跑，让手动任务处理 */
        if (!g_printer_sync && s_filament_busy) {
            continue;
        }

        ESP_LOGI(TAG, "[退料-被动] ams_status: %d (%s)", ams, ams_status_desc(ams));
''',
        '''        if (ams != prev_ams) continue;
        prev_ams = ams;

        if (ams != 260) continue;

        /* 被动模式下，如果手动任务正在跑，让手动任务处理 */
        if (!g_printer_sync && s_filament_busy) {
            continue;
        }
''',
    ),
    (
        "关闭进料任务 ams_status 变化日志",
        '''        if (ams != prev_ams) {
            if (ams == 261 || ams == 263 || ams == 0 || ams == 768) {
                int log_ch = (g_target_channel_signal >= 1 && g_target_channel_signal <= 8)

                             ? g_target_channel_signal : g_current_channel;

                ESP_LOGI(TAG, "[进料-被动] [ch=%d] ams_status: %d -> %d (%s)",

                         log_ch, prev_ams, ams, ams_status_desc(ams));
            }
            prev_ams = ams;
        }
''',
        '''        if (ams != prev_ams) {
            prev_ams = ams;
        }
''',
    ),
    (
        "关闭进料任务 hw_switch 变化日志",
        '''        if (hw != prev_hw) {
            if (hw == 2 || hw == 3) {
                int log_ch = (g_target_channel_signal >= 1 && g_target_channel_signal <= 8)

                             ? g_target_channel_signal : g_current_channel;

                ESP_LOGI(TAG, "[进料-被动] [ch=%d] hw_switch: %d -> %d (%s)",

                         log_ch, prev_hw, hw, hw_switch_desc(hw));
            }
            prev_hw = hw;
        }
''',
        '''        if (hw != prev_hw) {
            prev_hw = hw;
        }
''',
    ),
]


def apply_rules(path: str) -> None:
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()

    bak = path + BAK_SUFFIX
    if not os.path.exists(bak):
        shutil.copyfile(path, bak)
        print(f"[+] 已备份原文件到 {bak}")

    changed = 0

    # --- 规则 1：正则删除 [监控] 块 ---
    new_src, n = RE_MONITOR.subn(
        "        /* ★ 调试日志已关闭（方案三） */\n", src
    )
    if n == 0:
        print("[!] 未匹配到 5 秒 [监控] 日志块，请确认代码格式")
    else:
        src = new_src
        changed += n
        print(f"[+] 已处理: 关闭 auto_swap 监控周期日志 ({n} 处)")

    # --- 规则 2~5：精确文本 ---
    for desc, old, new in RULES:
        cnt = src.count(old)
        if cnt == 0:
            print(f"[!] 未匹配（可能已修改过或格式不同）: {desc}")
            continue
        if cnt > 1:
            print(f"[!] 匹配到 {cnt} 处，规则不唯一，跳过: {desc}")
            continue
        src = src.replace(old, new, 1)
        changed += 1
        print(f"[+] 已处理: {desc}")

    if changed == 0:
        print("[!] 没有任何改动，文件未写入。")
        return

    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    print(f"[✓] 完成，共修改 {changed} 处，已写回 {path}")


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    path = sys.argv[1]
    if not os.path.isfile(path):
        print(f"[!] 文件不存在: {path}")
        sys.exit(1)

    if "--restore" in sys.argv:
        restore(path)
    else:
        apply_rules(path)


if __name__ == "__main__":
    main()