idf_component_register(
    SRCS
        "src/device_link_operation.c"
        "src/device_link_core.c"
        "src/device_link_digest.c"
        "src/device_link_router.c"
        "src/device_link_tlv.c"
        "src/device_link_wire.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bt
)
