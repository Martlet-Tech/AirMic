#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * Remote ID 模块 - CAAC 广播式运行识别
 * 基于 ASTM F3411-22a / IB-TM-2024-01
 *
 * 广播格式：BLE non-connectable，Service UUID 0xFFFA
 * 报文长度：25字节（1字节报头 + 24字节内容）
 * 广播间隔：≤1秒
 */

// 用飞控 UID 初始化（在 fc_link 拿到 MSP_UID 后调用）
void rid_init(const uint8_t *fc_uid, uint8_t uid_len);

// 启用/禁用（对应 AirMic 配置项，持久化到 NVS）
void rid_set_enabled(bool enabled);
bool rid_get_enabled(void);

// 解锁时调用 → 开始广播
void rid_start(void);

// 落地时调用 → 停止广播
void rid_stop(void);