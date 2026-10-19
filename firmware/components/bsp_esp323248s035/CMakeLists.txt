set(BSP_SRCS
    "src/bsp_tft.c"
    "src/bsp_touch.c"
    "src/bsp_led.c"
    "src/bsp_ldr.c"
    "src/bsp_sd.c"
    "src/bsp.c"
)

# GPIO26 (DAC channel 1) can be driven either by the LEDC buzzer or by the
# DAC continuous audio driver, but never both at once.
if(CONFIG_BSP_AMP_DRIVER_BUZZER)
    list(APPEND BSP_SRCS "src/bsp_buzzer.c")
elseif(CONFIG_BSP_AMP_DRIVER_DAC)
    list(APPEND BSP_SRCS "src/bsp_audio.c")
endif()

idf_component_register(
    SRCS
        ${BSP_SRCS}
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_lcd
        esp_driver_spi
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_dac
        esp_driver_i2c
        esp_driver_sdspi
        sdmmc
        esp_adc
        esp_timer
        fatfs
        lvgl
)

# The LVGL glue code is only compiled when the port is enabled.
if(CONFIG_BSP_LVGL)
    target_sources(${COMPONENT_LIB} PRIVATE "src/bsp_lvgl.c")
endif()
