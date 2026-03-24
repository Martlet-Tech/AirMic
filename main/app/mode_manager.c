#include "mode_manager.h"
#include "recorder/recorder.h"
#include "ble_bridge/ble_bridge.h"
#include "esp_log.h"

// RGB 颜色约定（将来接 rgb 驱动）
// MODE_RECORDER   → 绿色慢呼吸
// MODE_BLE_BRIDGE → 蓝色常亮

static const char *TAG = "mode_manager";
static system_mode_t s_mode = MODE_RECORDER;

void mode_manager_init(void)
{
    // 上电默认进录音机模式
    mode_manager_switch(MODE_RECORDER);
}

void mode_manager_switch(system_mode_t mode)
{
    if (mode == s_mode) return;

    // 先停当前模式
    switch (s_mode) {
    case MODE_RECORDER:    recorder_stop();    break;
    case MODE_BLE_BRIDGE:  ble_bridge_stop();  break;
    }

    s_mode = mode;

    // 启动新模式
    switch (s_mode) {
    case MODE_RECORDER:
        ESP_LOGI(TAG, "→ RECORDER mode");
        recorder_start();
        // rgb_set(GREEN, BREATHE);
        break;
    case MODE_BLE_BRIDGE:
        ESP_LOGI(TAG, "→ BLE_BRIDGE mode");
        ble_bridge_start();
        // rgb_set(BLUE, SOLID);
        break;
    }
}

system_mode_t mode_manager_get(void)
{
    return s_mode;
}
