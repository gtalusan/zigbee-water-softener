idf_component_register(SRCS "main.c" "custom_cluster.c" "sensors.c" "vl53l0x.c"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES vfs nvs_flash esp_adc esp_timer driver)
