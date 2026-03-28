#include "bsp.h"
#include "drivers/led/led.h"
#include "drivers/button/button.h"
#include "ble/ble_nus.h"
#include "esp_log.h"
#include "recorder.h"
#include "sdcard.h"
#include "fc_link.h"
#include "mic.h"

static const char *TAG = "main";

static void on_button(btn_event_t event)
{
	switch (event) {
	case BTN_EVENT_SINGLE_CLICK:
		// 录音模式下：手动开始/停止录音（将来接 recorder）
		ESP_LOGI(TAG, "single click");
		recorder_toggle();
		break;
	case BTN_EVENT_DOUBLE_CLICK:
		ESP_LOGI(TAG, "double click- reserved");
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
	mic_init(PIN_I2S_WS, PIN_I2S_CLK, PIN_I2S_SD);
	sdcard_mount(PIN_SDIO_CLK, PIN_SDIO_CMD, PIN_SDIO_D0, PIN_SDIO_D1, PIN_SDIO_D2, PIN_SDIO_D3);
	button_init(PIN_KEY, on_button);
	recorder_init();
	ESP_LOGI(TAG, "hw init done");

	// FC 初始化
	fc_link_start();

	// BLE 初始化
	ble_nus_init();

	ESP_LOGI(TAG, "AirMic ready.");
}
