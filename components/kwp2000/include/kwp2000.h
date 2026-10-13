#ifndef KWP2000_H
#define KWP2000_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

// Set 1 jika memakai transistor NPN inverting (TX HIGH -> Bus LOW),
// Set 0 jika memakai L9637D / IC Transceiver non-inverting.
#define KLINE_HW_INVERTING 1

#define KWP_FRAME_MAX_SIZE 256
#define KWP_RX_HISTORY_SIZE 1024

// Honda CB150R KWP2000 Addresses & Commands
#define KWP_HONDA_DESTINATION          0x72
#define KWP_HONDA_REQUEST_ALL          0x71

// Frame structure
typedef struct {
    uint8_t length;
    uint8_t target_address;
    uint8_t sender_address;
    uint8_t service_id;
    uint8_t data[KWP_FRAME_MAX_SIZE];
    uint8_t data_len;
    uint8_t checksum;
} kwp2000_frame_t;

// Response queue message
typedef struct {
    kwp2000_frame_t frame;
    uint32_t timestamp;
    bool is_valid;
} kwp2000_message_t;

typedef struct {
    uint32_t idle_ms;
    uint32_t low_ms;
    uint32_t high_ms;
    uint32_t w4_us;
    uint32_t byte_gap_us;
} kwp_init_params_t;

// Inisialisasi awal port & driver K-Line
void kwp2000_init(int rx_gpio, int tx_gpio, int baudrate);

// Eksekusi Fast Init melalui UART (25ms LOW / 25ms HIGH)
bool kwp2000_fast_init(const kwp_init_params_t *params);

// Pengujian hardware fisik Bus LOW / Bus HIGH
bool kwp2000_start_tx_hardware_test(bool force_bus_low);
void kwp2000_stop_tx_hardware_test(void);

// Mulai sesi diagnostik Honda K-Line
bool kwp2000_begin_diagnostic_session(uint8_t session_type,
                                      const kwp_init_params_t *params);

// Reset status sesi diagnostik
void kwp2000_reset_session(void);

bool kwp2000_is_initialized(void);

// Transmisi & penerimaan frame
bool kwp2000_send_frame(kwp2000_frame_t *frame);
bool kwp2000_receive_frame(kwp2000_frame_t *frame, uint32_t timeout_ms);

// Read Data By ID (Honda Live Data Table)
bool kwp2000_read_data_by_id(uint16_t data_id,
                             uint8_t *response,
                             size_t response_capacity,
                             uint8_t *response_len);

size_t kwp2000_copy_rx_history(uint8_t *buffer,
                               size_t capacity,
                               uint32_t *dropped_bytes);
void kwp2000_clear_rx_history(void);

QueueHandle_t kwp2000_get_response_queue(void);

#ifdef __cplusplus
}
#endif

#endif // KWP2000_H
