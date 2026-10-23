idf_component_register(SRCS "main.c" "miot_spec.c" "miot_crypto.c" "miot_link.c" "gateway.c" INCLUDE_DIRS "." REQUIRES bt console mbedtls nvs_flash esp_timer esp_wifi esp_event esp_netif mqtt)
