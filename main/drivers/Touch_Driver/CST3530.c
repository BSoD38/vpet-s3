/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2025 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "esp_lcd_touch.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_lcd_panel_io.h"
#include "CST3530.h"

static const char *TAG = "CST3530";

/* Debug registers */
#define CST3530_REG_DEBUG_INFO_MODE                 0xD00000
#define CST3530_REG_DEBUG_SOFT_RESET                0xD00003
#define CST3530_REG_DEBUG_REPORT_MODE               0xD0000C
#define CST3530_REG_DEBUG_SLEEP_MODE_EN             0xD00022

/* CST3530 registers */
#define ESP_LCD_TOUCH_CST3530_END_READ_REG          0xD00002AB
#define ESP_LCD_TOUCH_CST3530_DATA_REG              0xD0070000
#define ESP_LCD_TOUCH_CST3530_MODE_REG              0xD0070200
#define ESP_LCD_TOUCH_CST3530_POINT_CNT_REG         0xD0070300
#define ESP_LCD_TOUCH_CST3530_COORD_REG             0xD0070400
#define ESP_LCD_TOUCH_CST3530_TOUCH_STATE_REG       0xD0070800
#define ESP_LCD_TOUCH_CST3530_COORD_NEXT_REG        0xD0070900

/* New-driver I2C device handle used by this single-instance driver */
static i2c_master_dev_handle_t g_cst3530_dev = NULL;

/*******************************************************************************
 * Function declarations
 *******************************************************************************/
static esp_err_t esp_lcd_touch_cst3530_read_data(esp_lcd_touch_handle_t tp);
static bool esp_lcd_touch_cst3530_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t esp_lcd_touch_cst3530_del(esp_lcd_touch_handle_t tp);
static esp_err_t touch_cst3530_i2c_read(esp_lcd_touch_handle_t tp, uint32_t reg, uint8_t *data, uint8_t len);
static esp_err_t touch_cst3530_i2c_write(esp_lcd_touch_handle_t tp, uint32_t reg, uint8_t *data, uint8_t len);
static esp_err_t touch_cst3530_reset(esp_lcd_touch_handle_t tp);

/*******************************************************************************
 * Public API functions
 *******************************************************************************/

esp_err_t esp_lcd_touch_new_i2c_cst3530(const esp_lcd_panel_io_handle_t io,
                                        const esp_lcd_touch_config_t *config,
                                        esp_lcd_touch_handle_t *out_touch)
{
    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t esp_lcd_touch_cst3530 = NULL;

    assert(config != NULL);
    assert(out_touch != NULL);

    if (g_cst3530_dev == NULL) {
        ESP_LOGE(TAG, "I2C device not initialized");
        *out_touch = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    esp_lcd_touch_cst3530 = heap_caps_calloc(1, sizeof(esp_lcd_touch_t), MALLOC_CAP_DEFAULT);
    ESP_GOTO_ON_FALSE(esp_lcd_touch_cst3530, ESP_ERR_NO_MEM, err, TAG, "No memory for CST3530 controller");

    esp_lcd_touch_cst3530->io = io;
    esp_lcd_touch_cst3530->read_data = esp_lcd_touch_cst3530_read_data;
    esp_lcd_touch_cst3530->get_xy = esp_lcd_touch_cst3530_get_xy;
    esp_lcd_touch_cst3530->del = esp_lcd_touch_cst3530_del;

    esp_lcd_touch_cst3530->data.lock.owner = portMUX_FREE_VAL;
    memcpy(&esp_lcd_touch_cst3530->config, config, sizeof(esp_lcd_touch_config_t));

    if (esp_lcd_touch_cst3530->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_gpio_config = {
            .mode = GPIO_MODE_INPUT,
            .intr_type = GPIO_INTR_NEGEDGE,
            .pin_bit_mask = BIT64(esp_lcd_touch_cst3530->config.int_gpio_num)
        };
        ret = gpio_config(&int_gpio_config);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "GPIO config failed");
    }

    if (esp_lcd_touch_cst3530->config.rst_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t rst_gpio_config = {
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
            .pin_bit_mask = BIT64(esp_lcd_touch_cst3530->config.rst_gpio_num)
        };
        ret = gpio_config(&rst_gpio_config);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "RST GPIO config failed");
    }

    ret = touch_cst3530_reset(esp_lcd_touch_cst3530);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "CST3530 reset failed");

    *out_touch = esp_lcd_touch_cst3530;
    return ESP_OK;

err:
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error (0x%x)! Touch controller CST3530 initialization failed!", ret);
        if (esp_lcd_touch_cst3530) {
            esp_lcd_touch_cst3530_del(esp_lcd_touch_cst3530);
        }
    }
    *out_touch = NULL;
    return ret;
}

/*******************************************************************************
 * Private functions
 *******************************************************************************/

static esp_err_t esp_lcd_touch_cst3530_read_data(esp_lcd_touch_handle_t tp)
{
    esp_err_t err;
    uint8_t buf[51];
    uint8_t touch_cnt = 0;
    size_t i = 0;

    assert(tp != NULL);

    taskENTER_CRITICAL(&tp->data.lock);
    tp->data.points = 0;
    taskEXIT_CRITICAL(&tp->data.lock);

    err = touch_cst3530_i2c_read(tp, ESP_LCD_TOUCH_CST3530_DATA_REG, buf, 9);
    ESP_RETURN_ON_ERROR(err, TAG, "I2C read error!");

    if ((buf[3] & 0x0F) == 0x00 || (buf[8] & 0xF0) == 0x00) {
        err = touch_cst3530_i2c_write(tp, ESP_LCD_TOUCH_CST3530_END_READ_REG, NULL, 0);
        ESP_RETURN_ON_ERROR(err, TAG, "I2C write error!");
        return ESP_OK;
    } else {
        touch_cnt = buf[3] & 0x0F;
        if (touch_cnt > CONFIG_ESP_LCD_TOUCH_MAX_POINTS || touch_cnt == 0) {
            err = touch_cst3530_i2c_write(tp, ESP_LCD_TOUCH_CST3530_END_READ_REG, NULL, 0);
            ESP_RETURN_ON_ERROR(err, TAG, "I2C write error!");
            return ESP_OK;
        }

        if (touch_cnt > 1) {
            err = touch_cst3530_i2c_read(tp, ESP_LCD_TOUCH_CST3530_COORD_NEXT_REG, &buf[9], (touch_cnt - 1) * 5);
            ESP_RETURN_ON_ERROR(err, TAG, "I2C read error!");
        }

        err = touch_cst3530_i2c_write(tp, ESP_LCD_TOUCH_CST3530_END_READ_REG, NULL, 0);
        ESP_RETURN_ON_ERROR(err, TAG, "I2C read error!");

        taskENTER_CRITICAL(&tp->data.lock);
        tp->data.points = (uint8_t)touch_cnt;

        for (i = 0; i < touch_cnt; i++) {
            tp->data.coords[i].x = (uint16_t)(((buf[(i * 5) + 7] & 0x0F) << 8) + buf[(i * 5) + 4]);
            tp->data.coords[i].y = (uint16_t)(((buf[(i * 5) + 7] & 0xF0) << 4) + buf[(i * 5) + 5]);
            tp->data.coords[i].strength = (uint16_t)buf[(i * 5) + 6];
        }

        taskEXIT_CRITICAL(&tp->data.lock);
    }

    return ESP_OK;
}

static bool esp_lcd_touch_cst3530_get_xy(esp_lcd_touch_handle_t tp,
                                         uint16_t *x,
                                         uint16_t *y,
                                         uint16_t *strength,
                                         uint8_t *point_num,
                                         uint8_t max_point_num)
{
    assert(tp != NULL);
    assert(x != NULL);
    assert(y != NULL);
    assert(point_num != NULL);
    assert(max_point_num > 0);

    taskENTER_CRITICAL(&tp->data.lock);

    if (tp->data.points > max_point_num) {
        tp->data.points = max_point_num;
    }

    for (size_t i = 0; i < tp->data.points; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }

    *point_num = tp->data.points;
    tp->data.points = 0;

    taskEXIT_CRITICAL(&tp->data.lock);
    return (*point_num > 0);
}

static esp_err_t esp_lcd_touch_cst3530_del(esp_lcd_touch_handle_t tp)
{
    assert(tp != NULL);

    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
    }

    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }

    if (g_cst3530_dev != NULL) {
        i2c_master_bus_rm_device(g_cst3530_dev);
        g_cst3530_dev = NULL;
    }
    free(tp);
    return ESP_OK;
}

static esp_err_t touch_cst3530_reset(esp_lcd_touch_handle_t tp)
{
    assert(tp != NULL);

    ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG, "GPIO set level error!");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG, "GPIO set level error!");
    vTaskDelay(pdMS_TO_TICKS(500));
    return ESP_OK;
}

static esp_err_t touch_cst3530_i2c_read(esp_lcd_touch_handle_t tp, uint32_t reg, uint8_t *data, uint8_t len)
{
    uint8_t reg_buf[4];

    assert(tp != NULL);
    assert(data != NULL);

    if (g_cst3530_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    reg_buf[0] = (uint8_t)((reg >> 24) & 0xFF);
    reg_buf[1] = (uint8_t)((reg >> 16) & 0xFF);
    reg_buf[2] = (uint8_t)((reg >> 8) & 0xFF);
    reg_buf[3] = (uint8_t)(reg & 0xFF);

    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(g_cst3530_dev,
                                    reg_buf,
                                    sizeof(reg_buf),
                                    data,
                                    len,
                                    1000),
        TAG,
        "I2C read data failed"
    );

    return ESP_OK;
}

static esp_err_t touch_cst3530_i2c_write(esp_lcd_touch_handle_t tp, uint32_t reg, uint8_t *data, uint8_t len)
{
    uint8_t tx_buf[20] = {0};

    assert(tp != NULL);

    if (g_cst3530_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > 16) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx_buf[0] = (uint8_t)((reg >> 24) & 0xFF);
    tx_buf[1] = (uint8_t)((reg >> 16) & 0xFF);
    tx_buf[2] = (uint8_t)((reg >> 8) & 0xFF);
    tx_buf[3] = (uint8_t)(reg & 0xFF);

    if ((data != NULL) && (len > 0)) {
        memcpy(&tx_buf[4], data, len);
    }

    ESP_RETURN_ON_ERROR(
        i2c_master_transmit(g_cst3530_dev,
                            tx_buf,
                            4 + len,
                            1000),
        TAG,
        "I2C write data failed"
    );

    return ESP_OK;
}

// /**
//  * @brief I2C master initialization
//  */
// esp_err_t Touch2_I2C_Init(void)
// {
//     int i2c_master_port = I2C_Touch_MASTER_NUM;

//     i2c_config_t conf = {
//         .mode = I2C_MODE_MASTER,
//         .sda_io_num = I2C_Touch_SDA_IO,
//         .scl_io_num = I2C_Touch_SCL_IO,
//         .sda_pullup_en = GPIO_PULLUP_ENABLE,
//         .scl_pullup_en = GPIO_PULLUP_ENABLE,
//         .master.clk_speed = I2C_MASTER_FREQ_HZ,
//     };

//     ESP_RETURN_ON_ERROR(i2c_param_config(i2c_master_port, &conf), TAG, "i2c_param_config failed");
//     ESP_RETURN_ON_ERROR(i2c_driver_install(i2c_master_port,
//                                            conf.mode,
//                                            I2C_MASTER_RX_BUF_DISABLE,
//                                            I2C_MASTER_TX_BUF_DISABLE,
//                                            0),
//                         TAG,
//                         "i2c_driver_install failed");

//     g_cst3530_i2c_port = (i2c_port_t)i2c_master_port;
//     return ESP_OK;
// }

esp_err_t TOUCH2_Init(esp_lcd_touch_handle_t *out_tp)
{
    esp_lcd_touch_handle_t local_tp = NULL;
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    i2c_master_bus_handle_t bus_handle = NULL;

    if (out_tp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_tp = NULL;

    /* Reuse the shared touch I2C bus (created by the CST328 probe attempt) and
     * register the CST3530 as a device on it. */
    ESP_RETURN_ON_ERROR(Touch_Bus_Get(&bus_handle), TAG, "I2C bus init failed");
    if (g_cst3530_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ESP_LCD_TOUCH_IO_I2C_CST3530_ADDRESS,
            .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_handle, &dev_cfg, &g_cst3530_dev), TAG, "add CST3530 device failed");
    }

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_V_RES,
        .y_max = EXAMPLE_LCD_H_RES,
        .rst_gpio_num = I2C_Touch_RST_IO,
        .int_gpio_num = I2C_Touch_INT_IO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_LOGI(TAG, "Initialize touch controller CST3530");
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_cst3530(tp_io_handle, &tp_cfg, &local_tp),
        TAG,
        "Initialize touch controller failed"
    );

    *out_tp = local_tp;
    return ESP_OK;
}