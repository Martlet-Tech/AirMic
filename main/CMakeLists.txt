file(GLOB_RECURSE  SRC_BLE "ble/*.c")
file(GLOB_RECURSE  SRC_BOARDS "boards/*.c")
file(GLOB_RECURSE  SRC_DRIVERS "drivers/*.c")
file(GLOB_RECURSE  SRC_APP "app/*.c")
file(GLOB_RECURSE  SRC_WIFI "wifi/*.c")

idf_component_register(
	SRCS 
		"main.c"
		${SRC_BLE}
		${SRC_BOARDS}
		${SRC_DRIVERS}
		${SRC_APP}
		${SRC_WIFI}
        PRIV_REQUIRES spi_flash
	REQUIRES bt nvs_flash driver fatfs sdmmc esp_wifi esp_netif esp_event
	INCLUDE_DIRS "" "boards" "ble" "wifi" "app" "app/airmicprotocol" "app/fc_link" "app/recorder"  "app/rid"  
		"drivers" "drivers/led" "drivers/button" "drivers/mic" "drivers/sdcard"
)