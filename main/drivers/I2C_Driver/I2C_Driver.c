#include "I2C_Driver.h"


static const char *I2C_TAG = "I2C";

/* New-driver (i2c_master) bus shared by all sensors on this port. The legacy
 * driver cannot be used anywhere in the app once the new driver is used (the
 * touch controller requires the new driver), so the whole bus is migrated. */
static i2c_master_bus_handle_t s_i2c_bus = NULL;

/* Small cache of per-address device handles. Devices (QMI8658, PCF85063, ...)
 * share the bus but each needs its own handle for transactions. */
#define I2C_MAX_DEVICES 8
static struct {
    uint8_t addr;
    i2c_master_dev_handle_t dev;
} s_devs[I2C_MAX_DEVICES];
static int s_dev_count = 0;

static esp_err_t I2C_Get_Device(uint8_t addr, i2c_master_dev_handle_t *out_dev)
{
    for (int i = 0; i < s_dev_count; i++) {
        if (s_devs[i].addr == addr) {
            *out_dev = s_devs[i].dev;
            return ESP_OK;
        }
    }

    if (s_i2c_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_dev_count >= I2C_MAX_DEVICES) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        return err;
    }

    s_devs[s_dev_count].addr = addr;
    s_devs[s_dev_count].dev = dev;
    s_dev_count++;
    *out_dev = dev;
    return ESP_OK;
}

/**
 * @brief i2c master initialization
 */
static esp_err_t i2c_master_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}
void I2C_Init(void)
{
    /********************* I2C *********************/
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(I2C_TAG, "I2C initialized successfully");
}


// Reg addr is 8 bit
esp_err_t I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = I2C_Get_Device(Driver_addr, &dev);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[Length + 1];
    buf[0] = Reg_addr;
    // Copy Reg_data to buf starting at buf[1]
    memcpy(&buf[1], Reg_data, Length);
    return i2c_master_transmit(dev, buf, Length + 1, I2C_MASTER_TIMEOUT_MS);
}



esp_err_t I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = I2C_Get_Device(Driver_addr, &dev);
    if (err != ESP_OK) {
        return err;
    }

    return i2c_master_transmit_receive(dev, &Reg_addr, 1, Reg_data, Length, I2C_MASTER_TIMEOUT_MS);
}
