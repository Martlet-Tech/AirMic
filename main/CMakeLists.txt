file(GLOB_RECURSE  SRC_BLE "ble/*.c")
file(GLOB_RECURSE  SRC_BOARDS "boards/*.c")
file(GLOB_RECURSE  SRC_DRIVERS "drivers/*.c")
file(GLOB_RECURSE  SRC_APP "app/*.c")

idf_component_register(
	SRCS 
		"main.c"
		${SRC_BLE}
		${SRC_BOARDS}
		${SRC_DRIVERS}
		${SRC_APP}
        PRIV_REQUIRES spi_flash
	REQUIRES bt nvs_flash driver fatfs sdmmc
	INCLUDE_DIRS "" "boards" "ble"  "app" "app/recorder" "app/ble_bridge" "app/rid"
		"drivers" "drivers/led" "drivers/button" "drivers/mic" "drivers/rgbled" "drivers/sdcard"
)
