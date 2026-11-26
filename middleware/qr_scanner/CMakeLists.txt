idf_component_register(
    SRCS "qr_scanner.c"
         "quirc/quirc.c"
         "quirc/identify.c"
         "quirc/decode.c"
         "quirc/version_db.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "quirc"
    PRIV_REQUIRES esp_rom heap
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE QUIRC_FLOAT_TYPE=float QUIRC_MAX_REGIONS=254)
