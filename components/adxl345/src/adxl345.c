#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "adxl345.h"

static const char *TAG = "ADXL345";

#define ADXL345_I2C_ADDR 0x53 // Default I2C address (SDO ke GND)

#define ADXL345_REG_DEVID       0x00
#define ADXL345_REG_BW_RATE     0x2C
#define ADXL345_REG_POWER_CTL   0x2D
#define ADXL345_REG_DATA_FORMAT 0x31
#define ADXL345_REG_DATAX0      0x32

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static i2c_master_dev_handle_t adxl345_dev_handle = NULL;
static bool adxl_available = false;

bool adxl345_init(int sda_gpio, int scl_gpio, uint32_t clk_speed_hz)
{
    ESP_LOGI(TAG, "Probing ADXL345 on SDA=%d, SCL=%d, Speed=%lu Hz",
             sda_gpio, scl_gpio, (unsigned long)clk_speed_hz);

    adxl_available = false;

    // 1. Inisialisasi bus master I2C jika belum ada
    if (i2c_bus_handle == NULL) {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = (gpio_num_t)sda_gpio,
            .scl_io_num = (gpio_num_t)scl_gpio,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };

        esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create I2C master bus: %s", esp_err_to_name(err));
            return false;
        }
    }

    // 2. Tambahkan handle perangkat ADXL345
    if (adxl345_dev_handle == NULL) {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ADXL345_I2C_ADDR,
            .scl_speed_hz = clk_speed_hz,
        };

        esp_err_t err = i2c_master_bus_add_device(i2c_bus_handle, &dev_config, &adxl345_dev_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to add ADXL345 device to I2C bus: %s", esp_err_to_name(err));
            return false;
        }
    }

    // 3. Verifikasi Device ID (0xE5)
    uint8_t reg_addr = ADXL345_REG_DEVID;
    uint8_t dev_id = 0;
    esp_err_t err = i2c_master_transmit_receive(adxl345_dev_handle, &reg_addr, 1, &dev_id, 1, 50);
    if (err != ESP_OK || dev_id != 0xE5) {
        ESP_LOGW(TAG, "ADXL345 not detected (dev_id=0x%02X, err=%s). Accelerometer disabled for now.",
                 dev_id, esp_err_to_name(err));
        return false;
    }

    // 4. Konfigurasi sensor (100 Hz ODR, Range ±4g, Measurement mode)
    uint8_t bw_rate[2] = {ADXL345_REG_BW_RATE, 0x0A};
    i2c_master_transmit(adxl345_dev_handle, bw_rate, 2, 50);

    uint8_t data_format[2] = {ADXL345_REG_DATA_FORMAT, 0x09}; // ±4g Full Resolution (3.9 mg/LSB)
    i2c_master_transmit(adxl345_dev_handle, data_format, 2, 50);

    uint8_t power_ctl[2] = {ADXL345_REG_POWER_CTL, 0x08}; // Measurement ON
    i2c_master_transmit(adxl345_dev_handle, power_ctl, 2, 50);

    adxl_available = true;
    ESP_LOGI(TAG, "ADXL345 successfully initialized (Device ID: 0xE5, Range: ±4g)");
    return true;
}

bool adxl345_is_available(void)
{
    return adxl_available;
}

bool adxl345_read_accel(float *x, float *y, float *z)
{
    if (!adxl_available || adxl345_dev_handle == NULL) {
        return false;
    }

    uint8_t reg_addr = ADXL345_REG_DATAX0;
    uint8_t raw_data[6] = {0};

    esp_err_t err = i2c_master_transmit_receive(adxl345_dev_handle, &reg_addr, 1, raw_data, 6, 20);
    if (err != ESP_OK) {
        return false;
    }

    int16_t rawX = (int16_t)((uint16_t)raw_data[1] << 8 | raw_data[0]);
    int16_t rawY = (int16_t)((uint16_t)raw_data[3] << 8 | raw_data[2]);
    int16_t rawZ = (int16_t)((uint16_t)raw_data[5] << 8 | raw_data[4]);

    // Skala 3.9 mg/LSB (0.0039 g)
    if (x) *x = rawX * 0.0039f;
    if (y) *y = rawY * 0.0039f;
    if (z) *z = rawZ * 0.0039f;

    return true;
}
