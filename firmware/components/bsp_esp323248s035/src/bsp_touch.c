/*
 * GT911 capacitive touch driver for the ESP32-3248S035.
 *
 * Direct C port of the Arduino BSP:
 *   https://github.com/ardnew/ESP32-3248S035
 * The raw register protocol (product id, status register, point buffer) and
 * the rotation dependent coordinate mapping are kept identical.
 */
#include "bsp_esp323248s035.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_touch";

#define GT911_REG_PROD_ID   0x8140
#define GT911_REG_STATUS    0x814E
#define GT911_REG_BASE_PT   0x814F
#define GT911_TOUCH_MAX     5

#define GT911_I2C_TIMEOUT_MS 100

typedef struct __attribute__((packed)) {
    uint8_t  track;
    uint16_t x;
    uint16_t y;
    uint16_t area;
    uint8_t  pad;
} gt911_point_t;

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

/* ------------------------------------------------------------------ */
/* low level register access                                           */
/* ------------------------------------------------------------------ */

static esp_err_t gt911_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_dev, addr, sizeof(addr), buf, len,
                                       GT911_I2C_TIMEOUT_MS);
}

static esp_err_t gt911_write(uint16_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[2 + len];
    tmp[0] = (uint8_t)(reg >> 8);
    tmp[1] = (uint8_t)(reg & 0xFF);
    if (len) memcpy(&tmp[2], buf, len);
    return i2c_master_transmit(s_dev, tmp, sizeof(tmp), GT911_I2C_TIMEOUT_MS);
}

/* ------------------------------------------------------------------ */
/* GT911 helpers                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t gt911_product_id(char out[5])
{
    uint8_t buf[4] = {0};
    ESP_RETURN_ON_ERROR(gt911_read(GT911_REG_PROD_ID, buf, sizeof(buf)), TAG, "read id");
    memcpy(out, buf, 4);
    out[4] = '\0';
    return ESP_OK;
}

/* Return the number of active touch points, clearing the status register. */
static int gt911_touch_count(void)
{
    uint8_t stat = 0;
    if (gt911_read(GT911_REG_STATUS, &stat, 1) != ESP_OK) {
        return 0;
    }
    int count = (stat & 0x80) ? (stat & 0x0F) : 0;
    if ((stat & 0x80) && ((stat & 0x0F) < GT911_TOUCH_MAX)) {
        uint8_t zero = 0;
        gt911_write(GT911_REG_STATUS, &zero, 1);
    }
    return count;
}

static void gt911_map_point(gt911_point_t *pt, uint8_t idx)
{
    uint16_t w = bsp_tft_width();
    uint16_t h = bsp_tft_height();
    uint16_t swap;
    switch (idx & 0x3) {
        case 0:
            pt->x = w - pt->x;
            pt->y = h - pt->y;
            break;
        case 1:
            swap = pt->x;
            pt->x = pt->y;
            pt->y = h - swap;
            break;
        case 2:
            break;
        case 3:
        default:
            swap = pt->x;
            pt->x = w - pt->y;
            pt->y = swap;
            break;
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t bsp_touch_init(void)
{
    if (s_dev) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BSP_TOUCH_I2C_PORT,
        .sda_io_num = BSP_TOUCH_PIN_SDA,
        .scl_io_num = BSP_TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        /* Bus already created (e.g. by bsp_init()); get its handle. */
        ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle(BSP_TOUCH_I2C_PORT, &s_bus), TAG, "get bus");
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_TOUCH_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "add device");

    char id[5];
    if (gt911_product_id(id) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 product id: %s", id);
    } else {
        ESP_LOGW(TAG, "GT911 not responding at addr 0x%02X", BSP_TOUCH_I2C_ADDR);
    }
    return ESP_OK;
}

bool bsp_touch_read(uint16_t *x, uint16_t *y)
{
    static uint16_t last_x = 0;
    static uint16_t last_y = 0;

    if (!s_dev) {
        return false;
    }

    int count = gt911_touch_count();
    if (count == 1) {
        gt911_point_t pt = {0};
        if (gt911_read(GT911_REG_BASE_PT, (uint8_t *)&pt, sizeof(pt)) == ESP_OK) {
            gt911_map_point(&pt, bsp_tft_rotation_index());
            last_x = pt.x;
            last_y = pt.y;
            if (x) *x = pt.x;
            if (y) *y = pt.y;
            return true;
        }
    }

    if (x) *x = last_x;
    if (y) *y = last_y;
    return false;
}
