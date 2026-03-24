#include "ble_bridge.h"
#include "ble_nus.h"
#include "bsp.h"
#include "led.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

static const char *TAG    = "ble_bridge";

#define FC_UART_PORT   UART_NUM_1
#define FC_UART_BAUD   115200

static TaskHandle_t s_task   = NULL;
static bool         s_running = false;

// ── FC → BLE 方向任务 ────────────────────────────────────────
// BLE → FC 方向在 ble_nus.c 的 nus_tx_chr_access 回调里直接写 UART
static void bridge_rx_task(void *arg)
{
    uint8_t buf[512];
    while (s_running) {
        int len = uart_read_bytes(FC_UART_PORT, buf, sizeof(buf),
                                  pdMS_TO_TICKS(20));
        if (len > 0) {
            ble_nus_send(buf, len);
        }
    }
    vTaskDelete(NULL);
}

void ble_bridge_start(void)
{
    if (s_task != NULL) return;

    uart_config_t cfg = {
        .baud_rate = FC_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(FC_UART_PORT, 1024, 1024, 0, NULL, 0);
    uart_param_config(FC_UART_PORT, &cfg);
    uart_set_pin(FC_UART_PORT, PIN_FC_TX, PIN_FC_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    s_running = true;
    led_set_mode(LED_MODE_OFF);   // RGB 那边会亮蓝色，单色 LED 关掉
    xTaskCreate(bridge_rx_task, "ble_bridge", 4096, NULL, 5, &s_task);
    ESP_LOGI(TAG, "ble_bridge mode started");
}

void ble_bridge_stop(void)
{
    s_running = false;
    s_task    = NULL;
    uart_driver_delete(FC_UART_PORT);
    ESP_LOGI(TAG, "ble_bridge mode stopped");
}
