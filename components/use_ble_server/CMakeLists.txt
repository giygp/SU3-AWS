idf_component_register(SRCS "ble_server.c"
                    INCLUDE_DIRS "."
                                "../common"
                                "../protobuf"
                    REQUIRES bt common
                    )
