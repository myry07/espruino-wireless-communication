idf_component_register(SRCS "line_detection.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp32-camera esp_timer driver esp_http_server esp_wifi nvs_flash esp_netif)
