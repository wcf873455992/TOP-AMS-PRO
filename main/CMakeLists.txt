idf_component_register(
    SRCS "main.c" "dns_server.c" "bambu_mqtt.c" "bambu_cmd.c" "config_store.c" "filament.c" "ota.c"
    EMBED_FILES "index.html" "wifi_config.html" "printer.html" "ota.html" "monitor.html"
    PRIV_REQUIRES esp_http_server nvs_flash esp_wifi esp_driver_gpio esp_https_ota app_update
)