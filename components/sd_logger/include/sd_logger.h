#ifndef SD_LOGGER_H
#define SD_LOGGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool sd_logger_init(void);
bool sd_logger_is_available(void);

/**
 * @brief Log baris dataset gabungan ECU + Akselerasi inersia untuk training TinyML.
 */
bool sd_logger_log_status(uint32_t timestamp_ms,
                          bool ecu_connected,
                          uint16_t rpm,
                          uint16_t tps_tenths,
                          uint16_t map_quarters,
                          int16_t coolant_temp_c,
                          uint16_t speed_kph,
                          float accel_x,
                          float accel_y,
                          float accel_z);

#ifdef __cplusplus
}
#endif

#endif // SD_LOGGER_H
