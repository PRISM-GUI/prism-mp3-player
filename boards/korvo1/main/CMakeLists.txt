idf_component_register(
    SRCS player_main.cpp library_loader.cpp settings_nvs.cpp
    REQUIRES player_core prism prism_bt korvo1 freertos pthread nvs_flash esp_driver_jpeg
)
