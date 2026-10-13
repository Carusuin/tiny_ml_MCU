#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "kwp2000.h"

static const char *TAG = "KWP2000";

#define KWP_UART_NUM            UART_NUM_1
#define KWP_UART_BUFFER_SIZE    512
#define KWP_RX_TIMEOUT_MS       120
#define KWP_TX_TIMEOUT_MS       1000
#define KWP_RESPONSE_TIMEOUT_MS 120
#define KWP_WAKEUP_RESPONSE_TIMEOUT_MS 300

#define KWP_FAST_INIT_RETRY_DELAY_US 2000000
#define KWP_POST_WAKEUP_READ_DELAY_MS 50
#define KWP_HARDWARE_TEST_TIMEOUT_US (45LL * 1000000LL)

static QueueHandle_t kwp_response_queue = NULL;
static SemaphoreHandle_t kwp_rx_paused_sem = NULL;
static SemaphoreHandle_t kwp_rx_resume_sem = NULL;
static int kwp_rx_gpio = 16;
static int kwp_tx_gpio = 17;
static int kwp_baudrate = 10400;
static uint32_t kwp_byte_gap_us = 0;

static bool kwp_uart_installed = false;
static bool kwp_session_established = false;
static bool kwp_tx_hardware_test_active = false;
static esp_timer_handle_t kwp_hardware_test_timer = NULL;
static TaskHandle_t kwp_task_handle = NULL;
static bool kwp_task_stop_requested = false;
static volatile bool kwp_rx_pause_requested = false;

static uint8_t kwp_last_tx[KWP_FRAME_MAX_SIZE];
static int kwp_last_tx_len = 0;
static portMUX_TYPE kwp_tx_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t kwp_rx_history[KWP_RX_HISTORY_SIZE];
static size_t kwp_rx_history_count = 0;
static size_t kwp_rx_history_next = 0;
static uint32_t kwp_rx_history_dropped = 0;
static portMUX_TYPE kwp_rx_history_lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    int64_t timestamp_us;
    uint8_t byte;
} kwp_rx_event_t;

#define KWP_RX_EVENT_COUNT 128
static kwp_rx_event_t kwp_rx_events[KWP_RX_EVENT_COUNT];
static kwp_rx_event_t kwp_rx_summary_events[KWP_RX_EVENT_COUNT];
static size_t kwp_rx_event_count = 0;
static size_t kwp_rx_event_next = 0;
static portMUX_TYPE kwp_rx_event_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t kwp_diagnostic_start_us = 0;
static bool kwp_diagnostic_timing_active = false;
static portMUX_TYPE kwp_diagnostic_timing_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t kwp_fast_init_idle_us = 0;
static int64_t kwp_fast_init_low_us = 0;
static int64_t kwp_fast_init_high_us = 0;
static int64_t kwp_fast_init_high_end_us = 0;

typedef struct {
    size_t byte_count;
    size_t echo_count;
    size_t non_echo_count;
} kwp_rx_exchange_summary_t;

static bool kwp_pause_rx_task(void)
{
    if (kwp_task_handle == NULL) {
        return true;
    }
    if (kwp_rx_paused_sem == NULL || kwp_rx_resume_sem == NULL) {
        ESP_LOGE(TAG, "RX task pause synchronization is not initialized");
        return false;
    }

    xSemaphoreTake(kwp_rx_paused_sem, 0);
    xSemaphoreTake(kwp_rx_resume_sem, 0);
    kwp_rx_pause_requested = true;
    if (xSemaphoreTake(kwp_rx_paused_sem, pdMS_TO_TICKS(600)) != pdTRUE) {
        kwp_rx_pause_requested = false;
        xSemaphoreGive(kwp_rx_resume_sem);
        ESP_LOGE(TAG, "Timed out waiting for KWP RX task to pause");
        return false;
    }
    return true;
}

static void kwp_resume_rx_task(void)
{
    if (kwp_task_handle != NULL && kwp_rx_resume_sem != NULL) {
        kwp_rx_pause_requested = false;
        xSemaphoreGive(kwp_rx_resume_sem);
    }
}

static void kwp2000_task(void *arg);

static int64_t kwp_diagnostic_timing_start(void)
{
    int64_t start_us = esp_timer_get_time();
    portENTER_CRITICAL(&kwp_diagnostic_timing_lock);
    kwp_diagnostic_start_us = start_us;
    kwp_diagnostic_timing_active = true;
    portEXIT_CRITICAL(&kwp_diagnostic_timing_lock);
    return start_us;
}

static int64_t kwp_diagnostic_elapsed_us(void)
{
    int64_t start_us;
    bool active;

    portENTER_CRITICAL(&kwp_diagnostic_timing_lock);
    start_us = kwp_diagnostic_start_us;
    active = kwp_diagnostic_timing_active;
    portEXIT_CRITICAL(&kwp_diagnostic_timing_lock);

    return active ? esp_timer_get_time() - start_us : -1;
}

static void kwp_diagnostic_timing_stop(void)
{
    portENTER_CRITICAL(&kwp_diagnostic_timing_lock);
    kwp_diagnostic_timing_active = false;
    portEXIT_CRITICAL(&kwp_diagnostic_timing_lock);
}

// Helper penggerak level bus K-Line sesuai skema hardware
static inline void kline_drive(bool bus_low)
{
    gpio_set_level((gpio_num_t)kwp_tx_gpio, KLINE_HW_INVERTING ? (bus_low ? 1 : 0) : (bus_low ? 0 : 1));
}

static uint8_t kwp2000_calculate_checksum(uint8_t *data, uint8_t len)
{
    uint16_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint8_t)(0x100 - (sum & 0xFF));
}

static void kwp_record_rx_bytes(const uint8_t *buffer, size_t length)
{
    portENTER_CRITICAL(&kwp_rx_history_lock);
    for (size_t i = 0; i < length; i++) {
        kwp_rx_history[kwp_rx_history_next] = buffer[i];
        kwp_rx_history_next = (kwp_rx_history_next + 1) % KWP_RX_HISTORY_SIZE;
        if (kwp_rx_history_count < KWP_RX_HISTORY_SIZE) {
            kwp_rx_history_count++;
        } else {
            kwp_rx_history_dropped++;
        }
    }
    portEXIT_CRITICAL(&kwp_rx_history_lock);
}

static int kwp_uart_read_captured(uint8_t *buffer,
                                  uint32_t length,
                                  TickType_t timeout_ticks)
{
    int received = uart_read_bytes(KWP_UART_NUM, buffer, length, timeout_ticks);
    if (received > 0) {
        kwp_record_rx_bytes(buffer, (size_t)received);
        for (int i = 0; i < received; i++) {
            kwp_rx_event_t event = {
                .timestamp_us = esp_timer_get_time(),
                .byte = buffer[i],
            };
            portENTER_CRITICAL(&kwp_rx_event_lock);
            kwp_rx_events[kwp_rx_event_next] = event;
            kwp_rx_event_next = (kwp_rx_event_next + 1) % KWP_RX_EVENT_COUNT;
            if (kwp_rx_event_count < KWP_RX_EVENT_COUNT) {
                kwp_rx_event_count++;
            }
            portEXIT_CRITICAL(&kwp_rx_event_lock);
        }
    }
    return received;
}

static size_t kwp_capture_pending_rx(uint8_t *captured, size_t capacity)
{
    uint8_t buffer[64];
    size_t buffered = 0;
    size_t total_received = 0;

    if (uart_get_buffered_data_len(KWP_UART_NUM, &buffered) == ESP_OK) {
        size_t remaining = buffered;
        while (remaining > 0) {
            uint32_t read_length = remaining < sizeof(buffer)
                                       ? (uint32_t)remaining
                                       : sizeof(buffer);
            int received = kwp_uart_read_captured(buffer, read_length, 0);
            if (received <= 0) {
                break;
            }
            size_t copy_length = (size_t)received;
            if (total_received + copy_length > capacity) {
                copy_length = total_received < capacity
                                  ? capacity - total_received
                                  : 0;
            }
            if (captured != NULL && copy_length > 0) {
                memcpy(captured + total_received, buffer, copy_length);
            }
            total_received += (size_t)received;
            remaining -= (size_t)received;
        }
    }
    return total_received;
}

static void kwp_log_pending_rx_and_flush(const char *phase)
{
    size_t buffered_before = 0;
    size_t buffered_after_capture = 0;
    uint8_t captured[KWP_UART_BUFFER_SIZE];
    size_t captured_count = 0;
    size_t stored_count = 0;

    for (int attempt = 0; attempt < 8; attempt++) {
        ESP_ERROR_CHECK(uart_get_buffered_data_len(KWP_UART_NUM, &buffered_after_capture));
        if (attempt == 0) {
            buffered_before = buffered_after_capture;
            ESP_LOGI(TAG, "%s UART RX buffered before capture: %u byte(s)",
                     phase, (unsigned)buffered_before);
        }
        if (buffered_after_capture == 0) {
            break;
        }

        size_t received = kwp_capture_pending_rx(
            captured + stored_count, sizeof(captured) - stored_count);
        if (received == 0) {
            break;
        }
        captured_count += received;
        stored_count = captured_count < sizeof(captured)
                           ? captured_count
                           : sizeof(captured);
    }
    ESP_ERROR_CHECK(uart_get_buffered_data_len(KWP_UART_NUM, &buffered_after_capture));

    if (captured_count == 0) {
        ESP_LOGI(TAG, "%s UART RX capture: no bytes (driver buffer was empty)",
                 phase);
    } else {
        static const char hex_digits[] = "0123456789ABCDEF";
        for (size_t base = 0; base < stored_count; base += 32) {
            char hex[32 * 3];
            size_t length = 0;
            size_t stop = base + 32 < stored_count ? base + 32 : stored_count;
            for (size_t i = base; i < stop; i++) {
                if (i > base) hex[length++] = ' ';
                hex[length++] = hex_digits[captured[i] >> 4];
                hex[length++] = hex_digits[captured[i] & 0x0F];
            }
            hex[length] = '\0';
            ESP_LOGI(TAG, "%s UART RX captured [%u-%u]: %s",
                     phase, (unsigned)base, (unsigned)(stop - 1), hex);
        }
        if (captured_count > stored_count) {
            ESP_LOGW(TAG, "%s UART RX captured %u byte(s); only first %u fit in log buffer",
                     phase, (unsigned)captured_count, (unsigned)stored_count);
        }
    }

    ESP_LOGI(TAG, "%s UART RX buffered immediately before flush: %u byte(s)",
             phase, (unsigned)buffered_after_capture);
    if (buffered_after_capture > 0) {
        ESP_LOGW(TAG, "%s UART RX still had pending data at flush", phase);
    }
    ESP_ERROR_CHECK(uart_flush_input(KWP_UART_NUM));
}

static kwp_rx_exchange_summary_t kwp_log_rx_exchange_summary(const uint8_t *tx,
                                                            size_t tx_length,
                                                            int64_t start_us,
                                                            int64_t end_us)
{
    kwp_rx_exchange_summary_t summary = {0};
    size_t event_count;

    portENTER_CRITICAL(&kwp_rx_event_lock);
    event_count = kwp_rx_event_count;
    size_t first = (kwp_rx_event_next + KWP_RX_EVENT_COUNT - event_count) %
                   KWP_RX_EVENT_COUNT;
    for (size_t i = 0; i < event_count; i++) {
        kwp_rx_summary_events[i] =
            kwp_rx_events[(first + i) % KWP_RX_EVENT_COUNT];
    }
    portEXIT_CRITICAL(&kwp_rx_event_lock);

    size_t exchange_count = 0;
    for (size_t i = 0; i < event_count; i++) {
        if (kwp_rx_summary_events[i].timestamp_us >= start_us &&
            kwp_rx_summary_events[i].timestamp_us <= end_us) {
            kwp_rx_summary_events[exchange_count++] = kwp_rx_summary_events[i];
        }
    }

    size_t echo_offset = exchange_count;
    if (tx != NULL && tx_length > 0 && tx_length <= exchange_count) {
        for (size_t offset = 0; offset <= exchange_count - tx_length; offset++) {
            size_t matched = 0;
            while (matched < tx_length &&
                   kwp_rx_summary_events[offset + matched].byte == tx[matched]) {
                matched++;
            }
            if (matched == tx_length) {
                echo_offset = offset;
                break;
            }
        }
    }
    size_t echo_count = echo_offset < exchange_count ? tx_length : 0;

    char non_echo[3 * KWP_RX_EVENT_COUNT + 1];
    size_t non_echo_length = 0;
    size_t non_echo_count = 0;
    for (size_t i = 0; i < exchange_count; i++) {
        if (i >= echo_offset && i < echo_offset + echo_count) {
            continue;
        }
        int written = snprintf(non_echo + non_echo_length,
                               sizeof(non_echo) - non_echo_length,
                               "%s%02X", non_echo_count == 0 ? "" : " ",
                               kwp_rx_summary_events[i].byte);
        if (written < 0 || (size_t)written >= sizeof(non_echo) - non_echo_length) {
            break;
        }
        non_echo_length += (size_t)written;
        non_echo_count++;
    }
    non_echo[non_echo_length] = '\0';
    summary.byte_count = exchange_count;
    summary.echo_count = echo_count;
    summary.non_echo_count = non_echo_count;
    ESP_LOGI(TAG, "RX exchange summary: bytes=%u echo=%u non-echo=%u [%s]",
             (unsigned)exchange_count, (unsigned)echo_count,
             (unsigned)non_echo_count, non_echo);

    for (size_t base = 1; base < exchange_count; base += 16) {
        char intervals[16 * 16];
        size_t interval_length = 0;
        size_t stop = base + 16 < exchange_count ? base + 16 : exchange_count;
        for (size_t i = base; i < stop; i++) {
            int64_t interval_us =
                kwp_rx_summary_events[i].timestamp_us -
                kwp_rx_summary_events[i - 1].timestamp_us;
            int written = snprintf(intervals + interval_length,
                                   sizeof(intervals) - interval_length,
                                   "%s%lld", i == base ? "" : ",",
                                   (long long)interval_us);
            if (written < 0 || (size_t)written >= sizeof(intervals) - interval_length) {
                break;
            }
            interval_length += (size_t)written;
        }
        intervals[interval_length] = '\0';
        ESP_LOGI(TAG, "RX inter-byte intervals us [%u-%u]: %s",
                 (unsigned)base, (unsigned)(stop - 1), intervals);
    }
    return summary;
}

static bool kwp_frame_checksum_valid(const kwp2000_frame_t *frame)
{
    if (frame == NULL || frame->length < 4 ||
        frame->data_len != frame->length - 4) {
        return false;
    }

    uint8_t bytes[KWP_FRAME_MAX_SIZE];
    bytes[0] = frame->target_address;
    bytes[1] = frame->length;
    bytes[2] = frame->service_id;
    memcpy(&bytes[3], frame->data, frame->data_len);
    bytes[frame->length - 1] = frame->checksum;
    return kwp2000_calculate_checksum(bytes, frame->length - 1) ==
           frame->checksum;
}

static void kwp_log_received_frame(const char *stage,
                                   const kwp2000_frame_t *frame)
{
    if (frame == NULL || frame->length < 4 ||
        frame->data_len != frame->length - 4) {
        ESP_LOGW(TAG, "%s response frame could not be reconstructed", stage);
        return;
    }

    char bytes[3 * KWP_FRAME_MAX_SIZE + 1];
    size_t offset = 0;
    int written = snprintf(bytes, sizeof(bytes), "%02X %02X %02X",
                           frame->target_address, frame->length,
                           frame->service_id);
    if (written < 0 || (size_t)written >= sizeof(bytes)) {
        return;
    }
    offset = (size_t)written;
    for (size_t i = 0; i < frame->data_len; i++) {
        written = snprintf(bytes + offset, sizeof(bytes) - offset,
                           " %02X", frame->data[i]);
        if (written < 0 || (size_t)written >= sizeof(bytes) - offset) {
            return;
        }
        offset += (size_t)written;
    }
    snprintf(bytes + offset, sizeof(bytes) - offset, " %02X", frame->checksum);
    ESP_LOGW(TAG, "%s received frame: %s (length=%u service=0x%02X checksum=%s)",
             stage, bytes, frame->length, frame->service_id,
             kwp_frame_checksum_valid(frame) ? "valid" : "invalid");
}

size_t kwp2000_copy_rx_history(uint8_t *buffer,
                               size_t capacity,
                               uint32_t *dropped_bytes)
{
    if (buffer == NULL || capacity == 0) {
        return 0;
    }

    portENTER_CRITICAL(&kwp_rx_history_lock);
    size_t copy_count = kwp_rx_history_count;
    if (copy_count > capacity) {
        copy_count = capacity;
    }
    size_t oldest = (kwp_rx_history_next + KWP_RX_HISTORY_SIZE -
                     kwp_rx_history_count) % KWP_RX_HISTORY_SIZE;
    size_t skip = kwp_rx_history_count - copy_count;
    for (size_t i = 0; i < copy_count; i++) {
        buffer[i] = kwp_rx_history[(oldest + skip + i) % KWP_RX_HISTORY_SIZE];
    }
    if (dropped_bytes != NULL) {
        *dropped_bytes = kwp_rx_history_dropped;
    }
    portEXIT_CRITICAL(&kwp_rx_history_lock);
    return copy_count;
}

void kwp2000_clear_rx_history(void)
{
    portENTER_CRITICAL(&kwp_rx_history_lock);
    kwp_rx_history_count = 0;
    kwp_rx_history_next = 0;
    kwp_rx_history_dropped = 0;
    portEXIT_CRITICAL(&kwp_rx_history_lock);
}

void kwp2000_reset_session(void)
{
    portENTER_CRITICAL(&kwp_tx_lock);
    kwp_session_established = false;
    kwp_last_tx_len = 0;
    portEXIT_CRITICAL(&kwp_tx_lock);
    if (kwp_response_queue != NULL) {
        xQueueReset(kwp_response_queue);
    }
    ESP_LOGI(TAG, "K-Line diagnostic session reset");
}

bool kwp2000_is_initialized(void)
{
    return kwp_session_established;
}

static void kwp_stop_task(void)
{
    if (kwp_task_handle != NULL) {
        kwp_task_stop_requested = true;
        for (int i = 0; i < 50; i++) {
            if (kwp_task_handle == NULL) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (kwp_task_handle != NULL) {
            vTaskDelete(kwp_task_handle);
            kwp_task_handle = NULL;
        }
    }
}

static bool kwp_configure_uart(bool start_task)
{
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

        if (KLINE_HW_INVERTING) {
            ESP_ERROR_CHECK(uart_set_line_inverse(KWP_UART_NUM, UART_SIGNAL_TXD_INV | UART_SIGNAL_RXD_INV));
        } else {
            ESP_ERROR_CHECK(uart_set_line_inverse(KWP_UART_NUM, UART_SIGNAL_RXD_INV));
        }

        ESP_ERROR_CHECK(uart_set_pin(KWP_UART_NUM, kwp_tx_gpio, kwp_rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_ERROR_CHECK(uart_driver_install(KWP_UART_NUM,
                                            KWP_UART_BUFFER_SIZE,
                                            KWP_UART_BUFFER_SIZE,
                                            0, NULL, 0));
        kwp_uart_installed = true;

        vTaskDelay(pdMS_TO_TICKS(50));
        kwp_capture_pending_rx(NULL, 0);
        ESP_ERROR_CHECK(uart_flush_input(KWP_UART_NUM));

        ESP_LOGI(TAG, "UART initialized on RX=%d, TX=%d, Baud=%d, Invert=%d",
                 kwp_rx_gpio, kwp_tx_gpio, kwp_baudrate, KLINE_HW_INVERTING);
    } else {
        ESP_ERROR_CHECK(uart_set_pin(KWP_UART_NUM, kwp_tx_gpio, kwp_rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    }

    if (start_task && kwp_task_handle == NULL) {
        kwp_task_stop_requested = false;
        BaseType_t task_result = xTaskCreatePinnedToCore(
            kwp2000_task, "kwp2000_task", 6144, NULL, 5, &kwp_task_handle, 1);
        if (task_result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create KWP2000 task");
            return false;
        }
    }

    return true;
}

static bool kwp_uart_write_frame(const uint8_t *buffer, int frame_len)
{
    if (buffer == NULL || frame_len <= 0) return false;

    int64_t tx_start_us = esp_timer_get_time();
    if (kwp_byte_gap_us > 0) {
        for (int i = 0; i < frame_len; i++) {
            if (uart_write_bytes(KWP_UART_NUM, (const char *)&buffer[i], 1) != 1) {
                ESP_LOGE(TAG, "UART write failed at frame byte %d", i);
                return false;
            }
            if (uart_wait_tx_done(KWP_UART_NUM,
                                  pdMS_TO_TICKS(KWP_TX_TIMEOUT_MS)) != ESP_OK) {
                ESP_LOGE(TAG, "UART TX timed out at frame byte %d", i);
                return false;
            }
            esp_rom_delay_us(kwp_byte_gap_us);
        }
        return true;
    }

    if (uart_write_bytes(KWP_UART_NUM, (const char *)buffer, frame_len) != frame_len) {
        ESP_LOGE(TAG, "UART write failed");
        return false;
    }

    if (uart_wait_tx_done(KWP_UART_NUM, pdMS_TO_TICKS(KWP_TX_TIMEOUT_MS)) != ESP_OK) {
        ESP_LOGE(TAG, "UART TX timed out");
        return false;
    }

    uint64_t expected_wire_us =
        ((uint64_t)frame_len * 10U * 1000000U + (uint32_t)kwp_baudrate - 1U) /
        (uint32_t)kwp_baudrate;
    int64_t elapsed_us = esp_timer_get_time() - tx_start_us;
    if (elapsed_us < (int64_t)expected_wire_us) {
        esp_rom_delay_us((uint32_t)(expected_wire_us - (uint64_t)elapsed_us));
    }

    return true;
}

static void kwp_hardware_test_timeout(void *arg)
{
    (void)arg;
    kwp2000_stop_tx_hardware_test();
}

bool kwp2000_start_tx_hardware_test(bool force_bus_low)
{
    if (kwp_session_established || kwp_tx_gpio < 0) {
        ESP_LOGE(TAG, "Cannot start TX hardware test while session is active");
        return false;
    }

    if (kwp_hardware_test_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = kwp_hardware_test_timeout,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "kline_hw_test"
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &kwp_hardware_test_timer));
    }

    gpio_reset_pin((gpio_num_t)kwp_tx_gpio);
    gpio_set_direction((gpio_num_t)kwp_tx_gpio, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode((gpio_num_t)kwp_tx_gpio, GPIO_PULLDOWN_ONLY);

    kline_drive(force_bus_low);

    kwp_tx_hardware_test_active = true;
    esp_timer_stop(kwp_hardware_test_timer);
    esp_timer_start_once(kwp_hardware_test_timer, KWP_HARDWARE_TEST_TIMEOUT_US);
    ESP_LOGW(TAG, "TX hardware test forced Bus %s (release in 45s)",
             force_bus_low ? "LOW (0V)" : "HIGH (12V)");
    return true;
}

void kwp2000_stop_tx_hardware_test(void)
{
    if (!kwp_tx_hardware_test_active) return;

    if (kwp_hardware_test_timer != NULL) {
        esp_timer_stop(kwp_hardware_test_timer);
    }
    kline_drive(false); // Bus HIGH
    if (kwp_uart_installed) {
        ESP_ERROR_CHECK(uart_set_pin(KWP_UART_NUM, kwp_tx_gpio, kwp_rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    } else {
        ESP_ERROR_CHECK(gpio_set_direction((gpio_num_t)kwp_tx_gpio, GPIO_MODE_INPUT));
    }
    kwp_tx_hardware_test_active = false;
    ESP_LOGI(TAG, "TX hardware test stopped; Bus released HIGH");
}

static int kwp_consume_last_tx_echo(uint8_t *buffer, int len)
{
    int echo_offset = -1;

    portENTER_CRITICAL(&kwp_tx_lock);
    if (kwp_last_tx_len > 0) {
        for (int offset = 0; offset <= len - kwp_last_tx_len; offset++) {
            if (memcmp(buffer + offset, kwp_last_tx, kwp_last_tx_len) == 0) {
                echo_offset = offset;
                break;
            }
        }

        if (echo_offset >= 0) {
            memmove(buffer, buffer + echo_offset + kwp_last_tx_len,
                    len - echo_offset - kwp_last_tx_len);
            len -= echo_offset + kwp_last_tx_len;
            kwp_last_tx_len = 0;
        }
    }
    portEXIT_CRITICAL(&kwp_tx_lock);

    return len;
}

static bool kwp2000_send_bytes(const uint8_t *buffer, int frame_len)
{
    if (buffer == NULL || frame_len <= 0 || frame_len > KWP_FRAME_MAX_SIZE) return false;

    portENTER_CRITICAL(&kwp_tx_lock);
    memcpy(kwp_last_tx, buffer, frame_len);
    kwp_last_tx_len = frame_len;
    portEXIT_CRITICAL(&kwp_tx_lock);

    return kwp_uart_write_frame(buffer, frame_len);
}

static bool kwp2000_parse_frame(uint8_t *buffer, int len, kwp2000_frame_t *frame)
{
    if (len < 4 || buffer[1] != len) return false;

    frame->target_address = buffer[0];
    frame->length = buffer[1];
    frame->service_id = buffer[2];
    frame->sender_address = 0;

    int data_len = len - 4;
    if (data_len < 0 || data_len >= KWP_FRAME_MAX_SIZE) return false;

    for (int i = 0; i < data_len; i++) {
        frame->data[i] = buffer[i + 3];
    }
    frame->data_len = data_len;
    frame->checksum = buffer[len - 1];

    uint8_t calc_checksum = kwp2000_calculate_checksum(buffer, len - 1);
    if (calc_checksum != frame->checksum) {
        ESP_LOGD(TAG, "Checksum mismatch: calc=0x%02X, got=0x%02X", calc_checksum, frame->checksum);
        return false;
    }

    return true;
}

static bool kwp_is_rx_header(uint8_t byte)
{
    return byte == 0xFE || byte == 0x0E || byte == 0x02;
}

static void kwp_process_rx_frame(uint8_t *buffer,
                                int frame_len,
                                kwp2000_frame_t *rx_frame,
                                kwp2000_message_t *msg)
{
    frame_len = kwp_consume_last_tx_echo(buffer, frame_len);
    if (frame_len <= 0 || !kwp2000_parse_frame(buffer, frame_len, rx_frame)) {
        return;
    }

    msg->frame = *rx_frame;
    msg->timestamp = xTaskGetTickCount();
    msg->is_valid = true;

    if (kwp_response_queue != NULL) {
        xQueueSend(kwp_response_queue, msg, 0);
    }
}

static void kwp2000_task(void *arg)
{
    uint8_t rx_buffer[KWP_UART_BUFFER_SIZE];
    size_t rx_length = 0;
    size_t expected_length = 0;
    kwp2000_frame_t rx_frame;
    kwp2000_message_t msg;

    ESP_LOGI(TAG, "KWP2000 task started on core %d", xPortGetCoreID());

    while (!kwp_task_stop_requested) {
        if (kwp_rx_pause_requested) {
            rx_length = 0;
            expected_length = 0;
            xSemaphoreGive(kwp_rx_paused_sem);
            xSemaphoreTake(kwp_rx_resume_sem, portMAX_DELAY);
            continue;
        }

        int received = kwp_uart_read_captured(
            rx_buffer, 1, pdMS_TO_TICKS(KWP_RX_TIMEOUT_MS));
        if (received != 1) {
            if (rx_length > 0) {
                ESP_LOGD(TAG, "RX frame candidate timed out after %u byte(s)",
                         (unsigned)rx_length);
                rx_length = 0;
                expected_length = 0;
            }
            continue;
        }

        uint8_t byte = rx_buffer[0];
        if (rx_length == 0) {
            if (kwp_is_rx_header(byte)) {
                rx_buffer[0] = byte;
                rx_length = 1;
            }
            continue;
        }

        if (rx_length == 1) {
            if (rx_buffer[0] == 0xFE) {
                if (byte == 0x04) {
                    rx_buffer[rx_length++] = byte;
                    expected_length = 4;
                } else if (kwp_is_rx_header(byte)) {
                    rx_buffer[0] = byte;
                } else {
                    rx_length = 0;
                }
                continue;
            }

            if (byte >= 4) {
                rx_buffer[rx_length++] = byte;
                expected_length = byte;
            } else if (kwp_is_rx_header(byte)) {
                rx_buffer[0] = byte;
            } else {
                rx_length = 0;
            }
            continue;
        }

        if (rx_length >= sizeof(rx_buffer)) {
            rx_length = 0;
            expected_length = 0;
            continue;
        }

        rx_buffer[rx_length++] = byte;
        if (rx_length == expected_length) {
            kwp_process_rx_frame(rx_buffer, (int)rx_length, &rx_frame, &msg);
            rx_length = 0;
            expected_length = 0;
        } else if (rx_length > expected_length) {
            rx_length = 0;
            expected_length = 0;
        }
    }

    kwp_task_handle = NULL;
    vTaskDelete(NULL);
}

void kwp2000_init(int rx_gpio, int tx_gpio, int baudrate)
{
    kwp_rx_gpio = rx_gpio;
    kwp_tx_gpio = tx_gpio;
    kwp_baudrate = baudrate;

    if (kwp_response_queue == NULL) {
        kwp_response_queue = xQueueCreate(10, sizeof(kwp2000_message_t));
    }
    if (kwp_rx_paused_sem == NULL) {
        kwp_rx_paused_sem = xSemaphoreCreateBinary();
    }
    if (kwp_rx_resume_sem == NULL) {
        kwp_rx_resume_sem = xSemaphoreCreateBinary();
    }
    if (kwp_response_queue == NULL || kwp_rx_paused_sem == NULL ||
        kwp_rx_resume_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create KWP queue or RX task synchronization");
        return;
    }

    kwp_configure_uart(true);
}

static bool kwp_init_params_valid(const kwp_init_params_t *params)
{
    return params != NULL &&
           params->low_ms >= 10 && params->low_ms <= 110 &&
           params->high_ms >= 10 && params->high_ms <= 200 &&
           params->w4_us <= 50000 &&
           params->byte_gap_us <= 5000 &&
           params->idle_ms >= 300 && params->idle_ms <= 5000;
}

bool kwp2000_fast_init(const kwp_init_params_t *params)
{
    if (!kwp_uart_installed) {
        ESP_LOGE(TAG, "Cannot run UART Fast Init before UART is installed");
        return false;
    }
    if (!kwp_init_params_valid(params)) {
        ESP_LOGE(TAG, "Invalid Fast Init timing parameters");
        return false;
    }

    int64_t idle_start_us = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(params->idle_ms));
    int64_t idle_end_us = esp_timer_get_time();

    if (uart_wait_tx_done(KWP_UART_NUM, pdMS_TO_TICKS(100)) != ESP_OK) {
        ESP_LOGE(TAG, "UART TX did not become idle before Fast Init");
        return false;
    }

    uint32_t low_us = params->low_ms * 1000U;
    uint32_t slow_baud = (uint32_t)((9000000ULL + low_us / 2U) / low_us);
    uint32_t stop_bit_us = (1000000U + slow_baud - 1U) / slow_baud;
    esp_err_t err = uart_set_baudrate(KWP_UART_NUM, (int)slow_baud);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART Fast Init baud rate: %s", esp_err_to_name(err));
        return false;
    }

    uint32_t actual_baud = 0;
    err = uart_get_baudrate(KWP_UART_NUM, &actual_baud);
    if (err != ESP_OK || actual_baud == 0) {
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read UART Fast Init baud rate: %s", esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "UART reported an invalid Fast Init baud rate");
        }
        esp_err_t restore_err = uart_set_baudrate(KWP_UART_NUM, kwp_baudrate);
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore UART baud rate after Fast Init setup error: %s",
                     esp_err_to_name(restore_err));
        }
        return false;
    }
    stop_bit_us = (1000000U + actual_baud - 1U) / actual_baud;
    ESP_LOGI(TAG, "Fast Init baud target=%u actual=%u",
             (unsigned)slow_baud, (unsigned)actual_baud);
    uint32_t baud_difference = actual_baud > slow_baud
                                   ? actual_baud - slow_baud
                                   : slow_baud - actual_baud;
    if ((uint64_t)baud_difference * 100U > (uint64_t)slow_baud * 2U) {
        ESP_LOGE(TAG, "Fast Init baud error exceeds 2%% (target=%u actual=%u)",
                 (unsigned)slow_baud, (unsigned)actual_baud);
        esp_err_t restore_err = uart_set_baudrate(KWP_UART_NUM, kwp_baudrate);
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore UART baud rate after Fast Init baud error: %s",
                     esp_err_to_name(restore_err));
        }
        return false;
    }

    const uint8_t low_pulse_frame = 0x00;
    int written = uart_write_bytes(KWP_UART_NUM,
                                   (const char *)&low_pulse_frame,
                                   sizeof(low_pulse_frame));
    if (written != sizeof(low_pulse_frame)) {
        esp_err_t restore_err = uart_set_baudrate(KWP_UART_NUM, kwp_baudrate);
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore UART baud rate after Fast Init TX error: %s",
                     esp_err_to_name(restore_err));
        }
        ESP_LOGE(TAG, "Failed to queue UART Fast Init low pulse");
        return false;
    }

    if (uart_wait_tx_done(KWP_UART_NUM, pdMS_TO_TICKS(KWP_TX_TIMEOUT_MS)) != ESP_OK) {
        esp_err_t restore_err = uart_set_baudrate(KWP_UART_NUM, kwp_baudrate);
        if (restore_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore UART baud rate after Fast Init TX timeout: %s",
                     esp_err_to_name(restore_err));
        }
        ESP_LOGE(TAG, "UART Fast Init low pulse timed out");
        return false;
    }

    int64_t frame_done_us = esp_timer_get_time();
    const int64_t low_pulse_us =
        (9LL * 1000000LL + actual_baud - 1U) / actual_baud;
    int64_t high_start_us = frame_done_us - stop_bit_us;
    int64_t high_elapsed_us = stop_bit_us;
    uint32_t high_us = params->high_ms * 1000U;
    if (high_elapsed_us < high_us) {
        esp_rom_delay_us((uint32_t)(high_us - high_elapsed_us));
    }
    int64_t high_end_us = esp_timer_get_time();

    err = uart_set_baudrate(KWP_UART_NUM, kwp_baudrate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore UART baud rate after Fast Init: %s",
                 esp_err_to_name(err));
        return false;
    }

    kwp_fast_init_idle_us = idle_end_us - idle_start_us;
    kwp_fast_init_low_us = low_pulse_us;
    kwp_fast_init_high_us = high_end_us - high_start_us;
    kwp_fast_init_high_end_us = high_end_us;

    ESP_LOGI(TAG,
             "TIMING UART Fast Init: baud=%d, low-pulse=%lld us, stop-bit=%lld us, high-total=%lld us",
             (int)actual_baud,
             (long long)kwp_fast_init_low_us,
             (long long)stop_bit_us,
             (long long)kwp_fast_init_high_us);
    return true;
}

bool kwp2000_begin_diagnostic_session(uint8_t session_type,
                                      const kwp_init_params_t *params)
{
    (void)session_type;
    if (!kwp_init_params_valid(params)) {
        ESP_LOGE(TAG, "Invalid diagnostic initialization timing parameters");
        return false;
    }

    ESP_LOGI(TAG,
             "Diagnostic timing: idle=%u ms low=%u ms high=%u ms W4=%u us byte-gap=%u us",
             (unsigned)params->idle_ms,
             (unsigned)params->low_ms,
             (unsigned)params->high_ms,
             (unsigned)params->w4_us,
             (unsigned)params->byte_gap_us);
    ESP_LOGI(TAG, "Starting Honda K-Line Fast Init & Diagnostic Sequence");
    int64_t diagnostic_start_us = kwp_diagnostic_timing_start();
    ESP_LOGI(TAG, "TIMING +0 us diagnostic sequence started");

    kwp2000_stop_tx_hardware_test();

    if (kwp_session_established) {
        kwp2000_reset_session();
    }

    if (!kwp_pause_rx_task()) {
        kwp_diagnostic_timing_stop();
        return false;
    }

    if (kwp_response_queue != NULL) {
        xQueueReset(kwp_response_queue);
    }

    kwp_byte_gap_us = params->byte_gap_us;
    if (!kwp2000_fast_init(params)) {
        kwp_log_pending_rx_and_flush("Fast Init failure");
        kwp_resume_rx_task();
        kwp_diagnostic_timing_stop();
        return false;
    }

    kwp_log_pending_rx_and_flush("Fast Init complete");
    int64_t rx_cleanup_us = esp_timer_get_time();

    int64_t w4_elapsed_us = rx_cleanup_us - kwp_fast_init_high_end_us;
    int64_t w4_remaining_us = (int64_t)params->w4_us - w4_elapsed_us;
    if (w4_remaining_us > 0) {
        esp_rom_delay_us((uint32_t)w4_remaining_us);
    }

    // Kirim Wake-up frame FE 04 72 8C
    const uint8_t wakeup_frame[] = {0xFE, 0x04, 0x72, 0x8C};
    int64_t wakeup_tx_start_us = esp_timer_get_time();
    bool wakeup_tx_ok = kwp2000_send_bytes(wakeup_frame, sizeof(wakeup_frame));
    int64_t wakeup_tx_done_us = esp_timer_get_time();
    kwp_resume_rx_task();
    int64_t actual_w4_us = wakeup_tx_start_us - kwp_fast_init_high_end_us;
    ESP_LOGI(TAG,
             "TIMING idle=%lld ms, Fast Init LOW=%lld us HIGH=%lld us, HIGH-end->RX-cleanup=%lld us",
             (long long)(kwp_fast_init_idle_us / 1000),
             (long long)kwp_fast_init_low_us,
             (long long)kwp_fast_init_high_us,
             (long long)(rx_cleanup_us - kwp_fast_init_high_end_us));
    ESP_LOGI(TAG,
             "TIMING W4 configured=%d us, HIGH-end->TX=%lld us, RX-cleanup->TX=%lld us, TX+drain=%lld us",
             (int)params->w4_us,
             (long long)actual_w4_us,
             (long long)(wakeup_tx_start_us - rx_cleanup_us),
             (long long)(wakeup_tx_done_us - wakeup_tx_start_us));
    if (actual_w4_us > 15000) {
        ESP_LOGW(TAG, "Measured Fast Init W4 exceeded 15 ms: %lld us",
                 (long long)actual_w4_us);
    }
    ESP_LOGI(TAG, "TIMING wake-up TX started at +%lld us, completed at +%lld us",
             (long long)(wakeup_tx_start_us - diagnostic_start_us),
             (long long)(wakeup_tx_done_us - diagnostic_start_us));
    if (!wakeup_tx_ok) {
        ESP_LOGW(TAG, "TIMING wake-up TX failed at +%lld us",
                 (long long)kwp_diagnostic_elapsed_us());
        kwp_diagnostic_timing_stop();
        return false;
    }

    kwp2000_frame_t wakeup_response;
    int64_t wakeup_wait_start_us = esp_timer_get_time();
    bool wakeup_ack = kwp2000_receive_frame(&wakeup_response, KWP_WAKEUP_RESPONSE_TIMEOUT_MS);
    int64_t wakeup_wait_end_us = esp_timer_get_time();
    kwp_rx_exchange_summary_t wakeup_rx_summary =
        kwp_log_rx_exchange_summary(wakeup_frame, sizeof(wakeup_frame),
                                    wakeup_tx_start_us, wakeup_wait_end_us);
    ESP_LOGI(TAG, "TIMING wake-up response wait=%lld us, result=%s",
             (long long)(wakeup_wait_end_us - wakeup_wait_start_us),
             wakeup_ack ? "frame received" : "timeout");

    if (!wakeup_ack) {
        ESP_LOGW(TAG,
                 "Wake-up timeout: echo OK (%u byte), byte non-echo dalam 300 ms: %u",
                 (unsigned)wakeup_rx_summary.echo_count,
                 (unsigned)wakeup_rx_summary.non_echo_count);
        kwp_diagnostic_timing_stop();
        return false;
    }

    bool wakeup_ack_valid =
        (wakeup_response.target_address == 0x0E ||
         wakeup_response.target_address == 0x02) &&
        wakeup_response.length == 4 &&
        kwp_frame_checksum_valid(&wakeup_response);
    if (!wakeup_ack_valid) {
        kwp_log_received_frame("Invalid wake-up ACK", &wakeup_response);
        kwp_diagnostic_timing_stop();
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(KWP_POST_WAKEUP_READ_DELAY_MS));

    // Kirim Session Init 72 05 71 00 18
    const uint8_t init_frame[] = {0x72, 0x05, 0x71, 0x00, 0x18};
    int64_t session_tx_start_us = esp_timer_get_time();
    if (!kwp2000_send_bytes(init_frame, sizeof(init_frame))) {
        kwp_diagnostic_timing_stop();
        return false;
    }

    kwp2000_frame_t session_response;
    bool session_ack = kwp2000_receive_frame(&session_response, KWP_RESPONSE_TIMEOUT_MS);
    kwp_log_rx_exchange_summary(init_frame, sizeof(init_frame),
                                session_tx_start_us, esp_timer_get_time());

    bool session_ack_valid =
        session_ack &&
        (session_response.target_address == 0x0E ||
         session_response.target_address == 0x02) &&
        session_response.length == 5 &&
        session_response.service_id == 0x71 &&
        session_response.data_len >= 1 &&
        session_response.data[0] == 0x00 &&
        kwp_frame_checksum_valid(&session_response);
    if (session_ack_valid) {
        kwp_session_established = true;
        ESP_LOGI(TAG, "K-Line Diagnostic Session Established Successfully!");
        ESP_LOGI(TAG, "TIMING diagnostic sequence completed at +%lld us",
                 (long long)kwp_diagnostic_elapsed_us());
        kwp_diagnostic_timing_stop();
        return true;
    }

    if (session_ack) {
        kwp_log_received_frame("Invalid session ACK", &session_response);
    } else {
        ESP_LOGW(TAG, "No valid session ACK frame; raw RX bytes are shown in the exchange summary above");
    }
    ESP_LOGW(TAG, "TIMING session response missing/invalid at +%lld us",
             (long long)kwp_diagnostic_elapsed_us());
    kwp_diagnostic_timing_stop();
    return false;
}

bool kwp2000_send_frame(kwp2000_frame_t *frame)
{
    if (frame == NULL) return false;
    uint8_t buffer[KWP_FRAME_MAX_SIZE];

    int index = 0;
    buffer[index++] = frame->target_address;
    buffer[index++] = frame->length;
    buffer[index++] = frame->service_id;
    for (int i = 0; i < frame->data_len; i++) {
        buffer[index++] = frame->data[i];
    }
    buffer[index] = kwp2000_calculate_checksum(buffer, index);
    index++;

    return kwp2000_send_bytes(buffer, index);
}

bool kwp2000_receive_frame(kwp2000_frame_t *frame, uint32_t timeout_ms)
{
    if (kwp_response_queue == NULL) return false;
    kwp2000_message_t msg;
    if (xQueueReceive(kwp_response_queue, &msg, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        *frame = msg.frame;
        return msg.is_valid;
    }
    return false;
}

bool kwp2000_read_data_by_id(uint16_t data_id,
                             uint8_t *response,
                             size_t response_capacity,
                             uint8_t *response_len)
{
    if (response == NULL || response_len == NULL || response_capacity == 0) return false;

    uint8_t tx_frame[5] = {
        KWP_HONDA_DESTINATION,
        0x05,
        KWP_HONDA_REQUEST_ALL,
        (uint8_t)data_id,
        0
    };
    tx_frame[4] = kwp2000_calculate_checksum(tx_frame, 4);

    if (kwp_response_queue != NULL) xQueueReset(kwp_response_queue);
    kwp_capture_pending_rx(NULL, 0);
    uart_flush_input(KWP_UART_NUM);

    if (!kwp2000_send_bytes(tx_frame, sizeof(tx_frame))) return false;

    kwp2000_frame_t rx_frame;
    if (kwp2000_receive_frame(&rx_frame, KWP_RESPONSE_TIMEOUT_MS)) {
        if (rx_frame.service_id == KWP_HONDA_REQUEST_ALL &&
            rx_frame.data_len >= 1 &&
            rx_frame.data[0] == (uint8_t)data_id) {

            if (rx_frame.data_len > response_capacity) return false;
            *response_len = rx_frame.data_len;
            memcpy(response, rx_frame.data, rx_frame.data_len);
            return true;
        }
    }

    return false;
}

QueueHandle_t kwp2000_get_response_queue(void)
{
    return kwp_response_queue;
}
