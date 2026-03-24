#pragma once

/**
 * 录音机模式
 * - 每100ms轮询FC解锁状态
 * - 解锁 → 自动开始录音
 * - 上锁 → 自动停止录音
 * - 未解锁时BLE可用于调参桥
 */

void recorder_start(void);   // 进入录音机模式，启动FC轮询任务
void recorder_stop(void);    // 退出录音机模式，停止所有任务

// 按钮单击调用：手动切换录音开始/停止（覆盖自动逻辑）
void recorder_toggle(void);
