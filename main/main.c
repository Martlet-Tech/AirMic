#include "bsp.h"
#include "drivers/led/led.h"
#include "drivers/button/button.h"
#include "ble/ble_nus.h"
#include "esp_log.h"
#include "recorder.h"
#include "sdcard.h"
#include "fc_link.h"
#include "mic.h"
#include "wifi/wifi.h"
#include "usb_msc.h"
#include "nvs_flash.h"

static const char *TAG = "main";

#define NVS_NAMESPACE "airmic"
#define NVS_KEY_BOOT "boot_mode"
#define BOOT_MODE_NORMAL 0
#define BOOT_MODE_USB_MSC 1
#define BOOT_MODE_OTA 2 // 预留

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

	ESP_ERROR_CHECK(nvs_flash_init());

	uint8_t boot_mode = BOOT_MODE_NORMAL;
	nvs_handle_t h;
	if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
		nvs_get_u8(h, NVS_KEY_BOOT, &boot_mode);
		nvs_set_u8(h, NVS_KEY_BOOT, BOOT_MODE_NORMAL); // 清掉
		nvs_commit(h);
		nvs_close(h);
	}

	if (boot_mode == BOOT_MODE_USB_MSC) {
		ESP_LOGI(TAG, "Boot mode: USB MSC");
		usb_msc_start(); // 不返回
		return;
	}

	// 驱动层初始化（传入 bsp 引脚，与硬件解耦）
	led_init(PIN_LED_MONO);
	//mic_init(PIN_I2S_WS, PIN_I2S_CLK, PIN_I2S_SD);
	mic_gpio_t gpio = {
		.clk = PIN_I2S_CLK, // GPIO26
		.ws = GPIO_NUM_NC, // PDM 不用
		.data = PIN_I2S_SD, // GPIO21
	};
	mic_init(&gpio);
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