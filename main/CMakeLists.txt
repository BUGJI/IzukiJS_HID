idf_component_register(
    SRCS
        "main.c"
        "izuki_ble.c"
        "izuki_input.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hid bt nvs_flash
)
