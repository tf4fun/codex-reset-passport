idf_component_register(
    SRCS "main.c" "reset_ui.c" "reset_presenter.c" "reset_feed.c"
         "reset_wifi.c" "reset_blufi_security.c" "reset_power.c"
         "reset_settings.c" "reset_controls.c" "reset_hold.c" "reset_idle.c" "reset_text.c"
         "../assets/fonts/reset_font_16.c" "../assets/fonts/reset_font_12.c" "../assets/fonts/reset_font_24.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             esp_http_client esp-tls mbedtls json
)
