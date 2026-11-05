idf_component_register(SRCS "use_wifi.c"
                            "aiot_mqtt_sign.c"
                            "use_ota.c"
                    INCLUDE_DIRS "."
                                "../common"
                                "../app_control"
                                "../protobuf"
                    REQUIRES nvs_flash mqtt json spi_flash fatfs app_update esp_http_client esp_https_ota esp-tls SU3
                    )