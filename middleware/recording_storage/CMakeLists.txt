idf_component_register(
    SRCS "recording_writer.c" "recording_store.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES ogg_opus esp_hw_support
)
