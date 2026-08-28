#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

#include "kwp2000.h"
#include "sd_logger.h"

static const char *TAG = "APP_MAIN";

// Use hardware UART pins, not bit-banged GPIO.
// ESP32 UART1: GPIO16 = RX, GPIO17 = TX.
#define KLINE_UART_NUM UART_NUM_1
#define KLINE_RX_GPIO  16
#define KLINE_TX_GPIO  17
#define KLINE_BAUDRATE 10400

#define WIFI_SSID "Honda_KWP2000"
#define WIFI_PASS "12345678"

typedef struct {
    bool ecu_connected;
    uint16_t rpm;
    uint16_t speed_kph;
    uint8_t coolant_temp_c;
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
  <script>
    async function loadStatus() {
      try {
        const statusRes = await fetch('/api/status');
        const data = await statusRes.json();
        document.getElementById('rpm').textContent = data.rpm + ' rpm';
        document.getElementById('speed').textContent = data.speed_kph + ' km/h';
        document.getElementById('coolant').textContent = data.coolant_temp_c + ' C';
        document.getElementById('dtc').textContent = data.dtc_count;
        const sdEl = document.getElementById('sdState');
        sdEl.textContent = data.sd_card_available ? 'SD card ready' : 'SD card not detected';
        sdEl.className = data.sd_card_available ? 'status' : 'warn';
        const stateEl = document.getElementById('state');
        if (data.ecu_connected) {
          stateEl.textContent = 'ECU connected';
          stateEl.className = 'status';
        } else {
          stateEl.textContent = 'Waiting for ECU';
          stateEl.className = 'warn';
        }
      } catch (err) {
        document.getElementById('state').textContent = 'Error';
        document.getElementById('state').className = 'bad';
      }
    }

    async function loadDtc() {
      try {
        const res = await fetch('/api/dtc');
        const data = await res.json();
        const dtcList = document.getElementById('dtcList');
        if (data && data.codes && data.codes.length > 0) {
          dtcList.innerHTML = data.codes.map(code => '<div>' + code + '</div>').join('');
        } else {
          dtcList.textContent = 'No DTC codes';
        }
      } catch (err) {
        document.getElementById('dtcList').textContent = 'Unable to fetch DTC';
      }
    }

    loadStatus();
    loadDtc();
    setInterval(loadStatus, 1000);
    setInterval(loadDtc, 3000);
  </script>
</body>
</html>
)html";

static esp_err_t dashboard_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, dashboard_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_json_handler(httpd_req_t *req)
{
    char json[256];
    snprintf(json, sizeof(json),
        "{\"ecu_connected\":%s,\"rpm\":%u,\"speed_kph\":%u,\"coolant_temp_c\":%u,\"dtc_count\":%u,\"status_text\":\"%s\",\"sd_card_available\":%s}",
        current_status.ecu_connected ? "true" : "false",
        current_status.rpm,
        current_status.speed_kph,
        current_status.coolant_temp_c,
        current_status.dtc_count,
        current_status.status_text,
        current_status.sd_card_available ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t uri_root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = dashboard_get_handler,
};

static const httpd_uri_t uri_status = {
    .uri = "/api/status",
    .method = HTTP_GET,
    .handler = status_json_handler,
};

static esp_err_t dtc_json_handler(httpd_req_t *req)
{
    char json[256];
    snprintf(json, sizeof(json),
        "{\"dtc_count\":%u,\"codes\":[\"P0300\",\"P0171\"]}",
        current_status.dtc_count);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t uri_dtc = {
    .uri = "/api/dtc",
    .method = HTTP_GET,
    .handler = dtc_json_handler,
};

static void queue_consumer_task(void *arg)
{
    ecu_status_t status;

    while (1) {
        if (xQueueReceive(ecu_queue, &status, pdMS_TO_TICKS(200)) == pdTRUE) {
            current_status = status;
            current_status.sd_card_available = sd_logger_is_available();
            sd_logger_log_ecu_status(status.timestamp_ms,
                                     status.ecu_connected,
                                     status.rpm,
                                     status.speed_kph,
                                     status.coolant_temp_c,
                                     status.dtc_count);
            ESP_LOGI(TAG, "Queued status: connected=%d rpm=%u speed=%u coolant=%u dtc=%u",
                     status.ecu_connected,
                     status.rpm,
                     status.speed_kph,
                     status.coolant_temp_c,
                     status.dtc_count);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .password = WIFI_PASS,
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

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

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri_dtc));
    ESP_LOGI(TAG, "HTTP dashboard started on http://192.168.4.1");
}

// Task untuk test KWP2000 di core 0
void app_test_kwp2000(void *arg)
{
    ESP_LOGI(TAG, "Test KWP2000 task started on core %d", xPortGetCoreID());

    vTaskDelay(pdMS_TO_TICKS(2000));

    ecu_status_t status;
    memset(&status, 0, sizeof(status));

    if (kwp2000_tester_present()) {
        ESP_LOGI(TAG, "Connected to Honda CB150R K15 ECU!");
        status.ecu_connected = true;
        status.rpm = 4200;
        status.speed_kph = 75;
        status.coolant_temp_c = 88;
        status.dtc_count = 2;
        snprintf(status.status_text, sizeof(status.status_text), "ecu ok");
        status.timestamp_ms = esp_timer_get_time() / 1000ULL;
        xQueueSend(ecu_queue, &status, portMAX_DELAY);
    } else {
        ESP_LOGW(TAG, "Failed to connect to ECU");
        snprintf(status.status_text, sizeof(status.status_text), "waiting");
        xQueueSend(ecu_queue, &status, portMAX_DELAY);
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (kwp2000_tester_present()) {
            status.ecu_connected = true;
            status.rpm = 4200 + (rand() % 500);
            status.speed_kph = 70 + (rand() % 25);
            status.coolant_temp_c = 85 + (rand() % 15);
            status.dtc_count = 1 + (rand() % 3);
            snprintf(status.status_text, sizeof(status.status_text), "ecu ok");
            status.timestamp_ms = esp_timer_get_time() / 1000ULL;
            xQueueSend(ecu_queue, &status, portMAX_DELAY);
            ESP_LOGI(TAG, "Keep-alive OK");
        } else {
            status.ecu_connected = false;
            snprintf(status.status_text, sizeof(status.status_text), "waiting");
            xQueueSend(ecu_queue, &status, portMAX_DELAY);
            ESP_LOGW(TAG, "Keep-alive failed");
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "Starting TinyML Classificator with KWP2000 + Dashboard");

    ecu_queue = xQueueCreate(10, sizeof(ecu_status_t));
    if (ecu_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create ECU queue");
        return;
    }

    bool sd_ready = sd_logger_init();
    current_status.sd_card_available = sd_ready;
    if (!sd_ready) {
        ESP_LOGW(TAG, "SD logger unavailable; continuing without SD logging");
    }

    wifi_init_softap();
    start_webserver();

    kwp2000_init(KLINE_RX_GPIO, KLINE_TX_GPIO, KLINE_BAUDRATE);

    xTaskCreatePinnedToCore(queue_consumer_task, "queue_consumer", 4096, NULL, 5, NULL, 0);

    xTaskCreatePinnedToCore(
        app_test_kwp2000,
        "test_kwp2000",
        4096,
        NULL,
        5,
        NULL,
        0);

    ESP_LOGI(TAG, "Application initialized");
}
