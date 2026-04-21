file(GLOB_RECURSE SRC_BLE     "ble/*.c")
file(GLOB_RECURSE SRC_BOARDS  "boards/*.c")
file(GLOB_RECURSE SRC_APP     "app/*.c")
file(GLOB_RECURSE SRC_WIFI    "wifi/*.c")

# drivers 排除整个 mic 目录，单独处理
file(GLOB_RECURSE SRC_DRIVERS
    "drivers/led/*.c"
    "drivers/button/*.c"
    "drivers/sdcard/*.c"
    "drivers/rgb_led/*.c"
)

# 根据 Kconfig 只编译一个 mic 实现
if(CONFIG_MIC_TYPE_PDM)
    set(SRC_MIC "drivers/mic/mic_pdm.c")
else()
    set(SRC_MIC "drivers/mic/mic_i2s.c")
endif()

idf_component_register(
    SRCS
        "main.c"
        ${SRC_BLE}
        ${SRC_BOARDS}
        ${SRC_DRIVERS}
        ${SRC_MIC}
        ${SRC_APP}
        ${SRC_WIFI}
    PRIV_REQUIRES spi_flash
    REQUIRES bt nvs_flash driver fatfs sdmmc esp_wifi esp_netif esp_event esp_http_server tinyusb
    INCLUDE_DIRS "" "boards" "ble" "wifi" "app" "app/airmicprotocol" "app/fc_link" "app/recorder"
        "app/rid" "app/usb_msc" "app/http_server"
        "drivers" "drivers/led" "drivers/button" "drivers/mic" "drivers/sdcard" "drivers/rgb_led"
)