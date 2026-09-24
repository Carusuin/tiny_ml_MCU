#ifndef KWP2000_H
#define KWP2000_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KWP_FRAME_MAX_SIZE 256

// KWP2000 Service IDs
#define KWP_START_DIAGNOSTIC           0x10
#define KWP_ECU_RESET                  0x11
#define KWP_READ_DTC                   0x18
#define KWP_READ_DATA_BY_ID            0x22
#define KWP_WRITE_DATA_BY_ID           0x2E
#define KWP_TESTER_PRESENT             0x3E
#define KWP_READ_EXTENDED_DATA         0x19
#define KWP_CLEAR_DTC                  0x14

// Honda CB150R specific addresses
#define KWP_HONDA_ECU_ADDRESS          0x11
#define KWP_TOOL_ADDRESS               0xF1

// KWP2000 Frame structure
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

// Initialize KWP2000 UART and task
void kwp2000_init(int rx_gpio, int tx_gpio, int baudrate);

// Send the K-Line fast-initialization wake-up pattern before UART traffic.
bool kwp2000_fast_init(void);

// Force the TX logic level for hardware testing. The UART must be idle.
bool kwp2000_start_tx_hardware_test(bool force_low);
void kwp2000_stop_tx_hardware_test(void);

// Run Fast Init, start UART communication, and open a diagnostic session.
bool kwp2000_begin_diagnostic_session(uint8_t session_type);

bool kwp2000_is_initialized(void);

// Send KWP2000 frame
bool kwp2000_send_frame(kwp2000_frame_t *frame);

// Receive KWP2000 frame
bool kwp2000_receive_frame(kwp2000_frame_t *frame, uint32_t timeout_ms);

// Tester Present (keep-alive)
bool kwp2000_tester_present(void);

// Read Data By ID
bool kwp2000_read_data_by_id(uint16_t data_id, uint8_t *response, uint8_t *response_len);

// Write Data By ID
bool kwp2000_write_data_by_id(uint16_t data_id, uint8_t *data, uint8_t data_len);

// Read DTC (Diagnostic Trouble Codes)
bool kwp2000_read_dtc(uint8_t *dtc_buffer, uint8_t *dtc_count);

// Clear DTC
bool kwp2000_clear_dtc(void);

// Start diagnostic session
bool kwp2000_start_diagnostic_session(uint8_t session_type);

// Get KWP2000 response queue
QueueHandle_t kwp2000_get_response_queue(void);

#ifdef __cplusplus
}
#endif

#endif // KWP2000_H
