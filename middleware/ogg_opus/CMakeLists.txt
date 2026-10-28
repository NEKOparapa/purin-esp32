idf_component_register(
    SRCS "ogg_opus.c" "encoder/pcm_opus_encoder.c" "muxer/ogg_opus_muxer.c"
    INCLUDE_DIRS "include" "encoder" "muxer"
    PRIV_REQUIRES 78__esp-opus
)
