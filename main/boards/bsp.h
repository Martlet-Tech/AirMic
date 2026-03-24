#ifndef __BSP_H__
#define __BSP_H__

#include "driver/gpio.h"

#define PIN_SDIO_D0 GPIO_NUM_16
#define PIN_SDIO_D1 GPIO_NUM_6
#define PIN_SDIO_D2 GPIO_NUM_7
#define PIN_SDIO_D3 GPIO_NUM_15
#define PIN_SDIO_CLK GPIO_NUM_18
#define PIN_SDIO_CMD GPIO_NUM_17

#define PIN_LED2812 GPIO_NUM_40
#define PIN_LED GPIO_NUM_41
#define PIN_KEY GPIO_NUM_42

#define PIN_I2S_WS GPIO_NUM_9
#define PIN_I2S_CLK GPIO_NUM_10
#define PIN_I2S_SD GPIO_NUM_11

#define FC_UART_RX_PIN GPIO_NUM_1 // FC_UTX → ESP RX
#define FC_UART_TX_PIN GPIO_NUM_2 // FC_URX → ESP TX

#endif