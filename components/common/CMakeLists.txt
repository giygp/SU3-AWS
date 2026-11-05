idf_component_register(SRCS "common.c"
                    INCLUDE_DIRS "."
                                "../protobuf"
                    REQUIRES nvs_flash protobuf heap
                    )