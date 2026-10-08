#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"
#include "kwp2000.h"

static const char *TAG = "KWP2000";

// Use ESP32 hardware UART1 for K-Line communication.
#define KWP_UART_NUM           UART_NUM_1
#define KWP_UART_BUFFER_SIZE   512
#define KWP_RX_TIMEOUT_MS      120
#define KWP_TX_TIMEOUT_MS      1000
#define KWP_RESPONSE_TIMEOUT_MS 120
#define KWP_INTER_BYTE_DELAY_US 1000
#define KWP_FAST_INIT_PRESET_A 1
#define KWP_FAST_INIT_PRESET_B 2
#define KWP_FAST_INIT_PRESET_C 3
#define KWP_FAST_INIT_PRESET_HDIAG 4
#define KWP_FAST_INIT_PRESET   KWP_FAST_INIT_PRESET_C

#if KWP_FAST_INIT_PRESET == KWP_FAST_INIT_PRESET_A
#define KWP_FAST_INIT_LOW_US    70000
#define KWP_FAST_INIT_HIGH_US   120000
#define KWP_FAST_INIT_W4_US     30000
#elif KWP_FAST_INIT_PRESET == KWP_FAST_INIT_PRESET_B
#define KWP_FAST_INIT_LOW_US    70000
#define KWP_FAST_INIT_HIGH_US   70000
#define KWP_FAST_INIT_W4_US     30000
#elif KWP_FAST_INIT_PRESET == KWP_FAST_INIT_PRESET_C
#define KWP_FAST_INIT_LOW_US    25000
#define KWP_FAST_INIT_HIGH_US   25000
#define KWP_FAST_INIT_W4_US     0
#elif KWP_FAST_INIT_PRESET == KWP_FAST_INIT_PRESET_HDIAG
#define KWP_FAST_INIT_LOW_US    70000
#define KWP_FAST_INIT_HIGH_US   144000
#define KWP_FAST_INIT_W4_US     0
#else
#error "Unsupported K-Line Fast Init preset"
#endif

#if KWP_FAST_INIT_PRESET == KWP_FAST_INIT_PRESET_HDIAG
#define KWP_FAST_INIT_IDLE_US   0
#else
#define KWP_FAST_INIT_IDLE_US   500000
#endif
#define KWP_FAST_INIT_RETRY_DELAY_US 2000000
#define KWP_POST_WAKEUP_READ_DELAY_MS 50
#define KWP_WAKEUP_RESPONSE_TIMEOUT_MS KWP_RESPONSE_TIMEOUT_MS
#define KWP_HARDWARE_TEST_TIMEOUT_US (45LL * 1000000LL)
#define KWP_HONDA_DESTINATION  0x72
#define KWP_HONDA_REQUEST_ALL  0x71

static QueueHandle_t kwp_response_queue = NULL;
static int kwp_rx_gpio = 16;
static int kwp_tx_gpio = 17;
static int kwp_baudrate = 10400;

static bool kwp_uart_installed = false;
static bool kwp_session_established = false;
static bool kwp_tx_hardware_test_active = false;
static esp_timer_handle_t kwp_hardware_test_timer = NULL;
static TaskHandle_t kwp_task_handle = NULL;
static int64_t kwp_fast_init_completed_us = 0;
static int64_t kwp_fast_init_idle_elapsed_us = 0;
static int64_t kwp_fast_init_low_elapsed_us = 0;
static int64_t kwp_fast_init_high_elapsed_us = 0;
static int64_t kwp_last_attempt_start_us = 0;
static uint8_t kwp_last_tx[KWP_FRAME_MAX_SIZE];
static int kwp_last_tx_len = 0;
static portMUX_TYPE kwp_tx_lock = portMUX_INITIALIZER_UNLOCKED;

static void kwp2000_task(void *arg);

static void kwp_wait_until_us(int64_t deadline_us)
{
    int64_t now_us;
    while ((now_us = esp_timer_get_time()) < deadline_us) {
        ets_delay_us((uint32_t)(deadline_us - now_us));
    }
}

static void kwp_stop_task(void)
{
    if (kwp_task_handle != NULL) {
        vTaskDelete(kwp_task_handle);
        kwp_task_handle = NULL;
    }
}

static bool kwp_configure_uart(bool start_task)
{
    ESP_ERROR_CHECK(uart_set_pin(KWP_UART_NUM, kwp_tx_gpio, kwp_rx_gpio,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    if (!kwp_uart_installed) {
        uart_config_t uart_config = {
            .baud_rate = kwp_baudrate,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };

        ESP_ERROR_CHECK(uart_param_config(KWP_UART_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_line_inverse(
            KWP_UART_NUM, UART_SIGNAL_TXD_INV | UART_SIGNAL_RXD_INV));
        ESP_ERROR_CHECK(uart_driver_install(KWP_UART_NUM,
                                            KWP_UART_BUFFER_SIZE,
                                            KWP_UART_BUFFER_SIZE,
                                            0, NULL, 0));
        kwp_uart_installed = true;

        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_ERROR_CHECK(uart_flush_input(KWP_UART_NUM));
        portENTER_CRITICAL(&kwp_tx_lock);
        kwp_last_tx_len = 0;
        portEXIT_CRITICAL(&kwp_tx_lock);

        ESP_LOGI(TAG, "UART initialized on pins RX=%d, TX=%d, Baudrate=%d, 8N1 inverted",
                 kwp_rx_gpio, kwp_tx_gpio, kwp_baudrate);
    }

    if (start_task) {
        BaseType_t task_result = xTaskCreatePinnedToCore(
            kwp2000_task,
            "kwp2000_task",
            4096,
            NULL,
            5,
            NULL,
            1);
        if (task_result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create KWP2000 task");
            ESP_ERROR_CHECK(uart_driver_delete(KWP_UART_NUM));
            kwp_uart_installed = false;
            kwp_task_handle = NULL;
            return false;
        }
        kwp_task_handle = xTaskGetHandle("kwp2000_task");
        if (kwp_task_handle == NULL) {
            ESP_LOGE(TAG, "KWP2000 task was created but handle lookup failed");
            ESP_ERROR_CHECK(uart_driver_delete(KWP_UART_NUM));
            kwp_uart_installed = false;
            return false;
        }
    }

    return true;
}

static bool kwp_uart_write_frame(const uint8_t *buffer, int frame_len)
{
    if (buffer == NULL || frame_len <= 0) {
        return false;
    }

    for (int i = 0; i < frame_len; i++) {
        if (uart_write_bytes(KWP_UART_NUM, (const char *)&buffer[i], 1) != 1) {
            ESP_LOGE(TAG, "UART write failed at byte %d/%d", i + 1, frame_len);
            return false;
        }

        if (uart_wait_tx_done(KWP_UART_NUM,
                              pdMS_TO_TICKS(KWP_TX_TIMEOUT_MS)) != ESP_OK) {
            ESP_LOGE(TAG, "UART TX timed out at byte %d/%d", i + 1, frame_len);
            return false;
        }

        if (i + 1 < frame_len) {
            ets_delay_us(KWP_INTER_BYTE_DELAY_US);
        }
    }

    return true;
}

static void kwp_hardware_test_timeout(void *arg)
{
    (void)arg;
    kwp2000_stop_tx_hardware_test();
}

bool kwp2000_start_tx_hardware_test(bool force_low)
{
    if (kwp_uart_installed || kwp_session_established || kwp_tx_gpio < 0) {
        ESP_LOGE(TAG, "Cannot start TX hardware test while UART or session is active");
        return false;
    }

    if (kwp_hardware_test_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = kwp_hardware_test_timeout,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "kline_hw_test"
        };
        esp_err_t err = esp_timer_create(&timer_args, &kwp_hardware_test_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create hardware test timer: %s", esp_err_to_name(err));
            return false;
        }
    }

    gpio_reset_pin((gpio_num_t)kwp_tx_gpio);
    gpio_set_direction((gpio_num_t)kwp_tx_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)kwp_tx_gpio, force_low ? 0 : 1);
    kwp_tx_hardware_test_active = true;
    esp_timer_stop(kwp_hardware_test_timer);
    esp_timer_start_once(kwp_hardware_test_timer, KWP_HARDWARE_TEST_TIMEOUT_US);
    ESP_LOGW(TAG, "TX hardware test forced %s; automatic release in 45 seconds",
             force_low ? "LOW" : "HIGH");
    return true;
}

void kwp2000_stop_tx_hardware_test(void)
{
    if (!kwp_tx_hardware_test_active) {
        return;
    }

    if (kwp_hardware_test_timer != NULL) {
        esp_timer_stop(kwp_hardware_test_timer);
    }
    gpio_set_level((gpio_num_t)kwp_tx_gpio, 1);
    gpio_set_direction((gpio_num_t)kwp_tx_gpio, GPIO_MODE_INPUT);
    kwp_tx_hardware_test_active = false;
    ESP_LOGI(TAG, "TX hardware test stopped; TX pin released HIGH");
}

static int kwp_consume_last_tx_echo(uint8_t *buffer, int len)
{
    int echo_offset = -1;
    int echo_len = 0;
    bool masked_checksum = false;

    portENTER_CRITICAL(&kwp_tx_lock);
    if (kwp_last_tx_len > 0) {
        for (int offset = 0; offset <= len - kwp_last_tx_len; offset++) {
            if (memcmp(buffer + offset, kwp_last_tx, kwp_last_tx_len) == 0) {
                echo_offset = offset;
                echo_len = kwp_last_tx_len;
                break;
            }

        }

        /*
         * Some transceivers return the first byte of the echo incorrectly.
         * Match the stable address/service/checksum suffix as well.
         */
        /*
         * The transceiver can alter the echoed checksum byte. Match the
         * complete frame except its checksum, but consume the complete echo.
         */
        if (echo_offset < 0 && kwp_last_tx_len >= 2) {
            const int frame_without_checksum = kwp_last_tx_len - 1;
            for (int offset = 0;
                 offset <= len - kwp_last_tx_len;
                 offset++) {
                if (memcmp(buffer + offset,
                           kwp_last_tx,
                           frame_without_checksum) == 0) {
                    echo_offset = offset;
                    echo_len = kwp_last_tx_len;
                    break;
                }

                /*
                 * Some connected DLC/transceiver combinations change only
                 * this echoed checksum byte from 0x73 to 0x71.
                 */
                if (memcmp(buffer + offset,
                           kwp_last_tx,
                           frame_without_checksum) == 0 &&
                    kwp_last_tx[kwp_last_tx_len - 1] == 0x73 &&
                    buffer[offset + frame_without_checksum] == 0x71) {
                    echo_offset = offset;
                    echo_len = kwp_last_tx_len;
                    masked_checksum = true;
                    break;
                }
            }
        }

        /*
         * The Honda wake-up message has no checksum. On this transceiver the
         * echoed payload may be altered, but its FE 04 prefix remains stable.
         */
        if (echo_offset < 0 && kwp_last_tx_len == 4 &&
            kwp_last_tx[0] == 0xFE && kwp_last_tx[1] == 0x04) {
            for (int offset = 0; offset <= len - 4; offset++) {
                if (buffer[offset] == 0xFE && buffer[offset + 1] == 0x04) {
                    echo_offset = offset;
                    echo_len = 4;
                    break;
                }
            }
        }

        if (echo_offset >= 0) {
            memmove(buffer, buffer + echo_offset + echo_len,
                    len - echo_offset - echo_len);
            len -= echo_offset + echo_len;
            kwp_last_tx_len = 0;
        }
    }
    portEXIT_CRITICAL(&kwp_tx_lock);

    if (echo_offset >= 0) {
        ESP_LOGD(TAG, "Discarded TX echo at offset %d (%d bytes)",
                 echo_offset, echo_len);
        if (masked_checksum) {
            ESP_LOGW(TAG, "Masked echoed TX checksum 0x71 as expected 0x73");
        }
    }

    return len;
}

static bool kwp2000_send_bytes(const uint8_t *buffer, int frame_len)
{
    if (buffer == NULL || frame_len <= 0 || frame_len > KWP_FRAME_MAX_SIZE) {
        return false;
    }

    portENTER_CRITICAL(&kwp_tx_lock);
    memcpy(kwp_last_tx, buffer, frame_len);
    kwp_last_tx_len = frame_len;
    portEXIT_CRITICAL(&kwp_tx_lock);

    bool sent = kwp_uart_write_frame(buffer, frame_len);
    ESP_LOGI(TAG, "TX frame: %d/%d bytes", sent ? frame_len : 0, frame_len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, buffer, frame_len, ESP_LOG_INFO);
    return sent;
}

static bool kwp2000_send_wakeup_frame(const uint8_t *buffer, int frame_len)
{
    if (buffer == NULL || frame_len <= 0 || frame_len > KWP_FRAME_MAX_SIZE ||
        kwp_fast_init_completed_us == 0) {
        return false;
    }

    portENTER_CRITICAL(&kwp_tx_lock);
    memcpy(kwp_last_tx, buffer, frame_len);
    kwp_last_tx_len = frame_len;
    portEXIT_CRITICAL(&kwp_tx_lock);

    if (KWP_FAST_INIT_W4_US > 0) {
        int64_t send_deadline_us =
            kwp_fast_init_completed_us + KWP_FAST_INIT_W4_US;
        int64_t remaining_us = send_deadline_us - esp_timer_get_time();
        if (remaining_us > 0) {
            ets_delay_us((uint32_t)remaining_us);
        } else if (remaining_us < 0) {
            ESP_LOGE(TAG, "UART setup exceeded the configured W4 interval by %lld us",
                     (long long)-remaining_us);
            portENTER_CRITICAL(&kwp_tx_lock);
            kwp_last_tx_len = 0;
            portEXIT_CRITICAL(&kwp_tx_lock);
            kwp_fast_init_completed_us = 0;
            return false;
        }
    }

    ESP_ERROR_CHECK(uart_flush_input(KWP_UART_NUM));
    int64_t write_start_us = esp_timer_get_time();
    int64_t measured_w4_us = write_start_us - kwp_fast_init_completed_us;
    bool sent = kwp_uart_write_frame(buffer, frame_len);
    kwp_fast_init_completed_us = 0;

    ESP_LOGI(TAG, "Fast Init measured us: idle=%lld low=%lld high=%lld W4_to_uart_write=%lld",
             (long long)kwp_fast_init_idle_elapsed_us,
             (long long)kwp_fast_init_low_elapsed_us,
             (long long)kwp_fast_init_high_elapsed_us,
             (long long)measured_w4_us);
    ESP_LOGI(TAG, "Wake-up UART write began %lld us after Fast Init HIGH ended",
             (long long)measured_w4_us);
    ESP_LOGI(TAG, "TX wake-up frame: %d/%d bytes",
             sent ? frame_len : 0, frame_len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, buffer, frame_len, ESP_LOG_INFO);
    return sent;
}

// Honda K-Line checksum is the two's-complement of the byte sum.
static uint8_t kwp2000_calculate_checksum(uint8_t *data, uint8_t len)
{
    uint16_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint8_t)(0x100 - (sum & 0xFF));
}

// Format frame into buffer
static int kwp2000_format_frame(kwp2000_frame_t *frame, uint8_t *buffer)
{
    int index = 0;
    
    buffer[index++] = frame->target_address;
    buffer[index++] = frame->length;
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
    if (len < 4 || buffer[1] != len) {
        return false;
    }

    frame->target_address = buffer[0];
    frame->length = buffer[1];
    frame->service_id = buffer[2];
    frame->sender_address = 0;
    
    int data_len = len - 4;
    if (data_len < 0 || data_len >= KWP_FRAME_MAX_SIZE) {
        return false;
    }
    
    for (int i = 0; i < data_len; i++) {
        frame->data[i] = buffer[i + 3];
    }
    frame->data_len = data_len;
    frame->checksum = buffer[len - 1];
    
    // Verify checksum
    uint8_t calc_checksum = kwp2000_calculate_checksum(buffer, len - 1);
    if (calc_checksum != frame->checksum) {
        ESP_LOGW(TAG, "Checksum mismatch: expected 0x%02X, got 0x%02X", calc_checksum, frame->checksum);
        ESP_LOG_BUFFER_HEXDUMP(TAG, buffer, len, ESP_LOG_WARN);
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
        int len = uart_read_bytes(KWP_UART_NUM, rx_buffer, 1,
                                  pdMS_TO_TICKS(KWP_RX_TIMEOUT_MS));
        if (len == 1) {
            if (rx_buffer[0] == 0x72) {
                int received_len = uart_read_bytes(
                    KWP_UART_NUM, rx_buffer + 1, 1,
                    pdMS_TO_TICKS(KWP_RX_TIMEOUT_MS));
                if (received_len == 1 && rx_buffer[1] == 0x8C) {
                    memset(&rx_frame, 0, sizeof(rx_frame));
                    rx_frame.target_address = 0x72;
                    rx_frame.length = 0x02;
                    rx_frame.service_id = 0x8C;
                    msg.frame = rx_frame;
                    msg.timestamp = xTaskGetTickCount();
                    msg.is_valid = true;
                    ESP_LOGI(TAG, "Received Honda wake-up ACK variant: 72 8C");
                    if (kwp_response_queue != NULL) {
                        xQueueSend(kwp_response_queue, &msg, portMAX_DELAY);
                    }
                } else {
                    ESP_LOGW(TAG, "Discarding unexpected RX byte after 0x72");
                }
                continue;
            }

            if (rx_buffer[0] == 0xFE) {
                int received_len = uart_read_bytes(KWP_UART_NUM, rx_buffer + 1,
                                                    3,
                                                    pdMS_TO_TICKS(KWP_RX_TIMEOUT_MS));
                if (received_len != 3) {
                    int partial_len = received_len + 1;
                    ESP_LOGW(TAG, "Incomplete Honda wake-up-like RX: received=%d/4",
                             partial_len);
                    ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buffer, partial_len, ESP_LOG_WARN);
                    continue;
                }
                len = 4;
            } else {
                if (rx_buffer[0] != 0x0E && rx_buffer[0] != 0x02) {
                    ESP_LOGW(TAG, "Discarding unexpected RX byte while seeking frame header: 0x%02X",
                             rx_buffer[0]);
                    continue;
                }

                int header_len = uart_read_bytes(KWP_UART_NUM, rx_buffer + 1, 1,
                                                 pdMS_TO_TICKS(KWP_RX_TIMEOUT_MS));
                if (header_len != 1) {
                    ESP_LOGW(TAG, "Incomplete Honda RX header: received=%d/2",
                             len + header_len);
                    ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buffer, len + header_len,
                                           ESP_LOG_WARN);
                    continue;
                }

                if (rx_buffer[1] < 4) {
                    ESP_LOGW(TAG, "Invalid Honda frame length byte: 0x%02X",
                             rx_buffer[1]);
                    ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buffer, 2, ESP_LOG_WARN);
                    continue;
                }

                int body_len = rx_buffer[1] - 2;
                int received_len = uart_read_bytes(KWP_UART_NUM, rx_buffer + 2,
                                                    body_len,
                                                    pdMS_TO_TICKS(KWP_RX_TIMEOUT_MS));
                if (received_len != body_len) {
                    int partial_len = received_len + 2;
                    if (rx_buffer[0] == 0x0E && rx_buffer[1] == 0x04 &&
                        received_len == 1 && rx_buffer[2] == 0x72) {
                        memset(&rx_frame, 0, sizeof(rx_frame));
                        rx_frame.target_address = 0x0E;
                        rx_frame.length = 0x04;
                        rx_frame.service_id = 0x72;
                        msg.frame = rx_frame;
                        msg.timestamp = xTaskGetTickCount();
                        msg.is_valid = true;
                        ESP_LOGI(TAG,
                                 "Received Honda wake-up ACK variant without checksum: 0E 04 72");
                        if (kwp_response_queue != NULL) {
                            xQueueSend(kwp_response_queue, &msg, portMAX_DELAY);
                        }
                        continue;
                    }
                    ESP_LOGW(TAG, "Incomplete Honda RX frame: received=%d/%d",
                             partial_len, rx_buffer[1]);
                    ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buffer, partial_len,
                                           ESP_LOG_WARN);
                    continue;
                }
                len = rx_buffer[1];
            }
        }
        
        if (len > 0) {
            ESP_LOGD(TAG, "Received %d bytes", len);
            ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buffer, len, ESP_LOG_WARN);

            int filtered_len = kwp_consume_last_tx_echo(rx_buffer, len);
            if (filtered_len != len) {
                len = filtered_len;
                if (len == 0) {
                    ESP_LOGW(TAG, "Received TX echo only; no ECU response in this read");
                    continue;
                }
            }
            
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
    
    if (kwp_response_queue == NULL) {
        kwp_response_queue = xQueueCreate(10, sizeof(kwp2000_message_t));
        if (kwp_response_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create response queue");
            return;
        }
    }

    ESP_LOGI(TAG, "KWP2000 configuration stored; UART will start with diagnostic session");
}

bool kwp2000_fast_init(void)
{
    if (kwp_task_handle != NULL || kwp_tx_gpio < 0) {
        ESP_LOGE(TAG, "Fast Init requires an installed UART with no active RX task");
        return false;
    }

    ESP_ERROR_CHECK(gpio_reset_pin((gpio_num_t)kwp_tx_gpio));
    ESP_ERROR_CHECK(gpio_set_direction((gpio_num_t)kwp_tx_gpio, GPIO_MODE_OUTPUT));

    gpio_set_level((gpio_num_t)kwp_tx_gpio, 1);
    int64_t idle_start_us = esp_timer_get_time();
    const int64_t idle_deadline_us = idle_start_us + KWP_FAST_INIT_IDLE_US;
    kwp_wait_until_us(idle_deadline_us);
    int64_t idle_end_us = esp_timer_get_time();
    kwp_fast_init_idle_elapsed_us = idle_end_us - idle_start_us;

    gpio_set_level((gpio_num_t)kwp_tx_gpio, 0);
    int64_t low_start_us = esp_timer_get_time();
    kwp_wait_until_us(low_start_us + KWP_FAST_INIT_LOW_US);
    int64_t low_end_us = esp_timer_get_time();
    kwp_fast_init_low_elapsed_us = low_end_us - low_start_us;

    gpio_set_level((gpio_num_t)kwp_tx_gpio, 1);
    int64_t high_start_us = esp_timer_get_time();
    kwp_wait_until_us(high_start_us + KWP_FAST_INIT_HIGH_US);
    kwp_fast_init_completed_us = esp_timer_get_time();
    kwp_fast_init_high_elapsed_us = kwp_fast_init_completed_us - high_start_us;
    return true;
}

bool kwp2000_begin_diagnostic_session(uint8_t session_type)
{
    ESP_LOGI(TAG, "Starting Honda K-Line diagnostic sequence");
    if (kwp_session_established) {
        ESP_LOGW(TAG, "KWP2000 communication already initialized");
        return false;
    }

    int64_t now_us = esp_timer_get_time();
    if (kwp_last_attempt_start_us != 0) {
        int64_t elapsed_us = now_us - kwp_last_attempt_start_us;
        if (elapsed_us < KWP_FAST_INIT_RETRY_DELAY_US) {
            uint32_t remaining_us =
                (uint32_t)(KWP_FAST_INIT_RETRY_DELAY_US - elapsed_us);
            ESP_LOGI(TAG, "Waiting %u ms before Fast Init retry",
                     (unsigned)((remaining_us + 999) / 1000));
            vTaskDelay(pdMS_TO_TICKS(remaining_us / 1000));
            now_us = esp_timer_get_time();
            if (now_us - kwp_last_attempt_start_us < KWP_FAST_INIT_RETRY_DELAY_US) {
                ets_delay_us((uint32_t)(KWP_FAST_INIT_RETRY_DELAY_US -
                                        (now_us - kwp_last_attempt_start_us)));
            }
        }
    }
    kwp_last_attempt_start_us = esp_timer_get_time();

    if (kwp_uart_installed) {
        kwp_stop_task();
        ESP_ERROR_CHECK(uart_driver_delete(KWP_UART_NUM));
        kwp_uart_installed = false;
    }

    if (!kwp_configure_uart(false)) {
        return false;
    }

    if (!kwp2000_fast_init()) {
        return false;
    }

    if (!kwp_configure_uart(false)) {
        return false;
    }

    if (kwp_response_queue != NULL) {
        xQueueReset(kwp_response_queue);
    }

    const uint8_t wakeup_frame[] = {0xFE, 0x04, 0x72, 0x8C};
    if (!kwp2000_send_wakeup_frame(wakeup_frame, sizeof(wakeup_frame))) {
        ESP_LOGE(TAG, "Wake-up frame transmission failed");
        return false;
    }

    ESP_LOGI(TAG, "Fast Init and wake-up TX complete (LOW=%d ms HIGH=%d ms W4=%d ms)",
             KWP_FAST_INIT_LOW_US / 1000,
             KWP_FAST_INIT_HIGH_US / 1000,
             KWP_FAST_INIT_W4_US / 1000);
    if (!kwp_configure_uart(true)) {
        return false;
    }

    kwp2000_frame_t wakeup_response;
    bool wakeup_response_received =
        kwp2000_receive_frame(&wakeup_response,
                              KWP_WAKEUP_RESPONSE_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(KWP_POST_WAKEUP_READ_DELAY_MS));

    if (wakeup_response_received &&
        wakeup_response.target_address == 0x0E &&
        wakeup_response.length == 0x04 &&
        wakeup_response.service_id == 0x72 &&
        wakeup_response.data_len == 0 &&
        wakeup_response.checksum == 0x7C) {
        ESP_LOGI(TAG, "Honda wake-up response acknowledged: 0E 04 72 7C");
    } else if (wakeup_response_received &&
               wakeup_response.target_address == 0x0E &&
               wakeup_response.length == 0x04 &&
               wakeup_response.service_id == 0x72 &&
               wakeup_response.data_len == 0 &&
               wakeup_response.checksum == 0) {
        ESP_LOGW(TAG,
                 "Accepted Honda wake-up ACK without checksum: 0E 04 72");
    } else if (wakeup_response_received &&
               wakeup_response.target_address == 0x72 &&
               wakeup_response.length == 0x02 &&
               wakeup_response.service_id == 0x8C) {
        ESP_LOGW(TAG, "Accepted Honda wake-up ACK variant: 72 8C");
    } else {
        ESP_LOGW(TAG,
                 "No recognized Honda wake-up response received "
                 "(accepted: 0E 04 72 7C, 0E 04 72, or 72 8C)");
        return false;
    }

    ESP_LOGI(TAG, "Sending Honda init frame");
    bool established = kwp2000_start_diagnostic_session(session_type);
    if (established) {
        kwp_session_established = true;
        ESP_LOGI(TAG, "KWP2000 diagnostic communication established");
    } else {
        ESP_LOGW(TAG, "KWP2000 diagnostic session was not established");
    }

    return established;
}

bool kwp2000_is_initialized(void)
{
    return kwp_session_established;
}

bool kwp2000_send_frame(kwp2000_frame_t *frame)
{
    if (frame == NULL) {
        return false;
    }
    
    uint8_t buffer[KWP_FRAME_MAX_SIZE];
    int frame_len = kwp2000_format_frame(frame, buffer);

    portENTER_CRITICAL(&kwp_tx_lock);
    memcpy(kwp_last_tx, buffer, frame_len);
    kwp_last_tx_len = frame_len;
    portEXIT_CRITICAL(&kwp_tx_lock);

    bool sent = kwp_uart_write_frame(buffer, frame_len);
    
    if (sent) {
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
    uint8_t tx_frame[5] = {
        KWP_HONDA_DESTINATION,
        0x05,
        KWP_HONDA_REQUEST_ALL,
        0x00,
        0
    };
    tx_frame[4] = kwp2000_calculate_checksum(tx_frame, 4);

    if (!kwp2000_send_bytes(tx_frame, sizeof(tx_frame))) {
        return false;
    }
    
    kwp2000_frame_t rx_frame;
    if (kwp2000_receive_frame(&rx_frame, KWP_RESPONSE_TIMEOUT_MS)) {
        if (rx_frame.service_id == KWP_HONDA_REQUEST_ALL) {
            ESP_LOGI(TAG, "Honda K-Line keep-alive acknowledged");
            return true;
        }
    }
    
    return false;
}

bool kwp2000_read_data_by_id(uint16_t data_id, uint8_t *response, uint8_t *response_len)
{
    uint8_t tx_frame[5] = {
        KWP_HONDA_DESTINATION,
        0x05,
        KWP_HONDA_REQUEST_ALL,
        (uint8_t)data_id,
        0
    };
    tx_frame[4] = kwp2000_calculate_checksum(tx_frame, 4);

    if (!kwp2000_send_bytes(tx_frame, sizeof(tx_frame))) {
        return false;
    }
    
    kwp2000_frame_t rx_frame;
    if (kwp2000_receive_frame(&rx_frame, KWP_RESPONSE_TIMEOUT_MS)) {
        if (rx_frame.service_id == KWP_HONDA_REQUEST_ALL &&
            rx_frame.data_len >= 1 &&
            rx_frame.data[0] == (uint8_t)data_id) {
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
    if (kwp2000_receive_frame(&rx_frame, KWP_RESPONSE_TIMEOUT_MS)) {
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
    if (kwp2000_receive_frame(&rx_frame, KWP_RESPONSE_TIMEOUT_MS)) {
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
    if (kwp2000_receive_frame(&rx_frame, KWP_RESPONSE_TIMEOUT_MS)) {
        if (rx_frame.service_id == (KWP_CLEAR_DTC + 0x40)) {
            return true;
        }
    }
    
    return false;
}

bool kwp2000_start_diagnostic_session(uint8_t session_type)
{
    (void)session_type;
    const uint8_t init_frame[] = {0x72, 0x05, 0x00, 0xF0, 0x99};
    if (!kwp2000_send_bytes(init_frame, sizeof(init_frame))) {
        ESP_LOGE(TAG, "Honda init frame transmission failed");
        return false;
    }
    
    kwp2000_frame_t rx_frame;
    if (kwp2000_receive_frame(&rx_frame, KWP_RESPONSE_TIMEOUT_MS)) {
        if (rx_frame.target_address == 0x02 &&
            rx_frame.length == 0x04 &&
            rx_frame.service_id == 0x00 &&
            rx_frame.data_len == 0) {
            ESP_LOGI(TAG, "Honda K-Line initialization acknowledged");
            return true;
        }
    }

    ESP_LOGW(TAG, "No valid Honda init response received (expected 02 04 00 FA)");
    return false;
}

QueueHandle_t kwp2000_get_response_queue(void)
{
    return kwp_response_queue;
}
