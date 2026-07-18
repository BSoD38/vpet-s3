#ifndef __ESP_LCD_TOUCH_CST3530_H__
#define __ESP_LCD_TOUCH_CST3530_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_panel_io.h"
#include "driver/i2c.h"
#include "ST7789.h"

// I2C settings
// #define I2C_Touch_SCL_IO            3      /*!< GPIO number used for I2C master clock */
// #define I2C_Touch_SDA_IO            1      /*!< GPIO number used for I2C master data  */
// #define I2C_Touch_INT_IO            4      /*!< GPIO number used for I2C master data  */
// #define I2C_Touch_RST_IO            2      /*!< GPIO number used for I2C master clock */
// #define I2C_Touch_MASTER_NUM        1                          /*!< I2C master i2c port number, the number of i2c peripheral interfaces available will depend on the chip */
// #define I2C_MASTER_FREQ_HZ          400000                     /*!< I2C master clock frequency */
// #define I2C_MASTER_TX_BUF_DISABLE   0                          /*!< I2C master doesn't need buffer */
// #define I2C_MASTER_RX_BUF_DISABLE   0                          /*!< I2C master doesn't need buffer */
// #define I2C_MASTER_TIMEOUT_MS       1000


/* CST3530 I2C slave address */
#define ESP_LCD_TOUCH_IO_I2C_CST3530_ADDRESS    (0x58)

/**
 * @brief Create a new CST3530 touch handle
 *
 * In legacy I2C mode, config->user_data should carry i2c_port_t.
 *
 * @param io Panel IO handle, can be NULL in legacy I2C mode
 * @param config Touch configuration
 * @param out_touch Output touch handle
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG on invalid argument
 *      - ESP_ERR_NO_MEM on memory allocation failure
 *      - Others from underlying drivers
 */
esp_err_t esp_lcd_touch_new_i2c_cst3530(const esp_lcd_panel_io_handle_t io,
                                        const esp_lcd_touch_config_t *config,
                                        esp_lcd_touch_handle_t *out_touch);

/**
 * @brief Initialize legacy I2C master for CST3530
 *
 * @return
 *      - ESP_OK on success
 *      - Others on failure
 */
esp_err_t TOUCH2_Init(esp_lcd_touch_handle_t *out_tp);


#ifdef __cplusplus
}
#endif

#endif /* __ESP_LCD_TOUCH_CST3530_H__ */