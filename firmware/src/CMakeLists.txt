idf_component_register(
    SRCS main.c hw.c ui.c proto.c lv_font_hack_20.c
    INCLUDE_DIRS .
    REQUIRES driver esp_lcd esp_timer nvs_flash mbedtls esp_driver_usb_serial_jtag
)
