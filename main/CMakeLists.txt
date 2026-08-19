idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES kwp2000 esp_wifi esp_http_server esp_netif nvs_flash esp_timer)

