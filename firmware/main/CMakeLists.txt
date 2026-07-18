idf_component_register(
    SRCS
        "app_main.c"
        "sd_hal.c"
        "sd_decode.c"
        "sd_mfg_db.c"
        "diag_read.c"
        "diag_write.c"
        "report.c"
    INCLUDE_DIRS "."
    REQUIRES
        sdmmc
        esp_driver_sdspi
        esp_driver_gpio
        esp_driver_spi
        esp_timer
        driver
)
