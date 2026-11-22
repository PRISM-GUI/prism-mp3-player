idf_component_register(
    SRCS player_main.cpp bt_output.cpp
    REQUIRES player_core prism korvo1 freertos pthread bt nvs_flash
)
