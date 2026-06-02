idf_component_register(
    SRCS "mqtt_new_btn.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "mosquitto.org.crt"
    REQUIRES esp_hal_gpio esp_wifi
    PRIV_REQUIRES nvs_flash esp_driver_gpio
)
