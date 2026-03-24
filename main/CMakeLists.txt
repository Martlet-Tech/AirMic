idf_component_register(
	SRCS 
		"main.c"
		"ble/ble_nus.c"
        PRIV_REQUIRES spi_flash
	REQUIRES bt nvs_flash driver
	INCLUDE_DIRS "" "boards" "ble"
)
