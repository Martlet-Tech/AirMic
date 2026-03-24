#include "boards/bsp.h" // ← 换板子只改这一行
#include "drivers/led/led.h"
#include "drivers/button/button.h"
#include "ble/ble_nus.h"
#include "app/mode_manager.h"
#include "esp_log.h"
#include "recorder.h"
#include "sdcard.h"

static const char *TAG = "main";

static void on_button(btn_event_t event)
{
	switch (event) {
	case BTN_EVENT_SINGLE_CLICK:
		// 录音模式下：手动开始/停止录音（将来接 recorder）
		ESP_LOGI(TAG, "single click");
		if (mode_manager_get() == MODE_RECORDER) {
			recorder_toggle();
		}
		break;
	case BTN_EVENT_DOUBLE_CLICK:
		// 切换模式
		if (mode_manager_get() == MODE_RECORDER) {
			mode_manager_switch(MODE_BLE_BRIDGE);
		} else {
			mode_manager_switch(MODE_RECORDER);
		}
		break;
	case BTN_EVENT_LONG_PRESS:
		ESP_LOGI(TAG, "long press - reserved");
		break;
	}
}

void app_main(void)
{
	ESP_LOGI(TAG, "AirMic starting...");

	// 驱动层初始化（传入 bsp 引脚，与硬件解耦）
	led_init(PIN_LED_MONO);
	button_init(PIN_KEY, on_button);

	// BLE 初始化
	ble_nus_init();

	sdcard_mount(PIN_SDIO_CLK, PIN_SDIO_CMD, PIN_SDIO_D0, PIN_SDIO_D1, PIN_SDIO_D2, PIN_SDIO_D3);

	// 业务逻辑
	mode_manager_init(); // 上电默认进录音机模式

	ESP_LOGI(TAG, "AirMic ready.");
}
