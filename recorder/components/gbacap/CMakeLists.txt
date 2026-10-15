idf_component_register(
    SRCS "gbac_packet.c" "gbac_decode.c" "gbac_asm.c"
         "gbac_scale.c" "gbac_audio.c" "gbac_pts.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer
)
