set(DEVICE_LINK_SOURCES
    "src/device_link_framing.c"
    "src/generated/microtech/link/v1/capabilities.pb-c.c"
    "src/generated/microtech/link/v1/envelope.pb-c.c"
    "src/generated/microtech/link/v1/errors.pb-c.c"
    "src/generated/microtech/link/v1/events.pb-c.c"
    "src/generated/microtech/link/v1/session.pb-c.c"
    "src/generated/microtech/link/v1/transfer.pb-c.c"
)

idf_component_register(
    SRCS ${DEVICE_LINK_SOURCES}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src/generated"
    PRIV_REQUIRES protobuf-c
)
