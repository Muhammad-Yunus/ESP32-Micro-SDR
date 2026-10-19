/*
 * microSD card (SPI) for the ESP32-3248S035.
 * Pin map ported from SDC_SPI in https://github.com/ardnew/ESP32-3248S035
 */
#include "bsp_esp323248s035.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

static const char *TAG = "bsp_sd";

static sdmmc_card_t *s_card = NULL;
static bool s_bus_ready = false;
static char s_mount_point[64] = {0};

esp_err_t bsp_sd_mount(const char *mount_point)
{
    ESP_RETURN_ON_FALSE(mount_point != NULL, ESP_ERR_INVALID_ARG, TAG, "null mount point");
    if (s_card) return ESP_OK;

    if (!s_bus_ready) {
        spi_bus_config_t buscfg = {
            .mosi_io_num = BSP_SD_PIN_MOSI,
            .miso_io_num = BSP_SD_PIN_MISO,
            .sclk_io_num = BSP_SD_PIN_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4000,
        };
        esp_err_t err = spi_bus_initialize(BSP_SD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
            return err;
        }
        s_bus_ready = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BSP_SD_SPI_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = BSP_SD_SPI_HOST;
    slot_config.gpio_cs = BSP_SD_PIN_CS;
    slot_config.gpio_cd = SDSPI_SLOT_NO_CD;
    slot_config.gpio_wp = SDSPI_SLOT_NO_WP;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config,
                                            &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }

    strncpy(s_mount_point, mount_point, sizeof(s_mount_point) - 1);
    ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t bsp_sd_unmount(void)
{
    if (!s_card) return ESP_OK;

    esp_err_t err = esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    if (err == ESP_OK) {
        s_card = NULL;
        s_mount_point[0] = '\0';
    }
    return err;
}
