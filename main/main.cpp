#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "kwp2000.h"
#include "sd_logger.h"

static const char *TAG = "APP_MAIN";

#define WEB_LOG_LINE_COUNT 64
#define WEB_LOG_LINE_SIZE 256

static char web_log_lines[WEB_LOG_LINE_COUNT][WEB_LOG_LINE_SIZE];
static size_t web_log_count = 0;
static size_t web_log_next = 0;

// Gunakan Mutex untuk log buffer & status (bukan Critical Section)
static SemaphoreHandle_t web_log_mutex = NULL;
static SemaphoreHandle_t status_mutex = NULL;
static SemaphoreHandle_t kwp_mutex = NULL;
static SemaphoreHandle_t diagnostic_request_sem = NULL;
static SemaphoreHandle_t diagnostic_state_mutex = NULL;

static vprintf_like_t web_log_previous_vprintf = NULL;
static bool diagnostic_request_active = false;

static int web_log_vprintf(const char *format, va_list args)
{
    va_list output_args;
    va_copy(output_args, args);
    int result = web_log_previous_vprintf != NULL
        ? web_log_previous_vprintf(format, output_args)
        : vprintf(format, output_args);
    va_end(output_args);

    va_list capture_args;
    va_copy(capture_args, args);
    char line[WEB_LOG_LINE_SIZE];
    vsnprintf(line, sizeof(line), format, capture_args);
    va_end(capture_args);

    if (strstr(line, "KWP2000:") != NULL || strstr(line, "APP_MAIN:") != NULL) {
        if (web_log_mutex && xSemaphoreTake(web_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            strncpy(web_log_lines[web_log_next], line, WEB_LOG_LINE_SIZE - 1);
            web_log_lines[web_log_next][WEB_LOG_LINE_SIZE - 1] = '\0';
            web_log_next = (web_log_next + 1) % WEB_LOG_LINE_COUNT;
            if (web_log_count < WEB_LOG_LINE_COUNT) {
                web_log_count++;
            }
            xSemaphoreGive(web_log_mutex);
        }
    }

    return result;
}

static void web_log_clear(void)
{
    if (web_log_mutex && xSemaphoreTake(web_log_mutex, portMAX_DELAY) == pdTRUE) {
        web_log_count = 0;
        web_log_next = 0;
        xSemaphoreGive(web_log_mutex);
    }
}

#define KLINE_UART_NUM UART_NUM_1
#define KLINE_RX_GPIO  16
#define KLINE_TX_GPIO  17
#define KLINE_BAUDRATE 10400

#define WIFI_SSID "Honda_KWP2000"
#define WIFI_PASS "12345678"

static void wifi_init_softap(void);
static void start_webserver(void);

typedef struct {
    bool ecu_connected;
    uint16_t rpm;
    uint16_t speed_kph;
    int16_t coolant_temp_c;
    uint16_t tps_percent_tenths;
    uint16_t map_kpa_quarters;
    uint8_t dtc_count;
    uint32_t timestamp_ms;
    char status_text[32];
    bool sd_card_available;
} ecu_status_t;

static QueueHandle_t ecu_queue = NULL;
static ecu_status_t current_status = {
    .ecu_connected = false,
    .rpm = 0,
    .speed_kph = 0,
    .coolant_temp_c = 0,
    .tps_percent_tenths = 0,
    .map_kpa_quarters = 0,
    .dtc_count = 0,
    .timestamp_ms = 0,
    .status_text = "waiting",
    .sd_card_available = false
};

static const char *dashboard_html = R"html(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Honda ECU Dashboard</title>
  <style>
    body { font-family: Arial, sans-serif; background: #101827; color: white; margin: 0; padding: 24px; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 16px; }
    .card { background: #1f2937; border-radius: 12px; padding: 18px; box-shadow: 0 8px 18px rgba(0,0,0,.22); }
    .label { font-size: 12px; color: #9ca3af; text-transform: uppercase; }
    .value { font-size: 28px; font-weight: bold; margin-top: 10px; }
    .status { color: #34d399; }
    .warn { color: #fbbf24; }
    .bad { color: #f87171; }
    h1 { margin-bottom: 20px; }
    .row { display:flex; justify-content: space-between; align-items: center; }
  </style>
</head>
<body>
  <h1>Honda ECU Dashboard</h1>
  <div class="row">
    <div class="label">Connection</div>
    <div id="state" class="status">Connecting...</div>
  </div>
  <div class="grid" style="margin-top:20px;">
    <div class="card"><div class="label">RPM</div><div id="rpm" class="value">0</div></div>
    <div class="card"><div class="label">Speed</div><div id="speed" class="value">0 km/h</div></div>
    <div class="card"><div class="label">Coolant</div><div id="coolant" class="value">0 C</div></div>
    <div class="card"><div class="label">TPS</div><div id="tps" class="value">0.0%</div></div>
    <div class="card"><div class="label">MAP</div><div id="map" class="value">0.00 kPa</div></div>
    <div class="card"><div class="label">DTC</div><div id="dtc" class="value">0</div></div>
  </div>
  <div class="card" style="margin-top:20px;">
    <div class="label">DTC Codes</div>
    <div id="dtcList" style="margin-top:12px;">Loading...</div>
  </div>
  <div class="row" style="margin-top:20px;">
    <div class="label">SD Logger</div>
    <div id="sdState" class="warn">Checking...</div>
  </div>
  <button id="initButton" onclick="startDiagnostic()" style="margin-top:20px;padding:12px 18px;">
    Start K-Line Communication
  </button>
  <div id="initMessage" style="margin-top:12px;"></div>
  <div class="card" style="margin-top:20px;">
    <div class="label">TX Hardware Test (45 second timeout)</div>
    <button onclick="setTxTest('high')" style="margin-top:12px;padding:10px 14px;">Force TX HIGH</button>
    <button onclick="setTxTest('low')" style="margin-top:12px;padding:10px 14px;">Force TX LOW</button>
    <button onclick="stopTxTest()" style="margin-top:12px;padding:10px 14px;">Stop / Release TX</button>
    <div id="txTestMessage" style="margin-top:12px;"></div>
  </div>
  <div class="card" style="margin-top:20px;">
    <div class="row">
      <div class="label">Serial Log (K-Line)</div>
      <button onclick="clearLogs()">Clear</button>
    </div>
    <pre id="serialLog" style="background:#030712;color:#d1d5db;padding:12px;max-height:360px;overflow:auto;white-space:pre-wrap;font-size:12px;"></pre>
  </div>
  <script>
    async function loadStatus() {
      try {
        const response = await fetch('/api/status');
        const data = await response.json();
        document.getElementById('rpm').textContent = data.rpm + ' rpm';
        document.getElementById('speed').textContent = data.speed_kph + ' km/h';
        document.getElementById('coolant').textContent = data.coolant_temp_c + ' C';
        document.getElementById('tps').textContent =
          (data.tps_percent_tenths / 10).toFixed(1) + '%';
        document.getElementById('map').textContent =
          (data.map_kpa_quarters / 4).toFixed(2) + ' kPa';
        document.getElementById('dtc').textContent = data.dtc_count;
        const sd = document.getElementById('sdState');
        sd.textContent = data.sd_card_available ? 'SD card ready' : 'SD card not detected';
        sd.className = data.sd_card_available ? 'status' : 'warn';
        const state = document.getElementById('state');
        state.textContent = data.ecu_connected ? 'ECU connected' : 'Waiting for ECU';
        state.className = data.ecu_connected ? 'status' : 'warn';
      } catch (error) {
        document.getElementById('state').textContent = 'Error';
        document.getElementById('state').className = 'bad';
      }
    }
    async function loadDtc() {
      try {
        const response = await fetch('/api/dtc');
        const data = await response.json();
        const list = document.getElementById('dtcList');
        list.innerHTML = data.codes && data.codes.length
          ? data.codes.map(code => '<div>' + code + '</div>').join('')
          : 'No DTC codes';
      } catch (error) {
        document.getElementById('dtcList').textContent = 'Unable to fetch DTC';
      }
    }
    async function startDiagnostic() {
      const button = document.getElementById('initButton');
      const message = document.getElementById('initMessage');
      button.disabled = true;
      message.textContent = 'Starting K-Line initialization...';
      try {
        const response = await fetch('/api/start-diagnostic', { method: 'POST' });
        const data = await response.json();
        message.textContent = data.message || (data.success
          ? 'K-Line communication started' : 'K-Line initialization failed');
      } catch (error) {
        message.textContent = 'Request failed';
      } finally {
        button.disabled = false;
      }
    }
    async function loadLogs() {
      try {
        const response = await fetch('/api/logs');
        const data = await response.json();
        const log = document.getElementById('serialLog');
        const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 8;
        log.textContent = data.lines.join('');
        if (atBottom) log.scrollTop = log.scrollHeight;
      } catch (error) {
        document.getElementById('serialLog').textContent = 'Unable to fetch serial log';
      }
    }
    async function clearLogs() {
      await fetch('/api/logs/clear', { method: 'POST' });
      await loadLogs();
    }
    async function setTxTest(level) {
      const response = await fetch('/api/tx-test/' + level, { method: 'POST' });
      const data = await response.json();
      document.getElementById('txTestMessage').textContent =
        data.success ? 'TX forced ' + level.toUpperCase() + ' for up to 45 seconds' : data.error;
    }
    async function stopTxTest() {
      const response = await fetch('/api/tx-test/stop', { method: 'POST' });
      const data = await response.json();
      document.getElementById('txTestMessage').textContent =
        data.success ? 'TX released' : data.error;
    }
    loadStatus(); loadDtc(); loadLogs();
    setInterval(loadStatus, 1000);
    setInterval(loadDtc, 3000);
    setInterval(loadLogs, 500);
  </script>
</body>
</html>
)html";

static esp_err_t dashboard_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, dashboard_html, HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t uri_root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = dashboard_get_handler,
    .user_ctx = NULL,
};

static esp_err_t status_json_handler(httpd_req_t *req)
{
    char json[320];
    ecu_status_t snapshot_status;

    if (status_mutex == NULL ||
        xSemaphoreTake(status_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Status lock busy");
        return ESP_FAIL;
    }
    snapshot_status = current_status;
    xSemaphoreGive(status_mutex);

    snprintf(json, sizeof(json),
        "{\"ecu_connected\":%s,\"rpm\":%u,\"speed_kph\":%u,\"coolant_temp_c\":%d,\"tps_percent_tenths\":%u,\"map_kpa_quarters\":%u,\"dtc_count\":%u,\"status_text\":\"%s\",\"sd_card_available\":%s}",
        snapshot_status.ecu_connected ? "true" : "false",
        (unsigned)snapshot_status.rpm,
        (unsigned)snapshot_status.speed_kph,
        (int)snapshot_status.coolant_temp_c,
        (unsigned)snapshot_status.tps_percent_tenths,
        (unsigned)snapshot_status.map_kpa_quarters,
        (unsigned)snapshot_status.dtc_count,
        snapshot_status.status_text,
        snapshot_status.sd_card_available ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t uri_status = {
    .uri = "/api/status",
    .method = HTTP_GET,
    .handler = status_json_handler,
    .user_ctx = NULL,
};

static esp_err_t dtc_json_handler(httpd_req_t *req)
{
    ecu_status_t snapshot_status;
    if (status_mutex == NULL ||
        xSemaphoreTake(status_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Status lock busy");
        return ESP_FAIL;
    }
    snapshot_status = current_status;
    xSemaphoreGive(status_mutex);

    char json[128];
    snprintf(json, sizeof(json),
             "{\"dtc_count\":%u,\"codes\":[\"P0300\",\"P0171\"]}",
             snapshot_status.dtc_count);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t uri_dtc = {
    .uri = "/api/dtc",
    .method = HTTP_GET,
    .handler = dtc_json_handler,
    .user_ctx = NULL,
};

// Handler start diagnostic menjadi Asynchronous agar tidak memblokir HTTP Server
static esp_err_t start_diagnostic_handler(httpd_req_t *req)
{
    if (diagnostic_request_sem == NULL || diagnostic_state_mutex == NULL ||
        xSemaphoreTake(diagnostic_state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Diagnostic service unavailable");
        return ESP_FAIL;
    }

    if (diagnostic_request_active) {
        xSemaphoreGive(diagnostic_state_mutex);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
                           "{\"success\":false,\"message\":\"Diagnostic request already active\"}");
        return ESP_OK;
    }

    diagnostic_request_active = true;
    BaseType_t request_queued = xSemaphoreGive(diagnostic_request_sem);
    if (request_queued != pdTRUE) {
        diagnostic_request_active = false;
        xSemaphoreGive(diagnostic_state_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to queue diagnostic request");
        return ESP_FAIL;
    }
    xSemaphoreGive(diagnostic_state_mutex);

    ESP_LOGI(TAG, "Diagnostic session initialization requested from Dashboard");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Init request sent\"}");
    return ESP_OK;
}

static const httpd_uri_t uri_start_diagnostic = {
    .uri = "/api/start-diagnostic",
    .method = HTTP_POST,
    .handler = start_diagnostic_handler,
    .user_ctx = NULL,
};

static esp_err_t tx_test_handler(httpd_req_t *req)
{
    const char *mode = static_cast<const char *>(req->user_ctx);
    if (kwp_mutex == NULL ||
        xSemaphoreTake(kwp_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "KWP2000 lock busy");
        return ESP_FAIL;
    }

    bool success;
    if (strcmp(mode, "high") == 0) {
        success = kwp2000_start_tx_hardware_test(false);
    } else if (strcmp(mode, "low") == 0) {
        success = kwp2000_start_tx_hardware_test(true);
    } else {
        kwp2000_stop_tx_hardware_test();
        success = true;
    }
    xSemaphoreGive(kwp_mutex);

    httpd_resp_set_type(req, "application/json");
    if (success) {
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        httpd_resp_sendstr(req,
            "{\"success\":false,\"error\":\"TX test unavailable while UART or diagnostic session is active\"}");
    }
    return ESP_OK;
}

static const httpd_uri_t uri_tx_test_high = {
    .uri = "/api/tx-test/high",
    .method = HTTP_POST,
    .handler = tx_test_handler,
    .user_ctx = (void *)"high",
};

static const httpd_uri_t uri_tx_test_low = {
    .uri = "/api/tx-test/low",
    .method = HTTP_POST,
    .handler = tx_test_handler,
    .user_ctx = (void *)"low",
};

static const httpd_uri_t uri_tx_test_stop = {
    .uri = "/api/tx-test/stop",
    .method = HTTP_POST,
    .handler = tx_test_handler,
    .user_ctx = (void *)"stop",
};

// Log handler aman tanpa mematikan interrupt CPU
static esp_err_t logs_json_handler(httpd_req_t *req)
{
    char (*snapshot)[WEB_LOG_LINE_SIZE] =
        (char (*)[WEB_LOG_LINE_SIZE])malloc(sizeof(web_log_lines));
    if (snapshot == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to allocate log snapshot");
        return ESP_FAIL;
    }

    if (web_log_mutex == NULL || xSemaphoreTake(web_log_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        free(snapshot);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Log lock busy");
        return ESP_FAIL;
    }

    const size_t snapshot_count = web_log_count;
    const size_t snapshot_first =
        (web_log_next + WEB_LOG_LINE_COUNT - snapshot_count) % WEB_LOG_LINE_COUNT;
    for (size_t i = 0; i < snapshot_count; i++) {
        memcpy(snapshot[i], web_log_lines[(snapshot_first + i) % WEB_LOG_LINE_COUNT],
               WEB_LOG_LINE_SIZE);
    }
    xSemaphoreGive(web_log_mutex);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr_chunk(req, "{\"lines\":[");
    for (size_t i = 0; err == ESP_OK && i < snapshot_count; i++) {
        const char *line = snapshot[i];
        char escaped[WEB_LOG_LINE_SIZE * 2];
        size_t escaped_len = 0;

        for (size_t j = 0; line[j] != '\0' && escaped_len + 2 < sizeof(escaped); j++) {
            char c = line[j];
            if (c == '\\' || c == '"') escaped[escaped_len++] = '\\';
            else if (c == '\n') { escaped[escaped_len++] = '\\'; escaped[escaped_len++] = 'n'; continue; }
            else if (c == '\r') continue;
            escaped[escaped_len++] = c;
        }
        escaped[escaped_len] = '\0';

        if (i > 0) err = httpd_resp_sendstr_chunk(req, ",");
        if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, "\"");
        if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, escaped);
        if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, "\"");
    }

    if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, "]}");
    if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, NULL);
    free(snapshot);
    return err;
}

static const httpd_uri_t uri_logs = {
    .uri = "/api/logs",
    .method = HTTP_GET,
    .handler = logs_json_handler,
    .user_ctx = NULL,
};

static esp_err_t clear_logs_handler(httpd_req_t *req)
{
    web_log_clear();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true}");
    return ESP_OK;
}

static const httpd_uri_t uri_clear_logs = {
    .uri = "/api/logs/clear",
    .method = HTTP_POST,
    .handler = clear_logs_handler,
    .user_ctx = NULL,
};

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));

    wifi_config_t wifi_config = {};
    memcpy(wifi_config.ap.ssid, WIFI_SSID, strlen(WIFI_SSID));
    wifi_config.ap.ssid_len = strlen(WIFI_SSID);
    memcpy(wifi_config.ap.password, WIFI_PASS, strlen(WIFI_PASS));
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = false;
    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP started: %s", WIFI_SSID);
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 6;
    config.max_uri_handlers = 12;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_dtc));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_start_diagnostic));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_tx_test_high));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_tx_test_low));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_tx_test_stop));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_logs));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_clear_logs));
    ESP_LOGI(TAG, "HTTP dashboard started on http://192.168.4.1");
}

static void queue_consumer_task(void *arg)
{
    ecu_status_t status;
    while (1) {
        if (xQueueReceive(ecu_queue, &status, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (status_mutex && xSemaphoreTake(status_mutex, portMAX_DELAY) == pdTRUE) {
                current_status = status;
                current_status.sd_card_available = sd_logger_is_available();
                xSemaphoreGive(status_mutex);
            }

            sd_logger_log_ecu_status(status.timestamp_ms,
                                     status.ecu_connected,
                                     status.rpm,
                                     status.speed_kph,
                                     status.coolant_temp_c,
                                     status.dtc_count);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Task KWP2000 terpusat (Mengelola Init & Sampling Data)
void app_test_kwp2000(void *arg)
{
    ESP_LOGI(TAG, "Test KWP2000 task started on core %d", xPortGetCoreID());
    vTaskDelay(pdMS_TO_TICKS(2000));

    ecu_status_t status;
    memset(&status, 0, sizeof(status));

    while (1) {
        if (diagnostic_request_sem != NULL &&
            xSemaphoreTake(diagnostic_request_sem, 0) == pdTRUE) {
            ESP_LOGI(TAG, "Executing Fast Init requested from Web UI...");

            if (kwp_mutex != NULL &&
                xSemaphoreTake(kwp_mutex, portMAX_DELAY) == pdTRUE) {
                bool success = kwp2000_begin_diagnostic_session(0x81);
                if (success) {
                    kwp2000_tester_present();
                    ESP_LOGI(TAG, "K-Line Session Initialized Successfully");
                } else {
                    ESP_LOGW(TAG, "K-Line Fast Init Failed");
                }
                xSemaphoreGive(kwp_mutex);
            } else {
                ESP_LOGE(TAG, "Unable to acquire KWP2000 mutex for initialization");
            }

            if (diagnostic_state_mutex != NULL &&
                xSemaphoreTake(diagnostic_state_mutex, portMAX_DELAY) == pdTRUE) {
                diagnostic_request_active = false;
                xSemaphoreGive(diagnostic_state_mutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));

        if (kwp_mutex == NULL ||
            xSemaphoreTake(kwp_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
            continue;
        }

        uint8_t table_data[64];
        uint8_t table_len = sizeof(table_data);
        bool read_ok = false;
        bool session_initialized = kwp2000_is_initialized();
        if (session_initialized) {
            read_ok = kwp2000_read_data_by_id(0x10, table_data, &table_len);
        }
        xSemaphoreGive(kwp_mutex);

        if (!session_initialized) {
            continue;
        }

        if (read_ok) {
            if (table_len < 15) {
                status.ecu_connected = false;
                snprintf(status.status_text, sizeof(status.status_text), "invalid table 0x10");
                xQueueSend(ecu_queue, &status, portMAX_DELAY);
                continue;
            }

            status.ecu_connected = true;
            status.tps_percent_tenths = (uint16_t)table_data[5] * 5;
            status.rpm = ((uint16_t)table_data[6] << 8) | table_data[7];
            status.speed_kph = table_data[14];
            status.coolant_temp_c = (int16_t)table_data[8] - 40;
            status.map_kpa_quarters = (uint16_t)table_data[11] * 3;
            status.dtc_count = 0;
            snprintf(status.status_text, sizeof(status.status_text), "ecu ok");
            status.timestamp_ms = esp_timer_get_time() / 1000ULL;
            xQueueSend(ecu_queue, &status, portMAX_DELAY);
        } else {
            status.ecu_connected = false;
            snprintf(status.status_text, sizeof(status.status_text), "waiting");
            xQueueSend(ecu_queue, &status, portMAX_DELAY);
        }
    }
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Buat Mutex
    web_log_mutex = xSemaphoreCreateMutex();
    status_mutex = xSemaphoreCreateMutex();
    kwp_mutex = xSemaphoreCreateMutex();
    diagnostic_request_sem = xSemaphoreCreateBinary();
    diagnostic_state_mutex = xSemaphoreCreateMutex();
    if (web_log_mutex == NULL || status_mutex == NULL || kwp_mutex == NULL ||
        diagnostic_request_sem == NULL || diagnostic_state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create application synchronization primitives");
        return;
    }

    web_log_previous_vprintf = esp_log_set_vprintf(web_log_vprintf);
    ESP_LOGI(TAG, "Starting TinyML Classificator with KWP2000 + Dashboard");

    ecu_queue = xQueueCreate(10, sizeof(ecu_status_t));
    if (ecu_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create ECU queue");
        return;
    }

    bool sd_ready = sd_logger_init();
    if (xSemaphoreTake(status_mutex, portMAX_DELAY) == pdTRUE) {
        current_status.sd_card_available = sd_ready;
        xSemaphoreGive(status_mutex);
    }

    kwp2000_init(KLINE_RX_GPIO, KLINE_TX_GPIO, KLINE_BAUDRATE);
    BaseType_t queue_task_result = xTaskCreatePinnedToCore(
        queue_consumer_task, "queue_consumer", 4096, NULL, 5, NULL, 1);
    BaseType_t kwp_task_result = xTaskCreatePinnedToCore(
        app_test_kwp2000, "test_kwp2000", 4096, NULL, 5, NULL, 1);
    if (queue_task_result != pdPASS || kwp_task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create application task(s): queue=%d KWP=%d",
                 (int)queue_task_result, (int)kwp_task_result);
        return;
    }

    wifi_init_softap();
    start_webserver();

    ESP_LOGI(TAG, "Application initialized");
}