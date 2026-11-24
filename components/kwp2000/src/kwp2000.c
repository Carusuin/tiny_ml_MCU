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

// ============================================================
//  TIMING CONSTANTS – dari binary HondaECU Lite Kline Edition
//  (analisis statis eculib\honda.cp39-win32.pyd, Nov 2026)
//
//  LOW  = 70 ms  → pulse LOW wake-up via Bit-Bang (0.07s di binary)
//  HIGH = 130 ms → HIGH setelah LOW sebelum wake-up (0.13s di binary)
//  W4   = 150 ms → timeout response ECU (150ms window)
//  P2   = 25 ms  → jeda minimum antar frame
//  P3   = 80 ms  → polling interval live data
//  RTO  = 100 ms → response timeout umum
//  RETRY= 1000ms → retry/reconnect delay
// ============================================================
#define KWP_UART_NUM                    UART_NUM_1
#define KWP_UART_BUFFER_SIZE            512
#define KWP_TX_TIMEOUT_MS               1000
#define KWP_HARDWARE_TEST_TIMEOUT_US    (45LL * 1000000LL)

// --- Timing dari HondaECU Lite Kline Edition (terkonfirmasi langsung dari binary) ---
#define KWP_WAKE_LOW_MS                 70      // Durasi pulse LOW wake-up (70ms)
#define KWP_WAKE_GAP_MS                 80      // Gap mode Analyst Pro UART jika low_ms == 0 (80ms)
#define KWP_WAKE_HIGH_MS                130     // HIGH stabilisasi sebelum frame wake-up (130ms)
#define KWP_WAKE_RESPONSE_TIMEOUT_MS    150     // W4: timeout response ECU setelah wake-up
#define KWP_INTER_FRAME_DELAY_MS        25      // P2: jeda antar frame
#define KWP_POLL_INTERVAL_MS            80      // P3: polling live data
#define KWP_RESPONSE_TIMEOUT_MS         100     // RTO: response timeout umum
#define KWP_RETRY_DELAY_MS              1000    // retry/reconnect
#define KWP_RX_TIMEOUT_MS               50      // 50ms per-byte timeout aman untuk frame panjang

static QueueHandle_t kwp_response_queue = NULL;
static SemaphoreHandle_t kwp_rx_paused_sem = NULL;
static SemaphoreHandle_t kwp_rx_resume_sem = NULL;
static int kwp_rx_gpio = 16;
static int kwp_tx_gpio = 17;
static int kwp_baudrate = 10400;
static uint32_t kwp_byte_gap_us = 0;
static bool kwp_tx_inverting = KLINE_TX_INVERTING;
static bool kwp_rx_inverting = KLINE_RX_INVERTING;

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

typedef struct {
    size_t buffered_before;
    size_t captured_count;
    size_t stored_count;
    size_t buffered_before_flush;
    esp_err_t buffered_status;
    esp_err_t capture_status;
    esp_err_t flush_status;
    uint8_t captured[KWP_UART_BUFFER_SIZE];
} kwp_rx_cleanup_report_t;

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
    gpio_set_level((gpio_num_t)kwp_tx_gpio,
                   kwp_tx_inverting ? (bus_low ? 1 : 0) : (bus_low ? 0 : 1));
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

static void __attribute__((unused)) kwp_capture_and_flush_silent(kwp_rx_cleanup_report_t *report)
{
    memset(report, 0, sizeof(*report));
    report->buffered_status = uart_get_buffered_data_len(
        KWP_UART_NUM, &report->buffered_before);
    report->capture_status = ESP_OK;
    report->flush_status = ESP_OK;
    if (report->buffered_status != ESP_OK) {
        report->capture_status = report->buffered_status;
        report->flush_status = uart_flush_input(KWP_UART_NUM);
        return;
    }

    size_t buffered = report->buffered_before;
    uint8_t overflow[KWP_UART_BUFFER_SIZE];
    for (int attempt = 0; attempt < 8 && buffered > 0; attempt++) {
        uint32_t read_length = buffered < sizeof(overflow)
                                   ? (uint32_t)buffered
                                   : sizeof(overflow);
        uint8_t *destination = report->stored_count < sizeof(report->captured)
                                   ? report->captured + report->stored_count
                                   : overflow;
        size_t remaining_capacity = report->stored_count < sizeof(report->captured)
                                        ? sizeof(report->captured) - report->stored_count
                                        : 0;
        if (remaining_capacity > 0 && read_length > remaining_capacity) {
            read_length = (uint32_t)remaining_capacity;
        }

        int received = kwp_uart_read_captured(destination, read_length, 0);
        if (received < 0) {
            report->capture_status = ESP_FAIL;
            break;
        }
        if (received == 0) {
            break;
        }
        report->captured_count += (size_t)received;
        if (remaining_capacity > 0) {
            report->stored_count += (size_t)received;
        }

        report->buffered_status = uart_get_buffered_data_len(KWP_UART_NUM, &buffered);
        if (report->buffered_status != ESP_OK) {
            report->capture_status = report->buffered_status;
            break;
        }
    }

    size_t buffered_after_capture = 0;
    report->buffered_status = uart_get_buffered_data_len(
        KWP_UART_NUM, &buffered_after_capture);
    if (report->buffered_status == ESP_OK) {
        report->buffered_before_flush = buffered_after_capture;
    } else {
        report->capture_status = report->buffered_status;
    }
    report->flush_status = uart_flush_input(KWP_UART_NUM);
}

static void __attribute__((unused)) kwp_print_cleanup_report(const char *phase,
                                    const kwp_rx_cleanup_report_t *report)
{
    ESP_LOGI(TAG, "%s UART RX buffered before capture: %u byte(s)",
             phase, (unsigned)report->buffered_before);
    if (report->buffered_status != ESP_OK) {
        ESP_LOGW(TAG, "%s UART RX buffer query failed: %s",
                 phase, esp_err_to_name(report->buffered_status));
    }

    if (report->captured_count == 0) {
        ESP_LOGI(TAG, "%s UART RX capture: no bytes", phase);
    } else {
        static const char hex_digits[] = "0123456789ABCDEF";
        for (size_t base = 0; base < report->stored_count; base += 32) {
            char hex[32 * 3];
            size_t length = 0;
            size_t stop = base + 32 < report->stored_count
                              ? base + 32
                              : report->stored_count;
            for (size_t i = base; i < stop; i++) {
                if (i > base) hex[length++] = ' ';
                hex[length++] = hex_digits[report->captured[i] >> 4];
                hex[length++] = hex_digits[report->captured[i] & 0x0F];
            }
            hex[length] = '\0';
            ESP_LOGI(TAG, "%s UART RX captured [%u-%u]: %s",
                     phase, (unsigned)base, (unsigned)(stop - 1), hex);
        }
        if (report->captured_count > report->stored_count) {
            ESP_LOGW(TAG, "%s UART RX captured %u byte(s); only first %u fit in report",
                     phase, (unsigned)report->captured_count,
                     (unsigned)report->stored_count);
        }
    }

    ESP_LOGI(TAG, "%s UART RX buffered immediately before flush: %u byte(s)",
             phase, (unsigned)report->buffered_before_flush);
    if (report->capture_status != ESP_OK) {
        ESP_LOGW(TAG, "%s UART RX capture incomplete: %s",
                 phase, esp_err_to_name(report->capture_status));
    }
    if (report->buffered_before_flush > 0) {
        ESP_LOGW(TAG, "%s UART RX still had pending data at flush", phase);
    }
    if (report->flush_status != ESP_OK) {
        ESP_LOGE(TAG, "%s UART RX flush failed: %s",
                 phase, esp_err_to_name(report->flush_status));
    }
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

    char intervals[16 * 16];
    size_t interval_length = 0;
    size_t interval_count = 0;
    size_t interval_group_start = 1;
    bool have_previous_non_echo = false;
    int64_t previous_non_echo_us = 0;
    for (size_t i = 0; i < exchange_count; i++) {
        if (i >= echo_offset && i < echo_offset + echo_count) {
            continue;
        }
        int64_t timestamp_us = kwp_rx_summary_events[i].timestamp_us;
        if (have_previous_non_echo) {
            int64_t interval_us = timestamp_us - previous_non_echo_us;
            int written = snprintf(intervals + interval_length,
                                   sizeof(intervals) - interval_length,
                                   "%s%lld", interval_count == 0 ? "" : ",",
                                   (long long)interval_us);
            if (written < 0 || (size_t)written >= sizeof(intervals) - interval_length) {
                break;
            }
            interval_length += (size_t)written;
            interval_count++;
            if (interval_count == 16) {
                intervals[interval_length] = '\0';
                ESP_LOGI(TAG, "RX non-echo inter-byte intervals us [%u-%u]: %s",
                         (unsigned)interval_group_start,
                         (unsigned)(interval_group_start + interval_count - 1),
                         intervals);
                interval_count = 0;
                interval_length = 0;
                interval_group_start += 16;
            }
        }
        previous_non_echo_us = timestamp_us;
        have_previous_non_echo = true;
    }
    if (interval_count > 0) {
        intervals[interval_length] = '\0';
        ESP_LOGI(TAG, "RX non-echo inter-byte intervals us [%u-%u]: %s",
                 (unsigned)interval_group_start,
                 (unsigned)(interval_group_start + interval_count - 1),
                 intervals);
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

        uint32_t inverse_mask = 0;
        if (kwp_tx_inverting) inverse_mask |= UART_SIGNAL_TXD_INV;
        if (kwp_rx_inverting) inverse_mask |= UART_SIGNAL_RXD_INV;
        ESP_ERROR_CHECK(uart_set_line_inverse(KWP_UART_NUM, inverse_mask));

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

        ESP_LOGI(TAG, "UART initialized on RX=%d, TX=%d, Baud=%d, TX invert=%d RX invert=%d",
                 kwp_rx_gpio, kwp_tx_gpio, kwp_baudrate,
                 kwp_tx_inverting, kwp_rx_inverting);
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
    gpio_set_pull_mode((gpio_num_t)kwp_tx_gpio,
                       KLINE_TX_INVERTING ? GPIO_PULLDOWN_ONLY : GPIO_PULLUP_ONLY);

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
    bool is_echo = false;
    portENTER_CRITICAL(&kwp_tx_lock);
    if (kwp_last_tx_len > 0 && len == kwp_last_tx_len &&
        memcmp(buffer, kwp_last_tx, (size_t)len) == 0) {
        is_echo = true;
    }
    portEXIT_CRITICAL(&kwp_tx_lock);

    return is_echo ? 0 : len;
}

static bool kwp2000_send_bytes(const uint8_t *buffer, int frame_len)
{
    if (buffer == NULL || frame_len <= 0 || frame_len > KWP_FRAME_MAX_SIZE) return false;

    portENTER_CRITICAL(&kwp_tx_lock);
    memcpy(kwp_last_tx, buffer, frame_len);
    kwp_last_tx_len = frame_len;
    portEXIT_CRITICAL(&kwp_tx_lock);

    if (!kwp_uart_write_frame(buffer, frame_len)) {
        return false;
    }

    char hex[KWP_FRAME_MAX_SIZE * 3 + 1];
    size_t offset = 0;
    for (int i = 0; i < frame_len; i++) {
        int written = snprintf(hex + offset, sizeof(hex) - offset,
                               i == 0 ? "%02X" : " %02X", buffer[i]);
        if (written < 0 || (size_t)written >= sizeof(hex) - offset) {
            ESP_LOGE(TAG, "Unable to format TX frame for logging");
            return true;
        }
        offset += (size_t)written;
    }
    ESP_LOGI(TAG, "TX frame (%d byte): %s", frame_len, hex);
    return true;
}

static bool kwp2000_parse_frame(uint8_t *buffer, int len, kwp2000_frame_t *frame)
{
    if (len < 4 || buffer[1] != len) {
        ESP_LOGW(TAG, "Discarding malformed RX frame: received=%d length-byte=%u",
                 len, len >= 2 ? (unsigned)buffer[1] : 0U);
        return false;
    }

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
    // Catatan Toleransi Checksum:
    // Secara matematis 2's complement 0x100 - (0x0E + 0x04 + 0x72) = 0x7C.
    // Namun varian ECU Honda Keihin tertentu dan capture Analist Pro mengirimkan 0x7E
    // sebagai byte penutup frame wake-up ACK [0E 04 72 7E]. Pengecualian ini sengaja
    // dibuat agar tahapan inisialisasi awal tidak gagal pada ECU asli.
    bool documented_hdiag_wakeup_ack =
        len == 4 && buffer[0] == 0x0E && buffer[1] == 0x04 &&
        buffer[2] == 0x72 && buffer[3] == 0x7E;
    if (calc_checksum != frame->checksum && !documented_hdiag_wakeup_ack) {
        ESP_LOGW(TAG,
                 "Discarding RX frame with invalid checksum: target=0x%02X "
                 "length=%d calculated=0x%02X received=0x%02X",
                 buffer[0], len, calc_checksum, frame->checksum);
        return false;
    }

    return true;
}

static bool kwp_is_rx_header(uint8_t byte)
{
    // Semua byte yang mungkin menjadi awal frame pada K-Line:
    // 0xFE = KWP_HDIAG_FRAME_HEADER (wake-up)
    // 0x72 = KWP_HDIAG_SERVICE (session/data request echo & part mem read)
    // 0x0E = ECU wake-up ACK target
    // 0x02 = ECU response target
    // 0x0D = ECU sub-service #1 response target (balasan service 0x7D)
    // 0x0B = ECU sub-service #2 response target (balasan service 0x7B)
    // 0x7D, 0x7B = Sub-service ID echo
    // 0x7E = Legacy model ID echo
    return byte == 0xFE || byte == 0x72 || byte == 0x0E || byte == 0x02 ||
           byte == 0x0D || byte == 0x0B || byte == 0x7D || byte == 0x7B || byte == 0x7E;
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

    if (rx_frame->target_address == 0xFE ||
        rx_frame->target_address == 0x72 ||
        rx_frame->target_address == 0x7E ||
        rx_frame->target_address == 0x7D ||
        rx_frame->target_address == 0x7B) {
        ESP_LOGD(TAG, "Ignoring tester TX echo frame target=0x%02X",
                 rx_frame->target_address);
        return;
    }

    msg->frame = *rx_frame;
    msg->timestamp = xTaskGetTickCount();
    msg->is_valid = true;

    if (kwp_response_queue != NULL) {
        if (xQueueSend(kwp_response_queue, msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "RX frame queue full; dropping frame target=0x%02X length=%u",
                     rx_frame->target_address, rx_frame->length);
        }
    }
}

static void kwp2000_task(void *arg)
{
    uint8_t rx_buffer[KWP_UART_BUFFER_SIZE];
    uint8_t received_byte;
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
            &received_byte, 1, pdMS_TO_TICKS(KWP_RX_TIMEOUT_MS));
        if (received != 1) {
            if (rx_length > 0) {
                ESP_LOGD(TAG, "RX frame candidate timed out after %u byte(s)",
                         (unsigned)rx_length);
                rx_length = 0;
                expected_length = 0;
            }
            continue;
        }

        uint8_t byte = received_byte;
        if (rx_length == 0) {
            if (kwp_is_rx_header(byte)) {
                rx_buffer[0] = byte;
                rx_length = 1;
            }
            continue;
        }

        if (rx_length == 1) {
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

void kwp2000_set_inversion(bool tx_invert, bool rx_invert)
{
    kwp_tx_inverting = tx_invert;
    kwp_rx_inverting = rx_invert;
    if (kwp_uart_installed) {
        uint32_t inverse_mask = 0;
        if (kwp_tx_inverting) inverse_mask |= UART_SIGNAL_TXD_INV;
        if (kwp_rx_inverting) inverse_mask |= UART_SIGNAL_RXD_INV;
        uart_set_line_inverse(KWP_UART_NUM, inverse_mask);
        ESP_LOGI(TAG, "Line inversion updated: TX invert=%d, RX invert=%d",
                 kwp_tx_inverting, kwp_rx_inverting);
    }
}

void kwp2000_get_inversion(bool *tx_invert, bool *rx_invert)
{
    if (tx_invert) *tx_invert = kwp_tx_inverting;
    if (rx_invert) *rx_invert = kwp_rx_inverting;
}

static bool kwp_init_params_valid(const kwp_init_params_t *params)
{
    // Izinkan NULL — gunakan default dari define jika params NULL
    if (params == NULL) return true;
    return params->low_ms  <= 110 &&
           params->high_ms >= 10 && params->high_ms <= 300 &&
           params->byte_gap_us <= 5000 &&
           params->idle_ms >= 100 && params->idle_ms <= 5000;
}

static void kwp_delay_ms_minimum(uint32_t delay_ms)
{
    TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
    if (delay_ticks > 0) {
        vTaskDelay(delay_ticks);
    }

    uint64_t elapsed_us =
        (uint64_t)delay_ticks * portTICK_PERIOD_MS * 1000ULL;
    uint64_t requested_us = (uint64_t)delay_ms * 1000ULL;
    if (elapsed_us < requested_us) {
        esp_rom_delay_us((uint32_t)(requested_us - elapsed_us));
    }
}

bool kwp2000_fast_init(const kwp_init_params_t *params)
{
    // ===================================================================
    //  Honda K-Line Wake-up – Fast Init
    //
    //  1. Mode Utama HondaECU Lite Kline Edition (Default: low_ms == 70):
    //     • GPIO Bus LOW selama 70 ms (0.07s di binary)
    //     • Kembalikan pin TX ke UART 10400 bps
    //     • Tahan Bus HIGH selama 130 ms (0.13s di binary)
    //  2. Mode Alternatif Analyst Pro (low_ms == 0):
    //     • Kirim byte 0x00 @ 10400 bps (~860us LOW pulse)
    //     • Gap bus HIGH selama 80 ms (KWP_WAKE_GAP_MS)
    //     • Kirim byte 0x01 @ 10400 bps
    //     • Tahan bus HIGH selama 130-150 ms
    // ===================================================================

    if (!kwp_uart_installed) {
        ESP_LOGE(TAG, "Cannot run Fast Init before UART is installed");
        return false;
    }

    uint32_t low_ms  = params ? params->low_ms  : KWP_WAKE_LOW_MS;
    uint32_t high_ms = params ? params->high_ms : KWP_WAKE_HIGH_MS;
    if (high_ms < 10 || high_ms > 300) high_ms = KWP_WAKE_HIGH_MS;

    int64_t t0 = esp_timer_get_time();

    if (low_ms == 0) {
        // Mode alternatif Analyst Pro (UART byte 0x00 -> 80ms gap -> 0x01):
        ESP_LOGI(TAG, "Fast Init (Analyst Pro UART): 0x00 -> %ums gap -> 0x01 -> %ums HIGH",
                 (unsigned)KWP_WAKE_GAP_MS, (unsigned)high_ms);

        const uint8_t byte_zero = 0x00;
        const uint8_t byte_one  = 0x01;

        uart_wait_tx_done(KWP_UART_NUM, pdMS_TO_TICKS(50));

        // Kirim byte 0x00
        uart_write_bytes(KWP_UART_NUM, (const char *)&byte_zero, 1);
        uart_wait_tx_done(KWP_UART_NUM, pdMS_TO_TICKS(50));

        // Gap 80ms (bus HIGH)
        kwp_delay_ms_minimum(KWP_WAKE_GAP_MS);

        // Kirim byte 0x01
        uart_write_bytes(KWP_UART_NUM, (const char *)&byte_one, 1);
        uart_wait_tx_done(KWP_UART_NUM, pdMS_TO_TICKS(50));

        // HIGH period sebelum frame pertama
        kwp_fast_init_high_us = esp_timer_get_time();
        kwp_delay_ms_minimum(high_ms);
        kwp_fast_init_high_end_us = esp_timer_get_time();
    } else {
        // Mode utama HondaECU Lite Kline Edition (70ms LOW, 130ms HIGH)
        if (low_ms > 110) low_ms = KWP_WAKE_LOW_MS;
        ESP_LOGI(TAG, "Fast Init (HondaECU Lite pattern): %u ms LOW, %u ms HIGH",
                 (unsigned)low_ms, (unsigned)high_ms);

        uart_set_pin(KWP_UART_NUM, UART_PIN_NO_CHANGE, kwp_rx_gpio,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        gpio_reset_pin((gpio_num_t)kwp_tx_gpio);
        gpio_set_direction((gpio_num_t)kwp_tx_gpio, GPIO_MODE_OUTPUT);

        // Bus HIGH settling
        kline_drive(false);
        esp_rom_delay_us(2000);

        // Pulse LOW presisi (70 ms)
        kline_drive(true);
        kwp_fast_init_low_us = esp_timer_get_time();
        kwp_delay_ms_minimum(low_ms);
        kline_drive(false);
        int64_t t_low_end = esp_timer_get_time();

        // Kembalikan pin TX ke UART 10400 bps segera setelah pulsa LOW selesai
        ESP_ERROR_CHECK(uart_set_pin(KWP_UART_NUM, kwp_tx_gpio, kwp_rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

        // HIGH period stabilisasi (130 ms)
        kwp_fast_init_high_us = t_low_end;
        kwp_delay_ms_minimum(high_ms);
        kwp_fast_init_high_end_us = esp_timer_get_time();
    }

    kwp_fast_init_idle_us = t0;
    ESP_LOGI(TAG, "Fast Init: DONE — total=%lld us", (long long)(esp_timer_get_time() - t0));
    return true;
}

// Mengirim request lalu menunggu respons ECU dengan target_address tertentu.
// Mengembalikan true jika respons matching diterima sebelum timeout.
static bool kwp_send_and_wait(const char *stage,
                              const uint8_t *request,
                              size_t request_length,
                              uint8_t expected_target,
                              kwp2000_frame_t *out_frame,
                              uint32_t timeout_ms)
{
    // Bersihkan queue sebelum mengirim agar tidak tercampur respons lama
    if (kwp_response_queue != NULL) {
        xQueueReset(kwp_response_queue);
    }

    if (!kwp2000_send_bytes(request, (int)request_length)) {
        ESP_LOGE(TAG, "%s TX FAILED", stage);
        return false;
    }

    // Tunggu respons yang cocok dengan expected_target
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline_us) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) break;
        uint32_t wait_ms = (uint32_t)((remaining_us + 999) / 1000);
        if (wait_ms == 0) wait_ms = 1;

        kwp2000_frame_t rx;
        if (!kwp2000_receive_frame(&rx, wait_ms)) {
            break;
        }

        bool match = (expected_target == 0) ||
                     (rx.target_address == expected_target) ||
                     (expected_target == 0x0D && rx.target_address == 0x02) ||
                     (expected_target == 0x0B && rx.target_address == 0x02) ||
                     (expected_target == 0x02 && (rx.target_address == 0x0D || rx.target_address == 0x0B));

        if (match) {
            if (out_frame != NULL) *out_frame = rx;
            kwp_log_received_frame(stage, &rx);
            return true;
        }

        // Frame tidak cocok, abaikan dan terus menunggu
        kwp_log_received_frame("Ignoring non-matching frame during", &rx);
    }

    ESP_LOGW(TAG, "%s: no response within %u ms", stage, (unsigned)timeout_ms);
    return false;
}

bool kwp2000_begin_diagnostic_session(uint8_t session_type,
                                      const kwp_init_params_t *params)
{
    (void)session_type;
    if (!kwp_init_params_valid(params)) {
        ESP_LOGE(TAG, "Invalid diagnostic initialization timing parameters");
        return false;
    }

    uint32_t idle_ms     = params ? params->idle_ms     : 3000;
    uint32_t low_ms      = params ? params->low_ms      : KWP_WAKE_LOW_MS;
    uint32_t high_ms     = params ? params->high_ms     : KWP_WAKE_HIGH_MS;
    uint32_t byte_gap_us = params ? params->byte_gap_us : 0;

    ESP_LOGI(TAG,
             "Diagnostic timing: idle=%u ms low=%u ms high=%u ms byte-gap=%u us",
             (unsigned)idle_ms,
             (unsigned)low_ms,
             (unsigned)high_ms,
             (unsigned)byte_gap_us);
    ESP_LOGI(TAG, "Starting Honda K-Line Diagnostic Sequence (HondaECU Lite pattern: %u ms LOW, %u ms HIGH)",
             (unsigned)low_ms, (unsigned)high_ms);
    kwp_diagnostic_timing_start();
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

    kwp_byte_gap_us = byte_gap_us;

    if (!kwp_uart_installed) {
        ESP_LOGE(TAG, "UART not installed, cannot start diagnostic session");
        kwp_resume_rx_task();
        kwp_diagnostic_timing_stop();
        return false;
    }

    // Jalankan Fast Init (HondaECU Lite: 70ms LOW -> 130ms HIGH, atau Analyst Pro jika low_ms == 0)
    if (!kwp2000_fast_init(params)) {
        ESP_LOGE(TAG, "Init: Fast Init failed");
        kwp_resume_rx_task();
        kwp_diagnostic_timing_stop();
        return false;
    }

    // Bersihkan sisa byte echo / noise dari fase init
    uart_flush_input(KWP_UART_NUM);

    // RESUME RX task SEBELUM frame wake-up dikirim agar respon ECU tertangkap
    kwp_resume_rx_task();
    ESP_LOGI(TAG, "TIMING init pattern completed at +%lld us",
             (long long)kwp_diagnostic_elapsed_us());

    // ===================================================================
    //  Honda Keihin K-Line Diagnostic Sequence
    //  (5 Langkah Terverifikasi - 100% Responsif di ECU Honda Keihin)
    //
    //  STEP 1: Wake-up Handshake      FE 04 72 8C           -> expect 0E 04 72 7C
    //  STEP 2: Session Init #1        72 05 00 F0 99        -> expect 02 04 00 FA
    //  STEP 3: Session Init #2        72 05 00 F0 99        -> expect 02 04 00 FA
    //  STEP 4: Part Memory Read       72 07 72 00 00 05 10  -> expect ROM ID (02 0B...)
    //  STEP 5: Sub-service ID #1      7D 06 01 01 03 78     -> expect ACK (0D 09...)
    // ===================================================================

    const uint8_t wakeup_frame[]      = {0xFE, 0x04, 0x72, 0x8C};
    const uint8_t session_request[]   = {0x72, 0x05, 0x00, 0xF0, 0x99};
    const uint8_t part_mem_read[]     = {0x72, 0x07, 0x72, 0x00, 0x00, 0x05, 0x10};
    const uint8_t sub_service_id_1[]  = {0x7D, 0x06, 0x01, 0x01, 0x03, 0x78};

    kwp2000_frame_t rx_frame;
    bool wakeup_ok     = false;
    bool session1_ok   = false;
    bool session2_ok   = false;
    bool part_mem_ok   = false;
    bool sub_svc1_ok   = false;

    // ---- STEP 1/5: Wake-up Handshake [FE 04 72 8C] ----
    ESP_LOGI(TAG, "==== STEP 1/5: Wake-up [FE 04 72 8C] ====");
    wakeup_ok = kwp_send_and_wait(
        "STEP 1 Wake-up", wakeup_frame, sizeof(wakeup_frame),
        0x0E, &rx_frame, KWP_WAKE_RESPONSE_TIMEOUT_MS);
    if (wakeup_ok) {
        ESP_LOGI(TAG, "STEP 1: OK - Wake-up ACK [%02X %02X %02X %02X]",
                 rx_frame.target_address, rx_frame.length,
                 rx_frame.service_id, rx_frame.checksum);
    }

    // ---- STEP 2/5: Session Init #1 [72 05 00 F0 99] ----
    kwp_delay_ms_minimum(KWP_INTER_FRAME_DELAY_MS);
    ESP_LOGI(TAG, "==== STEP 2/5: Session Init #1 [72 05 00 F0 99] ====");
    session1_ok = kwp_send_and_wait(
        "STEP 2 Session Init #1", session_request, sizeof(session_request),
        0x02, &rx_frame, KWP_RESPONSE_TIMEOUT_MS);
    if (session1_ok) {
        ESP_LOGI(TAG, "STEP 2: OK - Session #1 ACK [%02X %02X %02X %02X]",
                 rx_frame.target_address, rx_frame.length,
                 rx_frame.service_id, rx_frame.checksum);
    }

    // ---- STEP 3/5: Session Init #2 [72 05 00 F0 99] ----
    kwp_delay_ms_minimum(KWP_INTER_FRAME_DELAY_MS);
    ESP_LOGI(TAG, "==== STEP 3/5: Session Init #2 [72 05 00 F0 99] ====");
    session2_ok = kwp_send_and_wait(
        "STEP 3 Session Init #2", session_request, sizeof(session_request),
        0x02, &rx_frame, KWP_RESPONSE_TIMEOUT_MS);
    if (session2_ok) {
        ESP_LOGI(TAG, "STEP 3: OK - Session #2 ACK [%02X %02X %02X %02X]",
                 rx_frame.target_address, rx_frame.length,
                 rx_frame.service_id, rx_frame.checksum);
    }

    // ---- STEP 4/5: Part Memory Read [72 07 72 00 00 05 10] ----
    kwp_delay_ms_minimum(KWP_INTER_FRAME_DELAY_MS);
    ESP_LOGI(TAG, "==== STEP 4/5: Part Memory Read [72 07 72 00 00 05 10] ====");
    part_mem_ok = kwp_send_and_wait(
        "STEP 4 Part Memory Read", part_mem_read, sizeof(part_mem_read),
        0x02, &rx_frame, KWP_RESPONSE_TIMEOUT_MS);
    if (part_mem_ok) {
        // Cetak ROM ID sebagai hex string
        char rom_id_str[48] = {0};
        size_t rom_len = rx_frame.data_len;
        if (rom_len > sizeof(rom_id_str) / 3) rom_len = sizeof(rom_id_str) / 3;
        size_t pos = 0;
        for (size_t i = 0; i < rom_len; i++) {
            int written = snprintf(rom_id_str + pos, sizeof(rom_id_str) - pos,
                                   "%s%02X", i == 0 ? "" : " ",
                                   rx_frame.data[i]);
            if (written < 0 || (size_t)written >= sizeof(rom_id_str) - pos) break;
            pos += (size_t)written;
        }
        ESP_LOGI(TAG, "STEP 4: OK - ROM ID: [%s] (length=%u)",
                 rom_id_str, rx_frame.length);
    }

    // ---- STEP 5/5: Sub-service ID #1 [7D 06 01 01 03 78] ----
    kwp_delay_ms_minimum(KWP_INTER_FRAME_DELAY_MS);
    ESP_LOGI(TAG, "==== STEP 5/5: Sub-service ID #1 [7D 06 01 01 03 78] ====");
    sub_svc1_ok = kwp_send_and_wait(
        "STEP 5 Sub-service #1", sub_service_id_1, sizeof(sub_service_id_1),
        0x0D, &rx_frame, KWP_RESPONSE_TIMEOUT_MS);
    if (sub_svc1_ok) {
        ESP_LOGI(TAG, "STEP 5: OK - Sub-service #1 ACK [%02X %02X %02X %02X]",
                 rx_frame.target_address, rx_frame.length,
                 rx_frame.service_id, rx_frame.checksum);
    }

    // ---- Ringkasan Hasil Diagnostik ----
    bool all_ok = wakeup_ok && session1_ok && session2_ok && part_mem_ok && sub_svc1_ok;
    bool any_response = wakeup_ok || session1_ok || session2_ok || part_mem_ok || sub_svc1_ok;
    kwp_session_established = any_response;

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  Honda Diagnostic Sequence Summary:");
    ESP_LOGI(TAG, "  STEP 1 Wake-up      [FE 04 72 8C]:          %s",
             wakeup_ok ? "OK" : "NO RESPONSE");
    ESP_LOGI(TAG, "  STEP 2 Session #1   [72 05 00 F0 99]:       %s",
             session1_ok ? "OK" : "NO RESPONSE");
    ESP_LOGI(TAG, "  STEP 3 Session #2   [72 05 00 F0 99]:       %s",
             session2_ok ? "OK" : "NO RESPONSE");
    ESP_LOGI(TAG, "  STEP 4 Part Mem     [72 07 72 00 00 05 10]: %s",
             part_mem_ok ? "OK" : "NO RESPONSE");
    ESP_LOGI(TAG, "  STEP 5 Sub-svc #1   [7D 06 01 01 03 78]:   %s",
             sub_svc1_ok ? "OK" : "NO RESPONSE");
    ESP_LOGI(TAG, "  Status: %s",
             all_ok ? "ALL OK (100% RESPONSIVE)" : (any_response ? "PARTIAL OK" : "NO RESPONSE"));
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "TIMING diagnostic sequence completed at +%lld us",
             (long long)kwp_diagnostic_elapsed_us());
    kwp_diagnostic_timing_stop();
    return any_response;
}

bool kwp2000_send_diagnosis_transition(void)
{
    // Byte 00 dan 01 pada FTDI USB sniffing adalah representasi level LOW dan HIGH
    // pada mode bit-bang, BUKAN byte data UART. Tidak perlu dikirim ke bus K-Line.
    return true;
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

bool kwp2000_send_keep_alive(void)
{
    // Frame Session / Tester Present standard Honda H-Diag [72 05 00 F0 99]
    // Menjaga sesi komunikasi ECU Keihin tetap aktif selama mode standby.
    const uint8_t keep_alive_frame[] = {0x72, 0x05, 0x00, 0xF0, 0x99};
    kwp2000_frame_t rx_frame;
    return kwp_send_and_wait(
        "Keep-Alive Heartbeat", keep_alive_frame, sizeof(keep_alive_frame),
        0x02, &rx_frame, KWP_RESPONSE_TIMEOUT_MS);
}

bool kwp2000_read_data_by_id(uint16_t data_id,
                             uint8_t *response,
                             size_t response_capacity,
                             uint8_t *response_len)
{
    if (response == NULL || response_len == NULL || response_capacity == 0) {
        ESP_LOGE(TAG, "Invalid output buffer for H-Diag table 0x%02X",
                 (unsigned)data_id);
        return false;
    }

    uint8_t tx_frame[5] = {
        0x72, 0x05, 0x71, (uint8_t)data_id, 0
    };
    tx_frame[4] = kwp2000_calculate_checksum(tx_frame, 4);

    if (kwp_response_queue != NULL) xQueueReset(kwp_response_queue);

    int64_t tx_start_us = esp_timer_get_time();
    if (!kwp2000_send_bytes(tx_frame, sizeof(tx_frame))) {
        ESP_LOGE(TAG, "Table 0x%02X request TX failed", (unsigned)data_id);
        return false;
    }

    int64_t response_deadline_us =
        esp_timer_get_time() + (int64_t)KWP_RESPONSE_TIMEOUT_MS * 1000;
    kwp2000_frame_t rx_frame;
    while (esp_timer_get_time() < response_deadline_us) {
        int64_t remaining_us = response_deadline_us - esp_timer_get_time();
        uint32_t remaining_ms = (uint32_t)((remaining_us + 999) / 1000);
        if (!kwp2000_receive_frame(&rx_frame, remaining_ms)) {
            break;
        }
        if (rx_frame.target_address == 0x02 &&
            rx_frame.service_id == 0x71 &&
            rx_frame.data_len >= 1 &&
            rx_frame.data[0] == (uint8_t)data_id) {

            if (rx_frame.data_len > response_capacity) return false;
            *response_len = rx_frame.data_len;
            memcpy(response, rx_frame.data, rx_frame.data_len);
            int64_t response_end_us = esp_timer_get_time();
            kwp_rx_exchange_summary_t summary =
                kwp_log_rx_exchange_summary(tx_frame, sizeof(tx_frame),
                                            tx_start_us, response_end_us);
            ESP_LOGI(TAG,
                     "Table 0x%02X response accepted: payload=%u byte(s), "
                     "elapsed=%lld us, RX echo=%u",
                     (unsigned)data_id, (unsigned)*response_len,
                     (long long)(response_end_us - tx_start_us),
                     (unsigned)summary.echo_count);
            return true;
        }
        kwp_log_received_frame("Ignoring non-matching H-Diag table response",
                               &rx_frame);
    }

    int64_t timeout_us = esp_timer_get_time();
    kwp_rx_exchange_summary_t summary =
        kwp_log_rx_exchange_summary(tx_frame, sizeof(tx_frame),
                                    tx_start_us, timeout_us);
    ESP_LOGW(TAG,
             "Table 0x%02X request timed out after %lld us: "
             "RX bytes=%u echo=%u non-echo=%u",
             (unsigned)data_id, (long long)(timeout_us - tx_start_us),
             (unsigned)summary.byte_count, (unsigned)summary.echo_count,
             (unsigned)summary.non_echo_count);
    return false;
}

QueueHandle_t kwp2000_get_response_queue(void)
{
    return kwp_response_queue;
}
