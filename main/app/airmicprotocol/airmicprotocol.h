#pragma once
#include <stdint.h>
#include <stddef.h>

/**
 * AirMic 控制协议
 *
 * 下行（手机 → 板子）Write to Control Point:
 *   | CMD (1) | LEN (1) | PAYLOAD (LEN bytes) |
 *
 * 上行（板子 → 手机）Notify from Response:
 *   | CMD (1) | STATUS (1) | PAYLOAD (n bytes) |
 *   STATUS: 0x00=OK  0x01=ERR
 */

// ── 命令定义 ─────────────────────────────────────────────────
#define CMD_TIME_SYNC       0x01   // payload: uint64 unix时间戳(ms)
#define CMD_SET_SAMPLERATE  0x02   // payload: uint32 采样率
#define CMD_SET_CHANNELS    0x03   // payload: uint8  声道数
#define CMD_GET_STATUS      0x04   // payload: 无，notify回系统状态
#define CMD_SET_WIFI        0x05   // payload: ssid_len(1) + ssid + password_len(1) + password
#define CMD_GET_FILE_LIST   0x06   // payload: 无，notify回文件列表
#define CMD_DELETE_FILE     0x07   // payload: filename_len(1) + filename
#define CMD_RENAME_FILE     0x08   // payload: old_filename_len(1) + old_filename + new_filename_len(1) + new_filename

// ── 响应状态 ─────────────────────────────────────────────────
#define RESP_OK   0x00
#define RESP_ERR  0x01

// 初始化，注册 GATT Characteristic handle
void airmic_protocol_init(uint16_t ctrl_handle, uint16_t resp_handle);

// BLE 收到 Write 时调用
void airmic_protocol_on_write(uint16_t conn_handle,
                              const uint8_t *data, uint16_t len);