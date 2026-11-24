#include "sd_logger.h"
#include <stdio.h>
#include <string.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"

static const char *TAG = "SD_LOGGER";

#define SD_MOUNT_POINT "/sdcard"

static bool sd_available = false;
static sdmmc_card_t *card = NULL;
static FILE *log_file = NULL;

bool sd_logger_init(void)
{
    ESP_LOGI(TAG, "Initializing SD card logger...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s). Logger disabled.", esp_err_to_name(ret));
        sd_available = false;
        return false;
    }

    log_file = fopen(SD_MOUNT_POINT "/tinyml_dataset.csv", "a");
    if (log_file == NULL) {
        ESP_LOGE(TAG, "Failed to open dataset log file");
        sd_available = false;
        return false;
    }

    // Tulis header CSV jika file baru
    fseek(log_file, 0, SEEK_END);
    if (ftell(log_file) == 0) {
        fprintf(log_file, "timestamp_ms,ecu_connected,rpm,tps_pct,map_kpa,coolant_c,speed_kph,accel_x,accel_y,accel_z\n");
        fflush(log_file);
    }

    sd_available = true;
    ESP_LOGI(TAG, "SD card ready! Dataset logging to " SD_MOUNT_POINT "/tinyml_dataset.csv");
    return true;
}

bool sd_logger_is_available(void)
{
    return sd_available;
}

bool sd_logger_log_status(uint32_t timestamp_ms,
                          bool ecu_connected,
                          uint16_t rpm,
                          uint16_t tps_tenths,
                          uint16_t map_quarters,
                          int16_t coolant_temp_c,
                          uint16_t speed_kph,
                          float accel_x,
                          float accel_y,
                          float accel_z)
{
    if (!sd_available || log_file == NULL) {
        return false;
    }

    float tps_pct = tps_tenths / 10.0f;
    float map_kpa = map_quarters / 4.0f;

    int written = fprintf(log_file, "%lu,%d,%u,%.1f,%.2f,%d,%u,%.2f,%.2f,%.2f\n",
                          (unsigned long)timestamp_ms,
                          ecu_connected ? 1 : 0,
                          (unsigned)rpm,
                          tps_pct,
                          map_kpa,
                          coolant_temp_c,
                          (unsigned)speed_kph,
                          accel_x,
                          accel_y,
                          accel_z);
    if (written < 0) {
        ESP_LOGE(TAG, "SD write failed; disabling logger");
        fclose(log_file);
        log_file = NULL;
        sd_available = false;
        return false;
    }

    fflush(log_file);
    return true;
}
