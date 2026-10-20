idf_component_register(
    SRCS "thread_br_web.c" "esp_br_web_api.c" "esp_br_web_base.c" "thread_br_srp.c"
    INCLUDE_DIRS "."
    REQUIRES openthread esp_http_server json esp_netif esp_timer esp_app_format
)
