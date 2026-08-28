#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "kwp2000.h"

static const char *TAG = "KWP2000";

// Use ESP32 hardware UART1 for K-Line communication.
#define KWP_UART_NUM           UART_NUM_1
#define KWP_UART_BUFFER_SIZE   512
#define KWP_RX_TIMEOUT_MS      1000
#define KWP_TX_TIMEOUT_MS      1000

static QueueHandle_t kwp_response_queue = NULL;
static int kwp_rx_gpio = 16;
static int kwp_tx_gpio = 17;
static int kwp_baudrate = 10400;

// Calculate checksum (XOR of all bytes)
static uint8_t kwp2000_calculate_checksum(uint8_t *data, uint8_t len)
{
    uint8_t checksum = 0;
    for (int i = 0; i < len; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

// Format frame into buffer
static int kwp2000_format_frame(kwp2000_frame_t *frame, uint8_t *buffer)
{
    int index = 0;
    
    buffer[index++] = frame->length;
    buffer[index++] = frame->target_address;
    buffer[index++] = frame->sender_address;
    buffer[index++] = frame->service_id;
    
    for (int i = 0; i < frame->data_len; i++) {
        buffer[index++] = frame->data[i];
    }
    
    uint8_t checksum = kwp2000_calculate_checksum(buffer, index);
    buffer[index++] = checksum;
    
    return index;
}

// Parse frame from buffer
static bool kwp2000_parse_frame(uint8_t *buffer, int len, kwp2000_frame_t *frame)
{
    if (len < 5) {
        return false;
    }
    
    frame->length = buffer[0];
    frame->target_address = buffer[1];
    frame->sender_address = buffer[2];
    frame->service_id = buffer[3];
    
    int data_len = len - 5;
    if (data_len < 0 || data_len >= KWP_FRAME_MAX_SIZE) {
        return false;
    }
    
    for (int i = 0; i < data_len; i++) {
        frame->data[i] = buffer[i + 4];
    }
    frame->data_len = data_len;
    frame->checksum = buffer[len - 1];
    
    // Verify checksum
    uint8_t calc_checksum = kwp2000_calculate_checksum(buffer, len - 1);
    if (calc_checksum != frame->checksum) {
        ESP_LOGW(TAG, "Checksum mismatch: expected 0x%02X, got 0x%02X", calc_checksum, frame->checksum);
        return false;
    }
    
    return true;
}

// KWP2000 task (runs on core 1)
static void kwp2000_task(void *arg)
{
    uint8_t rx_buffer[KWP_UART_BUFFER_SIZE];
    kwp2000_frame_t rx_frame;
    kwp2000_message_t msg;
    
    ESP_LOGI(TAG, "KWP2000 task started on core %d", xPortGetCoreID());
    
    while (1) {
        int len = uart_read_bytes(KWP_UART_NUM, rx_buffer, KWP_UART_BUFFER_SIZE, 
                                   pdMS_TO_TICKS(KWP_RX_TIMEOUT_MS));
        
        if (len > 0) {
            ESP_LOGD(TAG, "Received %d bytes", len);
            
            if (kwp2000_parse_frame(rx_buffer, len, &rx_frame)) {
                msg.frame = rx_frame;
                msg.timestamp = xTaskGetTickCount();
                msg.is_valid = true;
                
                ESP_LOGI(TAG, "Valid frame - Service: 0x%02X, Data len: %d", 
                         rx_frame.service_id, rx_frame.data_len);
                
                if (kwp_response_queue != NULL) {
                    xQueueSend(kwp_response_queue, &msg, portMAX_DELAY);
                }
            } else {
                ESP_LOGW(TAG, "Failed to parse frame");
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void kwp2000_init(int rx_gpio, int tx_gpio, int baudrate)
{
    kwp_rx_gpio = rx_gpio;
    kwp_tx_gpio = tx_gpio;
    kwp_baudrate = baudrate;
    
    // Create response queue
    kwp_response_queue = xQueueCreate(10, sizeof(kwp2000_message_t));
    if (kwp_response_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create response queue");
        return;
    }
    
    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_ODD,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    uart_param_config(KWP_UART_NUM, &uart_config);
    uart_set_pin(KWP_UART_NUM, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_set_line_inverse(KWP_UART_NUM, UART_SIGNAL_TXD_INV | UART_SIGNAL_RXD_INV);
    uart_driver_install(KWP_UART_NUM, KWP_UART_BUFFER_SIZE, KWP_UART_BUFFER_SIZE, 0, NULL, 0);
    
    ESP_LOGI(TAG, "UART initialized on pins RX=%d, TX=%d, Baudrate=%d", rx_gpio, tx_gpio, baudrate);
    
    // Create KWP2000 task on core 1
    xTaskCreatePinnedToCore(
        kwp2000_task,
        "kwp2000_task",
        4096,
        NULL,
        5,
        NULL,
        1  // Core 1
    );
    
    ESP_LOGI(TAG, "KWP2000 initialized");
}

bool kwp2000_send_frame(kwp2000_frame_t *frame)
{
    if (frame == NULL) {
        return false;
    }
    
    uint8_t buffer[KWP_FRAME_MAX_SIZE];
    int frame_len = kwp2000_format_frame(frame, buffer);
    
    int bytes_written = uart_write_bytes(KWP_UART_NUM, buffer, frame_len);
    
    if (bytes_written == frame_len) {
        ESP_LOGD(TAG, "Sent frame - Service: 0x%02X, Length: %d", frame->service_id, frame_len);
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to send frame");
        return false;
    }
}

bool kwp2000_receive_frame(kwp2000_frame_t *frame, uint32_t timeout_ms)
{
    if (kwp_response_queue == NULL) {
        return false;
    }
    
    kwp2000_message_t msg;
    if (xQueueReceive(kwp_response_queue, &msg, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        *frame = msg.frame;
        return msg.is_valid;
    }
    
    return false;
}

bool kwp2000_tester_present(void)
{
    kwp2000_frame_t tx_frame;
    tx_frame.length = 2;
    tx_frame.target_address = KWP_HONDA_ECU_ADDRESS;
    tx_frame.sender_address = KWP_TOOL_ADDRESS;
    tx_frame.service_id = KWP_TESTER_PRESENT;
    tx_frame.data[0] = 0x00;
    tx_frame.data_len = 1;
    
    if (!kwp2000_send_frame(&tx_frame)) {
        return false;
    }
    
    kwp2000_frame_t rx_frame;
    if (kwp2000_receive_frame(&rx_frame, 500)) {
        if (rx_frame.service_id == (KWP_TESTER_PRESENT + 0x40)) {
            ESP_LOGI(TAG, "Tester Present acknowledged");
            return true;
        }
    }
    
    return false;
}

bool kwp2000_read_data_by_id(uint16_t data_id, uint8_t *response, uint8_t *response_len)
{
    kwp2000_frame_t tx_frame;
    tx_frame.length = 4;
    tx_frame.target_address = KWP_HONDA_ECU_ADDRESS;
    tx_frame.sender_address = KWP_TOOL_ADDRESS;
    tx_frame.service_id = KWP_READ_DATA_BY_ID;
    tx_frame.data[0] = (data_id >> 8) & 0xFF;
    tx_frame.data[1] = data_id & 0xFF;
    tx_frame.data_len = 2;
    
    if (!kwp2000_send_frame(&tx_frame)) {
        return false;
    }
    
    kwp2000_frame_t rx_frame;
    if (kwp2000_receive_frame(&rx_frame, 500)) {
        if (rx_frame.service_id == (KWP_READ_DATA_BY_ID + 0x40)) {
            *response_len = rx_frame.data_len;
            memcpy(response, rx_frame.data, rx_frame.data_len);
            return true;
        }
    }
    
    return false;
}

bool kwp2000_write_data_by_id(uint16_t data_id, uint8_t *data, uint8_t data_len)
{
    kwp2000_frame_t tx_frame;
    tx_frame.target_address = KWP_HONDA_ECU_ADDRESS;
    tx_frame.sender_address = KWP_TOOL_ADDRESS;
    tx_frame.service_id = KWP_WRITE_DATA_BY_ID;
    tx_frame.data[0] = (data_id >> 8) & 0xFF;
    tx_frame.data[1] = data_id & 0xFF;
    
    for (int i = 0; i < data_len; i++) {
        tx_frame.data[i + 2] = data[i];
    }
    
    tx_frame.data_len = data_len + 2;
    tx_frame.length = tx_frame.data_len + 2;
    
    if (!kwp2000_send_frame(&tx_frame)) {
        return false;
    }
    
    kwp2000_frame_t rx_frame;
    if (kwp2000_receive_frame(&rx_frame, 500)) {
        if (rx_frame.service_id == (KWP_WRITE_DATA_BY_ID + 0x40)) {
            return true;
        }
    }
    
    return false;
}

bool kwp2000_read_dtc(uint8_t *dtc_buffer, uint8_t *dtc_count)
{
    kwp2000_frame_t tx_frame;
    tx_frame.length = 2;
    tx_frame.target_address = KWP_HONDA_ECU_ADDRESS;
    tx_frame.sender_address = KWP_TOOL_ADDRESS;
    tx_frame.service_id = KWP_READ_DTC;
    tx_frame.data[0] = 0x01;
    tx_frame.data_len = 1;
    
    if (!kwp2000_send_frame(&tx_frame)) {
        return false;
    }
    
    kwp2000_frame_t rx_frame;
    if (kwp2000_receive_frame(&rx_frame, 500)) {
        if (rx_frame.service_id == (KWP_READ_DTC + 0x40)) {
            *dtc_count = rx_frame.data_len;
            memcpy(dtc_buffer, rx_frame.data, rx_frame.data_len);
            return true;
        }
    }
    
    return false;
}

bool kwp2000_clear_dtc(void)
{
    kwp2000_frame_t tx_frame;
    tx_frame.length = 2;
    tx_frame.target_address = KWP_HONDA_ECU_ADDRESS;
    tx_frame.sender_address = KWP_TOOL_ADDRESS;
    tx_frame.service_id = KWP_CLEAR_DTC;
    tx_frame.data[0] = 0xFF;
    tx_frame.data_len = 1;
    
    if (!kwp2000_send_frame(&tx_frame)) {
        return false;
    }
    
    kwp2000_frame_t rx_frame;
    if (kwp2000_receive_frame(&rx_frame, 500)) {
        if (rx_frame.service_id == (KWP_CLEAR_DTC + 0x40)) {
            return true;
        }
    }
    
    return false;
}

bool kwp2000_start_diagnostic_session(uint8_t session_type)
{
    kwp2000_frame_t tx_frame;
    tx_frame.length = 3;
    tx_frame.target_address = KWP_HONDA_ECU_ADDRESS;
    tx_frame.sender_address = KWP_TOOL_ADDRESS;
    tx_frame.service_id = KWP_START_DIAGNOSTIC;
    tx_frame.data[0] = session_type;
    tx_frame.data_len = 1;
    
    if (!kwp2000_send_frame(&tx_frame)) {
        return false;
    }
    
    kwp2000_frame_t rx_frame;
    if (kwp2000_receive_frame(&rx_frame, 1000)) {
        if (rx_frame.service_id == (KWP_START_DIAGNOSTIC + 0x40)) {
            ESP_LOGI(TAG, "Diagnostic session started");
            return true;
        }
    }
    
    return false;
}

QueueHandle_t kwp2000_get_response_queue(void)
{
    return kwp_response_queue;
}
