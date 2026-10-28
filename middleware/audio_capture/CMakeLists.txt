idf_component_register(
    SRCS "audio_capture.c"
    INCLUDE_DIRS "include"
    REQUIRES i2s_microphone
    PRIV_REQUIRES esp_timer freertos heap
)
