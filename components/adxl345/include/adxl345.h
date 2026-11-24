#ifndef ADXL345_H
#define ADXL345_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inisialisasi ADXL345 pada bus I2C master (ESP-IDF v6.0.2 i2c_master API).
 * @param sda_gpio Pin SDA (default: 21)
 * @param scl_gpio Pin SCL (default: 22)
 * @param clk_speed_hz Frekuensi I2C (default: 400000 Hz)
 * @return true jika ADXL345 terdeteksi (Device ID 0xE5), false jika tidak terpasang/gagal.
 */
bool adxl345_init(int sda_gpio, int scl_gpio, uint32_t clk_speed_hz);

/**
 * @brief Memeriksa apakah sensor ADXL345 tersedia dan siap dibaca.
 */
bool adxl345_is_available(void);

/**
 * @brief Membaca data akselerasi 3-sumbu dalam satuan gravitasi (g).
 * @param x Output akselerasi sumbu X (g)
 * @param y Output akselerasi sumbu Y (g)
 * @param z Output akselerasi sumbu Z (g)
 * @return true jika pembacaan berhasil.
 */
bool adxl345_read_accel(float *x, float *y, float *z);

#ifdef __cplusplus
}
#endif

#endif // ADXL345_H
