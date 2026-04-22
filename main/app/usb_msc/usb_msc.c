#include "usb_msc.h"
#include "bsp.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// TinyUSB
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "USB_MSC";

// boot_mode 值定义（和 main.c 保持一致）
#define BOOT_MODE_NORMAL 0
#define BOOT_MODE_USB_MSC 1
#define BOOT_MODE_OTA 2 // 预留

static gpio_num_t s_vbus_pin = GPIO_NUM_NC;

void usb_msc_init_vbus(gpio_num_t pin)
{
	s_vbus_pin = pin;
	gpio_config_t io = {
		.pin_bit_mask = 1ULL << pin,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_ENABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&io);
}
// ── USB MSC 回调 ─────────────────────────────────────────────
static bool s_ejected = false;

static void msc_callback(tinyusb_msc_event_t *event)
{
	if (event->type == TINYUSB_MSC_EVENT_MOUNT_CHANGED) {
		if (event->mount_changed_data.is_mounted) {
			ESP_LOGI(TAG, "USB MSC mounted by host");
		} else {
			ESP_LOGI(TAG, "USB MSC unmounted by host");
			s_ejected = true;
		}
	}
}

// ── SD 卡初始化（MSC 模式，不挂 FATFS）────────────────────────
static sdmmc_card_t *s_card = NULL;

static esp_err_t sdmmc_init_for_msc(void)
{
	sdmmc_host_t host = SDMMC_HOST_DEFAULT();
	host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

	sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
	slot.clk = PIN_SDIO_CLK;
	slot.cmd = PIN_SDIO_CMD;
	slot.d0 = PIN_SDIO_D0;
	slot.d1 = PIN_SDIO_D1;
	slot.d2 = PIN_SDIO_D2;
	slot.d3 = PIN_SDIO_D3;
	slot.width = 4;

	esp_err_t ret = sdmmc_host_init();
	if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
		ESP_LOGE(TAG, "sdmmc_host_init failed: %s", esp_err_to_name(ret));
		return ret;
	}

	ret = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "sdmmc_host_init_slot failed: %s", esp_err_to_name(ret));
		return ret;
	}

	s_card = malloc(sizeof(sdmmc_card_t));
	if (!s_card)
		return ESP_ERR_NO_MEM;

	ret = sdmmc_card_init(&host, s_card);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "sdmmc_card_init failed: %s", esp_err_to_name(ret));
		free(s_card);
		s_card = NULL;
		return ret;
	}

	sdmmc_card_print_info(stdout, s_card);
	ESP_LOGI(TAG, "SD card init ok for MSC");
	return ESP_OK;
}

// ── USB MSC 启动 ─────────────────────────────────────────────
void usb_msc_start(void)
{
	ESP_LOGI(TAG, "Starting USB MSC mode...");

	// 初始化 SD 卡（不挂 FATFS）
	if (sdmmc_init_for_msc() != ESP_OK) {
		ESP_LOGE(TAG, "SD card init failed, cannot start MSC");

		esp_restart();
		return;
	}

	// 配置 TinyUSB MSC 存储
	const tinyusb_msc_sdmmc_config_t msc_cfg = {
		.card = s_card,
		.callback_mount_changed  = msc_callback,
		.callback_premount_changed = NULL,
		.mount_config = {
			.max_files = 5,
		},
	};

	ESP_ERROR_CHECK(tinyusb_msc_storage_init_sdmmc(&msc_cfg));

	// TinyUSB 设备配置
	const tinyusb_config_t tusb_cfg = {
		.device_descriptor = NULL, // 使用默认描述符
		.string_descriptor = NULL,
		.string_descriptor_count = 0,
		.external_phy = false,
		.configuration_descriptor = NULL,
	};

	ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

	ESP_LOGI(TAG, "USB MSC ready, waiting for host...");

	// 等待主机弹出，弹出后重启回正常模式
	while (1) {
		vTaskDelay(pdMS_TO_TICKS(500));

		if (s_ejected) {
			ESP_LOGI(TAG, "Ejected by host");
			break;
		}

		if (!usb_host_connected()) {
			ESP_LOGI(TAG, "USB unplugged (no eject)");
			break;
		}
	}

	ESP_LOGI(TAG, "Ejected by host, rebooting to normal mode...");
	vTaskDelay(pdMS_TO_TICKS(500)); // 给 USB 协议栈时间完成断开

	esp_restart();
}

// ── VBUS 检测 ────────────────────────────────────────────────
bool usb_host_connected(void)
{
	if (s_vbus_pin == GPIO_NUM_NC)
		return false;
	return gpio_get_level(s_vbus_pin) == 1;
}
