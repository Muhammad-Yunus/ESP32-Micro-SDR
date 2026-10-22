set(SRCS
    "app_main.c"
    "sdr_config.c"
    "rx_adc.c"
    "dsp_chain.c"
    "audio_out.c"
    "spectrum.c"
    "ui/ui_main.c"
    "ui/ui_scope.c"
    "ui/ui_colors.c"
    "ui/font_dseg7_36.c"
)

# Build self-test (CONVENTIONS.md §5): modul uji hanya ikut pada build
# CONFIG_SDR_SELFTEST=y agar firmware produksi tetap tanpa BSS tambahan.
if(CONFIG_SDR_SELFTEST)
    list(APPEND SRCS
        "selftest.c"
        "iq_gen.c"
    )
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "."
    REQUIRES
        bsp_esp323248s035
        lvgl
        esp_adc
        esp_timer
        esp_driver_sdm
        esp-dsp
)
