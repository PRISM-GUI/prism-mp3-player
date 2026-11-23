idf_component_register(
    SRCS player_main.cpp library_loader.cpp bt_output.cpp bt_link.cpp bt_callbacks.cpp
         bt_bringup.cpp speaker_output.cpp settings_nvs.cpp jpeg_cover.cpp
    REQUIRES player_core prism korvo1 freertos pthread bt nvs_flash esp_codec_dev esp_driver_jpeg
)
