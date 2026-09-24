#ifndef SD_LOGGER_H
#define SD_LOGGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool sd_logger_init(void);
bool sd_logger_is_available(void);
bool sd_logger_log_ecu_status(uint32_t timestamp_ms,
                              bool ecu_connected,
                              uint16_t rpm,
                              uint16_t speed_kph,
                              uint8_t coolant_temp_c,
                              uint8_t dtc_count);

#ifdef __cplusplus
}
#endif

#endif
